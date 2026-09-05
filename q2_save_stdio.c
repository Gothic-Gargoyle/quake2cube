/*
 * Quake2Cube virtual save filesystem.
 *
 * Quake II expects a writable directory tree below:
 *
 *     <gamedir>/save/
 *
 * GameCube game data lives at dvd:/baseq2, which is read-only.
 *
 * Preserve Quake's normal file-oriented save code and virtualize only
 * dvd:/baseq2/save/...
 *
 * current:
 *     Runtime scratch tree. Never persisted.
 *
 * save0:
 *     Quake II autosave.
 *
 * save1..save3:
 *     Three GameCube manual save slots.
 *
 * Persistent numbered directories are serialized independently and stored
 * as CarryHandle persistent objects inside one transaction-backed CARD file.
 */

#undef fopen
#undef fread
#undef fwrite
#undef fclose
#undef remove

#include "q2_save_stdio.h"

#include <carryhandle/ch_memcard.h>
#include <carryhandle/ch_persist.h>
#include <carryhandle/ch_tx.h>
#include <carryhandle/ch_tx_memcard_backend.h>

#include <ogc/card.h>

#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>


#define Q2_SAVE_CARD_FILENAME        "Q2CSAVE0"

/*
 * 128 GameCube CARD sectors.
 *
 * On the normal 8 KiB-sector cards this is a 1 MiB physical save.
 * CHTX uses two arenas, so crash-safe compaction still has substantial
 * room for autosave + three manual Quake II slot snapshots.
 *
 * This intentionally requires something larger than a Card 59.
 */
#define Q2_SAVE_CARD_SECTORS         128u

#define Q2_SAVE_DIRECTORY_COUNT      5
#define Q2_SAVE_PERSIST_FIRST        1
#define Q2_SAVE_PERSIST_LAST         4

#define Q2_SAVE_MAX_FILES            64
#define Q2_SAVE_NAME_MAX             64
#define Q2_SAVE_PATH_MAX             512

#define Q2_SAVE_FILE_MAX             (512u * 1024u)

/*
 * Q2GC_SAVE_FILE_GROWTH_V1
 *
 * Quake II level saves commonly cross 256 KiB. Doubling a 256 KiB
 * virtual file straight to 512 KiB creates an unnecessary contiguous
 * allocation cliff under renderer pressure.
 *
 * Keep geometric growth while files are small, then use 32 KiB steps.
 */
#define Q2_SAVE_FILE_GROW_LINEAR_THRESHOLD (256u * 1024u)
#define Q2_SAVE_FILE_GROW_CHUNK            (32u * 1024u)

/* Q2GC_SAVE_BUNDLE_CAP_SPLIT_V1B */
#define Q2_SAVE_BUNDLE_MAX           (2u * 1024u * 1024u)
#define Q2_SAVE_STORED_MAX           (512u * 1024u)

#define Q2_SAVE_BUNDLE_MAGIC         0x51325356u /* Q2SV */
#define Q2_SAVE_BUNDLE_VERSION       1u

/*
 * Persistent representation of one complete virtual save directory.
 *
 * The raw Q2SV bundle remains the in-memory representation.
 * Only the CarryHandle object is compressed.
 *
 * This gives us:
 *
 *     Quake files
 *         -> one Q2SV directory bundle
 *         -> one zlib stream
 *         -> one CH_PersistPut()
 *
 * Existing uncompressed objects from the first experimental build remain
 * readable and are migrated naturally when that slot is next saved.
 */
#define Q2_SAVE_STORED_MAGIC         0x51325a31u /* Q2Z1 */
#define Q2_SAVE_STORED_VERSION       1u
#define Q2_SAVE_STORED_HEADER_SIZE   20u


/*
 * Q2GC_SAVE_MENU_INDEX_V1
 *
 * Tiny persistent directory used only for Save/Load menu presentation.
 *
 * It deliberately does NOT contain gameplay state.  Full save bundles
 * remain the authoritative savegame data.
 *
 * Format:
 *
 *   u32  magic       "Q2SI"
 *   u32  version     1
 *   u32  valid mask
 *   byte comments[4][32]
 *
 * The comments are copied verbatim from the first 32 bytes of each
 * committed server.ssv.
 */
#define Q2_SAVE_SLOT_COUNT              4u
#define Q2_SAVE_COMMENT_SIZE            32u

#define Q2_SAVE_SLOT_INDEX_MAGIC        0x51325349u /* Q2SI */
#define Q2_SAVE_SLOT_INDEX_VERSION      1u
#define Q2_SAVE_SLOT_INDEX_HEADER_SIZE  12u

#define Q2_SAVE_SLOT_INDEX_SIZE \
    (Q2_SAVE_SLOT_INDEX_HEADER_SIZE + \
     Q2_SAVE_SLOT_COUNT * Q2_SAVE_COMMENT_SIZE)


typedef struct q2_save_file_s
{
    char name[Q2_SAVE_NAME_MAX];

    unsigned char *data;

    size_t size;
    size_t capacity;

    /*
     * A failed fwrite poisons this virtual file. Stock Quake II does
     * not check every fwrite result, so persistence must reject it.
     */
    int write_failed;


} q2_save_file_t;


typedef struct q2_save_directory_s
{
    q2_save_file_t files[Q2_SAVE_MAX_FILES];

    size_t count;

} q2_save_directory_t;


typedef struct q2_save_stream_s
{
    int used;
    int writable;

    q2_save_directory_t *directory;
    q2_save_file_t *entry;

    size_t position;

} q2_save_stream_t;


#define Q2_SAVE_STREAM_COUNT 8

static q2_save_directory_t saveDirectories[
    Q2_SAVE_DIRECTORY_COUNT
];

static q2_save_stream_t saveStreams[
    Q2_SAVE_STREAM_COUNT
];


static const char *saveDirectoryNames[
    Q2_SAVE_DIRECTORY_COUNT
] =
{
    "current",
    "save0",
    "save1",
    "save2",
    "save3"
};


static const unsigned char persistScope[] =
{
    'q','u','a','k','e','2','c','u','b','e',
    '-','s','a','v','e','-','v','1'
};



static const unsigned char slotIndexKey[] =
    "slot-index-v1";



/*
 * Q2CF uses its own namespace.
 *
 * The abandoned config-v1 experiment is intentionally never read.
 */
static const unsigned char configPersistScope[] =
    "quake2cube-config-v2";

static const unsigned char configPersistKey[] =
    "config";



static int vfsInitialized;
static int cardAttempted;
static int cardReady;


static CH_MemCardSession cardSession;
static card_file cardFile;

static CH_TxMemCardBackendContext txContext;
static CH_TxSectorBackend txBackend;

static void *cardWorkArea;
static void *sectorBuffer;

static size_t sectorBufferSize;


/* wildcard enumeration state */
static int findActive;
static int findDirectory;
static size_t findIndex;

static char findExtension[8];
static char findPrefix[Q2_SAVE_PATH_MAX];
static char findResult[Q2_SAVE_PATH_MAX];


/*
 * A Quake save operation touches a destination directory many times:
 *
 *     wipe files
 *     copy server.ssv
 *     copy game.ssv
 *     copy *.sav
 *     copy matching *.sv2
 *
 * Those operations must form ONE persistent transaction.
 */
/*
 * Q2GC_SAVE_LAZY_CACHE_V2
 *
 * Persistent save slots are authoritative on Memory Card.
 *
 * saveDirectories[save0..save3] are disposable decoded caches.
 * Only one persistent slot may remain expanded at a time.
 */
static unsigned char
    persistentDirectoryLoaded[
        Q2_SAVE_DIRECTORY_COUNT
    ];

static int batchActive;
static int batchDirectory = -1;
static int batchDirty;


/*
 * Always-resident menu metadata.
 *
 * Unlike saveDirectories[save0..save3], this is intentionally tiny and
 * remains resident for the lifetime of the process.
 */
static uint32_t slotIndexValidMask;

static unsigned char
    slotIndexComments[
        Q2_SAVE_SLOT_COUNT
    ][
        Q2_SAVE_COMMENT_SIZE
    ];


/* ------------------------------------------------------------------------- */
/* endian helpers                                                            */
/* ------------------------------------------------------------------------- */

static uint16_t readBe16(
    const unsigned char *p)
{
    return
        ((uint16_t)p[0] << 8) |
        ((uint16_t)p[1]);
}


static uint32_t readBe32(
    const unsigned char *p)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}


static void writeBe16(
    unsigned char *p,
    uint16_t value)
{
    p[0] =
        (unsigned char)(value >> 8);

    p[1] =
        (unsigned char)value;
}


static void writeBe32(
    unsigned char *p,
    uint32_t value)
{
    p[0] =
        (unsigned char)(value >> 24);

    p[1] =
        (unsigned char)(value >> 16);

    p[2] =
        (unsigned char)(value >> 8);

    p[3] =
        (unsigned char)value;
}


/* ------------------------------------------------------------------------- */
/* directory helpers                                                         */
/* ------------------------------------------------------------------------- */

static int directoryIndex(
    const char *name,
    size_t length)
{
    int i;

    for (i = 0;
         i < Q2_SAVE_DIRECTORY_COUNT;
         ++i)
    {
        if (strlen(saveDirectoryNames[i]) ==
                length &&
            !memcmp(
                saveDirectoryNames[i],
                name,
                length))
        {
            return i;
        }
    }

    return -1;
}


