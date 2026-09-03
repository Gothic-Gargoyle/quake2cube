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
#define Q2_SAVE_BUNDLE_MAX           (512u * 1024u)

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

static int persistDirectory(
    int index)
{
    unsigned char *bundle =
        NULL;

    unsigned char *stored =
        NULL;

    size_t bundleSize =
        0;

    size_t storedSize;

    uLongf compressedCapacity;
    uLongf compressedSize;

    uLong rawCrc;

    int zResult;

    CH_PersistResult result;


    /*
     * current/ is deliberately RAM-only.
     */
    if (index == 0)
        return 1;


    if (index <
            Q2_SAVE_PERSIST_FIRST ||
        index >
            Q2_SAVE_PERSIST_LAST)
    {
        return 0;
    }


    if (!cardReady)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: CARD unavailable; %s remains RAM-only\n",
            saveDirectoryNames[index]
        );

        return 0;
    }


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


    /*
     * Q2_SAVE_BUNDLE_MAX is deliberately far below zlib's uLong limit
     * on GameCube, so these casts are bounded by our own VFS limits.
     */
    compressedCapacity =
        compressBound(
            (uLong)bundleSize
        );


    if ((size_t)compressedCapacity >
        SIZE_MAX -
            Q2_SAVE_STORED_HEADER_SIZE)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: compressed-size overflow for %s\n",
            saveDirectoryNames[index]
        );

        free(
            bundle
        );

        return 0;
    }


    storedSize =
        Q2_SAVE_STORED_HEADER_SIZE +
        (size_t)compressedCapacity;


    stored =
        malloc(
            storedSize
        );

    if (!stored)
    {
        free(
            bundle
        );

        return 0;
    }


    compressedSize =
        compressedCapacity;


    /*
     * Saves are dominated by sparse structs, configstrings and repeated
     * state. Z_BEST_SPEED dramatically cuts CARD sectors while keeping the
     * PowerPC-side compression pause small.
     */
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


    if (zResult !=
        Z_OK)
    {
        fprintf(
            stderr,
            "Q2GC SAVE: deflate %s failed: %d\n",
            saveDirectoryNames[index],
            zResult
        );

        free(
            stored
        );

        free(
            bundle
        );

        return 0;
    }


    rawCrc =
        crc32(
            0L,
            Z_NULL,
            0
        );

    rawCrc =
        crc32(
            rawCrc,
            (const Bytef *)bundle,
            (uLong)bundleSize
        );


    writeBe32(
        stored + 0,
        Q2_SAVE_STORED_MAGIC
    );

    writeBe32(
        stored + 4,
        Q2_SAVE_STORED_VERSION
    );

    writeBe32(
        stored + 8,
        (uint32_t)bundleSize
    );

    writeBe32(
        stored + 12,
        (uint32_t)compressedSize
    );

    writeBe32(
        stored + 16,
        (uint32_t)rawCrc
    );


    storedSize =
        Q2_SAVE_STORED_HEADER_SIZE +
        (size_t)compressedSize;


    fprintf(
        stderr,
        "Q2GC SAVE: DEFLATE %s raw=%lu stored=%lu\n",
        saveDirectoryNames[index],
        (unsigned long)bundleSize,
        (unsigned long)storedSize
    );


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


    free(
        stored
    );

    free(
        bundle
    );


    if (result !=
        CH_PERSIST_RESULT_OK)
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
        (unsigned long)bundleSize,
        (unsigned long)storedSize
    );


    /*
     * The full save bundle is authoritative.
     *
     * Only after that transaction succeeds do we publish its tiny menu
     * comment.  A power loss between these two operations can therefore
     * leave stale menu metadata, but can never advertise an uncommitted
     * save as valid.
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


    return 1;
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
        compressBound(
            (uLong)Q2_SAVE_BUNDLE_MAX
        );


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
        uint32_t rawSize =
            readBe32(
                stored + 8
            );

        uint32_t compressedSize =
            readBe32(
                stored + 12
            );

        uint32_t expectedCrc =
            readBe32(
                stored + 16
            );

        uLongf outputSize;

        uLong actualCrc;

        int zResult;


        if (rawSize < 12u ||
            rawSize >
                Q2_SAVE_BUNDLE_MAX ||
            compressedSize !=
                storedSize -
                Q2_SAVE_STORED_HEADER_SIZE)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: %s compressed header corrupt\n",
                saveDirectoryNames[index]
            );

            free(
                stored
            );

            return;
        }


        bundle =
            malloc(
                rawSize
            );

        if (!bundle)
        {
            free(
                stored
            );

            return;
        }


        outputSize =
            (uLongf)rawSize;


        zResult =
            uncompress(
                (Bytef *)bundle,
                &outputSize,
                (const Bytef *)(
                    stored +
                    Q2_SAVE_STORED_HEADER_SIZE
                ),
                (uLong)compressedSize
            );


        if (zResult !=
                Z_OK ||
            outputSize !=
                (uLongf)rawSize)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: inflate %s failed: %d\n",
                saveDirectoryNames[index],
                zResult
            );

            free(
                bundle
            );

            free(
                stored
            );

            return;
        }


        actualCrc =
            crc32(
                0L,
                Z_NULL,
                0
            );

        actualCrc =
            crc32(
                actualCrc,
                (const Bytef *)bundle,
                outputSize
            );


        if ((uint32_t)actualCrc !=
            expectedCrc)
        {
            fprintf(
                stderr,
                "Q2GC SAVE: %s CRC mismatch\n",
                saveDirectoryNames[index]
            );

            free(
                bundle
            );

            free(
                stored
            );

            return;
        }


        bundleSize =
            rawSize;


        if (!decodeDirectory(
                &saveDirectories[index],
                bundle,
                bundleSize))
        {
            fprintf(
                stderr,
                "Q2GC SAVE: %s bundle corrupt\n",
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
                "Q2GC SAVE: GET %s OK "
                "(%lu files raw=%lu stored=%lu)\n",
                saveDirectoryNames[index],
                (unsigned long)saveDirectories[index].count,
                (unsigned long)bundleSize,
                (unsigned long)storedSize
            );
        }


        free(
            bundle
        );
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

    ensurePersistentDirectoryLoaded(
        index
    );


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
        fprintf(
            stderr,
            "Q2GC SAVE: file %s exceeds %u-byte limit\n",
            stream->entry->name,
            (unsigned int)Q2_SAVE_FILE_MAX
        );

        return 0;
    }


    entry =
        stream->entry;


    if (required >
        entry->capacity)
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
                    Q2_SAVE_FILE_MAX)
            {
                newCapacity =
                    required;

                break;
            }


            newCapacity =
                next;
        }


        newData =
            realloc(
                entry->data,
                newCapacity
            );

        if (!newData)
            return 0;


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
