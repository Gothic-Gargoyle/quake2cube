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


/*
 * Q2GC_FLAT_BSP_WORLD_V1
 *
 * Correctness-first native GX world proof.
 *
 * V1 parses only:
 *
 *     LUMP_VERTEXES
 *     LUMP_FACES
 *     LUMP_EDGES
 *     LUMP_SURFEDGES
 *
 * Every BSP face is triangulated as a fan ONCE during
 * BeginRegistration().
 *
 * RenderFrame() then submits the resulting flat-colored
 * triangle list with depth testing.
 *
 * Deliberately absent in V1:
 *
 *     PVS
 *     BSP traversal
 *     frustum culling
 *     backface culling
 *     textures
 *     lightmaps
 *     special sky/water behavior
 */
typedef struct q2gx_world_vertex_s
{
    f32 x;
    f32 y;
    f32 z;

    u8 r;
    u8 g;
    u8 b;
    u8 a;
} q2gx_world_vertex_t;


/*
 * Q2GC_BSP_BACKFACE_REJECTION_V1
 *
 * Preserve the BSP plane and face.side semantics required by
 * Quake II's world-facing rule.
 *
 * visible_this_frame is transient scratch state used so the
 * first pass can count the exact GX primitive size and the
 * second pass can stream only accepted faces.
 */
/*
 * Q2GC_BSP_PVS_FACE_MARKING_V1
 *
 * Minimal Quake II BSP visibility data. V1 deliberately does
 * not reproduce recursive world rendering yet; it only marks
 * faces belonging to leaves in the current PVS. Existing
 * plane-side rejection then decides which marked faces face
 * the viewer.
 */
typedef struct q2gx_world_node_s
{
    f32 normal[3];
    f32 dist;
    int type;
    int children[2];
} q2gx_world_node_t;

typedef struct q2gx_world_leaf_s
{
    int contents;
    int cluster;

    /*
     * Q2GC_BSP_AREABITS_FILTERING_V1
     *
     * Quake II renderer area ID from dleaf.area.
     */
    int area;
    unsigned int first_leafface;
    unsigned int num_leaffaces;
} q2gx_world_leaf_t;

typedef struct q2gx_world_face_s
{
    unsigned int first_vertex;
    unsigned int vertex_count;
    unsigned int triangle_count;

    f32 normal[
        3
    ];

    f32 dist;

    qboolean plane_back;
    qboolean pvs_visible_this_frame;
    qboolean visible_this_frame;
} q2gx_world_face_t;


static q2gx_world_face_t *q2gx_world_faces;

static q2gx_world_node_t *q2gx_world_nodes;
static q2gx_world_leaf_t *q2gx_world_leaves;
static uint16_t *q2gx_world_leaffaces;
static byte *q2gx_world_vis_data;
static int32_t *q2gx_world_vis_pvs_offsets;
static byte *q2gx_world_pvs_row;
static byte *q2gx_world_pvs_row2;
static unsigned int q2gx_world_node_count;
static unsigned int q2gx_world_leaf_count;
static unsigned int q2gx_world_leafface_count;
static unsigned int q2gx_world_vis_bytes;
static unsigned int q2gx_world_vis_numclusters;
static unsigned int q2gx_world_vis_row_bytes;

static q2gx_world_vertex_t *q2gx_world_vertices;

static unsigned int q2gx_world_vertex_count;
static unsigned int q2gx_world_triangle_count;
static unsigned int q2gx_world_face_count;
static unsigned int q2gx_world_plane_count;

static size_t q2gx_world_bytes;
static size_t q2gx_world_face_bytes;

static char q2gx_world_name[
    MAX_QPATH
];

static qboolean q2gx_world_first_draw_logged;

static unsigned int q2gx_world_frames_window;

static unsigned int q2gx_world_pvs_faces_window;
static unsigned int q2gx_world_pvs_rejected_faces_window;
static unsigned int q2gx_world_backface_rejected_faces_window;

static unsigned int q2gx_world_pvs_visible_leaves_frame;
static unsigned int q2gx_world_area_rejected_leaves_frame;
static unsigned int q2gx_world_pvs_face_refs_frame;
static unsigned int q2gx_world_area_rejected_face_refs_frame;
static qboolean q2gx_world_areabits_active_frame;

static unsigned int q2gx_world_pvs_visible_leaves_window;
static unsigned int q2gx_world_area_rejected_leaves_window;
static unsigned int q2gx_world_pvs_face_refs_window;
static unsigned int q2gx_world_area_rejected_face_refs_window;
static unsigned int q2gx_world_areabits_frames_window;
static unsigned int q2gx_world_submitted_faces_window;
static unsigned int q2gx_world_submitted_triangles_window;
static unsigned int q2gx_world_submitted_vertices_window;
static unsigned int q2gx_world_pvs_fallback_frames_window;
static unsigned int q2gx_world_cluster_changes_window;
static int q2gx_world_last_cluster;
static int q2gx_world_last_cluster2;


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



#define Q2GX_BSP_VERSION          38
#define Q2GX_BSP_HEADER_LUMPS     19

#define Q2GX_BSP_LUMP_PLANES       1
#define Q2GX_BSP_LUMP_VERTEXES     2
#define Q2GX_BSP_LUMP_VISIBILITY   3
#define Q2GX_BSP_LUMP_NODES        4
#define Q2GX_BSP_LUMP_FACES        6
#define Q2GX_BSP_LUMP_LEAFS        8
#define Q2GX_BSP_LUMP_LEAFFACES    9
#define Q2GX_BSP_LUMP_EDGES       11
#define Q2GX_BSP_LUMP_SURFEDGES   12


static uint16_t Q2GX_BSPReadLE16(
    const byte *source)
{
    return
        (uint16_t)source[0]
        |
        (
            (uint16_t)source[1]
            <<
            8
        );
}


static uint32_t Q2GX_BSPReadLE32(
    const byte *source)
{
    return
        (uint32_t)source[0]
        |
        (
            (uint32_t)source[1]
            <<
            8
        )
        |
        (
            (uint32_t)source[2]
            <<
            16
        )
        |
        (
            (uint32_t)source[3]
            <<
            24
        );
}


static f32 Q2GX_BSPReadLEFloat(
    const byte *source)
{
    uint32_t bits;
    f32 value;

    bits =
        Q2GX_BSPReadLE32(
            source
        );

    memcpy(
        &value,
        &bits,
        sizeof(value)
    );

    return value;
}


static qboolean Q2GX_BSPGetLump(
    const byte *file_data,
    unsigned int file_length,
    unsigned int lump_index,
    unsigned int stride,
    const byte **data_out,
    unsigned int *bytes_out,
    unsigned int *count_out)
{
    const byte *lump;

    uint32_t file_offset;
    uint32_t file_bytes;

    if (
        !file_data ||
        !data_out ||
        !bytes_out ||
        !count_out ||
        lump_index >= Q2GX_BSP_HEADER_LUMPS ||
        stride == 0
    )
    {
        return false;
    }

    lump =
        file_data
        +
        8u
        +
        lump_index * 8u;

    file_offset =
        Q2GX_BSPReadLE32(
            lump + 0
        );

    file_bytes =
        Q2GX_BSPReadLE32(
            lump + 4
        );

    if (
        file_offset > file_length ||
        file_bytes > (
            file_length - file_offset
        )
    )
    {
        return false;
    }

    if (
        file_bytes % stride
    )
    {
        return false;
    }

    *data_out =
        file_data + file_offset;

    *bytes_out =
        file_bytes;

    *count_out =
        file_bytes / stride;

    return true;
}