static int parseSavePath(
    const char *path,
    int *directory,
    const char **filename)
{
    const char *save;
    const char *slash;

    size_t dirLength;


    if (!path ||
        !directory ||
        !filename)
    {
        return 0;
    }


    save =
        strstr(
            path,
            "/save/"
        );

    if (!save)
        return 0;


    save +=
        strlen("/save/");


    slash =
        strchr(
            save,
            '/'
        );

    if (!slash)
        return 0;


    dirLength =
        (size_t)(slash - save);


    *directory =
        directoryIndex(
            save,
            dirLength
        );

    if (*directory < 0)
        return 0;


    *filename =
        slash + 1;


    if (!(*filename)[0] ||
        strchr(*filename, '/'))
    {
        return 0;
    }


    return 1;
}


static void clearFile(
    q2_save_file_t *file)
{
    if (!file)
        return;

    free(
        file->data
    );

    memset(
        file,
        0,
        sizeof(*file)
    );
}


static void clearDirectory(
    q2_save_directory_t *directory)
{
    size_t i;

    if (!directory)
        return;


    for (i = 0;
         i < directory->count;
         ++i)
    {
        clearFile(
            &directory->files[i]
        );
    }


    directory->count =
        0;
}


static q2_save_file_t *findFile(
    q2_save_directory_t *directory,
    const char *name)
{
    size_t i;

    if (!directory ||
        !name)
    {
        return NULL;
    }


    for (i = 0;
         i < directory->count;
         ++i)
    {
        if (!strcmp(
                directory->files[i].name,
                name))
        {
            return
                &directory->files[i];
        }
    }


    return NULL;
}


static q2_save_file_t *createFile(
    q2_save_directory_t *directory,
    const char *name)
{
    q2_save_file_t *file;


    if (!directory ||
        !name ||
        !name[0] ||
        strlen(name) >=
            Q2_SAVE_NAME_MAX)
    {
        return NULL;
    }


    file =
        findFile(
            directory,
            name
        );

    if (file)
    {
        free(
            file->data
        );

        file->data =
            NULL;

        file->size =
            0;

        file->capacity =
            0;

        file->write_failed =
            0;


        return file;
    }


    if (directory->count >=
        Q2_SAVE_MAX_FILES)
    {
        return NULL;
    }


    file =
        &directory->files[
            directory->count++
        ];


    memset(
        file,
        0,
        sizeof(*file)
    );


    strcpy(
        file->name,
        name
    );


    return file;
}


static int directoryHasWriteFailure(
    const q2_save_directory_t *directory,
    const char **nameOut)
{
    size_t i;

    if (nameOut)
        *nameOut = NULL;

    if (!directory)
        return 1;

    for (i = 0u; i < directory->count; ++i)
    {
        if (directory->files[i].write_failed)
        {
            if (nameOut)
                *nameOut = directory->files[i].name;

            return 1;
        }
    }

    return 0;
}


static int removeFile(
    q2_save_directory_t *directory,
    const char *name)
{
    size_t i;


    if (!directory ||
        !name)
    {
        return -1;
    }


    for (i = 0;
         i < directory->count;
         ++i)
    {
        if (!strcmp(
                directory->files[i].name,
                name))
        {
            clearFile(
                &directory->files[i]
            );


            if (i + 1 <
                directory->count)
            {
                memmove(
                    &directory->files[i],
                    &directory->files[i + 1],
                    (
                        directory->count -
                        i -
                        1
                    ) *
                    sizeof(directory->files[0])
                );
            }


            --directory->count;


            memset(
                &directory->files[
                    directory->count
                ],
                0,
                sizeof(directory->files[0])
            );


            return 0;
        }
    }


    return -1;
}


/* ------------------------------------------------------------------------- */
/* CARD / CarryHandle persistence                                            */
/* ------------------------------------------------------------------------- */

static void shutdownCard(void)
{
    if (cardReady)
    {
        CH_MemCardClose(
            &cardFile
        );

        CH_MemCardUnmount(
            &cardSession
        );
    }


    free(
        sectorBuffer
    );

    free(
        cardWorkArea
    );


    sectorBuffer =
        NULL;

    cardWorkArea =
        NULL;

    sectorBufferSize =
        0;

    cardReady =
        0;


    memset(
        &cardSession,
        0,
        sizeof(cardSession)
    );

    memset(
        &cardFile,
        0,
        sizeof(cardFile)
    );

    memset(
        &txContext,
        0,
        sizeof(txContext)
    );

    memset(
        &txBackend,
        0,
        sizeof(txBackend)
    );
}


static int initializeCard(void)
{
    s32 result;

    uint32_t sectorSize;
    uint32_t fileSize;

    int created =
        0;

    card_stat status;

    CH_TxResult txResult;


    if (cardAttempted)
        return cardReady;


    cardAttempted =
        1;


    cardWorkArea =
        memalign(
            32,
            CARD_WORKAREA
        );

    if (!cardWorkArea)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: CARD work-area allocation failed\n"
        );

        return 0;
    }


    if (!CH_MemCardMount(
            &cardSession,
            CARD_SLOTA,
            "Q2GC",
            "SB",
            cardWorkArea))
    {
        fprintf(
            stderr,
            "Q2GC SAVE: Memory Card A mount failed\n"
        );

        shutdownCard();

        return 0;
    }


    if (cardSession.sector_size <= 0)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: invalid CARD sector size\n"
        );

        shutdownCard();

        return 0;
    }


    sectorSize =
        (uint32_t)
            cardSession.sector_size;


    fileSize =
        Q2_SAVE_CARD_SECTORS *
        sectorSize;


    sectorBuffer =
        memalign(
            32,
            sectorSize
        );

    if (!sectorBuffer)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: sector buffer allocation failed\n"
        );

        shutdownCard();

        return 0;
    }


    sectorBufferSize =
        sectorSize;


    result =
        CH_MemCardOpen(
            &cardSession,
            Q2_SAVE_CARD_FILENAME,
            &cardFile
        );


    if (result ==
        CARD_ERROR_NOFILE)
    {
        result =
            CH_MemCardCreate(
                &cardSession,
                Q2_SAVE_CARD_FILENAME,
                fileSize,
                &cardFile
            );

        if (result !=
            CARD_ERROR_READY)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: CARD create failed: %ld\n",
                (long)result
            );

            shutdownCard();

            return 0;
        }


        created =
            1;
    }
    else if (result !=
             CARD_ERROR_READY)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: CARD open failed: %ld\n",
            (long)result
        );

        shutdownCard();

        return 0;
    }


    memset(
        &status,
        0,
        sizeof(status)
    );


    result =
        CARD_GetStatus(
            cardFile.chn,
            cardFile.filenum,
            &status
        );


    if (result !=
        CARD_ERROR_READY)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: CARD status failed: %ld\n",
            (long)result
        );

        shutdownCard();

        return 0;
    }


    if ((uint32_t)status.len !=
        fileSize)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: incompatible CARD save geometry "
            "(got=%lu expected=%lu)\n",
            (unsigned long)status.len,
            (unsigned long)fileSize
        );

        shutdownCard();

        return 0;
    }


    /*
     * A freshly created GameCube CARD file is not guaranteed to contain
     * zeroes. Invalidate sector zero before transaction initialization.
     */
    if (created)
    {
        memset(
            sectorBuffer,
            0,
            sectorSize
        );


        result =
            CH_MemCardWrite(
                &cardFile,
                sectorBuffer,
                sectorSize,
                0u
            );


        if (result !=
            CARD_ERROR_READY)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: initial sector zero failed: %ld\n",
                (long)result
            );

            shutdownCard();

            return 0;
        }
    }


    if (!CH_TxMemCardBackendInit(
            &txContext,
            &cardSession,
            &cardFile,
            &txBackend))
    {
        fprintf(
            stderr,
            "Q2GC SAVE: transaction backend init failed\n"
        );

        shutdownCard();

        return 0;
    }


    txResult =
        CH_TxInitialize(
            &txBackend,
            sectorBuffer,
            sectorBufferSize
        );


    if (txResult !=
        CH_TX_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: transaction init failed: %d\n",
            (int)txResult
        );

        shutdownCard();

        return 0;
    }


    cardReady =
        1;


    fprintf(
        stderr,
        "Q2GC SAVE: %s CARD A %s (%lu sectors)\n",
        Q2_SAVE_CARD_FILENAME,
        created
            ? "created"
            : "opened",
        (unsigned long)Q2_SAVE_CARD_SECTORS
    );


    return 1;
}


/* ------------------------------------------------------------------------- */
/* bundle codec                                                              */
/* ------------------------------------------------------------------------- */

static int encodeDirectory(
    const q2_save_directory_t *directory,
    unsigned char **output,
    size_t *outputSize)
{
    unsigned char *buffer;
    unsigned char *p;

    size_t total =
        12;

    size_t i;


    if (!directory ||
        !output ||
        !outputSize)
    {
        return 0;
    }


    for (i = 0;
         i < directory->count;
         ++i)
    {
        size_t nameLength =
            strlen(
                directory->files[i].name
            );


        if (nameLength == 0 ||
            nameLength >
                UINT16_MAX ||
            directory->files[i].size >
                UINT32_MAX)
        {
            return 0;
        }


        if (total >
            Q2_SAVE_BUNDLE_MAX -
            8 -
            nameLength -
            directory->files[i].size)
        {
            return 0;
        }


        total +=
            8 +
            nameLength +
            directory->files[i].size;
    }


    buffer =
        malloc(
            total
        );

    if (!buffer)
        return 0;


    p =
        buffer;


    writeBe32(
        p + 0,
        Q2_SAVE_BUNDLE_MAGIC
    );

    writeBe32(
        p + 4,
        Q2_SAVE_BUNDLE_VERSION
    );

    writeBe32(
        p + 8,
        (uint32_t)directory->count
    );


    p +=
        12;


    for (i = 0;
         i < directory->count;
         ++i)
    {
        const q2_save_file_t *file =
            &directory->files[i];

        size_t nameLength =
            strlen(
                file->name
            );


        writeBe16(
            p + 0,
            (uint16_t)nameLength
        );

        writeBe16(
            p + 2,
            0
        );

        writeBe32(
            p + 4,
            (uint32_t)file->size
        );


        p +=
            8;


        memcpy(
            p,
            file->name,
            nameLength
        );


        p +=
            nameLength;


        if (file->size)
        {
            memcpy(
                p,
                file->data,
                file->size
            );


            p +=
                file->size;
        }
    }


    *output =
        buffer;

    *outputSize =
        total;


    return 1;
}


