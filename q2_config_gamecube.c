#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "qcommon/qcommon.h"
#include "client/keys.h"
#include "q2_save_stdio.h"


/*
 * Q2GC_CONFIG_Q2CF_V2
 *
 * Small, explicit GameCube preference object.
 *
 * This is deliberately NOT a Quake cfg file.
 *
 * No arbitrary commands are serialized or executed.
 * No global binding reset is performed.
 */


#define Q2CF_MAGIC          0x51324346u /* Q2CF */
#define Q2CF_VERSION        2u
#define Q2CF_SIZE           64u

#define Q2CF_BINDING_COUNT  19u

#define Q2CF_BINDINGS_OFFSET 36u

#define Q2CF_BINDING_UNBOUND  0u
#define Q2CF_BINDING_UNKNOWN  255u


#define Q2CF_FLAG_INVERT_LOOK  (1u << 0)
#define Q2CF_FLAG_ALWAYS_RUN   (1u << 1)
#define Q2CF_FLAG_FREELOOK     (1u << 2)
#define Q2CF_FLAG_LOOKSPRING   (1u << 3)
#define Q2CF_FLAG_LOOKSTRAFE   (1u << 4)


static const int q2cfPhysicalKeys[
    Q2CF_BINDING_COUNT
] =
{
    K_JOY1,
    K_JOY2,
    K_JOY3,
    K_JOY4,

    K_AUX1,
    K_AUX2,
    K_AUX3,

    K_GC_DPAD_UP,
    K_GC_DPAD_DOWN,
    K_GC_DPAD_LEFT,
    K_GC_DPAD_RIGHT,

    K_GC_STICK_UP,
    K_GC_STICK_DOWN,
    K_GC_STICK_LEFT,
    K_GC_STICK_RIGHT,

    K_GC_CSTICK_UP,
    K_GC_CSTICK_DOWN,
    K_GC_CSTICK_LEFT,
    K_GC_CSTICK_RIGHT
};


/*
 * Stable Q2CF action IDs.
 *
 * Do not reorder existing entries after public release.
 *
 * Add new actions only at the end.
 *
 * ID 0 is reserved for explicitly unbound.
 */
static const char *q2cfActions[] =
{
    "+attack",
    "weapnext",

    "+forward",
    "+back",

    "+left",
    "+right",

    "+speed",

    "+moveleft",
    "+moveright",
    "+strafe",

    "+lookup",
    "+lookdown",
    "centerview",

    "+mlook",
    "+klook",

    "+moveup",
    "+movedown",

    "inven",
    "invuse",
    "invdrop",
    "invprev",
    "invnext",

    "cmd help"
};


#define Q2CF_ACTION_COUNT \
    ((unsigned int)(sizeof(q2cfActions) / sizeof(q2cfActions[0])))


static unsigned char
    q2cfLastSnapshot[
        Q2CF_SIZE
    ];

static int
    q2cfLastSnapshotValid;


static uint32_t Q2CF_ReadBE32(
    const unsigned char *data)
{
    return
        ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        ((uint32_t)data[3]);
}


static void Q2CF_WriteBE32(
    unsigned char *data,
    uint32_t value)
{
    data[0] =
        (unsigned char)(
            value >> 24
        );

    data[1] =
        (unsigned char)(
            value >> 16
        );

    data[2] =
        (unsigned char)(
            value >> 8
        );

    data[3] =
        (unsigned char)value;
}


static uint32_t Q2CF_ClampUnsigned(
    float value,
    uint32_t minimum,
    uint32_t maximum)
{
    if (value <
        (float)minimum)
    {
        return minimum;
    }


    if (value >
        (float)maximum)
    {
        return maximum;
    }


    return
        (uint32_t)(
            value +
            0.5f
        );
}


static unsigned char Q2CF_BindingToId(
    const char *binding)
{
    unsigned int i;


    if (!binding ||
        !binding[0])
    {
        return
            Q2CF_BINDING_UNBOUND;
    }


    for (i = 0;
         i < Q2CF_ACTION_COUNT;
         ++i)
    {
        if (!strcmp(
                binding,
                q2cfActions[i]))
        {
            return
                (unsigned char)(
                    i + 1u
                );
        }
    }


    /*
     * Do not serialize arbitrary console text.
     *
     * Unknown means:
     *   "Q2CF does not own this binding."
     *
     * On load we leave the DVD baseline untouched.
     */
    return
        Q2CF_BINDING_UNKNOWN;
}


static const char *Q2CF_IdToBinding(
    unsigned char id)
{
    if (id == Q2CF_BINDING_UNBOUND)
        return "";


    if (id == Q2CF_BINDING_UNKNOWN)
        return NULL;


    if (id < 1u ||
        id > Q2CF_ACTION_COUNT)
    {
        return NULL;
    }


    return
        q2cfActions[
            (unsigned int)id - 1u
        ];
}