static qboolean Q2GX_BSPResolveVertex(
    const byte *vertex_data,
    unsigned int vertex_count,
    const byte *edge_data,
    unsigned int edge_count,
    const byte *surfedge_data,
    unsigned int surfedge_count,
    unsigned int firstedge,
    unsigned int local_edge,
    f32 position[3])
{
    unsigned int surfedge_index;

    int32_t surfedge;

    uint32_t edge_index;

    unsigned int edge_vertex_offset;
    uint16_t vertex_index;

    const byte *vertex;

    surfedge_index =
        firstedge
        +
        local_edge;

    if (
        surfedge_index >=
        surfedge_count
    )
    {
        return false;
    }

    surfedge =
        (int32_t)
        Q2GX_BSPReadLE32(
            surfedge_data
            +
            surfedge_index * 4u
        );

    if (
        surfedge == INT32_MIN
    )
    {
        return false;
    }

    /*
     * Match Quake II GL_BuildPolygonFromSurface exactly:
     *
     *     surfedge > 0
     *         edge.v[0]
     *
     *     surfedge <= 0
     *         edge.v[1]
     */
    if (surfedge > 0)
    {
        edge_index =
            (uint32_t)surfedge;

        edge_vertex_offset =
            0u;
    }
    else
    {
        edge_index =
            (uint32_t)(-surfedge);

        edge_vertex_offset =
            2u;
    }

    if (
        edge_index >= edge_count
    )
    {
        return false;
    }

    vertex_index =
        Q2GX_BSPReadLE16(
            edge_data
            +
            edge_index * 4u
            +
            edge_vertex_offset
        );

    if (
        vertex_index >= vertex_count
    )
    {
        return false;
    }

    vertex =
        vertex_data
        +
        (unsigned int)vertex_index * 12u;

    position[0] =
        Q2GX_BSPReadLEFloat(
            vertex + 0
        );

    position[1] =
        Q2GX_BSPReadLEFloat(
            vertex + 4
        );

    position[2] =
        Q2GX_BSPReadLEFloat(
            vertex + 8
        );

    return true;
}


static void Q2GX_FreeWorldGeometry(void)
{
    if (
        q2gx_world_vertices ||
        q2gx_world_faces ||
        q2gx_world_nodes ||
        q2gx_world_leaves ||
        q2gx_world_leaffaces ||
        q2gx_world_vis_data)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX WORLD FREE: "
            "%s planes=%u faces=%u triangles=%u vertices=%u "
            "nodes=%u leaves=%u leaffaces=%u clusters=%u "
            "vertex_bytes=%u face_bytes=%u vis_bytes=%u\n",
            q2gx_world_name[0] ? q2gx_world_name : "(unnamed)",
            q2gx_world_plane_count,
            q2gx_world_face_count,
            q2gx_world_triangle_count,
            q2gx_world_vertex_count,
            q2gx_world_node_count,
            q2gx_world_leaf_count,
            q2gx_world_leafface_count,
            q2gx_world_vis_numclusters,
            (unsigned int)q2gx_world_bytes,
            (unsigned int)q2gx_world_face_bytes,
            q2gx_world_vis_bytes);
    }

    if (q2gx_world_vertices) free(q2gx_world_vertices);
    if (q2gx_world_faces) free(q2gx_world_faces);
    if (q2gx_world_nodes) free(q2gx_world_nodes);
    if (q2gx_world_leaves) free(q2gx_world_leaves);
    if (q2gx_world_leaffaces) free(q2gx_world_leaffaces);
    if (q2gx_world_vis_data) free(q2gx_world_vis_data);
    if (q2gx_world_vis_pvs_offsets) free(q2gx_world_vis_pvs_offsets);
    if (q2gx_world_pvs_row) free(q2gx_world_pvs_row);
    if (q2gx_world_pvs_row2) free(q2gx_world_pvs_row2);

    q2gx_world_vertices = NULL;
    q2gx_world_faces = NULL;
    q2gx_world_nodes = NULL;
    q2gx_world_leaves = NULL;
    q2gx_world_leaffaces = NULL;
    q2gx_world_vis_data = NULL;
    q2gx_world_vis_pvs_offsets = NULL;
    q2gx_world_pvs_row = NULL;
    q2gx_world_pvs_row2 = NULL;

    q2gx_world_vertex_count = 0u;
    q2gx_world_triangle_count = 0u;
    q2gx_world_face_count = 0u;
    q2gx_world_plane_count = 0u;
    q2gx_world_node_count = 0u;
    q2gx_world_leaf_count = 0u;
    q2gx_world_leafface_count = 0u;
    q2gx_world_vis_bytes = 0u;
    q2gx_world_vis_numclusters = 0u;
    q2gx_world_vis_row_bytes = 0u;
    q2gx_world_bytes = 0u;
    q2gx_world_face_bytes = 0u;
    q2gx_world_name[0] = '\0';
    q2gx_world_first_draw_logged = false;
    q2gx_world_frames_window = 0u;
    q2gx_world_pvs_faces_window = 0u;
    q2gx_world_pvs_rejected_faces_window = 0u;
    q2gx_world_backface_rejected_faces_window = 0u;
    q2gx_world_submitted_faces_window = 0u;
    q2gx_world_submitted_triangles_window = 0u;
    q2gx_world_submitted_vertices_window = 0u;
    q2gx_world_pvs_fallback_frames_window = 0u;
    q2gx_world_cluster_changes_window = 0u;
    q2gx_world_last_cluster = INT32_MIN;
    q2gx_world_last_cluster2 = INT32_MIN;


    q2gx_world_pvs_visible_leaves_frame = 0u;
    q2gx_world_area_rejected_leaves_frame = 0u;
    q2gx_world_pvs_face_refs_frame = 0u;
    q2gx_world_area_rejected_face_refs_frame = 0u;
    q2gx_world_areabits_active_frame = false;

    q2gx_world_pvs_visible_leaves_window = 0u;
    q2gx_world_area_rejected_leaves_window = 0u;
    q2gx_world_pvs_face_refs_window = 0u;
    q2gx_world_area_rejected_face_refs_window = 0u;
    q2gx_world_areabits_frames_window = 0u;
}