static int decodeDirectory(
    q2_save_directory_t *directory,
    const unsigned char *buffer,
    size_t size)
{
    const unsigned char *p;
    const unsigned char *end;

    uint32_t count;
    uint32_t i;


    if (!directory ||
        !buffer ||
        size < 12)
    {
        return 0;
    }


    if (readBe32(buffer + 0) !=
            Q2_SAVE_BUNDLE_MAGIC ||
        readBe32(buffer + 4) !=
            Q2_SAVE_BUNDLE_VERSION)
    {
        return 0;
    }


    count =
        readBe32(
            buffer + 8
        );


    if (count >
        Q2_SAVE_MAX_FILES)
    {
        return 0;
    }


    p =
        buffer + 12;

    end =
        buffer + size;


    clearDirectory(
        directory
    );


    for (i = 0;
         i < count;
         ++i)
    {
        uint16_t nameLength;
        uint32_t fileSize;

        q2_save_file_t *file;


        if ((size_t)(end - p) <
            8)
        {
            clearDirectory(
                directory
            );

            return 0;
        }


        nameLength =
            readBe16(
                p + 0
            );

        fileSize =
            readBe32(
                p + 4
            );


        p +=
            8;


        if (nameLength == 0 ||
            nameLength >=
                Q2_SAVE_NAME_MAX ||
            fileSize >
                Q2_SAVE_FILE_MAX ||
            (size_t)(end - p) <
                (size_t)nameLength +
                (size_t)fileSize)
        {
            clearDirectory(
                directory
            );

            return 0;
        }


        file =
            &directory->files[
                directory->count++
            ];


        memset(
            file,
            0,
            sizeof(*file)
        );


        memcpy(
            file->name,
            p,
            nameLength
        );

        file->name[nameLength] =
            '\0';


        p +=
            nameLength;


        if (fileSize)
        {
            file->data =
                malloc(
                    fileSize
                );

            if (!file->data)
            {
                clearDirectory(
                    directory
                );

                return 0;
            }


            memcpy(
                file->data,
                p,
                fileSize
            );


            file->size =
                fileSize;

            file->capacity =
                fileSize;


            p +=
                fileSize;
        }
    }


    return
        p == end;
}


/* ------------------------------------------------------------------------- */
/* Save/Load menu metadata index                                             */
/* ------------------------------------------------------------------------- */


static void clearSlotIndexCache(void)
{
    slotIndexValidMask =
        0;

    memset(
        slotIndexComments,
        0,
        sizeof(slotIndexComments)
    );
}


static int persistSlotIndex(void)
{
    unsigned char encoded[
        Q2_SAVE_SLOT_INDEX_SIZE
    ];

    unsigned int slot;

    CH_PersistResult result;


    if (!cardReady)
        return 0;


    memset(
        encoded,
        0,
        sizeof(encoded)
    );


    writeBe32(
        encoded + 0,
        Q2_SAVE_SLOT_INDEX_MAGIC
    );

    writeBe32(
        encoded + 4,
        Q2_SAVE_SLOT_INDEX_VERSION
    );

    writeBe32(
        encoded + 8,
        slotIndexValidMask
    );


    for (slot = 0;
         slot < Q2_SAVE_SLOT_COUNT;
         ++slot)
    {
        memcpy(
            encoded +
                Q2_SAVE_SLOT_INDEX_HEADER_SIZE +
                slot * Q2_SAVE_COMMENT_SIZE,
            slotIndexComments[slot],
            Q2_SAVE_COMMENT_SIZE
        );
    }


    result =
        CH_PersistPut(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            persistScope,
            sizeof(persistScope),
            slotIndexKey,
            sizeof(slotIndexKey) - 1u,
            encoded,
            sizeof(encoded)
        );


    if (result !=
        CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE MENU: PUT slot-index failed: %d\n",
            (int)result
        );

        return 0;
    }


    fprintf(
        stderr,
        "Q2GC SAVE MENU: PUT slot-index OK "
        "mask=0x%02lx bytes=%lu\n",
        (unsigned long)slotIndexValidMask,
        (unsigned long)sizeof(encoded)
    );


    return 1;
}


static void loadSlotIndex(void)
{
    unsigned char encoded[
        Q2_SAVE_SLOT_INDEX_SIZE
    ];

    size_t encodedSize =
        0;

    unsigned int slot;

    CH_PersistResult result;


    clearSlotIndexCache();


    if (!cardReady)
        return;


    result =
        CH_PersistGet(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            persistScope,
            sizeof(persistScope),
            slotIndexKey,
            sizeof(slotIndexKey) - 1u,
            encoded,
            sizeof(encoded),
            &encodedSize
        );


    if (result ==
        CH_PERSIST_RESULT_NOT_FOUND)
    {
        fprintf(
            stderr,
            "Q2GC SAVE MENU: slot-index empty\n"
        );

        return;
    }


    if (result !=
        CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE MENU: GET slot-index failed: %d\n",
            (int)result
        );

        return;
    }


    if (encodedSize !=
            Q2_SAVE_SLOT_INDEX_SIZE ||
        readBe32(encoded + 0) !=
            Q2_SAVE_SLOT_INDEX_MAGIC ||
        readBe32(encoded + 4) !=
            Q2_SAVE_SLOT_INDEX_VERSION)
    {
        fprintf(
            stderr,
            "Q2GC SAVE MENU: slot-index invalid "
            "bytes=%lu\n",
            (unsigned long)encodedSize
        );

        return;
    }


    slotIndexValidMask =
        readBe32(
            encoded + 8
        ) &
        (
            (1u << Q2_SAVE_SLOT_COUNT) -
            1u
        );


    for (slot = 0;
         slot < Q2_SAVE_SLOT_COUNT;
         ++slot)
    {
        memcpy(
            slotIndexComments[slot],
            encoded +
                Q2_SAVE_SLOT_INDEX_HEADER_SIZE +
                slot * Q2_SAVE_COMMENT_SIZE,
            Q2_SAVE_COMMENT_SIZE
        );

        /*
         * Vanilla writes a NUL-padded 32-byte comment.
         * Keep the menu robust if a damaged index does not.
         */
        slotIndexComments[slot][
            Q2_SAVE_COMMENT_SIZE - 1u
        ] =
            '\0';
    }


    fprintf(
        stderr,
        "Q2GC SAVE MENU: GET slot-index OK "
        "mask=0x%02lx bytes=%lu\n",
        (unsigned long)slotIndexValidMask,
        (unsigned long)encodedSize
    );
}


static int updateSlotIndexFromDirectory(
    int index)
{
    int slot;

    uint32_t bit;

    q2_save_file_t *serverFile;


    if (index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST)
    {
        return 0;
    }


    slot =
        index -
        Q2_SAVE_PERSIST_FIRST;

    bit =
        1u <<
        (unsigned int)slot;


    serverFile =
        findFile(
            &saveDirectories[index],
            "server.ssv"
        );


    if (serverFile &&
        serverFile->data &&
        serverFile->size >=
            Q2_SAVE_COMMENT_SIZE)
    {
        memcpy(
            slotIndexComments[slot],
            serverFile->data,
            Q2_SAVE_COMMENT_SIZE
        );

        slotIndexComments[slot][
            Q2_SAVE_COMMENT_SIZE - 1u
        ] =
            '\0';

        slotIndexValidMask |=
            bit;
    }
    else
    {
        memset(
            slotIndexComments[slot],
            0,
            Q2_SAVE_COMMENT_SIZE
        );

        slotIndexValidMask &=
            ~bit;
    }


    fprintf(
        stderr,
        "Q2GC SAVE MENU: cache slot=%d "
        "valid=%d comment=\"%s\"\n",
        slot,
        (slotIndexValidMask & bit)
            ? 1
            : 0,
        slotIndexComments[slot]
    );


    return
        persistSlotIndex();
}


/* ------------------------------------------------------------------------- */
/* persistent slots                                                          */
/* ------------------------------------------------------------------------- */


/*
 * Q2GC_SAVE_STREAM_DEFLATE_V1
 *
 * Large virtual save directories already exist as individual VFS
 * file buffers. Building another contiguous ~850 KiB Q2SV image
 * only so zlib can read it doubles save-time peak memory.
 */
#define Q2_SAVE_STREAM_STORED_INITIAL (128u * 1024u)


static int Q2GC_SaveDirectoryRawSize(
    const q2_save_directory_t *directory,
    size_t *rawSize)
{
    size_t total = 12u;
    size_t i;

    if (!directory || !rawSize)
        return 0;

    for (i = 0; i < directory->count; ++i)
    {
        size_t nameLength = strlen(directory->files[i].name);
        size_t fileSize = directory->files[i].size;

        if (nameLength == 0 ||
            nameLength > UINT16_MAX ||
            fileSize > UINT32_MAX)
        {
            return 0;
        }

        if (total >
            Q2_SAVE_BUNDLE_MAX -
            8u -
            nameLength -
            fileSize)
        {
            return 0;
        }

        total += 8u + nameLength + fileSize;
    }

    *rawSize = total;
    return 1;
}


