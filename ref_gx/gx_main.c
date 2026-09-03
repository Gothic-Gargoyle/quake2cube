/*
 * Quake2Cube native GameCube GX renderer.
 *
 * REF_GX BOOTSTRAP V1c
 *
 * First vertical slice:
 *
 *     Quake II
 *       -> GetRefAPI
 *       -> native GX
 *       -> EFB
 *       -> XFB
 *       -> VI
 *
 * No Quake II geometry or 2D assets are rendered yet.
 * A stable bright-magenta screen is success.
 */

#include "../client/client.h"

#include <gccore.h>

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


#define Q2GX_FIFO_SIZE GX_FIFO_MINSIZE


/*
 * IMPORTANT:
 *
 * This is deliberately GLOBAL.
 *
 * The existing GameCube platform/input bridge currently includes
 * ref_soft/r_local.h and therefore expects the active renderer to
 * provide:
 *
 *     extern refimport_t ri;
 *
 * ref_soft and ref_gl both own this symbol in exactly the same way.
 */
refimport_t ri;


static GXRModeObj *q2gx_mode;

static void *q2gx_xfb[2];
static void *q2gx_fifo;

static unsigned int q2gx_draw_buffer;
static unsigned int q2gx_frame_count;

static qboolean q2gx_gx_initialized;
static qboolean q2gx_video_configured;


static const GXColor q2gx_clear_color =
{
    0xff,
    0x00,
    0xff,
    0xff
};


/*
 * Q2GC_NATIVE_DRAWFILL_V1
 *
 * Native GX 2D rendering state.
 *
 * Quake II uses a logical 640x480 coordinate system with
 * (0,0) at the top-left.
 */
static Mtx q2gx_2d_model;
static Mtx44 q2gx_2d_projection;


/*
 * Base Quake II 256-color palette loaded from:
 *
 *     pics/colormap.pcx
 */
static GXColor q2gx_palette[256];

static qboolean q2gx_palette_loaded;


/*
 * Q2GC_NATIVE_DRAWCHAR_V1C
 *
 * First native Quake II character renderer.
 *
 * pics/conchars.pcx contributes indexed raster data.
 *
 * The embedded PCX RGB palette is deliberately ignored.
 * Visible indices resolve through q2gx_palette[], which is
 * already the proven Quake II renderer palette.
 *
 * V1c is correctness-first:
 *
 *     glyph indexed pixels
 *       -> horizontal equal-index runs
 *       -> palette RGB
 *       -> native GX quads
 *
 * A textured font atlas comes later.
 */
#define Q2GX_CONCHARS_WIDTH  128
#define Q2GX_CONCHARS_HEIGHT 128

static byte q2gx_conchars[
    Q2GX_CONCHARS_WIDTH *
    Q2GX_CONCHARS_HEIGHT
];

static int q2gx_conchars_transparent;
static qboolean q2gx_conchars_loaded;

static unsigned int q2gx_drawchar_calls_window;
static unsigned int q2gx_drawchar_runs_window;
static unsigned int q2gx_drawchar_quads_window;
static unsigned int q2gx_drawfill_calls_window;

static qboolean q2gx_drawchar_first_logged;


/*
 * Q2GC_NATIVE_TEXTURE_PIPELINE_V1
 *
 * First generic native GX texture path.
 *
 *     linear CPU RGBA
 *       -> GX_TF_RGBA8 tiled storage
 *       -> cache writeback
 *       -> GXTexObj
 *       -> GX_LoadTexObj
 *       -> direct ST coordinates
 *       -> TEV sample
 *       -> source-alpha blend
 *       -> EFB
 *
 * Quake image registration is deliberately outside this
 * milestone.
 */
/*
 * Q2GC_REAL_PICTURE_CACHE_V1
 *
 * Native GX cache for:
 *
 *     RegisterPic
 *     DrawGetPicSize
 *     DrawPic
 *     DrawStretchPic
 *
 * Pictures remain resident until renderer shutdown in V1.
 *
 * The stock baseq2 picture set is small enough that an
 * eviction policy is deliberately deferred until we have
 * real runtime memory measurements.
 */
#define Q2GX_MAX_PICS 256

struct image_s
{
    char name[MAX_QPATH];

    int width;
    int height;

    u32 texture_bytes;

    void *texture_data;
    GXTexObj texture;

    qboolean has_alpha;
};

static struct image_s q2gx_pics[
    Q2GX_MAX_PICS
];

/*
 * Q2GC_DRAWCHAR_TEXTURE_ATLAS_V1
 *
 * DrawChar now reuses pics/conchars.pcx from the real GX
 * picture cache.
 *
 * The original indexed conchars decoder remains intact for
 * this correctness milestone, but is no longer consulted by
 * Q2GX_DrawChar itself.
 */
static struct image_s *q2gx_drawchar_atlas;

static qboolean q2gx_drawchar_atlas_logged;
static qboolean q2gx_drawchar_atlas_failed_logged;

static qboolean q2gx_texture_first_logged;
static qboolean q2gx_pic_first_draw_logged;

static unsigned int q2gx_texture_draws_window;

static unsigned int q2gx_pic_draws_window;
static unsigned int q2gx_stretchpic_draws_window;

static unsigned int q2gx_pic_loads_window;
static unsigned int q2gx_pic_hits_window;
static unsigned int q2gx_pic_misses_window;


static void Q2GX_FreeResources(void)
{
    if (q2gx_gx_initialized)
    {
        GX_DrawDone();
    }

    if (q2gx_video_configured)
    {
        VIDEO_SetBlack(true);
        VIDEO_Flush();
        VIDEO_WaitForFlush();
        VIDEO_WaitVSync();
    }

    if (q2gx_xfb[0])
    {
        free(q2gx_xfb[0]);
        q2gx_xfb[0] = NULL;
    }

    if (q2gx_xfb[1])
    {
        free(q2gx_xfb[1]);
        q2gx_xfb[1] = NULL;
    }

    if (q2gx_fifo)
    {
        free(q2gx_fifo);
        q2gx_fifo = NULL;
    }

    q2gx_mode = NULL;

    q2gx_draw_buffer = 0u;
    q2gx_frame_count = 0u;

    q2gx_gx_initialized = false;
    q2gx_video_configured = false;
}