static qboolean Q2GX_LoadWorldGeometry(
    const char *map)
{
    char path[MAX_QPATH];
    size_t map_length;
    void *file_buffer;
    byte *file_data;
    int file_length_int;
    unsigned int file_length;

    const byte *plane_data;
    const byte *vertex_data;
    const byte *visibility_data;
    const byte *node_data;
    const byte *face_data;
    const byte *leaf_data;
    const byte *leafface_data;
    const byte *edge_data;
    const byte *surfedge_data;

    unsigned int plane_bytes, vertex_bytes, visibility_bytes, node_bytes;
    unsigned int face_bytes, leaf_bytes, leafface_bytes, edge_bytes, surfedge_bytes;
    unsigned int plane_count, vertex_count, visibility_count, node_count;
    unsigned int face_count, leaf_count, leafface_count, edge_count, surfedge_count;
    unsigned int vis_numclusters = 0u;
    unsigned int vis_row_bytes = 0u;
    unsigned int triangle_count;
    unsigned int gx_vertex_count;

    size_t allocation_bytes, face_allocation_bytes;
    size_t node_allocation_bytes, leaf_allocation_bytes;
    size_t leafface_allocation_bytes, vis_offset_allocation_bytes = 0u;

    q2gx_world_vertex_t *world_vertices = NULL;
    q2gx_world_face_t *world_faces = NULL;
    q2gx_world_node_t *world_nodes = NULL;
    q2gx_world_leaf_t *world_leaves = NULL;
    uint16_t *world_leaffaces = NULL;
    byte *world_vis_data = NULL;
    int32_t *world_vis_pvs_offsets = NULL;
    byte *world_pvs_row = NULL;
    byte *world_pvs_row2 = NULL;

    unsigned int face_index;
    unsigned int output_index;
    unsigned int index;

    Q2GX_FreeWorldGeometry();

    if (!map || !map[0])
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: BeginRegistration received empty map\n");
        return false;
    }

    map_length = strlen(map);
    if (map_length + 10u > sizeof(path))
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: map name too long: %s\n", map);
        return false;
    }

    memcpy(path, "maps/", 5);
    memcpy(path + 5, map, map_length);
    memcpy(path + 5 + map_length, ".bsp", 5);

    file_buffer = NULL;
    file_length_int = ri.FS_LoadFile(path, &file_buffer);

    if (file_length_int <= 0 || !file_buffer)
    {
        if (file_buffer) ri.FS_FreeFile(file_buffer);
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: failed to load %s\n", path);
        return false;
    }

    file_data = (byte *)file_buffer;
    file_length = (unsigned int)file_length_int;

    if (file_length < 8u + Q2GX_BSP_HEADER_LUMPS * 8u)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: short BSP header %s\n", path);
        goto fail;
    }

    if (file_data[0] != 'I' || file_data[1] != 'B' ||
        file_data[2] != 'S' || file_data[3] != 'P')
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: bad BSP magic %s\n", path);
        goto fail;
    }

    if (Q2GX_BSPReadLE32(file_data + 4) != Q2GX_BSP_VERSION)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX WORLD: unsupported BSP version %u in %s\n",
            (unsigned int)Q2GX_BSPReadLE32(file_data + 4),
            path);
        goto fail;
    }

    if (!Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_PLANES, 20u,
                         &plane_data, &plane_bytes, &plane_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_VERTEXES, 12u,
                         &vertex_data, &vertex_bytes, &vertex_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_VISIBILITY, 1u,
                         &visibility_data, &visibility_bytes, &visibility_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_NODES, 28u,
                         &node_data, &node_bytes, &node_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_FACES, 20u,
                         &face_data, &face_bytes, &face_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_LEAFS, 28u,
                         &leaf_data, &leaf_bytes, &leaf_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_LEAFFACES, 2u,
                         &leafface_data, &leafface_bytes, &leafface_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_EDGES, 4u,
                         &edge_data, &edge_bytes, &edge_count) ||
        !Q2GX_BSPGetLump(file_data, file_length, Q2GX_BSP_LUMP_SURFEDGES, 4u,
                         &surfedge_data, &surfedge_bytes, &surfedge_count))
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: invalid BSP lumps in %s\n", path);
        goto fail;
    }

    (void)visibility_count;
    (void)plane_bytes;
    (void)vertex_bytes;
    (void)node_bytes;
    (void)face_bytes;
    (void)leaf_bytes;
    (void)leafface_bytes;
    (void)edge_bytes;
    (void)surfedge_bytes;

    if (plane_count == 0u || vertex_count == 0u || node_count == 0u ||
        face_count == 0u || leaf_count == 0u || leafface_count == 0u ||
        edge_count == 0u || surfedge_count == 0u)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: empty required BSP lumps in %s\n", path);
        goto fail;
    }

    if (visibility_bytes >= 4u)
    {
        uint32_t numclusters_u32 = Q2GX_BSPReadLE32(visibility_data);
        size_t header_bytes;

        if (numclusters_u32 > 32767u)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: implausible cluster count %u\n",
                          (unsigned int)numclusters_u32);
            goto fail;
        }

        vis_numclusters = (unsigned int)numclusters_u32;
        header_bytes = 4u + (size_t)vis_numclusters * 8u;
        if (header_bytes > (size_t)visibility_bytes)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: visibility header exceeds lump\n");
            goto fail;
        }
        vis_row_bytes = (vis_numclusters + 7u) >> 3;
    }

    triangle_count = 0u;
    for (face_index = 0u; face_index < face_count; ++face_index)
    {
        const byte *face = face_data + face_index * 20u;
        uint16_t planenum = Q2GX_BSPReadLE16(face + 0);
        int16_t side = (int16_t)Q2GX_BSPReadLE16(face + 2);
        int32_t firstedge_signed = (int32_t)Q2GX_BSPReadLE32(face + 4);
        int16_t numedges_signed = (int16_t)Q2GX_BSPReadLE16(face + 8);
        unsigned int firstedge;
        unsigned int numedges;
        unsigned int local_edge;
        f32 ignored_position[3];

        if (planenum >= plane_count || (side != 0 && side != 1) ||
            firstedge_signed < 0 || numedges_signed < 3)
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX WORLD: invalid face %u planenum=%u side=%d firstedge=%d numedges=%d\n",
                face_index, (unsigned int)planenum, (int)side,
                (int)firstedge_signed, (int)numedges_signed);
            goto fail;
        }

        firstedge = (unsigned int)firstedge_signed;
        numedges = (unsigned int)numedges_signed;

        if (firstedge > surfedge_count || numedges > surfedge_count - firstedge)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: face %u surfedge range invalid\n", face_index);
            goto fail;
        }

        for (local_edge = 0u; local_edge < numedges; ++local_edge)
        {
            if (!Q2GX_BSPResolveVertex(
                    vertex_data, vertex_count,
                    edge_data, edge_count,
                    surfedge_data, surfedge_count,
                    firstedge, local_edge,
                    ignored_position))
            {
                ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: face %u vertex %u invalid\n",
                              face_index, local_edge);
                goto fail;
            }
        }

        if (triangle_count > UINT32_MAX - (numedges - 2u))
            goto fail;

        triangle_count += numedges - 2u;
    }

    if (triangle_count > UINT32_MAX / 3u)
        goto fail;

    gx_vertex_count = triangle_count * 3u;
    if (gx_vertex_count == 0u)
        goto fail;

    if (gx_vertex_count > SIZE_MAX / sizeof(*world_vertices) ||
        face_count > SIZE_MAX / sizeof(*world_faces) ||
        node_count > SIZE_MAX / sizeof(*world_nodes) ||
        leaf_count > SIZE_MAX / sizeof(*world_leaves) ||
        leafface_count > SIZE_MAX / sizeof(*world_leaffaces))
    {
        goto fail;
    }

    allocation_bytes = (size_t)gx_vertex_count * sizeof(*world_vertices);
    face_allocation_bytes = (size_t)face_count * sizeof(*world_faces);
    node_allocation_bytes = (size_t)node_count * sizeof(*world_nodes);
    leaf_allocation_bytes = (size_t)leaf_count * sizeof(*world_leaves);
    leafface_allocation_bytes = (size_t)leafface_count * sizeof(*world_leaffaces);

    if (vis_numclusters > 0u)
    {
        if (vis_numclusters > SIZE_MAX / sizeof(*world_vis_pvs_offsets))
            goto fail;
        vis_offset_allocation_bytes =
            (size_t)vis_numclusters * sizeof(*world_vis_pvs_offsets);
    }

    world_vertices = memalign(32, allocation_bytes);
    world_faces = memalign(32, face_allocation_bytes);
    world_nodes = malloc(node_allocation_bytes);
    world_leaves = malloc(leaf_allocation_bytes);
    world_leaffaces = malloc(leafface_allocation_bytes);

    if (!world_vertices || !world_faces || !world_nodes ||
        !world_leaves || !world_leaffaces)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: allocation failure while loading %s\n", path);
        goto fail;
    }

    memset(world_faces, 0, face_allocation_bytes);

    if (visibility_bytes > 0u)
    {
        world_vis_data = malloc(visibility_bytes);
        if (!world_vis_data)
            goto fail;
        memcpy(world_vis_data, visibility_data, visibility_bytes);
    }

    if (vis_numclusters > 0u)
    {
        world_vis_pvs_offsets = malloc(vis_offset_allocation_bytes);
        world_pvs_row = malloc(vis_row_bytes);
        world_pvs_row2 = malloc(vis_row_bytes);
        if (!world_vis_pvs_offsets || !world_pvs_row || !world_pvs_row2)
            goto fail;

        for (index = 0u; index < vis_numclusters; ++index)
        {
            int32_t pvs_offset = (int32_t)Q2GX_BSPReadLE32(
                visibility_data + 4u + index * 8u);

            if (pvs_offset < -1 ||
                (pvs_offset >= 0 && (unsigned int)pvs_offset >= visibility_bytes))
            {
                ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: cluster %u invalid offset %d\n",
                              index, (int)pvs_offset);
                goto fail;
            }
            world_vis_pvs_offsets[index] = pvs_offset;
        }
    }

    for (index = 0u; index < leaf_count; ++index)
    {
        const byte *disk_leaf = leaf_data + index * 28u;
        int32_t contents = (int32_t)Q2GX_BSPReadLE32(disk_leaf + 0);
        int16_t cluster = (int16_t)Q2GX_BSPReadLE16(disk_leaf + 4);
        uint16_t first_leafface = Q2GX_BSPReadLE16(disk_leaf + 20);
        uint16_t num_leaffaces = Q2GX_BSPReadLE16(disk_leaf + 22);

        if (cluster < -1 ||
            (cluster >= 0 && vis_numclusters > 0u &&
             (unsigned int)cluster >= vis_numclusters))
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: leaf %u invalid cluster %d\n",
                          index, (int)cluster);
            goto fail;
        }

        if ((unsigned int)first_leafface > leafface_count ||
            (unsigned int)num_leaffaces >
                leafface_count - (unsigned int)first_leafface)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: leaf %u invalid leafface range\n", index);
            goto fail;
        }

        world_leaves[index].contents = (int)contents;
        world_leaves[index].cluster = (int)cluster;

        /*
         * dleaf_t layout:
         *
         *   +0  contents   int32
         *   +4  cluster    int16
         *   +6  area       int16
         */
        world_leaves[
            index
        ].area =
            (int)
            (
                (int16_t)
                Q2GX_BSPReadLE16(
                    disk_leaf + 6
                )
            );

        if (
            world_leaves[
                index
            ].area < 0
            ||
            world_leaves[
                index
            ].area >= 256
        )
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX AREABITS: "
                "leaf %u invalid area %d\n",
                index,
                world_leaves[
                    index
                ].area
            );

            goto fail;
        }

        world_leaves[index].first_leafface = (unsigned int)first_leafface;
        world_leaves[index].num_leaffaces = (unsigned int)num_leaffaces;
    }

    for (index = 0u; index < leafface_count; ++index)
    {
        uint16_t face_reference = Q2GX_BSPReadLE16(leafface_data + index * 2u);
        if (face_reference >= face_count)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: leafface %u references bad face %u\n",
                          index, (unsigned int)face_reference);
            goto fail;
        }
        world_leaffaces[index] = face_reference;
    }

    for (index = 0u; index < node_count; ++index)
    {
        const byte *disk_node = node_data + index * 28u;
        int32_t planenum = (int32_t)Q2GX_BSPReadLE32(disk_node + 0);
        int32_t child0 = (int32_t)Q2GX_BSPReadLE32(disk_node + 4);
        int32_t child1 = (int32_t)Q2GX_BSPReadLE32(disk_node + 8);
        const byte *plane;
        int32_t plane_type;
        unsigned int child_index;

        if (planenum < 0 || (unsigned int)planenum >= plane_count)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: node %u bad planenum %d\n",
                          index, (int)planenum);
            goto fail;
        }

        for (child_index = 0u; child_index < 2u; ++child_index)
        {
            int32_t child = child_index ? child1 : child0;
            if (child >= 0)
            {
                if ((unsigned int)child >= node_count)
                {
                    ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: node %u bad node child %d\n",
                                  index, (int)child);
                    goto fail;
                }
            }
            else
            {
                uint32_t leaf_index;
                if (child == INT32_MIN)
                    goto fail;
                leaf_index = (uint32_t)(-1 - child);
                if (leaf_index >= leaf_count)
                {
                    ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: node %u bad leaf child %u\n",
                                  index, (unsigned int)leaf_index);
                    goto fail;
                }
            }
        }

        plane = plane_data + (unsigned int)planenum * 20u;
        world_nodes[index].normal[0] = Q2GX_BSPReadLEFloat(plane + 0);
        world_nodes[index].normal[1] = Q2GX_BSPReadLEFloat(plane + 4);
        world_nodes[index].normal[2] = Q2GX_BSPReadLEFloat(plane + 8);
        world_nodes[index].dist = Q2GX_BSPReadLEFloat(plane + 12);
        plane_type = (int32_t)Q2GX_BSPReadLE32(plane + 16);
        world_nodes[index].type = (int)plane_type;
        world_nodes[index].children[0] = (int)child0;
        world_nodes[index].children[1] = (int)child1;
    }

    output_index = 0u;
    for (face_index = 0u; face_index < face_count; ++face_index)
    {
        const byte *face = face_data + face_index * 20u;
        uint16_t planenum = Q2GX_BSPReadLE16(face + 0);
        int16_t side = (int16_t)Q2GX_BSPReadLE16(face + 2);
        unsigned int firstedge = (unsigned int)((int32_t)Q2GX_BSPReadLE32(face + 4));
        unsigned int numedges = (unsigned int)((int16_t)Q2GX_BSPReadLE16(face + 8));
        const byte *plane = plane_data + (unsigned int)planenum * 20u;
        q2gx_world_face_t *world_face = &world_faces[face_index];
        unsigned int triangle;
        f32 fan_origin[3];
        u8 red, green, blue;

        world_face->first_vertex = output_index;
        world_face->triangle_count = numedges - 2u;
        world_face->vertex_count = world_face->triangle_count * 3u;
        world_face->normal[0] = Q2GX_BSPReadLEFloat(plane + 0);
        world_face->normal[1] = Q2GX_BSPReadLEFloat(plane + 4);
        world_face->normal[2] = Q2GX_BSPReadLEFloat(plane + 8);
        world_face->dist = Q2GX_BSPReadLEFloat(plane + 12);
        world_face->plane_back = side ? true : false;
        world_face->pvs_visible_this_frame = false;
        world_face->visible_this_frame = false;

        if (!Q2GX_BSPResolveVertex(
                vertex_data, vertex_count,
                edge_data, edge_count,
                surfedge_data, surfedge_count,
                firstedge, 0u, fan_origin))
            goto fail;

        red = (u8)(64u + (face_index * 53u) % 160u);
        green = (u8)(64u + (face_index * 97u) % 160u);
        blue = (u8)(64u + (face_index * 151u) % 160u);

        for (triangle = 1u; triangle + 1u < numedges; ++triangle)
        {
            f32 p1[3], p2[3];
            q2gx_world_vertex_t *out;

            if (!Q2GX_BSPResolveVertex(
                    vertex_data, vertex_count,
                    edge_data, edge_count,
                    surfedge_data, surfedge_count,
                    firstedge, triangle, p1) ||
                !Q2GX_BSPResolveVertex(
                    vertex_data, vertex_count,
                    edge_data, edge_count,
                    surfedge_data, surfedge_count,
                    firstedge, triangle + 1u, p2))
                goto fail;

            if (output_index + 3u > gx_vertex_count)
                goto fail;

            out = &world_vertices[output_index];

#define Q2GX_STORE_WORLD_VERTEX(dst, src) \
            do { \
                (dst)->x = (src)[0]; \
                (dst)->y = (src)[1]; \
                (dst)->z = (src)[2]; \
                (dst)->r = red; \
                (dst)->g = green; \
                (dst)->b = blue; \
                (dst)->a = 255u; \
            } while (0)

            Q2GX_STORE_WORLD_VERTEX(&out[0], fan_origin);
            Q2GX_STORE_WORLD_VERTEX(&out[1], p1);
            Q2GX_STORE_WORLD_VERTEX(&out[2], p2);
#undef Q2GX_STORE_WORLD_VERTEX
            output_index += 3u;
        }

        if (output_index - world_face->first_vertex != world_face->vertex_count)
        {
            ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX WORLD: face %u vertex range mismatch\n", face_index);
            goto fail;
        }
    }

    if (output_index != gx_vertex_count)
        goto fail;

    q2gx_world_vertices = world_vertices;
    q2gx_world_faces = world_faces;
    q2gx_world_nodes = world_nodes;
    q2gx_world_leaves = world_leaves;
    q2gx_world_leaffaces = world_leaffaces;
    q2gx_world_vis_data = world_vis_data;
    q2gx_world_vis_pvs_offsets = world_vis_pvs_offsets;
    q2gx_world_pvs_row = world_pvs_row;
    q2gx_world_pvs_row2 = world_pvs_row2;

    q2gx_world_vertex_count = gx_vertex_count;
    q2gx_world_triangle_count = triangle_count;
    q2gx_world_face_count = face_count;
    q2gx_world_plane_count = plane_count;
    q2gx_world_node_count = node_count;
    q2gx_world_leaf_count = leaf_count;
    q2gx_world_leafface_count = leafface_count;
    q2gx_world_vis_bytes = visibility_bytes;
    q2gx_world_vis_numclusters = vis_numclusters;
    q2gx_world_vis_row_bytes = vis_row_bytes;
    q2gx_world_bytes = allocation_bytes;
    q2gx_world_face_bytes = face_allocation_bytes;
    q2gx_world_last_cluster = INT32_MIN;
    q2gx_world_last_cluster2 = INT32_MIN;

    memcpy(q2gx_world_name, path, strlen(path) + 1);

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX WORLD LOAD: %s planes=%u nodes=%u leaves=%u leaffaces=%u "
        "clusters=%u visrow=%u faces=%u triangles=%u vertices=%u "
        "pvs_metadata=1 backface_metadata=1\n",
        q2gx_world_name,
        q2gx_world_plane_count,
        q2gx_world_node_count,
        q2gx_world_leaf_count,
        q2gx_world_leafface_count,
        q2gx_world_vis_numclusters,
        q2gx_world_vis_row_bytes,
        q2gx_world_face_count,
        q2gx_world_triangle_count,
        q2gx_world_vertex_count);

    world_vertices = NULL;
    world_faces = NULL;
    world_nodes = NULL;
    world_leaves = NULL;
    world_leaffaces = NULL;
    world_vis_data = NULL;
    world_vis_pvs_offsets = NULL;
    world_pvs_row = NULL;
    world_pvs_row2 = NULL;

    ri.FS_FreeFile(file_buffer);
    return true;