static int Q2GC_SaveGrowStored(
    unsigned char **stored,
    size_t *storedCapacity,
    z_stream *stream)
{
    size_t oldCapacity;
    size_t newCapacity;
    size_t nextOffset;
    unsigned char *grown;

    if (!stored || !*stored || !storedCapacity || !stream)
        return 0;

    oldCapacity = *storedCapacity;

    if (oldCapacity >=
        Q2_SAVE_STORED_HEADER_SIZE +
        Q2_SAVE_STORED_MAX)
    {
        return 0;
    }

    nextOffset =
        (size_t)(
            stream->next_out -
            (Bytef *)*stored
        );

    newCapacity = oldCapacity * 2u;

    if (newCapacity >
        Q2_SAVE_STORED_HEADER_SIZE +
        Q2_SAVE_STORED_MAX)
    {
        newCapacity =
            Q2_SAVE_STORED_HEADER_SIZE +
            Q2_SAVE_STORED_MAX;
    }

    if (newCapacity <= oldCapacity)
        return 0;

    grown = realloc(*stored, newCapacity);

    if (!grown)
        return 0;

    *stored = grown;
    *storedCapacity = newCapacity;

    stream->next_out =
        (Bytef *)grown +
        nextOffset;

    stream->avail_out =
        (uInt)(
            newCapacity -
            nextOffset
        );

    fprintf(
        stderr,
        "Q2GC SAVE: STREAM GROW stored_capacity=%lu\n",
        (unsigned long)newCapacity
    );

    return 1;
}


static int Q2GC_SaveDeflateChunk(
    z_stream *stream,
    unsigned char **stored,
    size_t *storedCapacity,
    uLong *rawCrc,
    size_t *rawWritten,
    const void *data,
    size_t size)
{
    int zResult;

    if (!stream ||
        !stored ||
        !storedCapacity ||
        !rawCrc ||
        !rawWritten ||
        (!data && size))
    {
        return 0;
    }

    if (size > UINT_MAX)
        return 0;

    if (!size)
        return 1;

    *rawCrc =
        crc32(
            *rawCrc,
            (const Bytef *)data,
            (uInt)size
        );

    *rawWritten += size;

    stream->next_in =
        (Bytef *)(void *)data;

    stream->avail_in =
        (uInt)size;

    while (stream->avail_in)
    {
        if (!stream->avail_out)
        {
            if (!Q2GC_SaveGrowStored(
                    stored,
                    storedCapacity,
                    stream))
            {
                return 0;
            }
        }

        zResult = deflate(stream, Z_NO_FLUSH);

        if (zResult != Z_OK)
            return 0;
    }

    return 1;
}


static int Q2GC_SaveEncodeLargeCompressed(
    const q2_save_directory_t *directory,
    unsigned char **output,
    size_t *outputSize,
    size_t expectedRawSize)
{
    unsigned char rawHeader[12];
    unsigned char fileHeader[8];

    unsigned char *stored = NULL;

    size_t storedCapacity =
        Q2_SAVE_STORED_HEADER_SIZE +
        Q2_SAVE_STREAM_STORED_INITIAL;

    size_t rawWritten = 0u;
    size_t compressedSize;
    size_t i;

    uLong rawCrc;
    z_stream stream;

    int initialized = 0;
    int zResult;

    if (!directory ||
        !output ||
        !outputSize ||
        expectedRawSize > Q2_SAVE_BUNDLE_MAX)
    {
        return 0;
    }

    memset(&stream, 0, sizeof(stream));

    stored = malloc(storedCapacity);

    if (!stored)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: stream deflate output allocation failed "
            "capacity=%lu\n",
            (unsigned long)storedCapacity
        );
        return 0;
    }

    stream.next_out =
        (Bytef *)(stored + Q2_SAVE_STORED_HEADER_SIZE);

    stream.avail_out =
        (uInt)(
            storedCapacity -
            Q2_SAVE_STORED_HEADER_SIZE
        );

    zResult = deflateInit(&stream, Z_BEST_SPEED);

    if (zResult != Z_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: stream deflate init failed: %d\n",
            zResult
        );
        free(stored);
        return 0;
    }

    initialized = 1;

    rawCrc = crc32(0L, Z_NULL, 0);

    writeBe32(rawHeader + 0, Q2_SAVE_BUNDLE_MAGIC);
    writeBe32(rawHeader + 4, Q2_SAVE_BUNDLE_VERSION);
    writeBe32(rawHeader + 8, (uint32_t)directory->count);

    if (!Q2GC_SaveDeflateChunk(
            &stream,
            &stored,
            &storedCapacity,
            &rawCrc,
            &rawWritten,
            rawHeader,
            sizeof(rawHeader)))
    {
        goto fail;
    }

    for (i = 0; i < directory->count; ++i)
    {
        const q2_save_file_t *file = &directory->files[i];
        size_t nameLength = strlen(file->name);

        writeBe16(fileHeader + 0, (uint16_t)nameLength);
        writeBe16(fileHeader + 2, 0);
        writeBe32(fileHeader + 4, (uint32_t)file->size);

        if (!Q2GC_SaveDeflateChunk(
                &stream,
                &stored,
                &storedCapacity,
                &rawCrc,
                &rawWritten,
                fileHeader,
                sizeof(fileHeader)) ||
            !Q2GC_SaveDeflateChunk(
                &stream,
                &stored,
                &storedCapacity,
                &rawCrc,
                &rawWritten,
                file->name,
                nameLength) ||
            !Q2GC_SaveDeflateChunk(
                &stream,
                &stored,
                &storedCapacity,
                &rawCrc,
                &rawWritten,
                file->data,
                file->size))
        {
            goto fail;
        }
    }

    if (rawWritten != expectedRawSize)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: stream deflate raw-size mismatch "
            "expected=%lu actual=%lu\n",
            (unsigned long)expectedRawSize,
            (unsigned long)rawWritten
        );
        goto fail;
    }

    for (;;)
    {
        if (!stream.avail_out)
        {
            if (!Q2GC_SaveGrowStored(
                    &stored,
                    &storedCapacity,
                    &stream))
            {
                goto fail;
            }
        }

        zResult = deflate(&stream, Z_FINISH);

        if (zResult == Z_STREAM_END)
            break;

        if (zResult != Z_OK)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: stream deflate finish failed: %d\n",
                zResult
            );
            goto fail;
        }
    }

    compressedSize = (size_t)stream.total_out;

    deflateEnd(&stream);
    initialized = 0;

    if (compressedSize > Q2_SAVE_STORED_MAX)
        goto fail;

    writeBe32(stored + 0, Q2_SAVE_STORED_MAGIC);
    writeBe32(stored + 4, Q2_SAVE_STORED_VERSION);
    writeBe32(stored + 8, (uint32_t)rawWritten);
    writeBe32(stored + 12, (uint32_t)compressedSize);
    writeBe32(stored + 16, (uint32_t)rawCrc);

    *output = stored;
    *outputSize =
        Q2_SAVE_STORED_HEADER_SIZE +
        compressedSize;

    fprintf(
        stderr,
        "Q2GC SAVE: STREAM DEFLATE "
        "raw=%lu stored=%lu capacity=%lu "
        "mode=q2sv_piecewise_zlib_v1\n",
        (unsigned long)rawWritten,
        (unsigned long)*outputSize,
        (unsigned long)storedCapacity
    );

    return 1;

fail:
    if (initialized)
        deflateEnd(&stream);

    free(stored);
    return 0;
}