/*
 * Existing GameCube client-specific hook.
 *
 * ref_soft uses it to throw away registration resources before
 * cinematic/static attract content so the decoder has MEM1 room.
 *
 * Bootstrap V1c owns:
 *
 *     no models
 *     no images
 *     no skins
 *     no world
 *     no registration heap
 *
 * Therefore the correct bootstrap implementation is a no-op.
 *
 * Once ref_gx actually owns registration resources, this becomes
 * their real purge implementation.
 */
void R_GC_PurgeRegistrationResources(void)
{
}



static qboolean Q2GX_LoadBasePalette(void)
{
    void *file_data;
    unsigned char *bytes;
    unsigned char *palette;

    int file_length;
    int i;

    file_data = NULL;

    file_length =
        ri.FS_LoadFile(
            "pics/colormap.pcx",
            &file_data
        );

    if (file_length < 769 ||
        !file_data)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PALETTE: "
            "load failed bytes=%d\n",
            file_length
        );

        if (file_data)
        {
            ri.FS_FreeFile(
                file_data
            );
        }

        return false;
    }

    bytes =
        (unsigned char *)file_data;

    /*
     * Standard 8-bit PCX palette:
     *
     *     final 769 bytes
     *
     *       0x0c
     *       R G B x 256
     */
    if (bytes[file_length - 769] != 0x0c)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PALETTE: "
            "PCX palette marker missing\n"
        );

        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    palette =
        bytes +
        file_length -
        768;

    for (i = 0;
         i < 256;
         ++i)
    {
        q2gx_palette[i].r =
            palette[i * 3 + 0];

        q2gx_palette[i].g =
            palette[i * 3 + 1];

        q2gx_palette[i].b =
            palette[i * 3 + 2];

        q2gx_palette[i].a =
            0xff;
    }

    ri.FS_FreeFile(
        file_data
    );

    q2gx_palette_loaded =
        true;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX PALETTE: "
        "0=%02x%02x%02x "
        "64=%02x%02x%02x "
        "128=%02x%02x%02x "
        "200=%02x%02x%02x "
        "255=%02x%02x%02x\n",

        q2gx_palette[0].r,
        q2gx_palette[0].g,
        q2gx_palette[0].b,

        q2gx_palette[64].r,
        q2gx_palette[64].g,
        q2gx_palette[64].b,

        q2gx_palette[128].r,
        q2gx_palette[128].g,
        q2gx_palette[128].b,

        q2gx_palette[200].r,
        q2gx_palette[200].g,
        q2gx_palette[200].b,

        q2gx_palette[255].r,
        q2gx_palette[255].g,
        q2gx_palette[255].b
    );

    return true;
}



static unsigned int Q2GX_ReadLE16(
    const byte *p)
{
    return
        (unsigned int)p[0]
        |
        (
            (unsigned int)p[1]
            << 8
        );
}


static qboolean Q2GX_LoadConchars(void)
{
    void *file_data;

    byte *bytes;
    byte *source;
    byte *source_end;

    int file_length;

    unsigned int xmin;
    unsigned int ymin;
    unsigned int xmax;
    unsigned int ymax;

    unsigned int width;
    unsigned int height;

    unsigned int planes;
    unsigned int bytes_per_line;

    unsigned int x;
    unsigned int y;

    file_data =
        NULL;

    file_length =
        ri.FS_LoadFile(
            "pics/conchars.pcx",
            &file_data
        );

    if (file_length < 128 ||
        !file_data)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX CONCHARS: "
            "load failed bytes=%d\n",
            file_length
        );

        if (file_data)
        {
            ri.FS_FreeFile(
                file_data
            );
        }

        return false;
    }

    bytes =
        (byte *)file_data;

    /*
     * PCX:
     *
     * manufacturer = 0x0a
     * encoding     = 1 (RLE)
     * bits/pixel   = 8
     */
    if (bytes[0] != 0x0a ||
        bytes[2] != 1 ||
        bytes[3] != 8)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX CONCHARS: "
            "unsupported PCX header\n"
        );

        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    xmin =
        Q2GX_ReadLE16(
            bytes + 4
        );

    ymin =
        Q2GX_ReadLE16(
            bytes + 6
        );

    xmax =
        Q2GX_ReadLE16(
            bytes + 8
        );

    ymax =
        Q2GX_ReadLE16(
            bytes + 10
        );

    if (xmax < xmin ||
        ymax < ymin)
    {
        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    width =
        xmax - xmin + 1;

    height =
        ymax - ymin + 1;

    planes =
        bytes[65];

    bytes_per_line =
        Q2GX_ReadLE16(
            bytes + 66
        );

    if (width != Q2GX_CONCHARS_WIDTH ||
        height != Q2GX_CONCHARS_HEIGHT ||
        planes != 1 ||
        bytes_per_line < width)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX CONCHARS: "
            "unexpected geometry "
            "%ux%u planes=%u bpl=%u\n",
            width,
            height,
            planes,
            bytes_per_line
        );

        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    /*
     * Stock asset has a trailing PCX VGA palette.
     *
     * We use its marker only to establish where the RLE raster
     * ends.
     *
     * We DO NOT use its RGB values.
     */
    if (file_length < 769 ||
        bytes[file_length - 769] != 0x0c)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX CONCHARS: "
            "trailing VGA palette missing\n"
        );

        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    source =
        bytes + 128;

    source_end =
        bytes +
        file_length -
        769;

    for (y = 0;
         y < height;
         ++y)
    {
        x =
            0;

        while (x < bytes_per_line)
        {
            unsigned int count;
            byte value;
            byte code;

            if (source >= source_end)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX CONCHARS: "
                    "truncated RLE stream\n"
                );

                ri.FS_FreeFile(
                    file_data
                );

                return false;
            }

            code =
                *source++;

            if ((code & 0xc0) == 0xc0)
            {
                count =
                    code & 0x3f;

                if (count == 0 ||
                    source >= source_end)
                {
                    ri.FS_FreeFile(
                        file_data
                    );

                    return false;
                }

                value =
                    *source++;
            }
            else
            {
                count =
                    1;

                value =
                    code;
            }

            if (x + count >
                bytes_per_line)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX CONCHARS: "
                    "RLE run crosses scanline\n"
                );

                ri.FS_FreeFile(
                    file_data
                );

                return false;
            }

            while (count--)
            {
                if (x < width)
                {
                    q2gx_conchars[
                        y *
                            Q2GX_CONCHARS_WIDTH
                        +
                        x
                    ] =
                        value;
                }

                ++x;
            }
        }
    }

    /*
     * Character 32 is the known blank space glyph.
     *
     * 16 glyphs per atlas row:
     *
     *     row = 32 >> 4 = 2
     *     y   = 2 * 8 = 16
     *
     * Host-side proof already established this entire 8x8
     * glyph consists of palette index 255.
     */
    q2gx_conchars_transparent =
        q2gx_conchars[
            16 *
            Q2GX_CONCHARS_WIDTH
        ];

    for (y = 16;
         y < 24;
         ++y)
    {
        for (x = 0;
             x < 8;
             ++x)
        {
            if (
                q2gx_conchars[
                    y *
                        Q2GX_CONCHARS_WIDTH
                    +
                    x
                ]
                !=
                q2gx_conchars_transparent
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX CONCHARS: "
                    "space glyph not uniform\n"
                );

                ri.FS_FreeFile(
                    file_data
                );

                return false;
            }
        }
    }

    /*
     * Exact stock-data expectation independently proven by the
     * preceding host parser.
     */
    if (q2gx_conchars_transparent != 255)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX CONCHARS: "
            "unexpected transparent index=%d\n",
            q2gx_conchars_transparent
        );

        ri.FS_FreeFile(
            file_data
        );

        return false;
    }

    q2gx_conchars_loaded =
        true;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX CONCHARS: "
        "128x128 "
        "bpl=%u "
        "transparent=%d "
        "indexed=1\n",
        bytes_per_line,
        q2gx_conchars_transparent
    );

    ri.FS_FreeFile(
        file_data
    );

    return true;
}