fail:
    if (world_vertices) free(world_vertices);
    if (world_faces) free(world_faces);
    if (world_nodes) free(world_nodes);
    if (world_leaves) free(world_leaves);
    if (world_leaffaces) free(world_leaffaces);
    if (world_vis_data) free(world_vis_data);
    if (world_vis_pvs_offsets) free(world_vis_pvs_offsets);
    if (world_pvs_row) free(world_pvs_row);
    if (world_pvs_row2) free(world_pvs_row2);
    ri.FS_FreeFile(file_buffer);
    Q2GX_FreeWorldGeometry();
    return false;
}


static void Q2GX_SetupWorld3D(
    refdef_t *fd)
{
    Mtx view_matrix;
    Mtx44 projection_matrix;

    vec3_t forward;
    vec3_t right;
    vec3_t quake_up;

    guVector eye;
    guVector target;
    guVector camera_up;

    f32 aspect;

    if (
        !fd ||
        fd->width <= 0 ||
        fd->height <= 0
    )
    {
        return;
    }

    AngleVectors(
        fd->viewangles,
        forward,
        right,
        quake_up
    );

    /*
     * right is intentionally generated as part of the exact
     * Quake view basis even though guLookAt only needs
     * eye/target/up.
     */
    (void)right;

    eye.x =
        fd->vieworg[0];

    eye.y =
        fd->vieworg[1];

    eye.z =
        fd->vieworg[2];

    target.x =
        eye.x
        +
        forward[0];

    target.y =
        eye.y
        +
        forward[1];

    target.z =
        eye.z
        +
        forward[2];

    camera_up.x =
        quake_up[0];

    camera_up.y =
        quake_up[1];

    camera_up.z =
        quake_up[2];

    guLookAt(
        view_matrix,
        &eye,
        &camera_up,
        &target
    );

    aspect =
        (f32)fd->width
        /
        (f32)fd->height;

    /*
     * Match Quake II ref_gl's perspective contract:
     *
     *     fov_y
     *     width / height aspect
     *     near = 4
     *     far  = 4096
     */
    guPerspective(
        projection_matrix,
        fd->fov_y,
        aspect,
        4.0f,
        4096.0f
    );

    GX_SetViewport(
        0.0f,
        0.0f,
        (f32)q2gx_mode->fbWidth,
        (f32)q2gx_mode->efbHeight,
        0.0f,
        1.0f
    );

    GX_SetScissor(
        0,
        0,
        q2gx_mode->fbWidth,
        q2gx_mode->efbHeight
    );

    GX_LoadProjectionMtx(
        projection_matrix,
        GX_PERSPECTIVE
    );

    GX_LoadPosMtxImm(
        view_matrix,
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
        GX_POS_XYZ,
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

    /*
     * V1 renders both sides deliberately.
     *
     * We first prove geometry + camera.
     * BSP side/backface rules come afterward.
     */
    GX_SetCullMode(
        GX_CULL_NONE
    );

    GX_SetBlendMode(
        GX_BM_NONE,
        GX_BL_ONE,
        GX_BL_ZERO,
        GX_LO_CLEAR
    );

    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_TRUE
    );
}