static int persistDirectory(
    int index)
{
    unsigned char *bundle = NULL;
    unsigned char *stored = NULL;

    size_t bundleSize = 0u;
    size_t rawSize = 0u;
    size_t storedCapacity;
    size_t storedSize = 0u;

    uLongf compressedSize;
    uLong rawCrc;

    int zResult;

    CH_PersistResult result;

    if (!cardReady ||
        index < Q2_SAVE_PERSIST_FIRST ||
        index > Q2_SAVE_PERSIST_LAST)
    {
        return 0;
    }

    if (!Q2GC_SaveDirectoryRawSize(
            &saveDirectories[index],
            &rawSize))
    {
        fprintf(
            stderr,
            "Q2GC SAVE: could not size %s\n",
            saveDirectoryNames[index]
        );
        return 0;
    }

    fprintf(
        stderr,
        "Q2GC SAVE: BUNDLE CAP "
        "raw=%lu raw_limit=%lu stored_limit=%lu "
        "mode=raw2m_stored512k_v1b\n",
        (unsigned long)rawSize,
        (unsigned long)Q2_SAVE_BUNDLE_MAX,
        (unsigned long)Q2_SAVE_STORED_MAX
    );

    if (rawSize <= Q2_SAVE_STORED_MAX)
    {
        if (!encodeDirectory(
                &saveDirectories[index],
                &bundle,
                &bundleSize))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: could not encode %s\n",
                saveDirectoryNames[index]
            );
            return 0;
        }

        storedCapacity =
            Q2_SAVE_STORED_HEADER_SIZE +
            Q2_SAVE_STORED_MAX;

        stored = malloc(storedCapacity);

        if (!stored)
        {
            free(bundle);
            return 0;
        }

        compressedSize =
            (uLongf)Q2_SAVE_STORED_MAX;

        zResult =
            compress2(
                (Bytef *)(
                    stored +
                    Q2_SAVE_STORED_HEADER_SIZE
                ),
                &compressedSize,
                (const Bytef *)bundle,
                (uLong)bundleSize,
                Z_BEST_SPEED
            );

        if (zResult != Z_OK)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: deflate %s failed: %d\n",
                saveDirectoryNames[index],
                zResult
            );
            free(stored);
            free(bundle);
            return 0;
        }

        rawCrc = crc32(0L, Z_NULL, 0);
        rawCrc =
            crc32(
                rawCrc,
                (const Bytef *)bundle,
                (uLong)bundleSize
            );

        writeBe32(stored + 0, Q2_SAVE_STORED_MAGIC);
        writeBe32(stored + 4, Q2_SAVE_STORED_VERSION);
        writeBe32(stored + 8, (uint32_t)bundleSize);
        writeBe32(stored + 12, (uint32_t)compressedSize);
        writeBe32(stored + 16, (uint32_t)rawCrc);

        storedSize =
            Q2_SAVE_STORED_HEADER_SIZE +
            (size_t)compressedSize;

        fprintf(
            stderr,
            "Q2GC SAVE: DEFLATE %s raw=%lu stored=%lu "
            "mode=contiguous_small_v1\n",
            saveDirectoryNames[index],
            (unsigned long)bundleSize,
            (unsigned long)storedSize
        );

        free(bundle);
        bundle = NULL;
    }
    else
    {
        /*
         * Q2GC_SAVE_STREAM_DEFLATE_V1
         */
        if (!Q2GC_SaveEncodeLargeCompressed(
                &saveDirectories[index],
                &stored,
                &storedSize,
                rawSize))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: stream deflate %s failed\n",
                saveDirectoryNames[index]
            );
            return 0;
        }
    }

    result =
        CH_PersistPut(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            persistScope,
            sizeof(persistScope),
            saveDirectoryNames[index],
            strlen(saveDirectoryNames[index]),
            stored,
            storedSize
        );

    free(stored);

    if (result != CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: PUT %s failed: %d\n",
            saveDirectoryNames[index],
            (int)result
        );
        return 0;
    }

    fprintf(
        stderr,
        "Q2GC SAVE: PUT %s OK "
        "(%lu files raw=%lu stored=%lu)\n",
        saveDirectoryNames[index],
        (unsigned long)saveDirectories[index].count,
        (unsigned long)rawSize,
        (unsigned long)storedSize
    );


    /*
     * Q2GC_SAVE_MENU_INDEX_REFRESH_RESTORE_V1
     *
     * Q2GC_SAVE_STREAM_DEFLATE_V1 replaced persistDirectory() and
     * accidentally dropped the already-proven post-PUT menu-index update.
     *
     * The full save bundle above is authoritative. Only after that PUT
     * succeeds do we publish the exact 32-byte server.ssv comment used by
     * Quake II's Save/Load menus.
     */
    if (!updateSlotIndexFromDirectory(
            index))
    {
        fprintf(
            stderr,
            "Q2GC SAVE MENU: slot-index update "
            "after %s failed\n",
            saveDirectoryNames[index]
        );

        return 0;
    }


    fprintf(
        stderr,
        "Q2GC SAVE MENU: refresh after %s OK\n",
        saveDirectoryNames[index]
    );

    return 1;
}


/*
 * Q2GC_SAVE_STREAM_INFLATE_V1
 *
 * Inflate compressed Q2SV directly into final per-file buffers.
 * This removes the aggregate malloc(rawSize) from the compressed
 * GET path while preserving Q2Z1/Q2SV framing and CRC semantics.
 */
static int Q2GC_SaveInflateExact(
    z_stream *stream,
    void *destination,
    size_t size,
    uLong *rawCrc,
    size_t *rawWritten,
    int *streamEnded)
{
    if (!stream ||
        (!destination && size) ||
        !rawCrc ||
        !rawWritten ||
        !streamEnded)
    {
        return 0;
    }


    if (!size)
        return 1;


    stream->next_out =
        (Bytef *)destination;

    stream->avail_out =
        (uInt)size;


    while (stream->avail_out)
    {
        uInt before =
            stream->avail_out;

        int zResult =
            inflate(
                stream,
                Z_NO_FLUSH
            );


        if (zResult ==
            Z_STREAM_END)
        {
            *streamEnded =
                1;

            if (stream->avail_out)
                return 0;

            break;
        }


        if (zResult !=
            Z_OK)
        {
            return 0;
        }


        if (stream->avail_out ==
                before &&
            stream->avail_in ==
                0)
        {
            return 0;
        }
    }


    *rawCrc =
        crc32(
            *rawCrc,
            (const Bytef *)destination,
            (uInt)size
        );

    *rawWritten +=
        size;


    return 1;
}


static int Q2GC_SaveInflateFinish(
    z_stream *stream,
    int *streamEnded)
{
    unsigned char extraByte;

    uLong beforeOut;

    int zResult;


    if (!stream ||
        !streamEnded)
    {
        return 0;
    }


    if (*streamEnded)
        return 1;


    beforeOut =
        stream->total_out;

    stream->next_out =
        &extraByte;

    stream->avail_out =
        1u;


    zResult =
        inflate(
            stream,
            Z_FINISH
        );


    if (zResult !=
        Z_STREAM_END)
    {
        return 0;
    }


    if (stream->total_out !=
        beforeOut)
    {
        return 0;
    }


    *streamEnded =
        1;


    return 1;
}


static int Q2GC_SaveDecodeCompressedStream(
    int index,
    const unsigned char *stored,
    size_t storedSize)
{
    unsigned char rawHeader[12];
    unsigned char fileHeader[8];

    q2_save_directory_t *directory;

    uint32_t rawSize;
    uint32_t compressedSize;
    uint32_t expectedCrc;
    uint32_t fileCount;

    size_t rawWritten =
        0u;

    size_t i;

    uLong actualCrc;

    z_stream stream;

    int initialized =
        0;

    int streamEnded =
        0;

    int zResult;


    if (index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST ||
        !stored ||
        storedSize <
            Q2_SAVE_STORED_HEADER_SIZE)
    {
        return 0;
    }


    if (readBe32(
            stored + 0) !=
            Q2_SAVE_STORED_MAGIC ||
        readBe32(
            stored + 4) !=
            Q2_SAVE_STORED_VERSION)
    {
        return 0;
    }


    rawSize =
        readBe32(
            stored + 8
        );

    compressedSize =
        readBe32(
            stored + 12
        );

    expectedCrc =
        readBe32(
            stored + 16
        );


    if (rawSize <
            sizeof(rawHeader) ||
        rawSize >
            Q2_SAVE_BUNDLE_MAX ||
        compressedSize !=
            storedSize -
            Q2_SAVE_STORED_HEADER_SIZE)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: STREAM INFLATE FAIL %s "
            "reason=header raw=%lu stored=%lu compressed=%lu\n",
            saveDirectoryNames[index],
            (unsigned long)rawSize,
            (unsigned long)storedSize,
            (unsigned long)compressedSize
        );

        return 0;
    }


    directory =
        &saveDirectories[index];

    clearDirectory(
        directory
    );


    memset(
        &stream,
        0,
        sizeof(stream)
    );


    stream.next_in =
        (Bytef *)(
            stored +
            Q2_SAVE_STORED_HEADER_SIZE
        );

    stream.avail_in =
        (uInt)compressedSize;


    zResult =
        inflateInit(
            &stream
        );

    if (zResult !=
        Z_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: STREAM INFLATE FAIL %s "
            "reason=inflate_init z=%d\n",
            saveDirectoryNames[index],
            zResult
        );

        return 0;
    }

    initialized =
        1;


    actualCrc =
        crc32(
            0L,
            Z_NULL,
            0
        );


    if (!Q2GC_SaveInflateExact(
            &stream,
            rawHeader,
            sizeof(rawHeader),
            &actualCrc,
            &rawWritten,
            &streamEnded))
    {
        goto fail;
    }


    if (readBe32(
            rawHeader + 0) !=
            Q2_SAVE_BUNDLE_MAGIC ||
        readBe32(
            rawHeader + 4) !=
            Q2_SAVE_BUNDLE_VERSION)
    {
        goto fail;
    }


    fileCount =
        readBe32(
            rawHeader + 8
        );


    if (fileCount >
        Q2_SAVE_MAX_FILES)
    {
        goto fail;
    }


    for (i = 0;
         i < (size_t)fileCount;
         ++i)
    {
        q2_save_file_t *file;

        uint16_t nameLength;
        uint16_t reserved;

        uint32_t fileSize;

        size_t j;


        if (!Q2GC_SaveInflateExact(
                &stream,
                fileHeader,
                sizeof(fileHeader),
                &actualCrc,
                &rawWritten,
                &streamEnded))
        {
            goto fail;
        }


        nameLength =
            readBe16(
                fileHeader + 0
            );

        reserved =
            readBe16(
                fileHeader + 2
            );

        fileSize =
            readBe32(
                fileHeader + 4
            );


        if (!nameLength ||
            nameLength >=
                Q2_SAVE_NAME_MAX ||
            reserved !=
                0u ||
            fileSize >
                Q2_SAVE_FILE_MAX)
        {
            goto fail;
        }


        if (rawWritten >
                (size_t)rawSize ||
            (size_t)nameLength >
                (size_t)rawSize -
                rawWritten)
        {
            goto fail;
        }


        file =
            &directory->files[
                directory->count
            ];

        memset(
            file,
            0,
            sizeof(*file)
        );


        if (!Q2GC_SaveInflateExact(
                &stream,
                file->name,
                (size_t)nameLength,
                &actualCrc,
                &rawWritten,
                &streamEnded))
        {
            goto fail;
        }


        file->name[
            nameLength
        ] =
            '\0';


        for (j = 0;
             j < directory->count;
             ++j)
        {
            if (!strcmp(
                    directory->files[j].name,
                    file->name))
            {
                goto fail;
            }
        }


        if (rawWritten >
                (size_t)rawSize ||
            (size_t)fileSize >
                (size_t)rawSize -
                rawWritten)
        {
            goto fail;
        }


        if (fileSize)
        {
            file->data =
                malloc(
                    (size_t)fileSize
                );

            if (!file->data)
            {
                fprintf(
                    stderr,
                    "Q2GC SAVE: STREAM INFLATE FAIL %s "
                    "reason=file_alloc file=%s bytes=%lu\n",
                    saveDirectoryNames[index],
                    file->name,
                    (unsigned long)fileSize
                );

                goto fail;
            }


            file->capacity =
                (size_t)fileSize;


            if (!Q2GC_SaveInflateExact(
                    &stream,
                    file->data,
                    (size_t)fileSize,
                    &actualCrc,
                    &rawWritten,
                    &streamEnded))
            {
                goto fail;
            }
        }


        file->size =
            (size_t)fileSize;

        directory->count++;


        fprintf(
            stderr,
            "Q2GC SAVE: STREAM INFLATE FILE %s "
            "%lu/%lu name=%s bytes=%lu\n",
            saveDirectoryNames[index],
            (unsigned long)directory->count,
            (unsigned long)fileCount,
            file->name,
            (unsigned long)file->size
        );
    }


    if (rawWritten !=
            (size_t)rawSize ||
        !Q2GC_SaveInflateFinish(
            &stream,
            &streamEnded))
    {
        goto fail;
    }


    if ((uint32_t)actualCrc !=
        expectedCrc)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: STREAM INFLATE FAIL %s "
            "reason=crc expected=%lu actual=%lu\n",
            saveDirectoryNames[index],
            (unsigned long)expectedCrc,
            (unsigned long)(
                (uint32_t)actualCrc
            )
        );

        goto fail;
    }


    inflateEnd(
        &stream
    );

    initialized =
        0;


    fprintf(
        stderr,
        "Q2GC SAVE: STREAM INFLATE %s OK "
        "files=%lu raw=%lu stored=%lu "
        "mode=q2sv_direct_files_v1\n",
        saveDirectoryNames[index],
        (unsigned long)directory->count,
        (unsigned long)rawSize,
        (unsigned long)storedSize
    );


    return 1;