static void Q2GX_Setup2D(void)
{
    /*
     * top
     * bottom
     * left
     * right
     * near
     * far
     */
    guOrtho(
        q2gx_2d_projection,
        0.0f,
        480.0f,
        0.0f,
        640.0f,
        0.0f,
        1.0f
    );

    guMtxIdentity(
        q2gx_2d_model
    );

    GX_LoadProjectionMtx(
        q2gx_2d_projection,
        GX_ORTHOGRAPHIC
    );

    GX_LoadPosMtxImm(
        q2gx_2d_model,
        GX_PNMTX0
    );

    GX_SetCurrentMtx(
        GX_PNMTX0
    );

    GX_ClearVtxDesc();

    GX_SetVtxDesc(
        GX_VA_POS,
        GX_DIRECT
    );

    GX_SetVtxDesc(
        GX_VA_CLR0,
        GX_DIRECT
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_POS,
        GX_POS_XY,
        GX_F32,
        0
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_CLR0,
        GX_CLR_RGBA,
        GX_RGBA8,
        0
    );

    GX_SetNumChans(
        1
    );

    GX_SetNumTexGens(
        0
    );

    GX_SetNumTevStages(
        1
    );

    /*
     * No texture:
     *
     * pass rasterized vertex color directly through TEV.
     */
    GX_SetTevOrder(
        GX_TEVSTAGE0,
        GX_TEXCOORDNULL,
        GX_TEXMAP_NULL,
        GX_COLOR0A0
    );

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_PASSCLR
    );

    GX_SetCullMode(
        GX_CULL_NONE
    );

    GX_SetZMode(
        GX_FALSE,
        GX_ALWAYS,
        GX_FALSE
    );

    GX_SetBlendMode(
        GX_BM_NONE,
        GX_BL_ZERO,
        GX_BL_ZERO,
        GX_LO_CLEAR
    );
}


static void Q2GX_DrawSolidRect(
    f32 x,
    f32 y,
    f32 w,
    f32 h,
    GXColor color)
{
    GX_Begin(
        GX_QUADS,
        GX_VTXFMT0,
        4
    );

    GX_Position2f32(
        x,
        y
    );

    GX_Color4u8(
        color.r,
        color.g,
        color.b,
        color.a
    );

    GX_Position2f32(
        x + w,
        y
    );

    GX_Color4u8(
        color.r,
        color.g,
        color.b,
        color.a
    );

    GX_Position2f32(
        x + w,
        y + h
    );

    GX_Color4u8(
        color.r,
        color.g,
        color.b,
        color.a
    );

    GX_Position2f32(
        x,
        y + h
    );

    GX_Color4u8(
        color.r,
        color.g,
        color.b,
        color.a
    );

    GX_End();
}



static void Q2GX_WriteRGBA8Texel(
    u8 *destination,
    unsigned int width,
    unsigned int x,
    unsigned int y,
    u8 red,
    u8 green,
    u8 blue,
    u8 alpha)
{
    unsigned int tiles_per_row;
    unsigned int tile_x;
    unsigned int tile_y;
    unsigned int block_offset;
    unsigned int pixel_offset;

    /*
     * GX_TF_RGBA8:
     *
     * 4x4 texels = 64-byte tile
     *
     * bytes  0..31:
     *     A,R
     *
     * bytes 32..63:
     *     G,B
     */
    tiles_per_row =
        (width + 3u) >> 2;

    tile_x =
        x >> 2;

    tile_y =
        y >> 2;

    block_offset =
        (
            tile_y *
                tiles_per_row
            +
            tile_x
        )
        *
        64u;

    pixel_offset =
        (
            (
                (y & 3u) *
                    4u
            )
            +
            (x & 3u)
        )
        *
        2u;

    destination[
        block_offset +
        pixel_offset +
        0u
    ] =
        alpha;

    destination[
        block_offset +
        pixel_offset +
        1u
    ] =
        red;

    destination[
        block_offset +
        32u +
        pixel_offset +
        0u
    ] =
        green;

    destination[
        block_offset +
        32u +
        pixel_offset +
        1u
    ] =
        blue;
}