static unsigned int Q2GX_MarkAllWorldFacesPVS(void)
{
    unsigned int face_index;
    for (face_index = 0u; face_index < q2gx_world_face_count; ++face_index)
        q2gx_world_faces[face_index].pvs_visible_this_frame = true;
    return q2gx_world_face_count;
}

static int Q2GX_PointInLeaf(const f32 point[3])
{
    int node_index = 0;
    unsigned int safety = 0u;

    if (!point || !q2gx_world_nodes || !q2gx_world_leaves ||
        q2gx_world_node_count == 0u || q2gx_world_leaf_count == 0u)
        return -1;

    for (;;)
    {
        const q2gx_world_node_t *node;
        f32 dot;
        int child;

        ++safety;
        if (safety > q2gx_world_node_count + 64u)
            return -1;
        if (node_index < 0 || (unsigned int)node_index >= q2gx_world_node_count)
            return -1;

        node = &q2gx_world_nodes[node_index];
        if (node->type >= 0 && node->type < 3)
            dot = point[node->type] - node->dist;
        else
            dot = point[0] * node->normal[0] +
                  point[1] * node->normal[1] +
                  point[2] * node->normal[2] - node->dist;

        child = node->children[dot > 0.0f ? 0 : 1];
        if (child >= 0)
        {
            node_index = child;
            continue;
        }
        if (child == INT32_MIN)
            return -1;
        node_index = -1 - child;
        if (node_index < 0 || (unsigned int)node_index >= q2gx_world_leaf_count)
            return -1;
        return node_index;
    }
}