fail:
    if (initialized)
    {
        inflateEnd(
            &stream
        );
    }


    clearDirectory(
        directory
    );


    fprintf(
        stderr,
        "Q2GC SAVE: STREAM INFLATE FAIL %s "
        "reason=parse raw_written=%lu raw_expected=%lu\n",
        saveDirectoryNames[index],
        (unsigned long)rawWritten,
        (unsigned long)rawSize
    );


    return 0;
}


static void loadPersistentDirectory(
    int index)
{
    unsigned char *stored =
        NULL;

    unsigned char *bundle =
        NULL;

    size_t storedCapacity;
    size_t storedSize =
        0;

    size_t bundleSize =
        0;

    uLong bound;

    CH_PersistResult result;


    if (!cardReady ||
        index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST)
    {
        return;
    }


    /*
     * Must be able to read either:
     *
     *   - old raw Q2SV object
     *   - new framed compressed Q2Z1 object
     */
    bound =
        (uLong)Q2_SAVE_STORED_MAX;


    if ((size_t)bound >
        SIZE_MAX -
            Q2_SAVE_STORED_HEADER_SIZE)
    {
        return;
    }


    storedCapacity =
        Q2_SAVE_STORED_HEADER_SIZE +
        (size_t)bound;


    stored =
        malloc(
            storedCapacity
        );

    if (!stored)
        return;


    result =
        CH_PersistGet(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            persistScope,
            sizeof(persistScope),
            saveDirectoryNames[index],
            strlen(saveDirectoryNames[index]),
            stored,
            storedCapacity,
            &storedSize
        );


    if (result ==
        CH_PERSIST_RESULT_NOT_FOUND)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: %s empty\n",
            saveDirectoryNames[index]
        );

        free(
            stored
        );

        return;
    }


    if (result !=
        CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: GET %s failed: %d\n",
            saveDirectoryNames[index],
            (int)result
        );

        free(
            stored
        );

        return;
    }


    /*
     * New compressed representation.
     */
    if (storedSize >=
            Q2_SAVE_STORED_HEADER_SIZE &&
        readBe32(stored + 0) ==
            Q2_SAVE_STORED_MAGIC &&
        readBe32(stored + 4) ==
            Q2_SAVE_STORED_VERSION)
    {
        /*
         * Q2GC_SAVE_STREAM_INFLATE_V1
         *
         * Do not allocate one aggregate raw Q2SV buffer.
         */
        if (!Q2GC_SaveDecodeCompressedStream(
                index,
                stored,
                storedSize))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: %s bundle corrupt\n",
                saveDirectoryNames[index]
            );
        }
        else
        {
            size_t rawSize =
                (size_t)readBe32(
                    stored + 8
                );


            fprintf(
                stderr,
                "Q2GC SAVE: GET %s OK "
                "(%lu files raw=%lu stored=%lu) "
                "mode=stream_inflate_v1\n",
                saveDirectoryNames[index],
                (unsigned long)saveDirectories[index].count,
                (unsigned long)rawSize,
                (unsigned long)storedSize
            );
        }
}
    else
    {
        /*
         * Compatibility with the first experimental uncompressed VFS.
         */
        if (!decodeDirectory(
                &saveDirectories[index],
                stored,
                storedSize))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: %s legacy bundle corrupt\n",
                saveDirectoryNames[index]
            );

            clearDirectory(
                &saveDirectories[index]
            );
        }
        else
        {
            fprintf(
                stderr,
                "Q2GC SAVE: GET %s LEGACY OK "
                "(%lu files raw=%lu)\n",
                saveDirectoryNames[index],
                (unsigned long)saveDirectories[index].count,
                (unsigned long)storedSize
            );
        }
    }


    free(
        stored
    );
}


static void releasePersistentDirectoryCache(
    int index)
{
    size_t oldCount;


    if (index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST)
    {
        return;
    }


    if (!persistentDirectoryLoaded[index])
        return;


    oldCount =
        saveDirectories[index].count;


    clearDirectory(
        &saveDirectories[index]
    );


    persistentDirectoryLoaded[index] =
        0;


    fprintf(
        stderr,
        "Q2GC SAVE: CACHE EVICT %s "
        "(%lu files)\n",
        saveDirectoryNames[index],
        (unsigned long)oldCount
    );
}


static void releaseAllPersistentDirectoryCaches(void)
{
    int index;


    for (index =
            Q2_SAVE_PERSIST_FIRST;
         index <=
            Q2_SAVE_PERSIST_LAST;
         ++index)
    {
        releasePersistentDirectoryCache(
            index
        );
    }
}


static void ensurePersistentDirectoryLoaded(
    int index)
{
    int other;


    /*
     * current/ is always RAM-only.
     */
    if (index == 0)
        return;


    if (index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST)
    {
        return;
    }


    if (persistentDirectoryLoaded[index])
        return;


    /*
     * Never retain two expanded persistent slots.
     */
    for (other =
            Q2_SAVE_PERSIST_FIRST;
         other <=
            Q2_SAVE_PERSIST_LAST;
         ++other)
    {
        if (other != index)
        {
            releasePersistentDirectoryCache(
                other
            );
        }
    }


    clearDirectory(
        &saveDirectories[index]
    );


    fprintf(
        stderr,
        "Q2GC SAVE: CACHE LOAD %s\n",
        saveDirectoryNames[index]
    );


    loadPersistentDirectory(
        index
    );


    /*
     * NOT_FOUND is a legitimate empty-slot state.
     *
     * Other GET failures are already reported by
     * loadPersistentDirectory(). Marking this operation loaded
     * prevents repeated CARD reads during one Quake operation.
     */
    persistentDirectoryLoaded[index] =
        1;
}


static void ensureSavePathDirectoryLoaded(
    const char *path)
{
    int index;

    const char *filename;


    if (!path)
        return;


    if (!parseSavePath(
            path,
            &index,
            &filename))
    {
        return;
    }


    (void)filename;


    ensurePersistentDirectoryLoaded(
        index
    );
}


static void initializeVfs(void)
{
    if (vfsInitialized)
        return;


    memset(
        saveDirectories,
        0,
        sizeof(saveDirectories)
    );

    memset(
        saveStreams,
        0,
        sizeof(saveStreams)
    );

    memset(
        persistentDirectoryLoaded,
        0,
        sizeof(persistentDirectoryLoaded)
    );

    clearSlotIndexCache();


    vfsInitialized =
        1;


    /*
     * Save-path virtualization must still work for current/ when no Memory
     * Card is present, otherwise normal Quake map transitions would once
     * again attempt to write to dvd:/.
     */
    if (!initializeCard())
    {
        fprintf(
            stderr,
            "Q2GC SAVE: running with RAM-only current save state\n"
        );

        return;
    }


    /*
     * The only startup persistence read.
     *
     * This is 140 bytes and is retained for menu presentation.  Full
     * save0..save3 bundles remain lazy.
     */
    loadSlotIndex();


    fprintf(
        stderr,
        "Q2GC SAVE: persistent slots lazy; "
        "menu index resident\n"
    );
}



/* ------------------------------------------------------------------------- */
/* Save/Load menu presentation                                               */
/* ------------------------------------------------------------------------- */