static void Q2GX_SetupTextured2D(void)
{
    GX_LoadProjectionMtx(
        q2gx_2d_projection,
        GX_ORTHOGRAPHIC
    );

    GX_LoadPosMtxImm(
        q2gx_2d_model,
        GX_PNMTX0
    );

    GX_SetCurrentMtx(
        GX_PNMTX0
    );

    GX_ClearVtxDesc();

    GX_SetVtxDesc(
        GX_VA_POS,
        GX_DIRECT
    );

    GX_SetVtxDesc(
        GX_VA_TEX0,
        GX_DIRECT
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_POS,
        GX_POS_XY,
        GX_F32,
        0
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_TEX0,
        GX_TEX_ST,
        GX_F32,
        0
    );

    GX_SetNumChans(
        0
    );

    GX_SetNumTexGens(
        1
    );

    GX_SetTexCoordGen(
        GX_TEXCOORD0,
        GX_TG_MTX2x4,
        GX_TG_TEX0,
        GX_IDENTITY
    );

    GX_SetNumTevStages(
        1
    );

    GX_SetTevOrder(
        GX_TEVSTAGE0,
        GX_TEXCOORD0,
        GX_TEXMAP0,
        GX_COLORNULL
    );

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_REPLACE
    );

    GX_SetCullMode(
        GX_CULL_NONE
    );

    GX_SetZMode(
        GX_FALSE,
        GX_ALWAYS,
        GX_FALSE
    );

    GX_SetBlendMode(
        GX_BM_BLEND,
        GX_BL_SRCALPHA,
        GX_BL_INVSRCALPHA,
        GX_LO_CLEAR
    );
}


static void Q2GX_DrawTexturedQuad(
    f32 x,
    f32 y,
    f32 width,
    f32 height,
    GXTexObj *texture)
{
    if (!texture)
    {
        return;
    }

    Q2GX_SetupTextured2D();

    GX_LoadTexObj(
        texture,
        GX_TEXMAP0
    );

    GX_Begin(
        GX_QUADS,
        GX_VTXFMT0,
        4
    );

    GX_Position2f32(
        x,
        y
    );

    GX_TexCoord2f32(
        0.0f,
        0.0f
    );

    GX_Position2f32(
        x + width,
        y
    );

    GX_TexCoord2f32(
        1.0f,
        0.0f
    );

    GX_Position2f32(
        x + width,
        y + height
    );

    GX_TexCoord2f32(
        1.0f,
        1.0f
    );

    GX_Position2f32(
        x,
        y + height
    );

    GX_TexCoord2f32(
        0.0f,
        1.0f
    );

    GX_End();

    ++q2gx_texture_draws_window;

    if (!q2gx_texture_first_logged)
    {
        q2gx_texture_first_logged =
            true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TEXTURE FIRST: "
            "textured GX quad submitted\n"
        );
    }

    /*
     * Restore known-good color-only state for DrawFill and
     * brute-force DrawChar.
     */
    Q2GX_Setup2D();
}


static void Q2GX_DrawTexturedQuadUV(
    f32 x,
    f32 y,
    f32 width,
    f32 height,
    f32 s0,
    f32 t0,
    f32 s1,
    f32 t1,
    GXTexObj *texture)
{
    if (!texture)
    {
        return;
    }

    Q2GX_SetupTextured2D();

    GX_LoadTexObj(
        texture,
        GX_TEXMAP0
    );

    GX_Begin(
        GX_QUADS,
        GX_VTXFMT0,
        4
    );

    GX_Position2f32(
        x,
        y
    );

    GX_TexCoord2f32(
        s0,
        t0
    );

    GX_Position2f32(
        x + width,
        y
    );

    GX_TexCoord2f32(
        s1,
        t0
    );

    GX_Position2f32(
        x + width,
        y + height
    );

    GX_TexCoord2f32(
        s1,
        t1
    );

    GX_Position2f32(
        x,
        y + height
    );

    GX_TexCoord2f32(
        s0,
        t1
    );

    GX_End();

    ++q2gx_texture_draws_window;

    if (!q2gx_texture_first_logged)
    {
        q2gx_texture_first_logged =
            true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TEXTURE FIRST: "
            "textured GX quad submitted\n"
        );
    }

    /*
     * Keep the existing renderer contract:
     *
     * every textured helper restores the known-good
     * untextured DrawFill state before returning.
     */
    Q2GX_Setup2D();
}




static qboolean Q2GX_NormalizePicName(
    const char *name,
    char normalized[MAX_QPATH])
{
    const char *source;
    size_t length;

    if (!name ||
        !name[0])
    {
        return false;
    }

    /*
     * Stock Draw_FindPic contract:
     *
     *     "/foo/bar.pcx"
     *         -> "foo/bar.pcx"
     *
     * otherwise:
     *
     *     "foo"
     *         -> "pics/foo.pcx"
     */
    if (
        name[0] == '/' ||
        name[0] == '\\'
    )
    {
        source =
            name + 1;

        length =
            strlen(source);

        if (
            length == 0 ||
            length >= MAX_QPATH
        )
        {
            return false;
        }

        memcpy(
            normalized,
            source,
            length + 1
        );

        return true;
    }

    length =
        strlen(name);

    /*
     * Five bytes "pics/"
     * plus four ".pcx"
     * plus NUL.
     */
    if (
        length + 10u >
        MAX_QPATH
    )
    {
        return false;
    }

    memcpy(
        normalized,
        "pics/",
        5
    );

    memcpy(
        normalized + 5,
        name,
        length
    );

    memcpy(
        normalized + 5 + length,
        ".pcx",
        5
    );

    return true;
}


static struct image_s *Q2GX_FindCachedPic(
    const char *normalized)
{
    unsigned int index;

    for (index = 0;
         index < Q2GX_MAX_PICS;
         ++index)
    {
        if (
            q2gx_pics[index].name[0] &&
            strcmp(
                q2gx_pics[index].name,
                normalized
            ) == 0
        )
        {
            return
                &q2gx_pics[index];
        }
    }

    return NULL;
}


static struct image_s *Q2GX_FindFreePicSlot(void)
{
    unsigned int index;

    for (index = 0;
         index < Q2GX_MAX_PICS;
         ++index)
    {
        if (
            !q2gx_pics[index].name[0] &&
            !q2gx_pics[index].texture_data
        )
        {
            return
                &q2gx_pics[index];
        }
    }

    return NULL;
}