static qboolean Q2GX_DecompressClusterPVS(int cluster, byte *output)
{
    unsigned int source_offset;
    unsigned int output_offset;
    int32_t compressed_offset;

    if (!output || q2gx_world_vis_row_bytes == 0u)
        return false;

    if (cluster == -1 || !q2gx_world_vis_data || !q2gx_world_vis_pvs_offsets ||
        q2gx_world_vis_numclusters == 0u)
    {
        memset(output, 0xff, q2gx_world_vis_row_bytes);
        return true;
    }

    if (cluster < 0 || (unsigned int)cluster >= q2gx_world_vis_numclusters)
        return false;

    compressed_offset = q2gx_world_vis_pvs_offsets[cluster];
    if (compressed_offset < 0)
    {
        memset(output, 0xff, q2gx_world_vis_row_bytes);
        return true;
    }
    if ((unsigned int)compressed_offset >= q2gx_world_vis_bytes)
        return false;

    source_offset = (unsigned int)compressed_offset;
    output_offset = 0u;

    while (output_offset < q2gx_world_vis_row_bytes)
    {
        byte value;
        if (source_offset >= q2gx_world_vis_bytes)
            return false;
        value = q2gx_world_vis_data[source_offset++];
        if (value)
        {
            output[output_offset++] = value;
            continue;
        }
        else
        {
            unsigned int zero_count;
            if (source_offset >= q2gx_world_vis_bytes)
                return false;
            zero_count = q2gx_world_vis_data[source_offset++];
            if (zero_count == 0u ||
                zero_count > q2gx_world_vis_row_bytes - output_offset)
                return false;
            memset(output + output_offset, 0, zero_count);
            output_offset += zero_count;
        }
    }

    return true;
}