int Q2_SaveMenuSlotComment(
    int slot,
    char comment[32])
{
    uint32_t bit;


    if (!comment)
        return 0;


    memset(
        comment,
        0,
        Q2_SAVE_COMMENT_SIZE
    );


    if (slot < 0 ||
        slot >=
            (int)Q2_SAVE_SLOT_COUNT)
    {
        return 0;
    }


    bit =
        1u <<
        (unsigned int)slot;


    if (!(slotIndexValidMask & bit))
        return 0;


    memcpy(
        comment,
        slotIndexComments[slot],
        Q2_SAVE_COMMENT_SIZE
    );

    comment[
        Q2_SAVE_COMMENT_SIZE - 1u
    ] =
        '\0';


    return 1;
}


/* ------------------------------------------------------------------------- */
/* Q2CF GameCube preference storage                                          */
/* ------------------------------------------------------------------------- */


int Q2_ConfigStorageGet(
    void *buffer,
    size_t capacity,
    size_t *size)
{
    CH_PersistResult result;


    if (size)
        *size = 0;


    if (!buffer ||
        capacity == 0)
    {
        return -1;
    }


    initializeVfs();


    if (!cardReady)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG STORE: CARD unavailable\n"
        );

        return -1;
    }


    result =
        CH_PersistGet(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            configPersistScope,
            sizeof(configPersistScope) - 1u,
            configPersistKey,
            sizeof(configPersistKey) - 1u,
            buffer,
            capacity,
            size
        );


    if (result ==
        CH_PERSIST_RESULT_NOT_FOUND)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG STORE: config-v2 empty\n"
        );

        return 0;
    }


    if (result !=
        CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG STORE: GET config-v2 failed: %d\n",
            (int)result
        );

        return -1;
    }


    fprintf(
        stderr,
        "Q2GC CONFIG STORE: GET config-v2 OK "
        "bytes=%lu\n",
        (unsigned long)(
            size
                ? *size
                : 0u
        )
    );


    return 1;
}


int Q2_ConfigStoragePut(
    const void *buffer,
    size_t size)
{
    CH_PersistResult result;


    if (!buffer ||
        size == 0)
    {
        return 0;
    }


    initializeVfs();


    if (!cardReady)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG STORE: CARD unavailable\n"
        );

        return 0;
    }


    result =
        CH_PersistPut(
            &txBackend,
            sectorBuffer,
            sectorBufferSize,
            configPersistScope,
            sizeof(configPersistScope) - 1u,
            configPersistKey,
            sizeof(configPersistKey) - 1u,
            buffer,
            size
        );


    if (result !=
        CH_PERSIST_RESULT_OK)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG STORE: PUT config-v2 failed: %d\n",
            (int)result
        );

        return 0;
    }


    fprintf(
        stderr,
        "Q2GC CONFIG STORE: PUT config-v2 OK "
        "bytes=%lu\n",
        (unsigned long)size
    );


    return 1;
}


/* ------------------------------------------------------------------------- */
/* Quake save-operation batching                                             */
/* ------------------------------------------------------------------------- */


int Q2_SavePrepareSource(
    const char *directoryName)
{
    int index;


    if (!directoryName ||
        !directoryName[0])
    {
        return 0;
    }


    index =
        directoryIndex(
            directoryName,
            strlen(directoryName)
        );


    if (index < 0)
        return 0;


    initializeVfs();


    ensurePersistentDirectoryLoaded(
        index
    );


    {
        const char *failedName = NULL;

        if (directoryHasWriteFailure(
                &saveDirectories[index],
                &failedName))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: SOURCE REJECT %s "
                "write_failed_file=%s\n",
                saveDirectoryNames[index],
                failedName
                    ? failedName
                    : "(unknown)"
            );

            return 0;
        }
    }


    fprintf(
        stderr,
        "Q2GC SAVE: SOURCE READY %s\n",
        saveDirectoryNames[index]
    );


    return 1;
}


void Q2_SaveReleasePersistentCaches(void)
{
    releaseAllPersistentDirectoryCaches();
}


int Q2_SaveBatchBegin(
    const char *directoryName)
{
    int index;


    if (!directoryName ||
        !directoryName[0] ||
        batchActive)
    {
        return 0;
    }


    index =
        directoryIndex(
            directoryName,
            strlen(directoryName)
        );


    if (index < 0)
        return 0;


    initializeVfs();

    /*
     * Q2GC_SAVE_FRESH_DESTINATION_V1
     *
     * SV_CopySaveGame() has already prepared the source and will
     * immediately SV_WipeSavegame(dst) before copying every save
     * file into dst.
     *
     * Loading the old persistent destination here is therefore
     * pure peak-memory and CARD-I/O overhead. Start persistent
     * destination batches from a known-empty decoded directory
     * instead. Mark it loaded so the wipe/remove calls inside the
     * active batch cannot lazily fetch the obsolete CARD object.
     *
     * current/ remains the existing RAM-only path.
     */
    if (index > 0)
    {
        size_t oldCount =
            saveDirectories[
                index
            ].count;

        clearDirectory(
            &saveDirectories[
                index
            ]
        );

        persistentDirectoryLoaded[
            index
        ] =
            1;

        fprintf(
            stderr,
            "Q2GC SAVE: BATCH FRESH %s "
            "old_files=%lu old_destination_read=skipped\n",
            saveDirectoryNames[index],
            (unsigned long)oldCount
        );
    }


    batchActive =
        1;

    batchDirectory =
        index;

    batchDirty =
        0;


    fprintf(
        stderr,
        "Q2GC SAVE: BATCH BEGIN %s\n",
        saveDirectoryNames[index]
    );


    return 1;
}


int Q2_SaveBatchEnd(void)
{
    int index;
    int dirty;
    int ok;


    if (!batchActive)
        return 0;


    index =
        batchDirectory;

    dirty =
        batchDirty;


    /*
     * Clear batch state before the persistent operation itself.
     */
    batchActive =
        0;

    batchDirectory =
        -1;

    batchDirty =
        0;


    /*
     * current/ is RAM scratch.
     */
    if (index == 0)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: BATCH END current RAM-only\n"
        );

        return 1;
    }


    if (!dirty)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: BATCH END %s clean\n",
            saveDirectoryNames[index]
        );

        return 1;
    }


    {
        const char *failedName = NULL;

        if (directoryHasWriteFailure(
                &saveDirectories[index],
                &failedName))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: BATCH REJECT %s "
                "write_failed_file=%s\n",
                saveDirectoryNames[index],
                failedName
                    ? failedName
                    : "(unknown)"
            );

            return 0;
        }
    }


    ok =
        persistDirectory(
            index
        );


    fprintf(
        stderr,
        "Q2GC SAVE: BATCH COMMIT %s %s\n",
        saveDirectoryNames[index],
        ok
            ? "OK"
            : "FAILED"
    );


    return ok;
}

/* ------------------------------------------------------------------------- */
/* stream helpers                                                            */
/* ------------------------------------------------------------------------- */

static q2_save_stream_t *allocateStream(void)
{
    int i;


    for (i = 0;
         i < Q2_SAVE_STREAM_COUNT;
         ++i)
    {
        if (!saveStreams[i].used)
        {
            memset(
                &saveStreams[i],
                0,
                sizeof(saveStreams[i])
            );


            saveStreams[i].used =
                1;


            return
                &saveStreams[i];
        }
    }


    return NULL;
}


static q2_save_stream_t *virtualStream(
    FILE *file)
{
    int i;


    for (i = 0;
         i < Q2_SAVE_STREAM_COUNT;
         ++i)
    {
        if (saveStreams[i].used &&
            (FILE *)&saveStreams[i] ==
                file)
        {
            return
                &saveStreams[i];
        }
    }


    return NULL;
}


/* ------------------------------------------------------------------------- */
/* stdio shim                                                                */
/* ------------------------------------------------------------------------- */

FILE *Q2_SaveFOpen(
    const char *path,
    const char *mode)
{
    int directoryIndexValue;

    const char *filename;

    q2_save_directory_t *directory;
    q2_save_file_t *entry;
    q2_save_stream_t *stream;


    if (!parseSavePath(
            path,
            &directoryIndexValue,
            &filename))
    {
        return
            fopen(
                path,
                mode
            );
    }


    initializeVfs();

    ensureSavePathDirectoryLoaded(
        path
    );


    directory =
        &saveDirectories[
            directoryIndexValue
        ];


    if (strchr(mode, 'w'))
    {
        entry =
            createFile(
                directory,
                filename
            );

        if (!entry)
            return NULL;


        stream =
            allocateStream();

        if (!stream)
            return NULL;


        stream->writable =
            1;

        stream->directory =
            directory;

        stream->entry =
            entry;

        stream->position =
            0;


        return
            (FILE *)stream;
    }


    if (strchr(mode, 'r'))
    {
        entry =
            findFile(
                directory,
                filename
            );

        if (!entry)
            return NULL;


        stream =
            allocateStream();

        if (!stream)
            return NULL;


        stream->directory =
            directory;

        stream->entry =
            entry;

        stream->position =
            0;


        return
            (FILE *)stream;
    }


    return NULL;
}


size_t Q2_SaveFRead(
    void *ptr,
    size_t size,
    size_t nmemb,
    FILE *file)
{
    q2_save_stream_t *stream =
        virtualStream(
            file
        );

    size_t requested;
    size_t available;
    size_t bytes;


    if (!stream)
    {
        return
            fread(
                ptr,
                size,
                nmemb,
                file
            );
    }


    if (!ptr ||
        size == 0 ||
        nmemb == 0)
    {
        return 0;
    }


    if (nmemb >
        SIZE_MAX / size)
    {
        return 0;
    }


    requested =
        size * nmemb;


    if (stream->position >=
        stream->entry->size)
    {
        return 0;
    }


    available =
        stream->entry->size -
        stream->position;


    bytes =
        requested <
            available
        ? requested
        : available;


    memcpy(
        ptr,
        stream->entry->data +
            stream->position,
        bytes
    );


    stream->position +=
        bytes;


    return
        bytes / size;
}