static qboolean Q2GX_LoadPCXPic(
    struct image_s *image,
    char *normalized)
{
    void *file_data;

    byte *bytes;
    byte *source;
    byte *source_end;

    void *texture_data;

    int file_length;

    unsigned int xmin;
    unsigned int ymin;
    unsigned int xmax;
    unsigned int ymax;

    unsigned int width;
    unsigned int height;

    unsigned int planes;
    unsigned int bytes_per_line;

    unsigned int x;
    unsigned int y;

    u32 texture_bytes;

    qboolean has_alpha;

    file_data =
        NULL;

    texture_data =
        NULL;

    if (!image ||
        !normalized ||
        !q2gx_palette_loaded)
    {
        return false;
    }

    file_length =
        ri.FS_LoadFile(
            normalized,
            &file_data
        );

    if (
        file_length < 128 ||
        !file_data
    )
    {
        if (file_data)
        {
            ri.FS_FreeFile(
                file_data
            );
        }

        return false;
    }

    bytes =
        (byte *)file_data;

    if (
        bytes[0] != 0x0a ||
        bytes[2] != 1 ||
        bytes[3] != 8
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC: "
            "unsupported PCX %s\n",
            normalized
        );

        goto fail;
    }

    xmin =
        Q2GX_ReadLE16(
            bytes + 4
        );

    ymin =
        Q2GX_ReadLE16(
            bytes + 6
        );

    xmax =
        Q2GX_ReadLE16(
            bytes + 8
        );

    ymax =
        Q2GX_ReadLE16(
            bytes + 10
        );

    if (
        xmax < xmin ||
        ymax < ymin
    )
    {
        goto fail;
    }

    width =
        xmax - xmin + 1;

    height =
        ymax - ymin + 1;

    planes =
        bytes[65];

    bytes_per_line =
        Q2GX_ReadLE16(
            bytes + 66
        );

    if (
        width == 0 ||
        height == 0 ||
        width > 1024 ||
        height > 1024 ||
        planes != 1 ||
        bytes_per_line < width
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC: "
            "unsupported PCX geometry "
            "%s %ux%u planes=%u bpl=%u\n",
            normalized,
            width,
            height,
            planes,
            bytes_per_line
        );

        goto fail;
    }

    /*
     * Use trailing VGA marker only to locate the end of the
     * encoded raster.
     *
     * The PCX's private RGB palette is deliberately ignored.
     * Decoded bytes remain Quake palette indices.
     */
    if (
        file_length < 769 ||
        bytes[file_length - 769] != 0x0c
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC: "
            "missing PCX palette marker %s\n",
            normalized
        );

        goto fail;
    }

    texture_bytes =
        GX_GetTexBufferSize(
            (u16)width,
            (u16)height,
            GX_TF_RGBA8,
            GX_FALSE,
            0
        );

    if (!texture_bytes)
    {
        goto fail;
    }

    texture_data =
        memalign(
            32,
            texture_bytes
        );

    if (!texture_data)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC: "
            "MEM1 allocation failed "
            "%s bytes=%u\n",
            normalized,
            (unsigned int)
                texture_bytes
        );

        goto fail;
    }

    /*
     * Any padded texels outside the real source dimensions
     * start transparent black.
     */
    memset(
        texture_data,
        0,
        texture_bytes
    );

    source =
        bytes + 128;

    source_end =
        bytes +
        file_length -
        769;

    has_alpha =
        false;

    for (y = 0;
         y < height;
         ++y)
    {
        x =
            0;

        while (x < bytes_per_line)
        {
            unsigned int count;
            byte value;
            byte code;

            if (source >= source_end)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX PIC: "
                    "truncated PCX %s\n",
                    normalized
                );

                goto fail;
            }

            code =
                *source++;

            if (
                (code & 0xc0) == 0xc0
            )
            {
                count =
                    code & 0x3f;

                if (
                    count == 0 ||
                    source >= source_end
                )
                {
                    goto fail;
                }

                value =
                    *source++;
            }
            else
            {
                count =
                    1;

                value =
                    code;
            }

            if (
                x + count >
                bytes_per_line
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX PIC: "
                    "PCX run crosses scanline "
                    "%s\n",
                    normalized
                );

                goto fail;
            }

            while (count--)
            {
                if (x < width)
                {
                    GXColor color;
                    u8 alpha;

                    color =
                        q2gx_palette[
                            value
                        ];

                    if (value == 255)
                    {
                        alpha =
                            0u;

                        has_alpha =
                            true;
                    }
                    else
                    {
                        alpha =
                            255u;
                    }

                    Q2GX_WriteRGBA8Texel(
                        (u8 *)texture_data,
                        width,
                        x,
                        y,
                        color.r,
                        color.g,
                        color.b,
                        alpha
                    );
                }

                ++x;
            }
        }
    }

    DCStoreRange(
        texture_data,
        texture_bytes
    );

    GX_InitTexObj(
        &image->texture,
        texture_data,
        (u16)width,
        (u16)height,
        GX_TF_RGBA8,
        GX_CLAMP,
        GX_CLAMP,
        GX_FALSE
    );

    /*
     * Exact indexed UI artwork first.
     *
     * Nearest also prevents transparent-index RGB from
     * bleeding into neighboring pixels.
     */
    GX_InitTexObjFilterMode(
        &image->texture,
        GX_NEAR,
        GX_NEAR
    );

    memcpy(
        image->name,
        normalized,
        strlen(normalized) + 1
    );

    image->width =
        (int)width;

    image->height =
        (int)height;

    image->texture_bytes =
        texture_bytes;

    image->texture_data =
        texture_data;

    image->has_alpha =
        has_alpha;

    ++q2gx_pic_loads_window;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX PIC LOAD: "
        "%s %ux%u bytes=%u alpha=%d\n",
        normalized,
        width,
        height,
        (unsigned int)
            texture_bytes,
        has_alpha ? 1 : 0
    );

    ri.FS_FreeFile(
        file_data
    );

    return true;


fail:

    if (texture_data)
    {
        free(
            texture_data
        );
    }

    if (file_data)
    {
        ri.FS_FreeFile(
            file_data
        );
    }

    memset(
        image,
        0,
        sizeof(*image)
    );

    return false;
}