static unsigned int Q2GX_MarkWorldPVS(
    const refdef_t *fd,
    int *leaf_index_out,
    int *cluster_out,
    int *cluster2_out,
    qboolean *fallback_out)
{
    unsigned int face_index;
    int leaf_index;
    int cluster;
    int cluster2;
    unsigned int marked_faces = 0u;

    if (leaf_index_out) *leaf_index_out = -1;
    if (cluster_out) *cluster_out = -1;
    if (cluster2_out) *cluster2_out = -1;
    if (fallback_out) *fallback_out = false;


    q2gx_world_pvs_visible_leaves_frame = 0u;
    q2gx_world_area_rejected_leaves_frame = 0u;
    q2gx_world_pvs_face_refs_frame = 0u;
    q2gx_world_area_rejected_face_refs_frame = 0u;
    q2gx_world_areabits_active_frame = false;

for (face_index = 0u; face_index < q2gx_world_face_count; ++face_index)
        q2gx_world_faces[face_index].pvs_visible_this_frame = false;

    if (!fd || !q2gx_world_nodes || !q2gx_world_leaves || !q2gx_world_leaffaces ||
        !q2gx_world_vis_data || !q2gx_world_vis_pvs_offsets ||
        !q2gx_world_pvs_row || !q2gx_world_pvs_row2 ||
        q2gx_world_vis_numclusters == 0u || q2gx_world_vis_row_bytes == 0u)
    {
        if (fallback_out) *fallback_out = true;
        return Q2GX_MarkAllWorldFacesPVS();
    }

    leaf_index = Q2GX_PointInLeaf(fd->vieworg);
    if (leaf_index < 0 || (unsigned int)leaf_index >= q2gx_world_leaf_count)
    {
        if (fallback_out) *fallback_out = true;
        return Q2GX_MarkAllWorldFacesPVS();
    }

    cluster = q2gx_world_leaves[leaf_index].cluster;
    cluster2 = cluster;

    {
        f32 temp[3];
        int second_leaf_index;
        const q2gx_world_leaf_t *view_leaf = &q2gx_world_leaves[leaf_index];

        temp[0] = fd->vieworg[0];
        temp[1] = fd->vieworg[1];
        temp[2] = fd->vieworg[2];

        if (!view_leaf->contents)
            temp[2] -= 16.0f;
        else
            temp[2] += 16.0f;

        second_leaf_index = Q2GX_PointInLeaf(temp);
        if (second_leaf_index >= 0 &&
            (unsigned int)second_leaf_index < q2gx_world_leaf_count)
        {
            const q2gx_world_leaf_t *second_leaf =
                &q2gx_world_leaves[second_leaf_index];

            if (!(second_leaf->contents & CONTENTS_SOLID) &&
                second_leaf->cluster != cluster2)
            {
                cluster2 = second_leaf->cluster;
            }
        }
    }

    if (leaf_index_out) *leaf_index_out = leaf_index;
    if (cluster_out) *cluster_out = cluster;
    if (cluster2_out) *cluster2_out = cluster2;

    if (cluster == -1)
    {
        if (fallback_out) *fallback_out = true;
        return Q2GX_MarkAllWorldFacesPVS();
    }

    if (!Q2GX_DecompressClusterPVS(cluster, q2gx_world_pvs_row))
    {
        if (fallback_out) *fallback_out = true;
        return Q2GX_MarkAllWorldFacesPVS();
    }

    if (cluster2 != cluster)
    {
        unsigned int byte_index;
        if (cluster2 == -1 ||
            !Q2GX_DecompressClusterPVS(cluster2, q2gx_world_pvs_row2))
        {
            if (fallback_out) *fallback_out = true;
            return Q2GX_MarkAllWorldFacesPVS();
        }

        for (byte_index = 0u; byte_index < q2gx_world_vis_row_bytes; ++byte_index)
            q2gx_world_pvs_row[byte_index] |= q2gx_world_pvs_row2[byte_index];
    }

    for (face_index = 0u; face_index < q2gx_world_leaf_count; ++face_index)
    {
        const q2gx_world_leaf_t *leaf = &q2gx_world_leaves[face_index];
        unsigned int reference_index;

        if (leaf->cluster < 0)
            continue;
        if ((unsigned int)leaf->cluster >= q2gx_world_vis_numclusters)
        {
            if (fallback_out) *fallback_out = true;
            return Q2GX_MarkAllWorldFacesPVS();
        }

        if (!(q2gx_world_pvs_row[(unsigned int)leaf->cluster >> 3] &
              (1u << ((unsigned int)leaf->cluster & 7u))))
            continue;


        ++q2gx_world_pvs_visible_leaves_frame;

        q2gx_world_pvs_face_refs_frame +=
            leaf->num_leaffaces;

        if (fd->areabits)
        {
            unsigned int area;

            q2gx_world_areabits_active_frame =
                true;

            area =
                (unsigned int)
                leaf->area;

            /*
             * Exact stock R_RecursiveWorldNode leaf test:
             *
             *   areabits[area >> 3]
             *       &
             *   (1 << (area & 7))
             */
            if (
                !(
                    fd->areabits[
                        area >> 3
                    ]
                    &
                    (
                        1u
                        <<
                        (
                            area
                            &
                            7u
                        )
                    )
                )
            )
            {
                ++q2gx_world_area_rejected_leaves_frame;

                q2gx_world_area_rejected_face_refs_frame +=
                    leaf->num_leaffaces;

                continue;
            }
        }

for (reference_index = leaf->first_leafface;
             reference_index < leaf->first_leafface + leaf->num_leaffaces;
             ++reference_index)
        {
            unsigned int referenced_face = q2gx_world_leaffaces[reference_index];
            if (referenced_face >= q2gx_world_face_count)
            {
                if (fallback_out) *fallback_out = true;
                return Q2GX_MarkAllWorldFacesPVS();
            }

            if (!q2gx_world_faces[referenced_face].pvs_visible_this_frame)
            {
                q2gx_world_faces[referenced_face].pvs_visible_this_frame = true;
                ++marked_faces;
            }
        }
    }

    return marked_faces;
}

static qboolean Q2GX_WorldFaceIsVisible(
    q2gx_world_face_t *face,
    const refdef_t *fd)
{
    f32 dot;

    if (
        !face ||
        !fd
    )
    {
        return false;
    }

    dot =
        fd->vieworg[0]
        *
        face->normal[0]
        +
        fd->vieworg[1]
        *
        face->normal[1]
        +
        fd->vieworg[2]
        *
        face->normal[2]
        -
        face->dist;

    /*
     * Exact R_RecursiveWorldNode world-facing rule:
     *
     *     dot >= 0
     *         sidebit = 0
     *
     *     dot < 0
     *         sidebit = SURF_PLANEBACK
     *
     * draw only if the surface's plane-back bit equals
     * sidebit.
     *
     * No epsilon here: stock ref_gl world traversal also
     * does not use BACKFACE_EPSILON for this test.
     */
    if (dot >= 0.0f)
    {
        return
            !face->plane_back;
    }

    return
        face->plane_back;
}