size_t Q2_SaveFWrite(
    const void *ptr,
    size_t size,
    size_t nmemb,
    FILE *file)
{
    q2_save_stream_t *stream =
        virtualStream(
            file
        );

    q2_save_file_t *entry;

    size_t requested;
    size_t required;
    size_t newCapacity;

    unsigned char *newData;


    if (!stream)
    {
        return
            fwrite(
                ptr,
                size,
                nmemb,
                file
            );
    }


    if (!stream->writable ||
        !ptr ||
        size == 0 ||
        nmemb == 0)
    {
        return 0;
    }


    if (nmemb >
        SIZE_MAX / size)
    {
        return 0;
    }


    requested =
        size * nmemb;


    if (stream->position >
        SIZE_MAX - requested)
    {
        return 0;
    }


    required =
        stream->position +
        requested;


    if (required >
        Q2_SAVE_FILE_MAX)
    {
        stream->entry->write_failed =
            1;

        fprintf(
            stderr,
            "Q2GC SAVE: FILE WRITE FAILED "
            "file=%s reason=file_limit "
            "required=%lu limit=%u\n",
            stream->entry->name,
            (unsigned long)required,
            (unsigned int)Q2_SAVE_FILE_MAX
        );

        return 0;
    }


    entry =
        stream->entry;


    if (required >
        entry->capacity)
    {
        size_t oldCapacity =
            entry->capacity;

        if (required <=
            Q2_SAVE_FILE_GROW_LINEAR_THRESHOLD)
        {
            newCapacity =
                entry->capacity
                    ? entry->capacity
                    : 4096u;

            while (newCapacity <
                   required)
            {
                size_t next =
                    newCapacity * 2u;

                if (next <
                        newCapacity ||
                    next >
                        Q2_SAVE_FILE_GROW_LINEAR_THRESHOLD)
                {
                    newCapacity =
                        required;

                    break;
                }

                newCapacity =
                    next;
            }
        }
        else
        {
            size_t rounded;

            if (required >
                SIZE_MAX -
                    (Q2_SAVE_FILE_GROW_CHUNK - 1u))
            {
                entry->write_failed =
                    1;

                fprintf(
                    stderr,
                    "Q2GC SAVE: FILE WRITE FAILED "
                    "file=%s reason=growth_overflow "
                    "required=%lu\n",
                    entry->name,
                    (unsigned long)required
                );

                return 0;
            }

            rounded =
                (
                    required
                    +
                    (Q2_SAVE_FILE_GROW_CHUNK - 1u)
                )
                &
                ~(
                    (size_t)
                    (Q2_SAVE_FILE_GROW_CHUNK - 1u)
                );

            if (rounded >
                Q2_SAVE_FILE_MAX)
            {
                rounded =
                    required;
            }

            newCapacity =
                rounded;
        }

        fprintf(
            stderr,
            "Q2GC SAVE: FILE GROW "
            "file=%s old=%lu required=%lu target=%lu\n",
            entry->name,
            (unsigned long)oldCapacity,
            (unsigned long)required,
            (unsigned long)newCapacity
        );

        newData =
            realloc(
                entry->data,
                newCapacity
            );

        /*
         * Headroom is optional. If the rounded allocation fails,
         * retry the exact bytes needed by this write.
         */
        if (!newData &&
            newCapacity != required)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: FILE GROW RETRY "
                "file=%s target=%lu exact=%lu\n",
                entry->name,
                (unsigned long)newCapacity,
                (unsigned long)required
            );

            newCapacity =
                required;

            newData =
                realloc(
                    entry->data,
                    newCapacity
                );
        }

        if (!newData)
        {
            entry->write_failed =
                1;

            fprintf(
                stderr,
                "Q2GC SAVE: FILE WRITE FAILED "
                "file=%s reason=realloc "
                "old=%lu required=%lu target=%lu\n",
                entry->name,
                (unsigned long)oldCapacity,
                (unsigned long)required,
                (unsigned long)newCapacity
            );

            return 0;
        }


        entry->data =
            newData;

        entry->capacity =
            newCapacity;
    }


    memcpy(
        entry->data +
            stream->position,
        ptr,
        requested
    );


    stream->position +=
        requested;


    if (stream->position >
        entry->size)
    {
        entry->size =
            stream->position;
    }


    return nmemb;
}


int Q2_SaveFClose(
    FILE *file)
{
    q2_save_stream_t *stream =
        virtualStream(
            file
        );

    int directoryIndexValue =
        -1;

    int persistOk =
        1;

    int i;


    if (!stream)
    {
        return
            fclose(
                file
            );
    }


    if (stream->writable)
    {
        for (i = 0;
             i < Q2_SAVE_DIRECTORY_COUNT;
             ++i)
        {
            if (&saveDirectories[i] ==
                stream->directory)
            {
                directoryIndexValue =
                    i;

                break;
            }
        }


        if (directoryIndexValue > 0)
        {
            if (batchActive &&
                batchDirectory ==
                    directoryIndexValue)
            {
                /*
                 * Quake is still constructing the save directory.
                 * Do not expose a half-written snapshot to CARD.
                 */
                batchDirty =
                    1;
            }
            else
            {
                persistOk =
                    persistDirectory(
                        directoryIndexValue
                    );
            }
        }
    }


    memset(
        stream,
        0,
        sizeof(*stream)
    );


    return
        persistOk
            ? 0
            : -1;
}

int Q2_SaveRemove(
    const char *path)
{
    int directoryIndexValue;

    const char *filename;

    int result;


    if (!parseSavePath(
            path,
            &directoryIndexValue,
            &filename))
    {
        return
            remove(
                path
            );
    }


    initializeVfs();

    ensureSavePathDirectoryLoaded(
        path
    );


    result =
        removeFile(
            &saveDirectories[
                directoryIndexValue
            ],
            filename
        );


    if (result == 0 &&
        directoryIndexValue > 0)
    {
        if (batchActive &&
            batchDirectory ==
                directoryIndexValue)
        {
            batchDirty =
                1;
        }
        else
        {
            (void)
                persistDirectory(
                    directoryIndexValue
                );
        }
    }


    return result;
}

/* ------------------------------------------------------------------------- */
/* virtual Sys_FindFirst / Sys_FindNext                                      */
/* ------------------------------------------------------------------------- */

static int extensionMatches(
    const char *name,
    const char *extension)
{
    size_t nameLength;
    size_t extensionLength;


    if (!name ||
        !extension)
    {
        return 0;
    }


    nameLength =
        strlen(name);

    extensionLength =
        strlen(extension);


    if (nameLength <
        extensionLength)
    {
        return 0;
    }


    return
        !strcmp(
            name +
                nameLength -
                extensionLength,
            extension
        );
}


static char *nextFindResult(void)
{
    q2_save_directory_t *directory;


    if (!findActive ||
        findDirectory < 0 ||
        findDirectory >=
            Q2_SAVE_DIRECTORY_COUNT)
    {
        return NULL;
    }


    directory =
        &saveDirectories[
            findDirectory
        ];


    while (findIndex <
           directory->count)
    {
        const char *name =
            directory->files[
                findIndex++
            ].name;


        if (!extensionMatches(
                name,
                findExtension))
        {
            continue;
        }


        if (snprintf(
                findResult,
                sizeof(findResult),
                "%s%s",
                findPrefix,
                name) >=
            (int)sizeof(findResult))
        {
            continue;
        }


        return
            findResult;
    }


    return NULL;
}


char *Q2_SaveFindFirst(
    char *path,
    unsigned musthave,
    unsigned canthave)
{
    const char *save;
    const char *slash;
    const char *wildcard;

    size_t directoryLength;
    size_t prefixLength;


    (void)musthave;
    (void)canthave;


    Q2_SaveFindClose();


    if (!path)
        return NULL;


    save =
        strstr(
            path,
            "/save/"
        );

    if (!save)
        return NULL;


    save +=
        strlen("/save/");


    slash =
        strchr(
            save,
            '/'
        );

    if (!slash)
        return NULL;


    directoryLength =
        (size_t)(slash - save);


    findDirectory =
        directoryIndex(
            save,
            directoryLength
        );

    if (findDirectory < 0)
        return NULL;


    wildcard =
        slash + 1;


    if (!strcmp(
            wildcard,
            "*.sav"))
    {
        strcpy(
            findExtension,
            ".sav"
        );
    }
    else if (!strcmp(
                 wildcard,
                 "*.sv2"))
    {
        strcpy(
            findExtension,
            ".sv2"
        );
    }
    else
    {
        return NULL;
    }


    prefixLength =
        (size_t)(
            slash -
            path +
            1
        );


    if (prefixLength >=
        sizeof(findPrefix))
    {
        return NULL;
    }


    memcpy(
        findPrefix,
        path,
        prefixLength
    );

    findPrefix[prefixLength] =
        '\0';


    initializeVfs();


    findActive =
        1;

    findIndex =
        0;


    return
        nextFindResult();
}


char *Q2_SaveFindNext(
    unsigned musthave,
    unsigned canthave)
{
    (void)musthave;
    (void)canthave;


    return
        nextFindResult();
}


void Q2_SaveFindClose(void)
{
    findActive =
        0;

    findDirectory =
        -1;

    findIndex =
        0;

    findExtension[0] =
        '\0';

    findPrefix[0] =
        '\0';

    findResult[0] =
        '\0';
}