static void Q2CF_EncodeCurrent(
    unsigned char encoded[
        Q2CF_SIZE
    ])
{
    uint32_t flags =
        0;

    uint32_t turnSensitivity;

    uint32_t crosshair;

    uint32_t volume;

    unsigned int i;


    memset(
        encoded,
        0,
        Q2CF_SIZE
    );


    if (Cvar_VariableValue(
            "m_pitch") < 0.0f)
    {
        flags |=
            Q2CF_FLAG_INVERT_LOOK;
    }


    if (Cvar_VariableValue(
            "cl_run") != 0.0f)
    {
        flags |=
            Q2CF_FLAG_ALWAYS_RUN;
    }


    if (Cvar_VariableValue(
            "freelook") != 0.0f)
    {
        flags |=
            Q2CF_FLAG_FREELOOK;
    }


    if (Cvar_VariableValue(
            "lookspring") != 0.0f)
    {
        flags |=
            Q2CF_FLAG_LOOKSPRING;
    }


    if (Cvar_VariableValue(
            "lookstrafe") != 0.0f)
    {
        flags |=
            Q2CF_FLAG_LOOKSTRAFE;
    }


    turnSensitivity =
        Q2CF_ClampUnsigned(
            Cvar_VariableValue(
                "gc_turn_sensitivity"),
            25u,
            200u
        );


    crosshair =
        Q2CF_ClampUnsigned(
            Cvar_VariableValue(
                "crosshair"),
            0u,
            3u
        );


    volume =
        Q2CF_ClampUnsigned(
            Cvar_VariableValue(
                "s_volume") *
                1000.0f,
            0u,
            1000u
        );


    Q2CF_WriteBE32(
        encoded + 0,
        Q2CF_MAGIC
    );

    Q2CF_WriteBE32(
        encoded + 4,
        Q2CF_VERSION
    );

    Q2CF_WriteBE32(
        encoded + 8,
        Q2CF_SIZE
    );

    Q2CF_WriteBE32(
        encoded + 12,
        Q2CF_BINDING_COUNT
    );

    Q2CF_WriteBE32(
        encoded + 16,
        flags
    );

    Q2CF_WriteBE32(
        encoded + 20,
        turnSensitivity
    );

    Q2CF_WriteBE32(
        encoded + 24,
        crosshair
    );

    Q2CF_WriteBE32(
        encoded + 28,
        volume
    );


    /*
     * Bytes 32..35 are reserved for future format evolution.
     */


    for (i = 0;
         i < Q2CF_BINDING_COUNT;
         ++i)
    {
        int key =
            q2cfPhysicalKeys[i];

        const char *binding =
            NULL;


        if (key >= 0 &&
            key < 256)
        {
            binding =
                keybindings[key];
        }


        encoded[
            Q2CF_BINDINGS_OFFSET +
            i
        ] =
            Q2CF_BindingToId(
                binding
            );
    }
}


static int Q2CF_Validate(
    const unsigned char encoded[
        Q2CF_SIZE
    ],
    size_t size)
{
    unsigned int i;


    if (size != Q2CF_SIZE)
        return 0;


    if (Q2CF_ReadBE32(
            encoded + 0) !=
        Q2CF_MAGIC)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 4) !=
        Q2CF_VERSION)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 8) !=
        Q2CF_SIZE)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 12) !=
        Q2CF_BINDING_COUNT)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 20) < 25u ||
        Q2CF_ReadBE32(
            encoded + 20) > 200u)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 24) > 3u)
    {
        return 0;
    }


    if (Q2CF_ReadBE32(
            encoded + 28) > 1000u)
    {
        return 0;
    }


    for (i = 0;
         i < Q2CF_BINDING_COUNT;
         ++i)
    {
        unsigned int id =
            encoded[
                Q2CF_BINDINGS_OFFSET +
                i
            ];


        if (id !=
                Q2CF_BINDING_UNKNOWN &&
            id >
                Q2CF_ACTION_COUNT)
        {
            return 0;
        }
    }


    return 1;
}