static struct image_s *Q2GX_FindPic(
    char *name)
{
    char normalized[
        MAX_QPATH
    ];

    struct image_s *image;

    if (
        !Q2GX_NormalizePicName(
            name,
            normalized
        )
    )
    {
        return NULL;
    }

    image =
        Q2GX_FindCachedPic(
            normalized
        );

    if (image)
    {
        ++q2gx_pic_hits_window;

        return image;
    }

    ++q2gx_pic_misses_window;

    image =
        Q2GX_FindFreePicSlot();

    if (!image)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC: "
            "cache full while loading %s\n",
            normalized
        );

        return NULL;
    }

    if (
        !Q2GX_LoadPCXPic(
            image,
            normalized
        )
    )
    {
        return NULL;
    }

    return image;
}


static void Q2GX_ClearPicCache(void)
{
    unsigned int index;

    unsigned int count;
    unsigned int bytes;

    count =
        0u;

    bytes =
        0u;

    for (index = 0;
         index < Q2GX_MAX_PICS;
         ++index)
    {
        struct image_s *image;

        image =
            &q2gx_pics[index];

        if (image->texture_data)
        {
            bytes +=
                image->texture_bytes;

            free(
                image->texture_data
            );

            ++count;
        }

        memset(
            image,
            0,
            sizeof(*image)
        );
    }

    if (count)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC CACHE FREE: "
            "images=%u bytes=%u\n",
            count,
            bytes
        );
    }

    q2gx_drawchar_atlas =
        NULL;

    q2gx_drawchar_atlas_logged =
        false;

    q2gx_drawchar_atlas_failed_logged =
        false;
}


static qboolean Q2GX_Init(
    void *hinstance,
    void *wndproc)
{
    f32 yscale;
    u32 xfb_height;

    (void)hinstance;
    (void)wndproc;

    if (q2gx_gx_initialized)
        return true;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: bootstrap V1c init begin\n"
    );

    if (!Q2GX_LoadBasePalette())
    {
        return (qboolean)-1;
    }

    if (!Q2GX_LoadConchars())
    {
        return (qboolean)-1;
    }

    VIDEO_Init();

    q2gx_mode =
        VIDEO_GetPreferredMode(NULL);

    if (!q2gx_mode)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX: VIDEO_GetPreferredMode failed\n"
        );

        Q2GX_FreeResources();

        return (qboolean)-1;
    }

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: mode fb=%u efb=%u xfb=%u vi=%ux%u\n",
        (unsigned int)q2gx_mode->fbWidth,
        (unsigned int)q2gx_mode->efbHeight,
        (unsigned int)q2gx_mode->xfbHeight,
        (unsigned int)q2gx_mode->viWidth,
        (unsigned int)q2gx_mode->viHeight
    );

    q2gx_xfb[0] =
        SYS_AllocateFramebuffer(
            q2gx_mode
        );

    q2gx_xfb[1] =
        SYS_AllocateFramebuffer(
            q2gx_mode
        );

    if (!q2gx_xfb[0] ||
        !q2gx_xfb[1])
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX: XFB allocation failed\n"
        );

        Q2GX_FreeResources();

        return (qboolean)-1;
    }

    VIDEO_ClearFrameBuffer(
        q2gx_mode,
        q2gx_xfb[0],
        COLOR_BLACK
    );

    VIDEO_ClearFrameBuffer(
        q2gx_mode,
        q2gx_xfb[1],
        COLOR_BLACK
    );

    VIDEO_Configure(
        q2gx_mode
    );

    q2gx_video_configured = true;

    VIDEO_SetNextFramebuffer(
        q2gx_xfb[0]
    );

    VIDEO_SetBlack(false);

    VIDEO_Flush();
    VIDEO_WaitForFlush();

    q2gx_fifo =
        memalign(
            32,
            Q2GX_FIFO_SIZE
        );

    if (!q2gx_fifo)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX: FIFO allocation failed\n"
        );

        Q2GX_FreeResources();

        return (qboolean)-1;
    }

    memset(
        q2gx_fifo,
        0,
        Q2GX_FIFO_SIZE
    );

    GX_Init(
        q2gx_fifo,
        Q2GX_FIFO_SIZE
    );

    q2gx_gx_initialized = true;

    GX_SetCopyClear(
        q2gx_clear_color,
        GX_MAX_Z24
    );

    GX_SetViewport(
        0.0f,
        0.0f,
        (f32)q2gx_mode->fbWidth,
        (f32)q2gx_mode->efbHeight,
        0.0f,
        1.0f
    );

    yscale =
        GX_GetYScaleFactor(
            q2gx_mode->efbHeight,
            q2gx_mode->xfbHeight
        );

    xfb_height =
        GX_SetDispCopyYScale(
            yscale
        );

    GX_SetScissor(
        0,
        0,
        q2gx_mode->fbWidth,
        q2gx_mode->efbHeight
    );

    GX_SetDispCopySrc(
        0,
        0,
        q2gx_mode->fbWidth,
        q2gx_mode->efbHeight
    );

    GX_SetDispCopyDst(
        q2gx_mode->fbWidth,
        xfb_height
    );

    GX_SetCopyFilter(
        q2gx_mode->aa,
        q2gx_mode->sample_pattern,
        GX_TRUE,
        q2gx_mode->vfilter
    );

    GX_SetFieldMode(
        q2gx_mode->field_rendering,
        (
            q2gx_mode->viHeight ==
            2 * q2gx_mode->xfbHeight
        )
            ? GX_ENABLE
            : GX_DISABLE
    );

    GX_SetDispCopyGamma(
        GX_GM_1_0
    );

    GX_SetCullMode(
        GX_CULL_NONE
    );

    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_TRUE
    );

    GX_SetColorUpdate(
        GX_TRUE
    );


    Q2GX_Setup2D();

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX DRAWFILL: "
        "palette-correct GX 2D initialized\n"
    );

    /*
     * Copy #1 discards initial EFB contents and clears the EFB
     * to the configured magenta clear color.
     */
    GX_CopyDisp(
        q2gx_xfb[0],
        GX_TRUE
    );

    GX_DrawDone();

    /*
     * Copy #2 copies the now-known-magenta EFB into XFB 0.
     */
    GX_CopyDisp(
        q2gx_xfb[0],
        GX_FALSE
    );

    GX_DrawDone();

    VIDEO_SetNextFramebuffer(
        q2gx_xfb[0]
    );

    VIDEO_Flush();
    VIDEO_WaitForFlush();
    VIDEO_WaitVSync();

    q2gx_draw_buffer = 1u;

    ri.Vid_NewWindow(
        640,
        480
    );

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: native GX initialized\n"
    );

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: EXPECT SOLID MAGENTA OUTPUT\n"
    );

    return true;
}


