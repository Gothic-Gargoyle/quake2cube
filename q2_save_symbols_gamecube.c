#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "q2_save_symbols_gamecube.h"


#define Q2SM_PATH "dvd:/q2save.sym"

#define Q2SM_VERSION 1u

#define Q2SM_MAX_SYMBOLS 4096u


typedef char
    q2sm_pointer_must_be_32_bits[
        sizeof(void *) == 4
            ? 1
            : -1
    ];


typedef struct
{
    uint32_t id;
    uint32_t address;

} q2sm_entry_t;


static q2sm_entry_t *q2sm_entries;

static uint32_t q2sm_function_count;

static uint32_t q2sm_mmove_count;

static unsigned int q2sm_users;


static uint32_t Q2SM_BE32(
    const unsigned char *p)
{
    return
        ((uint32_t)p[0] << 24) |
        ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) |
        ((uint32_t)p[3]);
}


static int Q2SM_Read(
    FILE *file,
    void *buffer,
    size_t size)
{
    return (
        fread(
            buffer,
            1,
            size,
            file
        )
        == size
    );
}


static void Q2SM_Unload(void)
{
    free(q2sm_entries);

    q2sm_entries = NULL;

    q2sm_function_count = 0;

    q2sm_mmove_count = 0;
}


static int Q2SM_Load(void)
{
    FILE *file;

    unsigned char header[16];

    unsigned char raw[8];

    uint32_t function_count;

    uint32_t mmove_count;

    uint32_t total;

    uint32_t i;

    q2sm_entry_t *entries;


    file = fopen(
        Q2SM_PATH,
        "rb"
    );

    if (!file)
    {
        fprintf(
            stderr,
            "Q2GC SAVE ABI: could not open %s\n",
            Q2SM_PATH
        );

        return 0;
    }


    if (!Q2SM_Read(
            file,
            header,
            sizeof(header)))
    {
        fprintf(
            stderr,
            "Q2GC SAVE ABI: truncated symbol-map header\n"
        );

        fclose(file);

        return 0;
    }


    if (
        memcmp(
            header,
            "Q2SM",
            4
        ) != 0
    )
    {
        fprintf(
            stderr,
            "Q2GC SAVE ABI: bad symbol-map magic\n"
        );

        fclose(file);

        return 0;
    }


    if (
        Q2SM_BE32(
            header + 4
        ) != Q2SM_VERSION
    )
    {
        fprintf(
            stderr,
            "Q2GC SAVE ABI: bad symbol-map version\n"
        );

        fclose(file);

        return 0;
    }


    function_count =
        Q2SM_BE32(
            header + 8
        );

    mmove_count =
        Q2SM_BE32(
            header + 12
        );

    total =
        function_count +
        mmove_count;


    if (
        function_count > Q2SM_MAX_SYMBOLS ||
        mmove_count > Q2SM_MAX_SYMBOLS ||
        total > Q2SM_MAX_SYMBOLS
    )
    {
        fprintf(
            stderr,
            "Q2GC SAVE ABI: unreasonable symbol counts\n"
        );

        fclose(file);

        return 0;
    }


    entries = NULL;

    if (total)
    {
        entries = malloc(
            total *
            sizeof(*entries)
        );

        if (!entries)
        {
            fprintf(
                stderr,
                "Q2GC SAVE ABI: symbol-map allocation failed\n"
            );

            fclose(file);

            return 0;
        }
    }


    for (
        i = 0;
        i < total;
        ++i
    )
    {
        if (!Q2SM_Read(
                file,
                raw,
                sizeof(raw)))
        {
            fprintf(
                stderr,
                "Q2GC SAVE ABI: truncated symbol entry %u\n",
                (unsigned int)i
            );

            free(entries);

            fclose(file);

            return 0;
        }


        entries[i].id =
            Q2SM_BE32(raw);

        entries[i].address =
            Q2SM_BE32(
                raw + 4
            );


        if (
            entries[i].id == 0 ||
            entries[i].address < 0x80000000u ||
            entries[i].address >= 0x81800000u
        )
        {
            fprintf(
                stderr,
                "Q2GC SAVE ABI: invalid symbol entry %u\n",
                (unsigned int)i
            );

            free(entries);

            fclose(file);

            return 0;
        }
    }


    fclose(file);


    for (
        i = 1;
        i < function_count;
        ++i
    )
    {
        if (
            entries[i - 1].id >=
            entries[i].id
        )
        {
            fprintf(
                stderr,
                "Q2GC SAVE ABI: unsorted function IDs\n"
            );

            free(entries);

            return 0;
        }
    }


    for (
        i = function_count + 1;
        i < total;
        ++i
    )
    {
        if (
            entries[i - 1].id >=
            entries[i].id
        )
        {
            fprintf(
                stderr,
                "Q2GC SAVE ABI: unsorted mmove IDs\n"
            );

            free(entries);

            return 0;
        }
    }


    q2sm_entries =
        entries;

    q2sm_function_count =
        function_count;

    q2sm_mmove_count =
        mmove_count;


    fprintf(
        stderr,
        "Q2GC SAVE ABI: symbol map loaded "
        "functions=%u mmoves=%u bytes=%u\n",
        (unsigned int)function_count,
        (unsigned int)mmove_count,
        (unsigned int)(
            total *
            sizeof(*entries)
        )
    );


    return 1;
}