static void Q2CF_Apply(
    const unsigned char encoded[
        Q2CF_SIZE
    ])
{
    uint32_t flags;

    uint32_t turnSensitivity;

    uint32_t crosshair;

    uint32_t volume;

    float pitch;

    unsigned int i;


    flags =
        Q2CF_ReadBE32(
            encoded + 16
        );

    turnSensitivity =
        Q2CF_ReadBE32(
            encoded + 20
        );

    crosshair =
        Q2CF_ReadBE32(
            encoded + 24
        );

    volume =
        Q2CF_ReadBE32(
            encoded + 28
        );


    Cvar_SetValue(
        "gc_turn_sensitivity",
        (float)turnSensitivity
    );


    pitch =
        Cvar_VariableValue(
            "m_pitch"
        );


    if (pitch < 0.0f)
        pitch = -pitch;


    if (pitch == 0.0f)
        pitch = 0.022f;


    if (flags &
        Q2CF_FLAG_INVERT_LOOK)
    {
        pitch = -pitch;
    }


    Cvar_SetValue(
        "m_pitch",
        pitch
    );


    Cvar_SetValue(
        "cl_run",
        (flags &
            Q2CF_FLAG_ALWAYS_RUN)
            ? 1.0f
            : 0.0f
    );


    Cvar_SetValue(
        "freelook",
        (flags &
            Q2CF_FLAG_FREELOOK)
            ? 1.0f
            : 0.0f
    );


    Cvar_SetValue(
        "lookspring",
        (flags &
            Q2CF_FLAG_LOOKSPRING)
            ? 1.0f
            : 0.0f
    );


    Cvar_SetValue(
        "lookstrafe",
        (flags &
            Q2CF_FLAG_LOOKSTRAFE)
            ? 1.0f
            : 0.0f
    );


    Cvar_SetValue(
        "crosshair",
        (float)crosshair
    );


    Cvar_SetValue(
        "s_volume",
        (float)volume /
            1000.0f
    );


    /*
     * IMPORTANT:
     *
     * We touch ONLY physical GameCube keys.
     *
     * Keyboard/default bindings remain whatever the DVD cfg stack
     * established.
     *
     * There is deliberately no global unbind operation.
     */
    for (i = 0;
         i < Q2CF_BINDING_COUNT;
         ++i)
    {
        unsigned char id =
            encoded[
                Q2CF_BINDINGS_OFFSET +
                i
            ];

        const char *binding =
            Q2CF_IdToBinding(
                id
            );


        if (!binding)
        {
            /*
             * Unknown/unmanaged.
             *
             * Preserve the baseline binding for this physical key.
             */
            continue;
        }


        Key_SetBinding(
            q2cfPhysicalKeys[i],
            (char *)binding
        );
    }


    fprintf(
        stderr,
        "Q2GC CONFIG: applied Q2CF v2 "
        "turn=%lu invert=%u run=%u freelook=%u "
        "crosshair=%lu volume=%lu\n",
        (unsigned long)turnSensitivity,
        (flags & Q2CF_FLAG_INVERT_LOOK)
            ? 1u
            : 0u,
        (flags & Q2CF_FLAG_ALWAYS_RUN)
            ? 1u
            : 0u,
        (flags & Q2CF_FLAG_FREELOOK)
            ? 1u
            : 0u,
        (unsigned long)crosshair,
        (unsigned long)volume
    );
}


void Q2_ConfigApplyPersisted(void)
{
    unsigned char encoded[
        Q2CF_SIZE
    ];

    size_t size =
        0;

    int result;


    /*
     * Establish the exact DVD/config baseline as the initial snapshot.
     *
     * If no Q2CF object exists, merely opening/closing Options therefore
     * does not create one.
     */
    Q2CF_EncodeCurrent(
        q2cfLastSnapshot
    );

    q2cfLastSnapshotValid =
        1;


    result =
        Q2_ConfigStorageGet(
            encoded,
            sizeof(encoded),
            &size
        );


    if (result == 0)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG: config-v2 empty; "
            "DVD baseline active\n"
        );

        return;
    }


    if (result < 0)
    {
        fprintf(
            stderr,
            "Q2GC CONFIG: config-v2 read failed; "
            "DVD baseline active\n"
        );

        return;
    }


    if (!Q2CF_Validate(
            encoded,
            size))
    {
        fprintf(
            stderr,
            "Q2GC CONFIG: invalid Q2CF object; "
            "DVD baseline active\n"
        );

        return;
    }


    Q2CF_Apply(
        encoded
    );


    /*
     * Snapshot what the engine actually contains after application,
     * rather than blindly trusting bytes that may contain a future
     * unmanaged binding ID.
     */
    Q2CF_EncodeCurrent(
        q2cfLastSnapshot
    );

    q2cfLastSnapshotValid =
        1;
}


void Q2_ConfigSaveIfChanged(void)
{
    unsigned char encoded[
        Q2CF_SIZE
    ];


    Q2CF_EncodeCurrent(
        encoded
    );


    if (q2cfLastSnapshotValid &&
        !memcmp(
            encoded,
            q2cfLastSnapshot,
            sizeof(encoded)))
    {
        fprintf(
            stderr,
            "Q2GC CONFIG: unchanged; skip PUT\n"
        );

        return;
    }


    if (!Q2_ConfigStoragePut(
            encoded,
            sizeof(encoded)))
    {
        fprintf(
            stderr,
            "Q2GC CONFIG: PUT failed; "
            "snapshot remains dirty\n"
        );

        return;
    }


    memcpy(
        q2cfLastSnapshot,
        encoded,
        sizeof(encoded)
    );

    q2cfLastSnapshotValid =
        1;


    fprintf(
        stderr,
        "Q2GC CONFIG: PUT Q2CF v2 OK "
        "bytes=%u\n",
        (unsigned int)sizeof(encoded)
    );
}