static void Q2GX_Shutdown(void)
{
    Q2GX_ClearPicCache();

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: shutdown\n"
    );

    Q2GX_FreeResources();
}


static void Q2GX_BeginRegistration(
    char *map)
{
    (void)map;
}


static struct model_s *Q2GX_RegisterModel(
    char *name)
{
    (void)name;
    return NULL;
}


static struct image_s *Q2GX_RegisterSkin(
    char *name)
{
    (void)name;
    return NULL;
}


static struct image_s *Q2GX_RegisterPic(
    char *name)
{
    return Q2GX_FindPic(
        name
    );
}


static void Q2GX_SetSky(
    char *name,
    float rotate,
    vec3_t axis)
{
    (void)name;
    (void)rotate;
    (void)axis;
}


static void Q2GX_EndRegistration(void)
{
}


static void Q2GX_RenderFrame(
    refdef_t *fd)
{
    (void)fd;
}


static void Q2GX_DrawGetPicSize(
    int *w,
    int *h,
    char *name)
{
    struct image_s *image;

    image =
        Q2GX_FindPic(
            name
        );

    if (!image)
    {
        if (w)
            *w = -1;

        if (h)
            *h = -1;

        return;
    }

    if (w)
        *w = image->width;

    if (h)
        *h = image->height;
}


static void Q2GX_DrawPic(
    int x,
    int y,
    char *name)
{
    struct image_s *image;

    image =
        Q2GX_FindPic(
            name
        );

    if (!image)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Can't find pic: %s\n",
            name ? name : "(null)"
        );

        return;
    }

    ++q2gx_pic_draws_window;

    if (!q2gx_pic_first_draw_logged)
    {
        q2gx_pic_first_draw_logged =
            true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC FIRST DRAW: "
            "DrawPic %s %dx%d at %d,%d\n",
            image->name,
            image->width,
            image->height,
            x,
            y
        );
    }

    Q2GX_DrawTexturedQuad(
        (f32)x,
        (f32)y,
        (f32)image->width,
        (f32)image->height,
        &image->texture
    );
}


static void Q2GX_DrawStretchPic(
    int x,
    int y,
    int w,
    int h,
    char *name)
{
    struct image_s *image;

    if (
        w <= 0 ||
        h <= 0
    )
    {
        return;
    }

    image =
        Q2GX_FindPic(
            name
        );

    if (!image)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Can't find pic: %s\n",
            name ? name : "(null)"
        );

        return;
    }

    ++q2gx_stretchpic_draws_window;

    if (!q2gx_pic_first_draw_logged)
    {
        q2gx_pic_first_draw_logged =
            true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PIC FIRST DRAW: "
            "DrawStretchPic %s "
            "%dx%d -> %dx%d at %d,%d\n",
            image->name,
            image->width,
            image->height,
            w,
            h,
            x,
            y
        );
    }

    Q2GX_DrawTexturedQuad(
        (f32)x,
        (f32)y,
        (f32)w,
        (f32)h,
        &image->texture
    );
}


static void Q2GX_DrawChar(
    int x,
    int y,
    int c)
{
    int glyph;

    int row;
    int column;

    f32 s0;
    f32 t0;
    f32 s1;
    f32 t1;

    glyph =
        c & 255;

    ++q2gx_drawchar_calls_window;

    /*
     * Stock Quake II Draw_Char semantics.
     *
     * Both normal and high-bit spaces are blank.
     */
    if (
        (glyph & 127) == 32
    )
    {
        return;
    }

    /*
     * Stock renderer rejects characters wholly above the
     * viewport. Other clipping is handled by GX.
     */
    if (y <= -8)
    {
        return;
    }

    /*
     * Reuse the exact PCX -> palette -> RGBA8 -> GXTexObj
     * cache proven by DrawPic.
     *
     * Leading slash means "use this exact virtual path".
     */
    if (!q2gx_drawchar_atlas)
    {
        q2gx_drawchar_atlas =
            Q2GX_FindPic(
                "/pics/conchars.pcx"
            );

        if (!q2gx_drawchar_atlas)
        {
            if (
                !q2gx_drawchar_atlas_failed_logged
            )
            {
                q2gx_drawchar_atlas_failed_logged =
                    true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX DRAWCHAR ATLAS: "
                    "failed to load pics/conchars.pcx\n"
                );
            }

            return;
        }

        if (
            q2gx_drawchar_atlas->width != 128 ||
            q2gx_drawchar_atlas->height != 128
        )
        {
            if (
                !q2gx_drawchar_atlas_failed_logged
            )
            {
                q2gx_drawchar_atlas_failed_logged =
                    true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX DRAWCHAR ATLAS: "
                    "unexpected dimensions %dx%d\n",
                    q2gx_drawchar_atlas->width,
                    q2gx_drawchar_atlas->height
                );
            }

            q2gx_drawchar_atlas =
                NULL;

            return;
        }

        if (!q2gx_drawchar_atlas_logged)
        {
            q2gx_drawchar_atlas_logged =
                true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX DRAWCHAR ATLAS: "
                "pics/conchars.pcx "
                "128x128 cells=16x16 glyph=8x8 "
                "textured=1\n"
            );
        }
    }

    row =
        glyph >> 4;

    column =
        glyph & 15;

    /*
     * 128x128 atlas:
     *
     *     16 columns
     *     16 rows
     *      8x8 texels per glyph
     *
     * Therefore one cell is exactly 1/16 of the texture.
     */
    s0 =
        (f32)column /
        16.0f;

    t0 =
        (f32)row /
        16.0f;

    s1 =
        (f32)(column + 1) /
        16.0f;

    t1 =
        (f32)(row + 1) /
        16.0f;

    ++q2gx_drawchar_quads_window;

    if (!q2gx_drawchar_first_logged)
    {
        q2gx_drawchar_first_logged =
            true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX DRAWCHAR FIRST: "
            "c=%d x=%d y=%d textured=1\n",
            glyph,
            x,
            y
        );
    }

    Q2GX_DrawTexturedQuadUV(
        (f32)x,
        (f32)y,
        8.0f,
        8.0f,
        s0,
        t0,
        s1,
        t1,
        &q2gx_drawchar_atlas->texture
    );
}