int Q2_SaveSymbolsBegin(void)
{
    if (q2sm_users)
    {
        ++q2sm_users;

        return 1;
    }


    if (!Q2SM_Load())
        return 0;


    q2sm_users = 1;

    return 1;
}


void Q2_SaveSymbolsEnd(void)
{
    if (!q2sm_users)
        return;


    --q2sm_users;


    if (!q2sm_users)
    {
        Q2SM_Unload();

        fprintf(
            stderr,
            "Q2GC SAVE ABI: symbol map released\n"
        );
    }
}


static uint32_t Q2SM_AddressToId(
    const q2sm_entry_t *entries,
    uint32_t count,
    const void *pointer)
{
    uint32_t address;

    uint32_t i;


    if (!pointer)
        return 0;


    address =
        (uint32_t)(
            uintptr_t
        )pointer;


    for (
        i = 0;
        i < count;
        ++i
    )
    {
        if (
            entries[i].address ==
            address
        )
        {
            return entries[i].id;
        }
    }


    return 0;
}


static void *Q2SM_IdToAddress(
    const q2sm_entry_t *entries,
    uint32_t count,
    uint32_t id)
{
    uint32_t low = 0;

    uint32_t high = count;


    if (!id)
        return NULL;


    while (low < high)
    {
        uint32_t middle =
            low +
            (
                high - low
            ) / 2;


        if (
            entries[middle].id ==
            id
        )
        {
            return (
                void *
            )(
                uintptr_t
            )entries[middle].address;
        }


        if (
            entries[middle].id <
            id
        )
        {
            low =
                middle + 1;
        }
        else
        {
            high =
                middle;
        }
    }


    return NULL;
}


uint32_t Q2_SaveFunctionToId(
    const void *address)
{
    if (!q2sm_entries)
        return 0;


    return Q2SM_AddressToId(
        q2sm_entries,
        q2sm_function_count,
        address
    );
}


void *Q2_SaveFunctionFromId(
    uint32_t id)
{
    if (!q2sm_entries)
        return NULL;


    return Q2SM_IdToAddress(
        q2sm_entries,
        q2sm_function_count,
        id
    );
}


uint32_t Q2_SaveMMoveToId(
    const void *address)
{
    if (!q2sm_entries)
        return 0;


    return Q2SM_AddressToId(
        q2sm_entries +
            q2sm_function_count,
        q2sm_mmove_count,
        address
    );
}


void *Q2_SaveMMoveFromId(
    uint32_t id)
{
    if (!q2sm_entries)
        return NULL;


    return Q2SM_IdToAddress(
        q2sm_entries +
            q2sm_function_count,
        q2sm_mmove_count,
        id
    );
}