static void Q2GX_DrawFlatWorld(
    refdef_t *fd)
{
    unsigned int face_index;
    unsigned int pvs_faces;
    unsigned int pvs_rejected_faces;
    unsigned int backface_rejected_faces;
    unsigned int submitted_faces;
    unsigned int submitted_triangles;
    unsigned int submitted_vertices;
    unsigned int remaining_vertices;
    unsigned int stream_face;
    unsigned int face_vertex_offset;
    int leaf_index = -1;
    int cluster = -1;
    int cluster2 = -1;
    qboolean pvs_fallback = false;

    if (!fd || !q2gx_world_vertices || !q2gx_world_faces ||
        q2gx_world_vertex_count == 0u || q2gx_world_face_count == 0u)
        return;

    if (fd->rdflags & RDF_NOWORLDMODEL)
        return;

    pvs_faces = Q2GX_MarkWorldPVS(
        fd, &leaf_index, &cluster, &cluster2, &pvs_fallback);

    if (pvs_faces > q2gx_world_face_count)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: marked face count exceeds total\n");
        return;
    }

    pvs_rejected_faces = q2gx_world_face_count - pvs_faces;
    backface_rejected_faces = 0u;
    submitted_faces = 0u;
    submitted_triangles = 0u;
    submitted_vertices = 0u;

    for (face_index = 0u; face_index < q2gx_world_face_count; ++face_index)
    {
        q2gx_world_face_t *face = &q2gx_world_faces[face_index];
        face->visible_this_frame = false;

        if (!face->pvs_visible_this_frame)
            continue;

        face->visible_this_frame = Q2GX_WorldFaceIsVisible(face, fd);
        if (face->visible_this_frame)
        {
            ++submitted_faces;
            submitted_triangles += face->triangle_count;
            submitted_vertices += face->vertex_count;
        }
        else
        {
            ++backface_rejected_faces;
        }
    }

    if (submitted_faces + backface_rejected_faces != pvs_faces)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: PVS/backface accounting mismatch\n");
        return;
    }

    if (pvs_faces + pvs_rejected_faces != q2gx_world_face_count)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: total face accounting mismatch\n");
        return;
    }

    if (submitted_vertices != submitted_triangles * 3u)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: triangle accounting mismatch\n");
        return;
    }

    Q2GX_SetupWorld3D(fd);

    remaining_vertices = submitted_vertices;
    stream_face = 0u;
    face_vertex_offset = 0u;

    while (remaining_vertices > 0u)
    {
        unsigned int chunk = remaining_vertices;
        unsigned int emitted = 0u;

        if (chunk > 65532u)
            chunk = 65532u;
        chunk -= chunk % 3u;
        if (chunk == 0u)
            break;

        GX_Begin(GX_TRIANGLES, GX_VTXFMT0, (u16)chunk);

        while (emitted < chunk)
        {
            q2gx_world_face_t *face;
            unsigned int available;
            unsigned int take;
            unsigned int index;

            while (stream_face < q2gx_world_face_count &&
                   !q2gx_world_faces[stream_face].visible_this_frame)
            {
                ++stream_face;
                face_vertex_offset = 0u;
            }

            if (stream_face >= q2gx_world_face_count)
                break;

            face = &q2gx_world_faces[stream_face];
            available = face->vertex_count - face_vertex_offset;
            take = chunk - emitted;
            if (take > available)
                take = available;

            for (index = 0u; index < take; ++index)
            {
                const q2gx_world_vertex_t *vertex =
                    &q2gx_world_vertices[
                        face->first_vertex + face_vertex_offset + index];

                GX_Position3f32(vertex->x, vertex->y, vertex->z);
                GX_Color4u8(vertex->r, vertex->g, vertex->b, vertex->a);
            }

            emitted += take;
            face_vertex_offset += take;

            if (face_vertex_offset == face->vertex_count)
            {
                ++stream_face;
                face_vertex_offset = 0u;
            }
        }

        GX_End();

        if (emitted != chunk)
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX PVS: stream accounting mismatch emitted=%u chunk=%u\n",
                emitted, chunk);
            break;
        }

        remaining_vertices -= chunk;
    }

    ++q2gx_world_frames_window;
    q2gx_world_pvs_faces_window += pvs_faces;
    q2gx_world_pvs_rejected_faces_window += pvs_rejected_faces;
    q2gx_world_backface_rejected_faces_window += backface_rejected_faces;
    q2gx_world_submitted_faces_window += submitted_faces;
    q2gx_world_submitted_triangles_window += submitted_triangles;
    q2gx_world_submitted_vertices_window += submitted_vertices;

    q2gx_world_pvs_visible_leaves_window +=
        q2gx_world_pvs_visible_leaves_frame;

    q2gx_world_area_rejected_leaves_window +=
        q2gx_world_area_rejected_leaves_frame;

    q2gx_world_pvs_face_refs_window +=
        q2gx_world_pvs_face_refs_frame;

    q2gx_world_area_rejected_face_refs_window +=
        q2gx_world_area_rejected_face_refs_frame;

    if (
        q2gx_world_areabits_active_frame
    )
    {
        ++q2gx_world_areabits_frames_window;
    }


    if (pvs_fallback)
        ++q2gx_world_pvs_fallback_frames_window;

    if (cluster != q2gx_world_last_cluster || cluster2 != q2gx_world_last_cluster2)
    {
        ++q2gx_world_cluster_changes_window;
        q2gx_world_last_cluster = cluster;
        q2gx_world_last_cluster2 = cluster2;
    }

    if (!q2gx_world_first_draw_logged)
    {
        q2gx_world_first_draw_logged = true;
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PVS FIRST DRAW: %s leaf=%d cluster=%d cluster2=%d "
            "total_faces=%u pvs_faces=%u pvs_rejected=%u backface_rejected=%u "
            "submitted_faces=%u submitted_triangles=%u submitted_vertices=%u "
            "fallback=%u mode=minimal_pvs_face_marking\n",
            q2gx_world_name,
            leaf_index, cluster, cluster2,
            q2gx_world_face_count,
            pvs_faces,
            pvs_rejected_faces,
            backface_rejected_faces,
            submitted_faces,
            submitted_triangles,
            submitted_vertices,
            pvs_fallback ? 1u : 0u);
    }

    if (q2gx_world_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PVS 120: frames=%u total_faces=%u pvs_faces_total=%u "
            "pvs_rejected_total=%u backface_rejected_total=%u submitted_faces_total=%u "
            "submitted_triangles_total=%u submitted_vertices_total=%u fallback_frames=%u "
            "cluster_changes=%u cluster=%d cluster2=%d mode=minimal_pvs_face_marking\n",
            q2gx_world_frames_window,
            q2gx_world_face_count,
            q2gx_world_pvs_faces_window,
            q2gx_world_pvs_rejected_faces_window,
            q2gx_world_backface_rejected_faces_window,
            q2gx_world_submitted_faces_window,
            q2gx_world_submitted_triangles_window,
            q2gx_world_submitted_vertices_window,
            q2gx_world_pvs_fallback_frames_window,
            q2gx_world_cluster_changes_window,
            q2gx_world_last_cluster,
            q2gx_world_last_cluster2);

                ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX AREABITS 120: "
            "frames=%u "
            "pvs_visible_leaves_total=%u "
            "area_rejected_leaves_total=%u "
            "pvs_face_refs_total=%u "
            "area_rejected_face_refs_total=%u "
            "areabits_frames=%u "
            "mode=leaf_areabits\n",
            q2gx_world_frames_window,
            q2gx_world_pvs_visible_leaves_window,
            q2gx_world_area_rejected_leaves_window,
            q2gx_world_pvs_face_refs_window,
            q2gx_world_area_rejected_face_refs_window,
            q2gx_world_areabits_frames_window
        );

q2gx_world_frames_window = 0u;
        q2gx_world_pvs_faces_window = 0u;
        q2gx_world_pvs_rejected_faces_window = 0u;

        q2gx_world_pvs_visible_leaves_window = 0u;
        q2gx_world_area_rejected_leaves_window = 0u;
        q2gx_world_pvs_face_refs_window = 0u;
        q2gx_world_area_rejected_face_refs_window = 0u;
        q2gx_world_areabits_frames_window = 0u;

        q2gx_world_backface_rejected_faces_window = 0u;
        q2gx_world_submitted_faces_window = 0u;
        q2gx_world_submitted_triangles_window = 0u;
        q2gx_world_submitted_vertices_window = 0u;
        q2gx_world_pvs_fallback_frames_window = 0u;
        q2gx_world_cluster_changes_window = 0u;
    }

    Q2GX_Setup2D();
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
    Q2GX_FreeWorldGeometry();

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
    if (
        !Q2GX_LoadWorldGeometry(
            map
        )
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX WORLD: "
            "BeginRegistration failed for %s\n",
            map ? map : "(null)"
        );
    }
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
    if (!fd)
    {
        return;
    }

    Q2GX_DrawFlatWorld(
        fd
    );
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

    /*
     * Q2GC_WORLD_DEPTH_CLEAR_V1C
     *
     * Q2GX_DrawFlatWorld() restores Q2GX_Setup2D() before
     * EndFrame. The 2D state intentionally disables Z-buffer
     * updates.
     *
     * libogc2 documents that GX_SetZMode()'s update_enable
     * ALSO controls whether GX_CopyDisp(clear=true) clears
     * the EFB Z buffer.
     *
     * Therefore the old sequence could clear magenta color
     * every frame while leaving the previous world's depth
     * values resident.
     *
     * Re-enable both Z and color updates only for the
     * display-copy clear. The next rendering helper will
     * install its own normal 2D or 3D state.
     */
    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_TRUE
    );

    GX_SetColorUpdate(
        GX_TRUE
    );

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