static void Q2GX_DrawTileClear(
    int x,
    int y,
    int w,
    int h,
    char *name)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)name;
}


static void Q2GX_DrawFill(
    int x,
    int y,
    int w,
    int h,
    int c)
{
    int right;
    int bottom;

    if ((unsigned int)c > 255u)
    {
        ri.Sys_Error(
            ERR_FATAL,
            "Q2GX_DrawFill: bad color"
        );

        return;
    }

    if (!q2gx_palette_loaded ||
        w <= 0 ||
        h <= 0)
    {
        return;
    }

    ++q2gx_drawfill_calls_window;

    right =
        x + w;

    bottom =
        y + h;

    if (right <= 0 ||
        bottom <= 0 ||
        x >= 640 ||
        y >= 480)
    {
        return;
    }

    if (x < 0)
        x = 0;

    if (y < 0)
        y = 0;

    if (right > 640)
        right = 640;

    if (bottom > 480)
        bottom = 480;

    w =
        right - x;

    h =
        bottom - y;

    Q2GX_DrawSolidRect(
        (f32)x,
        (f32)y,
        (f32)w,
        (f32)h,
        q2gx_palette[c]
    );
}


static void Q2GX_DrawFadeScreen(void)
{
}


static void Q2GX_DrawStretchRaw(
    int x,
    int y,
    int w,
    int h,
    int cols,
    int rows,
    byte *data)
{
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    (void)cols;
    (void)rows;
    (void)data;
}


static void Q2GX_CinematicSetPalette(
    const unsigned char *palette)
{
    (void)palette;
}


static void Q2GX_BeginFrame(
    float camera_separation)
{
    (void)camera_separation;

    if (!q2gx_gx_initialized)
        return;

    GX_SetViewport(
        0.0f,
        0.0f,
        (f32)q2gx_mode->fbWidth,
        (f32)q2gx_mode->efbHeight,
        0.0f,
        1.0f
    );


    }


static void Q2GX_EndFrame(void)
{
    if (!q2gx_gx_initialized)
        return;

    GX_CopyDisp(
        q2gx_xfb[q2gx_draw_buffer],
        GX_TRUE
    );

    GX_DrawDone();

    VIDEO_SetNextFramebuffer(
        q2gx_xfb[q2gx_draw_buffer]
    );

    VIDEO_Flush();
    VIDEO_WaitVSync();

    q2gx_draw_buffer ^= 1u;

    ++q2gx_frame_count;

    if (q2gx_frame_count >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX FRAME 120: native GX presentation alive\n"
        );

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX 2D 120: "
            "drawchar_calls=%u "
            "drawchar_runs=%u "
            "drawchar_quads=%u "
            "drawfill_calls=%u "
            "texture_draws=%u "
            "pic_draws=%u "
            "stretchpic_draws=%u "
            "pic_loads=%u "
            "pic_hits=%u "
            "pic_misses=%u\n",
            q2gx_drawchar_calls_window,
            q2gx_drawchar_runs_window,
            q2gx_drawchar_quads_window,
            q2gx_drawfill_calls_window,
            q2gx_texture_draws_window,
            q2gx_pic_draws_window,
            q2gx_stretchpic_draws_window,
            q2gx_pic_loads_window,
            q2gx_pic_hits_window,
            q2gx_pic_misses_window
        );

        q2gx_drawchar_calls_window =
            0u;

        q2gx_drawchar_runs_window =
            0u;

        q2gx_drawchar_quads_window =
            0u;

        q2gx_drawfill_calls_window =
            0u;

        q2gx_texture_draws_window =
            0u;

        q2gx_pic_draws_window =
            0u;

        q2gx_stretchpic_draws_window =
            0u;

        q2gx_pic_loads_window =
            0u;

        q2gx_pic_hits_window =
            0u;

        q2gx_pic_misses_window =
            0u;

        q2gx_frame_count =
            0u;
    }
}


static void Q2GX_AppActivate(
    qboolean activate)
{
    (void)activate;
}


refexport_t GetRefAPI(
    refimport_t rimp)
{
    refexport_t re;

    /*
     * Renderer-global import table.
     *
     * This deliberately mirrors ref_soft/ref_gl because the
     * current GameCube platform/input bridge also consumes ri.
     */
    ri = rimp;

    memset(
        &re,
        0,
        sizeof(re)
    );

    re.api_version =
        API_VERSION;

    re.Init =
        Q2GX_Init;

    re.Shutdown =
        Q2GX_Shutdown;

    re.BeginRegistration =
        Q2GX_BeginRegistration;

    re.RegisterModel =
        Q2GX_RegisterModel;

    re.RegisterSkin =
        Q2GX_RegisterSkin;

    re.RegisterPic =
        Q2GX_RegisterPic;

    re.SetSky =
        Q2GX_SetSky;

    re.EndRegistration =
        Q2GX_EndRegistration;

    re.RenderFrame =
        Q2GX_RenderFrame;

    re.DrawGetPicSize =
        Q2GX_DrawGetPicSize;

    re.DrawPic =
        Q2GX_DrawPic;

    re.DrawStretchPic =
        Q2GX_DrawStretchPic;

    re.DrawChar =
        Q2GX_DrawChar;

    re.DrawTileClear =
        Q2GX_DrawTileClear;

    re.DrawFill =
        Q2GX_DrawFill;

    re.DrawFadeScreen =
        Q2GX_DrawFadeScreen;

    re.DrawStretchRaw =
        Q2GX_DrawStretchRaw;

    re.CinematicSetPalette =
        Q2GX_CinematicSetPalette;

    re.BeginFrame =
        Q2GX_BeginFrame;

    re.EndFrame =
        Q2GX_EndFrame;

    re.AppActivate =
        Q2GX_AppActivate;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX: GetRefAPI API %d\n",
        API_VERSION
    );

    return re;
}
