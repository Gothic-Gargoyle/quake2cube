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
#include <limits.h>
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

    /*
     * Q2GC_WORLD_TEXCOORDS_V1
     * Normalized Quake II base-texture coordinates.
     */
    f32 s;
    f32 t;
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

/*
 * Q2GC_WORLD_TEXCOORDS_V1
 *
 * WAL pixels remain deliberately out of scope. Width/height
 * are read only so normalized S/T exactly matches ref_gl.
 */
typedef struct q2gx_world_texinfo_s
{
    f32 vecs[2][4];
    unsigned int flags;
    char texture[33];
    unsigned int width;
    unsigned int height;

    /*
     * Q2GC_WAL_WORLD_TEXTURES_V1
     * Animated chains remain flat in this milestone.
     */
    int next_texinfo;
    int wal_cache_index;
} q2gx_world_texinfo_t;


/*
 * Q2GC_WAL_WORLD_TEXTURES_V1
 * Unique static ordinary WAL mip0 converted to GX RGBA8.
 */
/*
 * Q2GC_BRUSH_ENTITIES_V1
 *
 * client/ref.h deliberately exposes model_s as opaque.  ref_gx
 * therefore owns a tiny handle type sufficient for the renderer
 * families it has actually implemented.
 */
#define Q2GX_MODEL_HANDLE_MAGIC 0x51324758u
#define Q2GX_MODEL_KIND_WORLD 1u
#define Q2GX_MODEL_KIND_INLINE_BSP 2u
#define Q2GX_MODEL_KIND_ALIAS_MD2 3u
#define Q2GX_BRUSH_BACKFACE_EPSILON 0.01f

struct model_s
{
    uint32_t magic;
    unsigned int kind;
    unsigned int model_index;
};

typedef struct q2gx_brush_model_s
{
    struct model_s handle;

    f32 mins[3];
    f32 maxs[3];
    f32 origin[3];

    int headnode;

    unsigned int first_face;
    unsigned int face_count;

    qboolean registered;
} q2gx_brush_model_t;

/* Q2GC_ALIAS_MODELS_V1 */
#define Q2GX_ALIAS_MAX_SKINS 32u
#define Q2GX_ALIAS_MAX_XYZ 2048u
#define Q2GX_ALIAS_MAX_ST 4096u
#define Q2GX_ALIAS_MAX_FRAMES 2048u
#define Q2GX_ALIAS_MAX_TRIANGLES 21845u

typedef struct q2gx_alias_skin_s
{
    char name[128];
    unsigned int width;
    unsigned int height;

    void *storage;
    unsigned int storage_bytes;
    GXTexObj texture;

    void *palette_storage;
    unsigned int palette_bytes;
    GXTlutObj tlut;

    struct q2gx_alias_skin_s *next;
} q2gx_alias_skin_t;

typedef struct q2gx_alias_st_s
{
    int16_t s;
    int16_t t;
} q2gx_alias_st_t;

typedef struct q2gx_alias_triangle_s
{
    uint16_t xyz[3];
    uint16_t st[3];
} q2gx_alias_triangle_t;

typedef struct q2gx_alias_frame_s
{
    f32 scale[3];
    f32 translate[3];
    byte *verts;
} q2gx_alias_frame_t;

typedef struct q2gx_alias_model_s
{
    struct model_s handle;
    char name[128];

    unsigned int skin_width;
    unsigned int skin_height;
    unsigned int num_skins;
    unsigned int num_xyz;
    unsigned int num_st;
    unsigned int num_tris;
    unsigned int num_frames;

    q2gx_alias_skin_t **skins;
    q2gx_alias_st_t *st;
    q2gx_alias_triangle_t *triangles;
    q2gx_alias_frame_t *frames;
    byte *frame_vertices;

    unsigned int source_bytes;
    unsigned int frame_storage_bytes;
    qboolean first_draw_logged;

    struct q2gx_alias_model_s *next;
} q2gx_alias_model_t;

typedef struct q2gx_alias_transform_s
{
    f32 origin[3];
    f32 sin_roll, cos_roll;
    f32 sin_pitch, cos_pitch;
    f32 sin_yaw, cos_yaw;
} q2gx_alias_transform_t;

typedef struct q2gx_alias_lerp_s
{
    const q2gx_alias_frame_t *frame;
    const q2gx_alias_frame_t *old_frame;
    f32 move[3];
    f32 frontv[3];
    f32 backv[3];
    f32 frontlerp;
    f32 backlerp;
} q2gx_alias_lerp_t;



typedef struct q2gx_world_wal_texture_s
{
    char name[33];

    unsigned int width;
    unsigned int height;

    u8 *allocation;
    u8 *texture_data;
    unsigned int texture_bytes;

    GXTexObj texture;

    /*
     * Q2GC_WORLD_TEXTURE_BATCHING_V1
     *
     * Static ordinary BSP faces using this exact WAL.
     * Built once during world registration.
     */
    unsigned int *face_indices;
    unsigned int face_count;
} q2gx_world_wal_texture_t;


typedef struct q2gx_world_face_s
{
    unsigned int first_vertex;
    unsigned int vertex_count;
    unsigned int triangle_count;

    f32 normal[
        3
    ];

    f32 dist;

    unsigned int texinfo_index;
    unsigned int surface_flags;

    qboolean plane_back;
    qboolean pvs_visible_this_frame;
    qboolean visible_this_frame;
} q2gx_world_face_t;


static q2gx_world_face_t *q2gx_world_faces;

static q2gx_world_texinfo_t *q2gx_world_texinfos;
static unsigned int q2gx_world_texinfo_count;

static q2gx_world_wal_texture_t *q2gx_world_wal_textures;
static unsigned int q2gx_world_wal_texture_count;
static unsigned int q2gx_world_wal_texture_bytes;

static qboolean q2gx_world_wal_first_draw_logged;
static unsigned int q2gx_world_wal_textured_faces_window;
static unsigned int q2gx_world_wal_animated_flat_faces_window;

/* Q2GC_WORLD_ANIMATED_WAL_V1 */
static unsigned int q2gx_world_wal_animated_textured_faces_window;
static unsigned int q2gx_world_wal_animated_texture_binds_window;

static unsigned int q2gx_world_wal_special_flat_faces_window;
static unsigned int q2gx_world_wal_texture_binds_window;

static unsigned int q2gx_world_wal_visible_textures_window;
static unsigned int q2gx_world_wal_batch_draw_calls_window;
static unsigned int q2gx_world_wal_batch_vertices_window;

static qboolean q2gx_world_texcoord_first_draw_logged;
static unsigned int q2gx_world_texcoord_vertices_window;

#define Q2GX_WORLD_CHECKER_SIZE 64u

static u8 q2gx_world_checker_pixels[
    Q2GX_WORLD_CHECKER_SIZE *
    Q2GX_WORLD_CHECKER_SIZE *
    4u
] __attribute__((aligned(32)));

static GXTexObj q2gx_world_checker_texobj;
static qboolean q2gx_world_checker_ready;

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

static unsigned int q2gx_world_static_first_face;
static unsigned int q2gx_world_static_face_count;

static q2gx_brush_model_t *q2gx_brush_models;
static unsigned int q2gx_brush_model_count;
static unsigned int q2gx_brush_registered_inline_count;

static qboolean q2gx_brush_first_draw_logged;

static unsigned int q2gx_brush_entities_window;
static unsigned int q2gx_brush_faces_tested_window;
static unsigned int q2gx_brush_backface_rejected_window;
static unsigned int q2gx_brush_visible_faces_window;
static unsigned int q2gx_brush_textured_faces_window;
static unsigned int q2gx_brush_fallback_faces_window;
static unsigned int q2gx_brush_wal_binds_window;
static unsigned int q2gx_brush_vertices_window;

static q2gx_alias_model_t *q2gx_alias_models;
static q2gx_alias_skin_t *q2gx_alias_skins;
static unsigned int q2gx_alias_model_serial;

static unsigned int q2gx_alias_registered_models;
static unsigned int q2gx_alias_registered_skins;
static unsigned int q2gx_alias_source_bytes;
static unsigned int q2gx_alias_skin_bytes;
static unsigned int q2gx_alias_tlut_bytes;

static unsigned int q2gx_alias_frames_window;
static unsigned int q2gx_alias_entities_window;
static unsigned int q2gx_alias_weapon_skipped_window;
static unsigned int q2gx_alias_translucent_skipped_window;
static unsigned int q2gx_alias_beam_skipped_window;
static unsigned int q2gx_alias_triangles_window;
static unsigned int q2gx_alias_vertices_window;
static unsigned int q2gx_alias_skin_binds_window;
static unsigned int q2gx_alias_tlut_loads_window;
static unsigned int q2gx_alias_invalid_frame_window;
static unsigned int q2gx_alias_invalid_skin_window;
static unsigned int q2gx_alias_custom_skin_fallback_window;
static unsigned int q2gx_alias_animated_samples_window;

/* Q2GC_TRANSLUCENT_ALIAS_V1D_SHARED_CORE_INPUT_RESTORE */
static qboolean q2gx_alias_translucent_pass;

static unsigned int q2gx_transalias_frames_window;
static unsigned int q2gx_transalias_seen_window;
static unsigned int q2gx_transalias_drawn_window;
static unsigned int q2gx_transalias_shell_skipped_window;
static unsigned int q2gx_transalias_non_smoke_skipped_window;
static unsigned int q2gx_transalias_triangles_window;
static unsigned int q2gx_transalias_vertices_window;
static unsigned int q2gx_transalias_skin_binds_window;
static unsigned int q2gx_transalias_tlut_loads_window;
static unsigned int q2gx_transalias_invalid_frame_window;
static unsigned int q2gx_transalias_invalid_skin_window;
static unsigned int q2gx_transalias_alpha_min_window = 255u;
static unsigned int q2gx_transalias_alpha_max_window;
static qboolean q2gx_transalias_first_draw_logged;
static qboolean q2gx_transalias_nonsmoke_first_draw_logged;

/* Q2GC_PARTICLES_V1 */
#define Q2GX_PARTICLE_POINT_SIZE_SIXTHS 18u

static unsigned int q2gx_particles_frames_window;
static unsigned int q2gx_particles_frames_with_particles_window;
static unsigned int q2gx_particles_count_window;
static unsigned int q2gx_particles_vertices_window;
static unsigned int q2gx_particles_draw_calls_window;
static unsigned int q2gx_particles_invalid_color_window;
static unsigned int q2gx_particles_alpha_clamped_window;
static unsigned int q2gx_particles_color_e0_window;
static unsigned int q2gx_particles_color_d0_window;
static unsigned int q2gx_particles_min_frame_window = 0xffffffffu;
static unsigned int q2gx_particles_max_frame_window;
static qboolean q2gx_particles_first_draw_logged;

/* Q2GC_WORLD_WARP_V1 */
#define Q2GX_WARP_SUBDIVIDE_SIZE 64.0f
#define Q2GX_WARP_SUBDIVIDE_EDGE_EPSILON 8.0f
#define Q2GX_WARP_MAX_VERTS 64u
#define Q2GX_WARP_TURBSCALE (256.0f / (2.0f * 3.14159265358979323846f))

static q2gx_world_wal_texture_t *q2gx_world_warp_textures;
static qboolean *q2gx_world_warp_texture_loaded;
static qboolean *q2gx_world_warp_texture_failed;
static unsigned int q2gx_world_warp_texture_capacity;

static unsigned int q2gx_world_warp_frames_window;
static unsigned int q2gx_world_warp_visible_faces_window;
static unsigned int q2gx_world_warp_drawn_faces_window;
static unsigned int q2gx_world_warp_translucent_skipped_window;
static unsigned int q2gx_world_warp_flowing_faces_window;
static unsigned int q2gx_world_warp_texture_binds_window;
static unsigned int q2gx_world_warp_subpolys_window;
static unsigned int q2gx_world_warp_vertices_window;
static unsigned int q2gx_world_warp_load_fail_window;
static qboolean q2gx_world_warp_first_draw_logged;

/* Q2GC_BRUSH_TRANSLUCENT_V1 */
typedef struct q2gx_trans_brush_sort_s
{
    unsigned int entity_index;
    unsigned int face_index;
    f32 depth_sq;
} q2gx_trans_brush_sort_t;

static q2gx_trans_brush_sort_t *q2gx_trans_brush_sort;
static unsigned int q2gx_trans_brush_sort_capacity;

static unsigned int q2gx_trans_brush_frames_window;
static unsigned int q2gx_trans_brush_candidates_window;
static unsigned int q2gx_trans_brush_drawn_window;
static unsigned int q2gx_trans_brush_alpha33_window;
static unsigned int q2gx_trans_brush_alpha66_window;
static unsigned int q2gx_trans_brush_wal_binds_window;
static unsigned int q2gx_trans_brush_vertices_window;
static unsigned int q2gx_trans_brush_alloc_fail_window;
static unsigned int q2gx_trans_brush_invalid_window;
static unsigned int q2gx_brush_deferred_trans_faces_window;
static qboolean q2gx_trans_brush_first_draw_logged;


/* Q2GC_UNDERWATER_WARP_V1 */
#define Q2GX_UNDERWATER_COPY_WIDTH 640u
#define Q2GX_UNDERWATER_COPY_HEIGHT 480u
#define Q2GX_UNDERWATER_MESH_STEP 16u
#define Q2GX_UNDERWATER_SOFT_CYCLE 128u
#define Q2GX_UNDERWATER_SOFT_AMP2 3u
#define Q2GX_UNDERWATER_SOFT_SPEED 20u

static void *q2gx_underwater_copy_buffer;
static u32 q2gx_underwater_copy_bytes;
static GXTexObj q2gx_underwater_copy_texture;
static qboolean q2gx_underwater_copy_ready;

static unsigned int q2gx_underwater_warp_frames_window;
static unsigned int q2gx_underwater_warp_active_frames_window;
static unsigned int q2gx_underwater_warp_copies_window;
static unsigned int q2gx_underwater_warp_quads_window;
static unsigned int q2gx_underwater_warp_vertices_window;
static unsigned int q2gx_underwater_warp_alloc_fail_window;
static unsigned int q2gx_underwater_warp_bad_refdef_window;
static qboolean q2gx_underwater_warp_first_logged;


/* Q2GC_REFDEF_POLYBLEND_V1 */
static unsigned int q2gx_polyblend_frames_window;
static unsigned int q2gx_polyblend_active_frames_window;
static unsigned int q2gx_polyblend_underwater_frames_window;
static unsigned int q2gx_polyblend_quads_window;
static unsigned int q2gx_polyblend_alpha_min_window;
static unsigned int q2gx_polyblend_alpha_max_window;
static qboolean q2gx_polyblend_underwater_first_logged;


/* Q2GC_WORLD_TRANSWARP_V1 */
typedef struct q2gx_world_transwarp_sort_s
{
    unsigned int face_index;
    f32 depth_sq;
} q2gx_world_transwarp_sort_t;

static q2gx_world_transwarp_sort_t *q2gx_world_transwarp_sort;
static unsigned int q2gx_world_transwarp_sort_capacity;

static unsigned int q2gx_world_transwarp_frames_window;
static unsigned int q2gx_world_transwarp_visible_faces_window;
static unsigned int q2gx_world_transwarp_drawn_faces_window;
static unsigned int q2gx_world_transwarp_alpha33_faces_window;
static unsigned int q2gx_world_transwarp_alpha66_faces_window;
static unsigned int q2gx_world_transwarp_flowing_faces_window;
static unsigned int q2gx_world_transwarp_texture_binds_window;
static unsigned int q2gx_world_transwarp_subpolys_window;
static unsigned int q2gx_world_transwarp_vertices_window;
static qboolean q2gx_world_transwarp_first_draw_logged;

static const f32 q2gx_world_warp_sin[256] =
{
    0.0f, 0.098165f, 0.1962705f, 0.2942585f, 0.3920685f, 0.4896425f, 0.58692f, 0.68385f,
    0.78036f, 0.876405f, 0.97192f, 1.06685f, 1.16114f, 1.254725f, 1.34756f, 1.43958f,
    1.530735f, 1.620965f, 1.71022f, 1.798445f, 1.885585f, 1.971595f, 2.05641f, 2.13999f,
    2.22228f, 2.303235f, 2.382795f, 2.460925f, 2.537575f, 2.61269f, 2.686235f, 2.75816f,
    2.828425f, 2.89699f, 2.963805f, 3.028835f, 3.09204f, 3.153385f, 3.21283f, 3.27034f,
    3.32588f, 3.379415f, 3.430915f, 3.48035f, 3.527685f, 3.572895f, 3.615955f, 3.65684f,
    3.69552f, 3.73197f, 3.766175f, 3.798115f, 3.82776f, 3.855105f, 3.880125f, 3.90281f,
    3.92314f, 3.94111f, 3.956705f, 3.96992f, 3.98074f, 3.98916f, 3.99518f, 3.998795f,
    4.0f, 3.998795f, 3.99518f, 3.98916f, 3.98074f, 3.96992f, 3.956705f, 3.94111f,
    3.92314f, 3.90281f, 3.880125f, 3.855105f, 3.82776f, 3.798115f, 3.766175f, 3.73197f,
    3.69552f, 3.65684f, 3.615955f, 3.572895f, 3.527685f, 3.48035f, 3.430915f, 3.379415f,
    3.32588f, 3.27034f, 3.21283f, 3.153385f, 3.09204f, 3.028835f, 2.963805f, 2.89699f,
    2.828425f, 2.75816f, 2.686235f, 2.61269f, 2.537575f, 2.460925f, 2.382795f, 2.303235f,
    2.22228f, 2.13999f, 2.05641f, 1.971595f, 1.885585f, 1.798445f, 1.71022f, 1.620965f,
    1.530735f, 1.43958f, 1.34756f, 1.254725f, 1.16114f, 1.06685f, 0.97192f, 0.876405f,
    0.78036f, 0.68385f, 0.58692f, 0.4896425f, 0.3920685f, 0.2942585f, 0.1962705f, 0.098165f,
    4.898585e-16f, -0.098165f, -0.1962705f, -0.2942585f, -0.3920685f, -0.4896425f, -0.58692f, -0.68385f,
    -0.78036f, -0.876405f, -0.97192f, -1.06685f, -1.16114f, -1.254725f, -1.34756f, -1.43958f,
    -1.530735f, -1.620965f, -1.71022f, -1.798445f, -1.885585f, -1.971595f, -2.05641f, -2.13999f,
    -2.22228f, -2.303235f, -2.382795f, -2.460925f, -2.537575f, -2.61269f, -2.686235f, -2.75816f,
    -2.828425f, -2.89699f, -2.963805f, -3.028835f, -3.09204f, -3.153385f, -3.21283f, -3.27034f,
    -3.32588f, -3.379415f, -3.430915f, -3.48035f, -3.527685f, -3.572895f, -3.615955f, -3.65684f,
    -3.69552f, -3.73197f, -3.766175f, -3.798115f, -3.82776f, -3.855105f, -3.880125f, -3.90281f,
    -3.92314f, -3.94111f, -3.956705f, -3.96992f, -3.98074f, -3.98916f, -3.99518f, -3.998795f,
    -4.0f, -3.998795f, -3.99518f, -3.98916f, -3.98074f, -3.96992f, -3.956705f, -3.94111f,
    -3.92314f, -3.90281f, -3.880125f, -3.855105f, -3.82776f, -3.798115f, -3.766175f, -3.73197f,
    -3.69552f, -3.65684f, -3.615955f, -3.572895f, -3.527685f, -3.48035f, -3.430915f, -3.379415f,
    -3.32588f, -3.27034f, -3.21283f, -3.153385f, -3.09204f, -3.028835f, -2.963805f, -2.89699f,
    -2.828425f, -2.75816f, -2.686235f, -2.61269f, -2.537575f, -2.460925f, -2.382795f, -2.303235f,
    -2.22228f, -2.13999f, -2.05641f, -1.971595f, -1.885585f, -1.798445f, -1.71022f, -1.620965f,
    -1.530735f, -1.43958f, -1.34756f, -1.254725f, -1.16114f, -1.06685f, -0.97192f, -0.876405f,
    -0.78036f, -0.68385f, -0.58692f, -0.4896425f, -0.3920685f, -0.2942585f, -0.1962705f, -0.098165f
};


/* Q2GC_WORLD_SKY_V1 */
#define Q2GX_SKY_FACE_COUNT 6u
#define Q2GX_SKY_MAX_CLIP_VERTS 64u
#define Q2GX_SKY_ON_EPSILON 0.1f
#define Q2GX_SKY_DISTANCE 2300.0f

static struct image_s *q2gx_sky_images[Q2GX_SKY_FACE_COUNT];
static char q2gx_sky_name[MAX_QPATH];
static f32 q2gx_sky_rotate;
static f32 q2gx_sky_axis[3];
static qboolean q2gx_sky_ready;
static qboolean q2gx_sky_first_draw_logged;

static f32 q2gx_sky_mins[2][Q2GX_SKY_FACE_COUNT];
static f32 q2gx_sky_maxs[2][Q2GX_SKY_FACE_COUNT];

static unsigned int q2gx_sky_frames_window;
static unsigned int q2gx_sky_frames_with_sky_window;
static unsigned int q2gx_sky_source_faces_window;
static unsigned int q2gx_sky_box_faces_window;
static unsigned int q2gx_sky_vertices_window;

static const char *q2gx_sky_suffix[Q2GX_SKY_FACE_COUNT] =
{
    "rt",
    "bk",
    "lf",
    "ft",
    "up",
    "dn"
};

static const int q2gx_sky_st_to_vec[6][3] =
{
	{3,-1,2},
	{-3,1,2},

	{1,3,2},
	{-1,-3,2},

	{-2,-1,3},		// 0 degrees yaw, look straight up
	{2,-1,-3}		// look straight down

//	{-1,2,3},
//	{1,2,-3}
};

static const int q2gx_sky_vec_to_st[6][3] =
{
	{-2,3,1},
	{2,3,-1},

	{1,3,2},
	{-1,3,-2},

	{-2,-1,3},
	{-2,1,-3}

//	{-1,2,3},
//	{1,2,-3}
};

static const int q2gx_sky_tex_order[6] =
{0,2,1,3,4,5};

static const f32 q2gx_sky_clip[6][3] =
{
	{1,1,0},
	{1,-1,0},
	{0,-1,1},
	{0,1,1},
	{1,0,1},
	{-1,0,1}
};


/* Q2GC_VIEW_WEAPON_V1 */
static cvar_t *q2gx_viewweapon_hand;

static unsigned int q2gx_viewweapon_frames_window;
static unsigned int q2gx_viewweapon_seen_window;
static unsigned int q2gx_viewweapon_drawn_window;
static unsigned int q2gx_viewweapon_hidden_hand2_window;
static unsigned int q2gx_viewweapon_mirrored_hand1_window;
static unsigned int q2gx_viewweapon_depthhack_window;
static unsigned int q2gx_viewweapon_triangles_window;
static unsigned int q2gx_viewweapon_vertices_window;
static unsigned int q2gx_viewweapon_skin_binds_window;
static unsigned int q2gx_viewweapon_tlut_loads_window;
static unsigned int q2gx_viewweapon_invalid_frame_window;
static unsigned int q2gx_viewweapon_invalid_skin_window;
static qboolean q2gx_viewweapon_first_draw_logged;

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
#define Q2GX_BSP_LUMP_TEXINFO       5
#define Q2GX_BSP_LUMP_MODELS 13u
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


static void Q2GX_SetupWorld3D(
    refdef_t *fd);


#define Q2GX_SURF_SKY       0x0004u
#define Q2GX_SURF_WARP      0x0008u
#define Q2GX_SURF_TRANS33   0x0010u
#define Q2GX_SURF_TRANS66   0x0020u
#define Q2GX_SURF_FLOWING   0x0040u
#define Q2GX_SURF_NODRAW    0x0080u

#define Q2GX_WORLD_TEXCOORD_SPECIAL_MASK \
    (Q2GX_SURF_SKY | Q2GX_SURF_WARP | Q2GX_SURF_TRANS33 | \
     Q2GX_SURF_TRANS66 | Q2GX_SURF_FLOWING | Q2GX_SURF_NODRAW)


static qboolean Q2GX_ReadWALDimensions(
    const char *texture,
    unsigned int *width_out,
    unsigned int *height_out)
{
    char path[MAX_QPATH];
    size_t texture_length;
    void *file_buffer;
    int file_length;
    const byte *data;
    uint32_t width;
    uint32_t height;

    if (!texture || !texture[0] || !width_out || !height_out)
        return false;

    texture_length = strlen(texture);

    if (texture_length + 14u > sizeof(path))
        return false;

    memcpy(path, "textures/", 9u);
    memcpy(path + 9u, texture, texture_length);
    memcpy(path + 9u + texture_length, ".wal", 5u);

    file_buffer = NULL;
    file_length = ri.FS_LoadFile(path, &file_buffer);

    if (file_length < 100 || !file_buffer)
    {
        if (file_buffer)
            ri.FS_FreeFile(file_buffer);

        return false;
    }

    data = (const byte *)file_buffer;

    width = Q2GX_BSPReadLE32(data + 32u);
    height = Q2GX_BSPReadLE32(data + 36u);

    ri.FS_FreeFile(file_buffer);

    if (width == 0u || height == 0u ||
        width > 4096u || height > 4096u)
    {
        return false;
    }

    *width_out = (unsigned int)width;
    *height_out = (unsigned int)height;

    return true;
}


static void Q2GX_InitWorldCheckerTexture(void)
{
    unsigned int tile_y;
    unsigned int tile_x;

    if (q2gx_world_checker_ready)
        return;

    for (tile_y = 0u;
         tile_y < Q2GX_WORLD_CHECKER_SIZE / 4u;
         ++tile_y)
    {
        for (tile_x = 0u;
             tile_x < Q2GX_WORLD_CHECKER_SIZE / 4u;
             ++tile_x)
        {
            unsigned int local_y;
            unsigned int local_x;
            unsigned int block_offset;

            block_offset =
                (tile_y * (Q2GX_WORLD_CHECKER_SIZE / 4u) + tile_x) *
                64u;

            for (local_y = 0u; local_y < 4u; ++local_y)
            {
                for (local_x = 0u; local_x < 4u; ++local_x)
                {
                    unsigned int x = tile_x * 4u + local_x;
                    unsigned int y = tile_y * 4u + local_y;
                    unsigned int texel = local_y * 4u + local_x;
                    unsigned int ar_offset =
                        block_offset + texel * 2u;
                    unsigned int gb_offset =
                        block_offset + 32u + texel * 2u;

                    u8 value =
                        ((((x >> 3) ^ (y >> 3)) & 1u) != 0u)
                            ? 32u
                            : 255u;

                    /*
                     * GX_TF_RGBA8 4x4 tile:
                     * first 32 bytes A/R, second 32 G/B.
                     *
                     * Cell 0,0 is white. Special surfaces
                     * sample that white cell so GX_MODULATE
                     * preserves their old flat pastel color.
                     */
                    q2gx_world_checker_pixels[ar_offset + 0u] = 255u;
                    q2gx_world_checker_pixels[ar_offset + 1u] = value;
                    q2gx_world_checker_pixels[gb_offset + 0u] = value;
                    q2gx_world_checker_pixels[gb_offset + 1u] = value;
                }
            }
        }
    }

    DCStoreRange(
        q2gx_world_checker_pixels,
        sizeof(q2gx_world_checker_pixels)
    );

    GX_InitTexObj(
        &q2gx_world_checker_texobj,
        q2gx_world_checker_pixels,
        (u16)Q2GX_WORLD_CHECKER_SIZE,
        (u16)Q2GX_WORLD_CHECKER_SIZE,
        GX_TF_RGBA8,
        GX_REPEAT,
        GX_REPEAT,
        GX_FALSE
    );

    GX_InitTexObjFilterMode(
        &q2gx_world_checker_texobj,
        GX_NEAR,
        GX_NEAR
    );

    q2gx_world_checker_ready = true;
}


static void Q2GX_ComputeWorldDiagnosticST(
    const q2gx_world_texinfo_t *texinfo,
    const f32 position[3],
    f32 *s_out,
    f32 *t_out)
{
    f32 raw_s;
    f32 raw_t;

    if (!texinfo || !position || !s_out || !t_out ||
        texinfo->width == 0u || texinfo->height == 0u)
    {
        if (s_out)
            *s_out = 0.03125f;
        if (t_out)
            *t_out = 0.03125f;
        return;
    }

    /*
     * Q2GC_WORLD_ANIMATED_WAL_V1
     *
     * Animated ordinary surfaces use the same base texture
     * coordinates as static ordinary surfaces. Quake II WAL
     * animation frames are selected at draw time.
     *
     * Only genuinely special surfaces retain diagnostic ST here.
     */
    if (
        texinfo->flags
        &
        Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
    )
    {
        *s_out = 0.03125f;
        *t_out = 0.03125f;
        return;
    }

    /*
     * Exact stock GL_BuildPolygonFromSurface base texture:
     *
     * s = (dot(position, vecs[0].xyz) + vecs[0][3]) / width
     * t = (dot(position, vecs[1].xyz) + vecs[1][3]) / height
     */
    raw_s =
        position[0] * texinfo->vecs[0][0] +
        position[1] * texinfo->vecs[0][1] +
        position[2] * texinfo->vecs[0][2] +
        texinfo->vecs[0][3];

    raw_t =
        position[0] * texinfo->vecs[1][0] +
        position[1] * texinfo->vecs[1][1] +
        position[2] * texinfo->vecs[1][2] +
        texinfo->vecs[1][3];

    *s_out = raw_s / (f32)texinfo->width;
    *t_out = raw_t / (f32)texinfo->height;
}


static void Q2GX_SetupTexturedWorld3D(
    refdef_t *fd)
{
    Q2GX_SetupWorld3D(fd);
    Q2GX_InitWorldCheckerTexture();

    GX_SetVtxDesc(
        GX_VA_TEX0,
        GX_DIRECT
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_TEX0,
        GX_TEX_ST,
        GX_F32,
        0
    );

    GX_SetNumTexGens(1);

    GX_SetTexCoordGen(
        GX_TEXCOORD0,
        GX_TG_MTX2x4,
        GX_TG_TEX0,
        GX_IDENTITY
    );

    GX_SetNumTevStages(1);

    GX_SetTevOrder(
        GX_TEVSTAGE0,
        GX_TEXCOORD0,
        GX_TEXMAP0,
        GX_COLOR0A0
    );

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_MODULATE
    );

    GX_LoadTexObj(
        &q2gx_world_checker_texobj,
        GX_TEXMAP0
    );
}


static void Q2GX_FreeWorldWALTextureArray(
    q2gx_world_wal_texture_t *textures,
    unsigned int count)
{
    unsigned int index;

    if (!textures)
        return;

    for (index = 0u; index < count; ++index)
    {
        if (textures[index].face_indices)
        {
            free(
                textures[index].face_indices
            );

            textures[index].face_indices =
                NULL;

            textures[index].face_count =
                0u;
        }

        if (textures[index].allocation)
        {
            free(textures[index].allocation);
            textures[index].allocation = NULL;
            textures[index].texture_data = NULL;
        }
    }

    free(textures);
}


static int Q2GX_FindWorldWALTexture(
    q2gx_world_wal_texture_t *textures,
    unsigned int count,
    const char *name)
{
    unsigned int index;

    if (!textures || !name)
        return -1;

    for (index = 0u; index < count; ++index)
    {
        if (strcmp(textures[index].name, name) == 0)
            return (int)index;
    }

    return -1;
}


static qboolean Q2GX_LoadWorldWALTexture(
    const char *name,
    q2gx_world_wal_texture_t *texture)
{
    char path[MAX_QPATH];

    size_t name_length;

    void *file_buffer = NULL;
    int file_length;

    const byte *data;
    const byte *indexed;

    uint32_t width;
    uint32_t height;
    uint32_t offset0;

    unsigned int tile_columns;
    unsigned int tile_rows;
    unsigned int texture_bytes;

    unsigned int x;
    unsigned int y;

    uintptr_t aligned;

    if (!name || !name[0] || !texture || !q2gx_palette_loaded)
        return false;

    name_length = strlen(name);

    if (name_length + 14u > sizeof(path))
        return false;

    memcpy(path, "textures/", 9u);
    memcpy(path + 9u, name, name_length);
    memcpy(path + 9u + name_length, ".wal", 5u);

    file_length = ri.FS_LoadFile(path, &file_buffer);

    if (file_length < 100 || !file_buffer)
    {
        if (file_buffer)
            ri.FS_FreeFile(file_buffer);
        return false;
    }

    data = (const byte *)file_buffer;

    width = Q2GX_BSPReadLE32(data + 32u);
    height = Q2GX_BSPReadLE32(data + 36u);
    offset0 = Q2GX_BSPReadLE32(data + 40u);

    if (
        width == 0u ||
        height == 0u ||
        width > 1024u ||
        height > 1024u ||
        offset0 > (uint32_t)file_length ||
        (uint64_t)width * (uint64_t)height >
            (uint64_t)file_length - (uint64_t)offset0
    )
    {
        ri.FS_FreeFile(file_buffer);
        return false;
    }

    tile_columns = ((unsigned int)width + 3u) >> 2;
    tile_rows = ((unsigned int)height + 3u) >> 2;

    if ((uint64_t)tile_columns * (uint64_t)tile_rows * 64u > 0xffffffffu)
    {
        ri.FS_FreeFile(file_buffer);
        return false;
    }

    texture_bytes = tile_columns * tile_rows * 64u;

    texture->allocation =
        calloc(
            1u,
            texture_bytes + 31u
        );

    if (!texture->allocation)
    {
        ri.FS_FreeFile(file_buffer);
        return false;
    }

    aligned =
        (
            (uintptr_t)texture->allocation
            +
            31u
        )
        &
        ~(uintptr_t)31u;

    texture->texture_data =
        (u8 *)aligned;

    indexed =
        data + offset0;

    for (y = 0u; y < (unsigned int)height; ++y)
    {
        for (x = 0u; x < (unsigned int)width; ++x)
        {
            unsigned int tile_x = x >> 2;
            unsigned int tile_y = y >> 2;
            unsigned int local_x = x & 3u;
            unsigned int local_y = y & 3u;
            unsigned int block_offset =
                (tile_y * tile_columns + tile_x) * 64u;
            unsigned int texel = local_y * 4u + local_x;
            unsigned int ar_offset = block_offset + texel * 2u;
            unsigned int gb_offset = block_offset + 32u + texel * 2u;

            GXColor color =
                q2gx_palette[
                    indexed[
                        y * (unsigned int)width + x
                    ]
                ];

            texture->texture_data[ar_offset + 0u] = 255u;
            texture->texture_data[ar_offset + 1u] = color.r;
            texture->texture_data[gb_offset + 0u] = color.g;
            texture->texture_data[gb_offset + 1u] = color.b;
        }
    }

    ri.FS_FreeFile(file_buffer);

    memset(texture->name, 0, sizeof(texture->name));
    memcpy(texture->name, name, name_length);

    texture->width = (unsigned int)width;
    texture->height = (unsigned int)height;
    texture->texture_bytes = texture_bytes;

    DCStoreRange(
        texture->texture_data,
        texture_bytes
    );

    GX_InitTexObj(
        &texture->texture,
        texture->texture_data,
        (u16)texture->width,
        (u16)texture->height,
        GX_TF_RGBA8,
        GX_REPEAT,
        GX_REPEAT,
        GX_FALSE
    );

    GX_InitTexObjFilterMode(
        &texture->texture,
        GX_NEAR,
        GX_NEAR
    );

    return true;
}


static void Q2GX_BindWorldWALTexture(
    q2gx_world_wal_texture_t *texture)
{
    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_REPLACE
    );

    GX_LoadTexObj(
        &texture->texture,
        GX_TEXMAP0
    );
}


static void Q2GX_BindWorldFlatFallback(void)
{
    Q2GX_InitWorldCheckerTexture();

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_MODULATE
    );

    GX_LoadTexObj(
        &q2gx_world_checker_texobj,
        GX_TEXMAP0
    );
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


    if (q2gx_world_texinfos)
        free(q2gx_world_texinfos);

    q2gx_world_texinfos = NULL;
    q2gx_world_texinfo_count = 0u;
    q2gx_world_texcoord_first_draw_logged = false;
    q2gx_world_texcoord_vertices_window = 0u;


    Q2GX_FreeWorldWALTextureArray(
        q2gx_world_wal_textures,
        q2gx_world_wal_texture_count
    );

    q2gx_world_wal_textures = NULL;
    q2gx_world_wal_texture_count = 0u;
    q2gx_world_wal_texture_bytes = 0u;

    q2gx_world_wal_first_draw_logged = false;
    q2gx_world_wal_textured_faces_window = 0u;
    q2gx_world_wal_animated_flat_faces_window = 0u;
    q2gx_world_wal_animated_textured_faces_window = 0u;
    q2gx_world_wal_animated_texture_binds_window = 0u;
    q2gx_world_wal_special_flat_faces_window = 0u;
    q2gx_world_wal_texture_binds_window = 0u;

    q2gx_world_wal_visible_textures_window =
        0u;

    q2gx_world_wal_batch_draw_calls_window =
        0u;

    q2gx_world_wal_batch_vertices_window =
        0u;



    if (q2gx_brush_models)
    {
        free(
            q2gx_brush_models
        );

        q2gx_brush_models = NULL;
    }

    q2gx_brush_model_count = 0u;
    q2gx_brush_registered_inline_count = 0u;

    q2gx_world_static_first_face = 0u;
    q2gx_world_static_face_count = 0u;

    q2gx_brush_first_draw_logged = false;

    q2gx_brush_entities_window = 0u;
    q2gx_brush_faces_tested_window = 0u;
    q2gx_brush_backface_rejected_window = 0u;
    q2gx_brush_visible_faces_window = 0u;
    q2gx_brush_textured_faces_window = 0u;
    q2gx_brush_fallback_faces_window = 0u;
    q2gx_brush_wal_binds_window = 0u;
    q2gx_brush_vertices_window = 0u;


    Q2GX_FreeWorldWALTextureArray(
        q2gx_world_warp_textures,
        q2gx_world_warp_texture_capacity
    );

    q2gx_world_warp_textures = NULL;

    if (q2gx_world_warp_texture_loaded)
    {
        free(q2gx_world_warp_texture_loaded);
        q2gx_world_warp_texture_loaded = NULL;
    }

    if (q2gx_world_warp_texture_failed)
    {
        free(q2gx_world_warp_texture_failed);
        q2gx_world_warp_texture_failed = NULL;
    }

    q2gx_world_warp_texture_capacity = 0u;

    q2gx_world_warp_frames_window = 0u;
    q2gx_world_warp_visible_faces_window = 0u;
    q2gx_world_warp_drawn_faces_window = 0u;
    q2gx_world_warp_translucent_skipped_window = 0u;
    q2gx_world_warp_flowing_faces_window = 0u;
    q2gx_world_warp_texture_binds_window = 0u;
    q2gx_world_warp_subpolys_window = 0u;
    q2gx_world_warp_vertices_window = 0u;
    q2gx_world_warp_load_fail_window = 0u;
    q2gx_world_warp_first_draw_logged = false;

    if (q2gx_world_transwarp_sort)
    {
        free(q2gx_world_transwarp_sort);
        q2gx_world_transwarp_sort = NULL;
    }

    q2gx_world_transwarp_sort_capacity = 0u;
    q2gx_world_transwarp_frames_window = 0u;
    q2gx_world_transwarp_visible_faces_window = 0u;
    q2gx_world_transwarp_drawn_faces_window = 0u;
    q2gx_world_transwarp_alpha33_faces_window = 0u;
    q2gx_world_transwarp_alpha66_faces_window = 0u;
    q2gx_world_transwarp_flowing_faces_window = 0u;
    q2gx_world_transwarp_texture_binds_window = 0u;
    q2gx_world_transwarp_subpolys_window = 0u;
    q2gx_world_transwarp_vertices_window = 0u;
    q2gx_world_transwarp_first_draw_logged = false;


    q2gx_polyblend_frames_window = 0u;
    q2gx_polyblend_active_frames_window = 0u;
    q2gx_polyblend_underwater_frames_window = 0u;
    q2gx_polyblend_quads_window = 0u;
    q2gx_polyblend_alpha_min_window = 0u;
    q2gx_polyblend_alpha_max_window = 0u;
    q2gx_polyblend_underwater_first_logged = false;


    if (q2gx_underwater_copy_buffer)
    {
        free(q2gx_underwater_copy_buffer);
        q2gx_underwater_copy_buffer = NULL;
    }

    q2gx_underwater_copy_bytes = 0u;
    q2gx_underwater_copy_ready = false;

    q2gx_underwater_warp_frames_window = 0u;
    q2gx_underwater_warp_active_frames_window = 0u;
    q2gx_underwater_warp_copies_window = 0u;
    q2gx_underwater_warp_quads_window = 0u;
    q2gx_underwater_warp_vertices_window = 0u;
    q2gx_underwater_warp_alloc_fail_window = 0u;
    q2gx_underwater_warp_bad_refdef_window = 0u;
    q2gx_underwater_warp_first_logged = false;


    if (q2gx_trans_brush_sort)
    {
        free(q2gx_trans_brush_sort);
        q2gx_trans_brush_sort = NULL;
    }

    q2gx_trans_brush_sort_capacity = 0u;

    q2gx_trans_brush_frames_window = 0u;
    q2gx_trans_brush_candidates_window = 0u;
    q2gx_trans_brush_drawn_window = 0u;
    q2gx_trans_brush_alpha33_window = 0u;
    q2gx_trans_brush_alpha66_window = 0u;
    q2gx_trans_brush_wal_binds_window = 0u;
    q2gx_trans_brush_vertices_window = 0u;
    q2gx_trans_brush_alloc_fail_window = 0u;
    q2gx_trans_brush_invalid_window = 0u;
    q2gx_brush_deferred_trans_faces_window = 0u;
    q2gx_trans_brush_first_draw_logged = false;
}


static qboolean Q2GX_LoadWorldGeometry(
    const char *map)
{
    const byte *model_data = NULL;
    unsigned int model_bytes = 0u;
    unsigned int model_count = 0u;

    q2gx_brush_model_t *world_brush_models = NULL;

    unsigned int world_model_first_face = 0u;
    unsigned int world_model_face_count = 0u;


    unsigned int *wal_face_fill_counts = NULL;

    unsigned int wal_batched_face_count = 0u;
    unsigned int wal_max_batch_faces = 0u;
    unsigned int wal_max_batch_vertices = 0u;


    q2gx_world_wal_texture_t *world_wal_textures = NULL;
    unsigned int world_wal_texture_count = 0u;
    unsigned int world_wal_texture_bytes = 0u;

    unsigned int wal_textured_face_count = 0u;
    unsigned int wal_animated_flat_face_count = 0u;
    unsigned int wal_special_flat_face_count = 0u;


    const byte *texinfo_data = NULL;
    unsigned int texinfo_bytes = 0u;
    unsigned int texinfo_count = 0u;

    q2gx_world_texinfo_t *world_texinfos = NULL;

    unsigned int missing_wal_dimensions = 0u;
    unsigned int ordinary_face_count = 0u;
    unsigned int special_face_count = 0u;


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
    if (
        !Q2GX_BSPGetLump(
            file_data,
            file_length,
            Q2GX_BSP_LUMP_MODELS,
            48u,
            &model_data,
            &model_bytes,
            &model_count
        )
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: invalid MODELS lump in %s\n",
            path
        );

        goto fail;
    }

    if (model_count == 0u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: map has no BSP models\n"
        );

        goto fail;
    }

    world_brush_models =
        calloc(
            model_count,
            sizeof(*world_brush_models)
        );

    if (!world_brush_models)
        goto fail;

    {
        unsigned int model_index;

        for (
            model_index = 0u;
            model_index < model_count;
            ++model_index
        )
        {
            const byte *disk_model =
                model_data
                +
                model_index * 48u;

            q2gx_brush_model_t *brush_model =
                &world_brush_models[
                    model_index
                ];

            int32_t first_face_signed;
            int32_t face_count_signed;

            unsigned int axis;

            brush_model->handle.magic =
                Q2GX_MODEL_HANDLE_MAGIC;

            brush_model->handle.kind =
                (
                    model_index == 0u
                )
                ?
                Q2GX_MODEL_KIND_WORLD
                :
                Q2GX_MODEL_KIND_INLINE_BSP;

            brush_model->handle.model_index =
                model_index;

            for (axis = 0u; axis < 3u; ++axis)
            {
                brush_model->mins[axis] =
                    Q2GX_BSPReadLEFloat(
                        disk_model
                        +
                        axis * 4u
                    )
                    -
                    1.0f;

                brush_model->maxs[axis] =
                    Q2GX_BSPReadLEFloat(
                        disk_model
                        +
                        12u
                        +
                        axis * 4u
                    )
                    +
                    1.0f;

                brush_model->origin[axis] =
                    Q2GX_BSPReadLEFloat(
                        disk_model
                        +
                        24u
                        +
                        axis * 4u
                    );
            }

            brush_model->headnode =
                (int)
                (
                    (int32_t)
                    Q2GX_BSPReadLE32(
                        disk_model + 36u
                    )
                );

            first_face_signed =
                (int32_t)
                Q2GX_BSPReadLE32(
                    disk_model + 40u
                );

            face_count_signed =
                (int32_t)
                Q2GX_BSPReadLE32(
                    disk_model + 44u
                );

            if (
                first_face_signed < 0
                ||
                face_count_signed < 0
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BRUSH: "
                    "model *%u has negative face range\n",
                    model_index
                );

                goto fail;
            }

            brush_model->first_face =
                (unsigned int)
                first_face_signed;

            brush_model->face_count =
                (unsigned int)
                face_count_signed;

            if (
                brush_model->first_face
                >
                face_count
                ||
                brush_model->face_count
                >
                face_count
                -
                brush_model->first_face
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BRUSH: "
                    "model *%u invalid face range "
                    "first=%u count=%u total=%u\n",
                    model_index,
                    brush_model->first_face,
                    brush_model->face_count,
                    face_count
                );

                goto fail;
            }
        }
    }

    world_model_first_face =
        world_brush_models[0].first_face;

    world_model_face_count =
        world_brush_models[0].face_count;

    if (world_model_face_count == 0u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: world model has no faces\n"
        );

        goto fail;
    }

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


    if (
        !Q2GX_BSPGetLump(
            file_data,
            file_length,
            Q2GX_BSP_LUMP_TEXINFO,
            76u,
            &texinfo_data,
            &texinfo_bytes,
            &texinfo_count
        )
        ||
        texinfo_count == 0u
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TEXCOORD: invalid texinfo lump in %s\n",
            path
        );

        goto fail;
    }

    if (texinfo_count > SIZE_MAX / sizeof(*world_texinfos))
        goto fail;

    world_texinfos =
        calloc(
            texinfo_count,
            sizeof(*world_texinfos)
        );

    world_wal_textures =
        calloc(
            texinfo_count,
            sizeof(*world_wal_textures)
        );

    if (!world_wal_textures)
        goto fail;


    if (!world_texinfos)
        goto fail;

    for (index = 0u; index < texinfo_count; ++index)
    {
        const byte *disk_texinfo =
            texinfo_data + index * 76u;

        unsigned int component;

        q2gx_world_texinfo_t *texinfo =
            &world_texinfos[index];

        for (component = 0u; component < 4u; ++component)
        {
            texinfo->vecs[0][component] =
                Q2GX_BSPReadLEFloat(
                    disk_texinfo + component * 4u
                );

            texinfo->vecs[1][component] =
                Q2GX_BSPReadLEFloat(
                    disk_texinfo + 16u + component * 4u
                );
        }

        texinfo->flags =
            Q2GX_BSPReadLE32(
                disk_texinfo + 32u
            );

        memcpy(
            texinfo->texture,
            disk_texinfo + 40u,
            32u
        );

        texinfo->texture[32] = '\0';

        texinfo->next_texinfo =
            (int)
            (
                (int32_t)
                Q2GX_BSPReadLE32(
                    disk_texinfo + 72u
                )
            );

        if (
            texinfo->next_texinfo < -1
            ||
            (
                texinfo->next_texinfo >= 0
                &&
                (unsigned int)texinfo->next_texinfo >= texinfo_count
            )
        )
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX WAL: "
                "texinfo %u invalid next=%d\n",
                index,
                texinfo->next_texinfo
            );

            goto fail;
        }

        texinfo->wal_cache_index = -1;


        /*
         * Width/height are lazy-resolved only if a BSP face
         * actually references this texinfo.
         */
        texinfo->width = 0u;
        texinfo->height = 0u;
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

        world_face->texinfo_index =
            (unsigned int)
            Q2GX_BSPReadLE16(
                face + 10u
            );

        if (world_face->texinfo_index >= texinfo_count)
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX TEXCOORD: "
                "face %u invalid texinfo %u\n",
                face_index,
                world_face->texinfo_index
            );

            goto fail;
        }

        if (
            world_texinfos[
                world_face->texinfo_index
            ].width == 0u
            ||
            world_texinfos[
                world_face->texinfo_index
            ].height == 0u
        )
        {
            q2gx_world_texinfo_t *face_texinfo =
                &world_texinfos[
                    world_face->texinfo_index
                ];

            if (
                !Q2GX_ReadWALDimensions(
                    face_texinfo->texture,
                    &face_texinfo->width,
                    &face_texinfo->height
                )
            )
            {
                /*
                 * Stock missing-image fallback is tiny.
                 * base1 is expected to hit this ZERO times.
                 */
                face_texinfo->width = 8u;
                face_texinfo->height = 8u;

                ++missing_wal_dimensions;
            }
        }

        world_face->surface_flags =
            world_texinfos[
                world_face->texinfo_index
            ].flags;

        if (
            world_face->surface_flags
            &
            Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
        )
        {
            ++special_face_count;
            ++wal_special_flat_face_count;
        }
        else
        {
            q2gx_world_texinfo_t *face_texinfo =
                &world_texinfos[
                    world_face->texinfo_index
                ];

            ++ordinary_face_count;

            if (face_texinfo->next_texinfo >= 0)
            {
                ++wal_animated_flat_face_count;
            }
            else
            {
                int cache_index =
                    face_texinfo->wal_cache_index;

                if (cache_index < 0)
                {
                    cache_index =
                        Q2GX_FindWorldWALTexture(
                            world_wal_textures,
                            world_wal_texture_count,
                            face_texinfo->texture
                        );

                    if (cache_index < 0)
                    {
                        q2gx_world_wal_texture_t *wal_texture =
                            &world_wal_textures[
                                world_wal_texture_count
                            ];

                        if (
                            !Q2GX_LoadWorldWALTexture(
                                face_texinfo->texture,
                                wal_texture
                            )
                        )
                        {
                            ri.Con_Printf(
                                PRINT_ALL,
                                "Q2GC REF_GX WAL: load failed %s\n",
                                face_texinfo->texture
                            );

                            goto fail;
                        }

                        cache_index =
                            (int)world_wal_texture_count;

                        world_wal_texture_bytes +=
                            wal_texture->texture_bytes;

                        ++world_wal_texture_count;
                    }

                    face_texinfo->wal_cache_index =
                        cache_index;
                }

                ++wal_textured_face_count;
            }
        }

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

            Q2GX_ComputeWorldDiagnosticST(
                &world_texinfos[
                    world_face->texinfo_index
                ],
                fan_origin,
                &out[0].s,
                &out[0].t
            );

            Q2GX_STORE_WORLD_VERTEX(&out[1], p1);

            Q2GX_ComputeWorldDiagnosticST(
                &world_texinfos[
                    world_face->texinfo_index
                ],
                p1,
                &out[1].s,
                &out[1].t
            );

            Q2GX_STORE_WORLD_VERTEX(&out[2], p2);

            Q2GX_ComputeWorldDiagnosticST(
                &world_texinfos[
                    world_face->texinfo_index
                ],
                p2,
                &out[2].s,
                &out[2].t
            );

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


    /*
     * Q2GC_WORLD_TEXTURE_BATCHING_V1
     *
     * Build exact static ordinary face lists once.
     * No visibility semantics are changed here.
     */
    if (world_wal_texture_count > 0u)
    {
        unsigned int texture_index;

        for (
            texture_index = 0u;
            texture_index < world_wal_texture_count;
            ++texture_index
        )
        {
            world_wal_textures[
                texture_index
            ].face_count = 0u;
        }

        for (
            face_index = world_model_first_face;
            face_index
            <
            world_model_first_face
            +
            world_model_face_count;
            ++face_index
        )
        {
            q2gx_world_face_t *batch_face =
                &world_faces[
                    face_index
                ];

            q2gx_world_texinfo_t *batch_texinfo =
                &world_texinfos[
                    batch_face->texinfo_index
                ];

            unsigned int cache_index;

            if (
                batch_face->surface_flags
                &
                Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
            )
            {
                continue;
            }

            if (batch_texinfo->next_texinfo >= 0)
                continue;

            if (
                batch_texinfo->wal_cache_index < 0
                ||
                (unsigned int)
                batch_texinfo->wal_cache_index
                >=
                world_wal_texture_count
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BATCH: "
                    "static face %u invalid cache=%d\n",
                    face_index,
                    batch_texinfo->wal_cache_index
                );

                goto fail;
            }

            cache_index =
                (unsigned int)
                batch_texinfo->wal_cache_index;

            ++world_wal_textures[
                cache_index
            ].face_count;

            ++wal_batched_face_count;
        }

        for (
            texture_index = 0u;
            texture_index < world_wal_texture_count;
            ++texture_index
        )
        {
            q2gx_world_wal_texture_t *batch_texture =
                &world_wal_textures[
                    texture_index
                ];

            if (batch_texture->face_count == 0u)
            {
                /*
                 * A texture may be used only by an inline BSP
                 * model. Keep it cached for brush entities, but
                 * it has no static-world batch list.
                 */
                continue;
            }

            if (
                batch_texture->face_count
                >
                SIZE_MAX
                /
                sizeof(
                    *batch_texture->face_indices
                )
            )
            {
                goto fail;
            }

            batch_texture->face_indices =
                malloc(
                    batch_texture->face_count
                    *
                    sizeof(
                        *batch_texture->face_indices
                    )
                );

            if (!batch_texture->face_indices)
                goto fail;
        }

        wal_face_fill_counts =
            calloc(
                world_wal_texture_count,
                sizeof(
                    *wal_face_fill_counts
                )
            );

        if (!wal_face_fill_counts)
            goto fail;

        for (
            face_index = world_model_first_face;
            face_index
            <
            world_model_first_face
            +
            world_model_face_count;
            ++face_index
        )
        {
            q2gx_world_face_t *batch_face =
                &world_faces[
                    face_index
                ];

            q2gx_world_texinfo_t *batch_texinfo =
                &world_texinfos[
                    batch_face->texinfo_index
                ];

            unsigned int cache_index;
            unsigned int fill_index;

            if (
                batch_face->surface_flags
                &
                Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
            )
            {
                continue;
            }

            if (batch_texinfo->next_texinfo >= 0)
                continue;

            cache_index =
                (unsigned int)
                batch_texinfo->wal_cache_index;

            fill_index =
                wal_face_fill_counts[
                    cache_index
                ]++;

            if (
                fill_index
                >=
                world_wal_textures[
                    cache_index
                ].face_count
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BATCH: "
                    "face-list overflow cache=%u\n",
                    cache_index
                );

                goto fail;
            }

            world_wal_textures[
                cache_index
            ].face_indices[
                fill_index
            ] =
                face_index;
        }

        for (
            texture_index = 0u;
            texture_index < world_wal_texture_count;
            ++texture_index
        )
        {
            q2gx_world_wal_texture_t *batch_texture =
                &world_wal_textures[
                    texture_index
                ];

            unsigned int list_index;
            unsigned int full_vertices = 0u;

            if (
                wal_face_fill_counts[
                    texture_index
                ]
                !=
                batch_texture->face_count
            )
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BATCH: "
                    "face-list fill mismatch cache=%u "
                    "expected=%u actual=%u\n",
                    texture_index,
                    batch_texture->face_count,
                    wal_face_fill_counts[
                        texture_index
                    ]
                );

                goto fail;
            }

            for (
                list_index = 0u;
                list_index < batch_texture->face_count;
                ++list_index
            )
            {
                unsigned int referenced_face =
                    batch_texture->face_indices[
                        list_index
                    ];

                if (referenced_face >= face_count)
                    goto fail;

                if (
                    UINT_MAX
                    -
                    full_vertices
                    <
                    world_faces[
                        referenced_face
                    ].vertex_count
                )
                {
                    goto fail;
                }

                full_vertices +=
                    world_faces[
                        referenced_face
                    ].vertex_count;
            }

            if (
                batch_texture->face_count
                >
                wal_max_batch_faces
            )
            {
                wal_max_batch_faces =
                    batch_texture->face_count;
            }

            if (
                full_vertices
                >
                wal_max_batch_vertices
            )
            {
                wal_max_batch_vertices =
                    full_vertices;
            }
        }

        free(
            wal_face_fill_counts
        );

        wal_face_fill_counts =
            NULL;
    }

q2gx_world_vertices = world_vertices;
    q2gx_world_faces = world_faces;

    q2gx_world_texinfos = world_texinfos;
    q2gx_world_texinfo_count = texinfo_count;

    q2gx_world_wal_textures =
        world_wal_textures;

    q2gx_world_wal_texture_count =
        world_wal_texture_count;

    q2gx_world_wal_texture_bytes =
        world_wal_texture_bytes;


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

    q2gx_world_static_first_face =
        world_model_first_face;

    q2gx_world_static_face_count =
        world_model_face_count;

    q2gx_brush_models =
        world_brush_models;

    q2gx_brush_model_count =
        model_count;

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

    world_texinfos = NULL;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX TEXCOORD LOAD: "
        "%s "
        "texinfos=%u "
        "ordinary_faces=%u "
        "special_faces=%u "
        "missing_wal_dims=%u "
        "checker=64x64 "
        "mode=diagnostic_checker\n",
        q2gx_world_name,
        q2gx_world_texinfo_count,
        ordinary_face_count,
        special_face_count,
        missing_wal_dimensions
    );


    world_wal_textures = NULL;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX WAL LOAD: "
        "%s "
        "cached_textures=%u "
        "rgba8_bytes=%u "
        "textured_faces=%u "
        "animated_flat_faces=%u "
        "special_flat_faces=%u "
        "mode=wal_mip0\n",
        q2gx_world_name,
        q2gx_world_wal_texture_count,
        q2gx_world_wal_texture_bytes,
        wal_textured_face_count,
        wal_animated_flat_face_count,
        wal_special_flat_face_count
    );


    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX BATCH LOAD: "
        "%s "
        "cached_textures=%u "
        "batched_faces=%u "
        "max_batch_faces=%u "
        "max_batch_vertices=%u "
        "mode=precomputed_wal_face_lists\n",
        q2gx_world_name,
        q2gx_world_wal_texture_count,
        wal_batched_face_count,
        wal_max_batch_faces,
        wal_max_batch_vertices
    );


    world_brush_models = NULL;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX BRUSH LOAD: "
        "%s "
        "models=%u "
        "inline_models=%u "
        "world_first_face=%u "
        "world_faces=%u "
        "inline_faces=%u "
        "total_faces=%u "
        "mode=inline_bsp_model_split\n",
        q2gx_world_name,
        q2gx_brush_model_count,
        q2gx_brush_model_count > 0u
            ? q2gx_brush_model_count - 1u
            : 0u,
        q2gx_world_static_first_face,
        q2gx_world_static_face_count,
        q2gx_world_face_count
            -
            q2gx_world_static_face_count,
        q2gx_world_face_count
    );

return true;

fail:

    if (world_brush_models)
    {
        free(
            world_brush_models
        );

        world_brush_models = NULL;
    }


    if (wal_face_fill_counts)
    {
        free(
            wal_face_fill_counts
        );

        wal_face_fill_counts =
            NULL;
    }


    Q2GX_FreeWorldWALTextureArray(
        world_wal_textures,
        world_wal_texture_count
    );

    world_wal_textures = NULL;


    if (world_texinfos)
    {
        free(world_texinfos);
        world_texinfos = NULL;
    }

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


static qboolean Q2GX_IsStaticWorldFaceIndex(
    unsigned int face_index)
{
    return
        face_index
        >=
        q2gx_world_static_first_face
        &&
        face_index
        -
        q2gx_world_static_first_face
        <
        q2gx_world_static_face_count;
}


static unsigned int Q2GX_MarkAllWorldFacesPVS(void)
{
    unsigned int face_index;

    if (!q2gx_world_faces)
        return 0u;

    for (
        face_index = 0u;
        face_index < q2gx_world_face_count;
        ++face_index
    )
    {
        q2gx_world_faces[
            face_index
        ].pvs_visible_this_frame =
            false;
    }

    for (
        face_index = q2gx_world_static_first_face;
        face_index
        <
        q2gx_world_static_first_face
        +
        q2gx_world_static_face_count;
        ++face_index
    )
    {
        q2gx_world_faces[
            face_index
        ].pvs_visible_this_frame =
            true;
    }

    return
        q2gx_world_static_face_count;
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

            if (
                !Q2GX_IsStaticWorldFaceIndex(
                    referenced_face
                )
            )
            {
                continue;
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



static uint16_t Q2GX_AliasReadLE16(const byte *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t Q2GX_AliasReadLE32(const byte *data)
{
    return
        (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

static f32 Q2GX_AliasReadLEFloat(const byte *data)
{
    uint32_t bits = Q2GX_AliasReadLE32(data);
    f32 value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

static qboolean Q2GX_AliasRangeOK(
    unsigned int offset,
    unsigned int count,
    unsigned int item_size,
    unsigned int file_bytes)
{
    uint64_t end =
        (uint64_t)offset
        +
        (uint64_t)count
        *
        (uint64_t)item_size;

    return offset <= file_bytes && end <= file_bytes;
}

/* Q2GC_ALIAS_MODELS_V1b_CI8
 *
 * GX CI8 is stored as 8x4 texel / 32-byte blocks.
 * Keep the PCX's original 8-bit indices in main memory and let
 * GX resolve them through a 256-entry RGB565 TLUT.
 *
 * A single hardware TLUT slot is deliberately reused. Each alias
 * entity already binds its skin once, so V1b loads that skin's
 * palette into GX_TLUT0 immediately before GX_LoadTexObj().
 */
static unsigned int Q2GX_AliasTextureStorageBytes(
    unsigned int width,
    unsigned int height)
{
    unsigned int padded_width = (width + 7u) & ~7u;
    unsigned int padded_height = (height + 3u) & ~3u;

    return padded_width * padded_height;
}

static void Q2GX_AliasWriteCI8Texel(
    byte *storage,
    unsigned int padded_width,
    unsigned int x,
    unsigned int y,
    byte palette_index)
{
    unsigned int tile_x = x >> 3;
    unsigned int tile_y = y >> 2;
    unsigned int tiles_per_row = padded_width >> 3;
    unsigned int tile_index =
        tile_y * tiles_per_row + tile_x;
    unsigned int within =
        (y & 3u) * 8u + (x & 7u);
    unsigned int base = tile_index * 32u;

    storage[base + within] = palette_index;
}

static u16 Q2GX_AliasPackRGB565(
    byte r,
    byte g,
    byte b)
{
    return
        (u16)(
            ((u16)(r >> 3) << 11)
            | ((u16)(g >> 2) << 5)
            | (u16)(b >> 3)
        );
}

static q2gx_alias_skin_t *Q2GX_FindAliasSkin(const char *name)
{
    q2gx_alias_skin_t *skin;

    for (skin = q2gx_alias_skins; skin; skin = skin->next)
    {
        if (!strcmp(skin->name, name))
            return skin;
    }

    return NULL;
}

static q2gx_alias_skin_t *Q2GX_LoadAliasSkin(const char *name)
{
    q2gx_alias_skin_t *cached;
    byte *file_data = NULL;
    int file_length;

    unsigned int xmin, ymin, xmax, ymax;
    unsigned int width, height, bytes_per_line;
    unsigned int palette_offset, data_end;
    unsigned int source_pos, y;
    unsigned int padded_width, storage_bytes;
    unsigned int palette_bytes;

    byte *indices = NULL;
    q2gx_alias_skin_t *skin = NULL;

    if (!name || !name[0])
        return NULL;

    cached = Q2GX_FindAliasSkin(name);
    if (cached)
        return cached;

    file_length =
        ri.FS_LoadFile(
            (char *)name,
            (void **)&file_data
        );

    if (file_length < 897 || !file_data)
    {
        if (file_data)
            ri.FS_FreeFile(file_data);
        return NULL;
    }

    if (
        file_data[0] != 0x0Au
        || file_data[1] != 5u
        || file_data[2] != 1u
        || file_data[3] != 8u
        || file_data[65] != 1u
    )
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    xmin = Q2GX_AliasReadLE16(file_data + 4u);
    ymin = Q2GX_AliasReadLE16(file_data + 6u);
    xmax = Q2GX_AliasReadLE16(file_data + 8u);
    ymax = Q2GX_AliasReadLE16(file_data + 10u);

    if (xmax < xmin || ymax < ymin)
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    width = xmax - xmin + 1u;
    height = ymax - ymin + 1u;
    bytes_per_line = Q2GX_AliasReadLE16(file_data + 66u);

    if (
        width == 0u
        || height == 0u
        || width > 1024u
        || height > 1024u
        || bytes_per_line < width
    )
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    palette_offset = (unsigned int)file_length - 768u;

    data_end =
        (
            palette_offset > 0u
            && file_data[palette_offset - 1u] == 0x0Cu
        )
        ?
        palette_offset - 1u
        :
        palette_offset;

    if (data_end <= 128u)
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    indices = malloc(width * height);
    if (!indices)
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    source_pos = 128u;

    for (y = 0u; y < height; ++y)
    {
        unsigned int x = 0u;

        while (x < bytes_per_line)
        {
            unsigned int run = 1u;
            byte value;
            byte token;

            if (source_pos >= data_end)
                goto fail;

            token = file_data[source_pos++];

            if ((token & 0xC0u) == 0xC0u)
            {
                run = token & 0x3Fu;

                if (run == 0u || source_pos >= data_end)
                    goto fail;

                value = file_data[source_pos++];
            }
            else
            {
                value = token;
            }

            while (run > 0u && x < bytes_per_line)
            {
                if (x < width)
                    indices[y * width + x] = value;

                ++x;
                --run;
            }
        }
    }

    skin = calloc(1u, sizeof(*skin));
    if (!skin)
        goto fail;

    padded_width = (width + 7u) & ~7u;
    storage_bytes =
        Q2GX_AliasTextureStorageBytes(width, height);

    palette_bytes = 256u * sizeof(u16);

    skin->storage = memalign(32, storage_bytes);
    if (!skin->storage)
        goto fail;

    skin->palette_storage =
        memalign(32, palette_bytes);

    if (!skin->palette_storage)
        goto fail;

    memset(skin->storage, 0, storage_bytes);

    for (y = 0u; y < height; ++y)
    {
        unsigned int x;

        for (x = 0u; x < width; ++x)
        {
            byte palette_index = indices[y * width + x];

            Q2GX_AliasWriteCI8Texel(
                (byte *)skin->storage,
                padded_width,
                x,
                y,
                palette_index
            );
        }
    }

    for (y = 0u; y < 256u; ++y)
    {
        const byte *rgb =
            file_data
            + palette_offset
            + y * 3u;

        ((u16 *)skin->palette_storage)[y] =
            Q2GX_AliasPackRGB565(
                rgb[0],
                rgb[1],
                rgb[2]
            );
    }

    DCStoreRange(skin->storage, storage_bytes);
    DCStoreRange(skin->palette_storage, palette_bytes);

    GX_InitTlutObj(
        &skin->tlut,
        skin->palette_storage,
        GX_TL_RGB565,
        256u
    );

    GX_InitTexObjCI(
        &skin->texture,
        skin->storage,
        (u16)width,
        (u16)height,
        GX_TF_CI8,
        GX_CLAMP,
        GX_CLAMP,
        GX_FALSE,
        GX_TLUT0
    );

    GX_InitTexObjFilterMode(
        &skin->texture,
        GX_NEAR,
        GX_NEAR
    );

    snprintf(skin->name, sizeof(skin->name), "%s", name);
    skin->width = width;
    skin->height = height;
    skin->storage_bytes = storage_bytes;
    skin->palette_bytes = palette_bytes;

    skin->next = q2gx_alias_skins;
    q2gx_alias_skins = skin;

    ++q2gx_alias_registered_skins;
    q2gx_alias_skin_bytes += storage_bytes;
    q2gx_alias_tlut_bytes += palette_bytes;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX ALIAS SKIN LOAD: "
        "%s size=%ux%u ci8_bytes=%u tlut_bytes=%u "
        "tlut=GX_TLUT0 mode=pcx_ci8_rgb565\n",
        skin->name,
        skin->width,
        skin->height,
        skin->storage_bytes,
        skin->palette_bytes
    );

    free(indices);
    ri.FS_FreeFile(file_data);

    return skin;

fail:
    if (skin)
    {
        if (skin->storage)
            free(skin->storage);

        if (skin->palette_storage)
            free(skin->palette_storage);

        free(skin);
    }

    if (indices)
        free(indices);

    if (file_data)
        ri.FS_FreeFile(file_data);

    return NULL;
}

static q2gx_alias_model_t *Q2GX_FindAliasModel(const char *name)
{
    q2gx_alias_model_t *model;

    for (model = q2gx_alias_models; model; model = model->next)
    {
        if (!strcmp(model->name, name))
            return model;
    }

    return NULL;
}

static void Q2GX_FreeUnlinkedAliasModel(q2gx_alias_model_t *model)
{
    if (!model)
        return;

    free(model->skins);
    free(model->st);
    free(model->triangles);
    free(model->frames);
    free(model->frame_vertices);
    free(model);
}

static struct model_s *Q2GX_RegisterAliasModel(const char *name)
{
    q2gx_alias_model_t *cached;
    q2gx_alias_model_t *model = NULL;

    byte *file_data = NULL;
    int file_length;

    uint32_t ident;
    int32_t version;
    int32_t skin_width, skin_height, frame_size;
    int32_t num_skins, num_xyz, num_st, num_tris, num_frames;
    int32_t ofs_skins, ofs_st, ofs_tris, ofs_frames, ofs_end;

    unsigned int i;
    size_t name_length;

    if (!name || !name[0])
        return NULL;

    name_length = strlen(name);

    if (
        name_length < 4u
        || strcmp(name + name_length - 4u, ".md2")
    )
    {
        return NULL;
    }

    if (name[0] == '#')
        return NULL;

    cached = Q2GX_FindAliasModel(name);
    if (cached)
        return &cached->handle;

    file_length =
        ri.FS_LoadFile(
            (char *)name,
            (void **)&file_data
        );

    if (file_length < 68 || !file_data)
    {
        if (file_data)
            ri.FS_FreeFile(file_data);

        return NULL;
    }

    ident = Q2GX_AliasReadLE32(file_data + 0u);
    version = (int32_t)Q2GX_AliasReadLE32(file_data + 4u);
    skin_width = (int32_t)Q2GX_AliasReadLE32(file_data + 8u);
    skin_height = (int32_t)Q2GX_AliasReadLE32(file_data + 12u);
    frame_size = (int32_t)Q2GX_AliasReadLE32(file_data + 16u);
    num_skins = (int32_t)Q2GX_AliasReadLE32(file_data + 20u);
    num_xyz = (int32_t)Q2GX_AliasReadLE32(file_data + 24u);
    num_st = (int32_t)Q2GX_AliasReadLE32(file_data + 28u);
    num_tris = (int32_t)Q2GX_AliasReadLE32(file_data + 32u);
    num_frames = (int32_t)Q2GX_AliasReadLE32(file_data + 40u);
    ofs_skins = (int32_t)Q2GX_AliasReadLE32(file_data + 44u);
    ofs_st = (int32_t)Q2GX_AliasReadLE32(file_data + 48u);
    ofs_tris = (int32_t)Q2GX_AliasReadLE32(file_data + 52u);
    ofs_frames = (int32_t)Q2GX_AliasReadLE32(file_data + 56u);
    ofs_end = (int32_t)Q2GX_AliasReadLE32(file_data + 64u);

    if (
        ident != 0x32504449u
        || version != 8
        || skin_width <= 0
        || skin_height <= 0
        || skin_width > 1024
        || skin_height > 1024
        || frame_size <= 0
        || num_skins < 0
        || num_skins > (int32_t)Q2GX_ALIAS_MAX_SKINS
        || num_xyz <= 0
        || num_xyz > (int32_t)Q2GX_ALIAS_MAX_XYZ
        || num_st <= 0
        || num_st > (int32_t)Q2GX_ALIAS_MAX_ST
        || num_tris <= 0
        || num_tris > (int32_t)Q2GX_ALIAS_MAX_TRIANGLES
        || num_frames <= 0
        || num_frames > (int32_t)Q2GX_ALIAS_MAX_FRAMES
        || frame_size < 40 + num_xyz * 4
        || ofs_skins < 0
        || ofs_st < 0
        || ofs_tris < 0
        || ofs_frames < 0
        || ofs_end < 68
        || ofs_end > file_length
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX ALIAS REGISTER: invalid header %s\n",
            name
        );

        ri.FS_FreeFile(file_data);
        return NULL;
    }

    if (
        !Q2GX_AliasRangeOK(
            (unsigned int)ofs_skins,
            (unsigned int)num_skins,
            64u,
            (unsigned int)ofs_end
        )
        || !Q2GX_AliasRangeOK(
            (unsigned int)ofs_st,
            (unsigned int)num_st,
            4u,
            (unsigned int)ofs_end
        )
        || !Q2GX_AliasRangeOK(
            (unsigned int)ofs_tris,
            (unsigned int)num_tris,
            12u,
            (unsigned int)ofs_end
        )
        || !Q2GX_AliasRangeOK(
            (unsigned int)ofs_frames,
            (unsigned int)num_frames,
            (unsigned int)frame_size,
            (unsigned int)ofs_end
        )
    )
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    model = calloc(1u, sizeof(*model));
    if (!model)
    {
        ri.FS_FreeFile(file_data);
        return NULL;
    }

    model->handle.magic = Q2GX_MODEL_HANDLE_MAGIC;
    model->handle.kind = Q2GX_MODEL_KIND_ALIAS_MD2;
    model->handle.model_index = ++q2gx_alias_model_serial;

    snprintf(model->name, sizeof(model->name), "%s", name);

    model->skin_width = (unsigned int)skin_width;
    model->skin_height = (unsigned int)skin_height;
    model->num_skins = (unsigned int)num_skins;
    model->num_xyz = (unsigned int)num_xyz;
    model->num_st = (unsigned int)num_st;
    model->num_tris = (unsigned int)num_tris;
    model->num_frames = (unsigned int)num_frames;
    model->source_bytes = (unsigned int)file_length;

    if (model->num_skins > 0u)
    {
        model->skins =
            calloc(
                model->num_skins,
                sizeof(*model->skins)
            );

        if (!model->skins)
            goto fail;
    }

    model->st =
        calloc(model->num_st, sizeof(*model->st));

    model->triangles =
        calloc(model->num_tris, sizeof(*model->triangles));

    model->frames =
        calloc(model->num_frames, sizeof(*model->frames));

    model->frame_storage_bytes =
        model->num_frames * model->num_xyz * 4u;

    model->frame_vertices =
        malloc(model->frame_storage_bytes);

    if (
        !model->st
        || !model->triangles
        || !model->frames
        || !model->frame_vertices
    )
    {
        goto fail;
    }

    for (i = 0u; i < model->num_st; ++i)
    {
        const byte *disk_st =
            file_data + (unsigned int)ofs_st + i * 4u;

        model->st[i].s =
            (int16_t)Q2GX_AliasReadLE16(disk_st);

        model->st[i].t =
            (int16_t)Q2GX_AliasReadLE16(disk_st + 2u);
    }

    for (i = 0u; i < model->num_tris; ++i)
    {
        const byte *disk_tri =
            file_data + (unsigned int)ofs_tris + i * 12u;

        unsigned int corner;

        for (corner = 0u; corner < 3u; ++corner)
        {
            uint16_t xyz_index =
                Q2GX_AliasReadLE16(
                    disk_tri + corner * 2u
                );

            uint16_t st_index =
                Q2GX_AliasReadLE16(
                    disk_tri + 6u + corner * 2u
                );

            if (
                xyz_index >= model->num_xyz
                || st_index >= model->num_st
            )
            {
                goto fail;
            }

            model->triangles[i].xyz[corner] = xyz_index;
            model->triangles[i].st[corner] = st_index;
        }
    }

    for (i = 0u; i < model->num_frames; ++i)
    {
        const byte *disk_frame =
            file_data
            + (unsigned int)ofs_frames
            + i * (unsigned int)frame_size;

        q2gx_alias_frame_t *frame = &model->frames[i];
        unsigned int axis;

        for (axis = 0u; axis < 3u; ++axis)
        {
            frame->scale[axis] =
                Q2GX_AliasReadLEFloat(
                    disk_frame + axis * 4u
                );

            frame->translate[axis] =
                Q2GX_AliasReadLEFloat(
                    disk_frame + 12u + axis * 4u
                );
        }

        frame->verts =
            model->frame_vertices
            + i * model->num_xyz * 4u;

        memcpy(
            frame->verts,
            disk_frame + 40u,
            model->num_xyz * 4u
        );
    }

    for (i = 0u; i < model->num_skins; ++i)
    {
        const byte *disk_name =
            file_data
            + (unsigned int)ofs_skins
            + i * 64u;

        char skin_name[65];
        unsigned int length = 0u;

        while (length < 64u && disk_name[length] != 0u)
        {
            skin_name[length] = (char)disk_name[length];
            ++length;
        }

        skin_name[length] = '\0';

        if (!skin_name[0])
            continue;

        model->skins[i] =
            Q2GX_LoadAliasSkin(skin_name);

        if (!model->skins[i])
            goto fail;
    }

    model->next = q2gx_alias_models;
    q2gx_alias_models = model;

    ++q2gx_alias_registered_models;
    q2gx_alias_source_bytes += model->source_bytes;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX ALIAS REGISTER: "
        "%s handle=%u skins=%u xyz=%u st=%u tris=%u "
        "frames=%u skin_size=%ux%u source_bytes=%u "
        "frame_bytes=%u mode=md2_alias_handle\n",
        model->name,
        model->handle.model_index,
        model->num_skins,
        model->num_xyz,
        model->num_st,
        model->num_tris,
        model->num_frames,
        model->skin_width,
        model->skin_height,
        model->source_bytes,
        model->frame_storage_bytes
    );

    ri.FS_FreeFile(file_data);
    return &model->handle;

fail:
    Q2GX_FreeUnlinkedAliasModel(model);
    ri.FS_FreeFile(file_data);
    return NULL;
}

static void Q2GX_InitAliasTransform(
    const entity_t *entity,
    q2gx_alias_transform_t *transform)
{
    const f32 deg = 0.01745329251994329577f;

    f32 roll = -entity->angles[2] * deg;
    f32 pitch = entity->angles[0] * deg;
    f32 yaw = entity->angles[1] * deg;

    memset(transform, 0, sizeof(*transform));

    transform->origin[0] = entity->origin[0];
    transform->origin[1] = entity->origin[1];
    transform->origin[2] = entity->origin[2];

    transform->sin_roll = sinf(roll);
    transform->cos_roll = cosf(roll);
    transform->sin_pitch = sinf(pitch);
    transform->cos_pitch = cosf(pitch);
    transform->sin_yaw = sinf(yaw);
    transform->cos_yaw = cosf(yaw);
}

static void Q2GX_TransformAliasPoint(
    const q2gx_alias_transform_t *transform,
    f32 local_x,
    f32 local_y,
    f32 local_z,
    f32 *world_x,
    f32 *world_y,
    f32 *world_z)
{
    f32 x1 = local_x;
    f32 y1 =
        transform->cos_roll * local_y
        - transform->sin_roll * local_z;
    f32 z1 =
        transform->sin_roll * local_y
        + transform->cos_roll * local_z;

    f32 x2 =
        transform->cos_pitch * x1
        + transform->sin_pitch * z1;
    f32 y2 = y1;
    f32 z2 =
        -transform->sin_pitch * x1
        + transform->cos_pitch * z1;

    *world_x =
        transform->cos_yaw * x2
        - transform->sin_yaw * y2
        + transform->origin[0];

    *world_y =
        transform->sin_yaw * x2
        + transform->cos_yaw * y2
        + transform->origin[1];

    *world_z = z2 + transform->origin[2];
}

static void Q2GX_InitAliasLerp(
    const q2gx_alias_model_t *model,
    const entity_t *entity,
    unsigned int frame_index,
    unsigned int old_frame_index,
    q2gx_alias_lerp_t *lerp)
{
    vec3_t delta;
    vec3_t vectors[3];
    unsigned int axis;

    memset(lerp, 0, sizeof(*lerp));

    lerp->frame = &model->frames[frame_index];
    lerp->old_frame = &model->frames[old_frame_index];

    lerp->backlerp = entity->backlerp;
    lerp->frontlerp = 1.0f - lerp->backlerp;

    VectorSubtract(
        entity->oldorigin,
        entity->origin,
        delta
    );

    AngleVectors(
        entity->angles,
        vectors[0],
        vectors[1],
        vectors[2]
    );

    lerp->move[0] = DotProduct(delta, vectors[0]);
    lerp->move[1] = -DotProduct(delta, vectors[1]);
    lerp->move[2] = DotProduct(delta, vectors[2]);

    for (axis = 0u; axis < 3u; ++axis)
    {
        lerp->move[axis] += lerp->old_frame->translate[axis];

        lerp->move[axis] =
            lerp->backlerp * lerp->move[axis]
            + lerp->frontlerp * lerp->frame->translate[axis];

        lerp->frontv[axis] =
            lerp->frontlerp * lerp->frame->scale[axis];

        lerp->backv[axis] =
            lerp->backlerp * lerp->old_frame->scale[axis];
    }
}

static void Q2GX_LerpAliasVertex(
    const q2gx_alias_lerp_t *lerp,
    unsigned int vertex_index,
    f32 *x,
    f32 *y,
    f32 *z)
{
    const byte *new_vertex =
        lerp->frame->verts + vertex_index * 4u;

    const byte *old_vertex =
        lerp->old_frame->verts + vertex_index * 4u;

    *x =
        lerp->move[0]
        + (f32)old_vertex[0] * lerp->backv[0]
        + (f32)new_vertex[0] * lerp->frontv[0];

    *y =
        lerp->move[1]
        + (f32)old_vertex[1] * lerp->backv[1]
        + (f32)new_vertex[1] * lerp->frontv[1];

    *z =
        lerp->move[2]
        + (f32)old_vertex[2] * lerp->backv[2]
        + (f32)new_vertex[2] * lerp->frontv[2];
}

static q2gx_alias_skin_t *Q2GX_SelectAliasSkin(
    const q2gx_alias_model_t *model,
    const entity_t *entity,
    qboolean *invalid_skin,
    qboolean *custom_skin_fallback)
{
    int skin_index;

    *invalid_skin = false;
    *custom_skin_fallback = false;

    if (model->num_skins == 0u || !model->skins)
    {
        *invalid_skin = true;
        return NULL;
    }

    if (entity->skin)
    {
        *custom_skin_fallback = true;
        skin_index = 0;
    }
    else
    {
        skin_index = entity->skinnum;
    }

    if (
        skin_index < 0
        || (unsigned int)skin_index >= model->num_skins
        || !model->skins[(unsigned int)skin_index]
    )
    {
        *invalid_skin = true;
        skin_index = 0;
    }

    return model->skins[(unsigned int)skin_index];
}

static void Q2GX_SetupAlias3D(void)
{
    GX_SetNumTexGens(1);

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_REPLACE
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

    GX_SetColorUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);
}

/*
 * Q2GC_TRANSLUCENT_ALIAS_V1D_SHARED_CORE_INPUT_RESTORE
 *
 * Q2GX_SetupWorld3D() intentionally installs the untextured
 * POS+CLR world contract. The ordinary alias stream submits
 * POS+CLR+TEX0. Rebuild that complete input contract before an
 * alias draw that follows the special view-weapon pass.
 */
static void Q2GX_RebuildAliasInputContract(void)
{
    GX_ClearVtxDesc();

    GX_SetVtxDesc(
        GX_VA_POS,
        GX_DIRECT
    );

    GX_SetVtxDesc(
        GX_VA_CLR0,
        GX_DIRECT
    );

    GX_SetVtxDesc(
        GX_VA_TEX0,
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

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_TEX0,
        GX_TEX_ST,
        GX_F32,
        0
    );

    GX_SetNumChans(1);
    GX_SetNumTexGens(1);

    GX_SetTexCoordGen(
        GX_TEXCOORD0,
        GX_TG_MTX2x4,
        GX_TG_TEX0,
        GX_IDENTITY
    );

    GX_SetNumTevStages(1);

    GX_SetTevOrder(
        GX_TEVSTAGE0,
        GX_TEXCOORD0,
        GX_TEXMAP0,
        GX_COLOR0A0
    );
}

/*
 * Q2GC_PARTICLES_V1
 *
 * Stock Quake II accelerated GL path:
 *   particle_t = origin + palette color index + alpha
 *   GL_POINTS
 *   SRC_ALPHA / ONE_MINUS_SRC_ALPHA
 *   depth writes disabled
 *
 * Native GX V1 deliberately uses a fixed 3-pixel point.
 * GX point sizes are specified in 1/6-pixel units:
 *   18 == 3 pixels.
 *
 * No texture is allocated or sampled.
 */
static void Q2GX_SetupParticles3D(refdef_t *fd)
{
    Q2GX_SetupWorld3D(fd);

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

    GX_SetNumChans(1);
    GX_SetNumTexGens(0);
    GX_SetNumTevStages(1);

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

    GX_SetBlendMode(
        GX_BM_BLEND,
        GX_BL_SRCALPHA,
        GX_BL_INVSRCALPHA,
        GX_LO_CLEAR
    );

    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_FALSE
    );

    GX_SetColorUpdate(GX_TRUE);
    GX_SetCullMode(GX_CULL_NONE);

    GX_SetPointSize(
        (u8)Q2GX_PARTICLE_POINT_SIZE_SIXTHS,
        GX_TO_ZERO
    );
}


static void Q2GX_DrawParticles(refdef_t *fd)
{
    unsigned int particle_count = 0u;
    unsigned int vertices = 0u;
    unsigned int draw_calls = 0u;
    unsigned int invalid_colors = 0u;
    unsigned int alpha_clamped = 0u;
    unsigned int color_e0 = 0u;
    unsigned int color_d0 = 0u;

    unsigned int first_color = 0u;
    unsigned int first_alpha_u8 = 0u;
    f32 first_alpha = 0.0f;

    unsigned int base;

    if (!fd)
        return;

    ++q2gx_particles_frames_window;

    if (
        fd->num_particles > 0
        &&
        fd->particles
        &&
        q2gx_palette_loaded
    )
    {
        particle_count =
            (unsigned int)fd->num_particles;

        ++q2gx_particles_frames_with_particles_window;

        if (
            particle_count
            <
            q2gx_particles_min_frame_window
        )
        {
            q2gx_particles_min_frame_window =
                particle_count;
        }

        if (
            particle_count
            >
            q2gx_particles_max_frame_window
        )
        {
            q2gx_particles_max_frame_window =
                particle_count;
        }

        Q2GX_SetupParticles3D(fd);

        base = 0u;

        while (base < particle_count)
        {
            unsigned int remaining =
                particle_count - base;

            unsigned int chunk =
                remaining > 65535u
                ?
                65535u
                :
                remaining;

            unsigned int i;

            GX_Begin(
                GX_POINTS,
                GX_VTXFMT0,
                (u16)chunk
            );

            for (i = 0u; i < chunk; ++i)
            {
                const particle_t *particle =
                    &fd->particles[base + i];

                unsigned int color_index;
                f32 alpha;
                unsigned int alpha_u8;

                if (
                    particle->color < 0
                    ||
                    particle->color > 255
                )
                {
                    ++invalid_colors;
                    color_index = 0u;
                }
                else
                {
                    color_index =
                        (unsigned int)particle->color;
                }

                alpha = particle->alpha;

                if (alpha < 0.0f)
                {
                    alpha = 0.0f;
                    ++alpha_clamped;
                }
                else if (alpha > 1.0f)
                {
                    alpha = 1.0f;
                    ++alpha_clamped;
                }

                /*
                 * Match stock point path:
                 *     color[3] = p->alpha * 255;
                 */
                alpha_u8 =
                    (unsigned int)(
                        alpha * 255.0f
                    );

                if (alpha_u8 > 255u)
                    alpha_u8 = 255u;

                if (color_index == 0xe0u)
                    ++color_e0;

                if (color_index == 0xd0u)
                    ++color_d0;

                if (base == 0u && i == 0u)
                {
                    first_color = color_index;
                    first_alpha = particle->alpha;
                    first_alpha_u8 = alpha_u8;
                }

                GX_Position3f32(
                    particle->origin[0],
                    particle->origin[1],
                    particle->origin[2]
                );

                GX_Color4u8(
                    q2gx_palette[color_index].r,
                    q2gx_palette[color_index].g,
                    q2gx_palette[color_index].b,
                    (u8)alpha_u8
                );
            }

            GX_End();

            ++draw_calls;
            vertices += chunk;
            base += chunk;
        }

        Q2GX_SetupWorld3D(fd);

        if (!q2gx_particles_first_draw_logged)
        {
            q2gx_particles_first_draw_logged = true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX PARTICLES FIRST DRAW: "
                "count=%u "
                "first_color=%u "
                "first_alpha=%.4f "
                "first_alpha_u8=%u "
                "point_sixths=%u "
                "point_pixels=3.00 "
                "mode=gx_points_palette_alpha_v1\n",
                particle_count,
                first_color,
                first_alpha,
                first_alpha_u8,
                Q2GX_PARTICLE_POINT_SIZE_SIXTHS
            );
        }
    }

    q2gx_particles_count_window +=
        particle_count;

    q2gx_particles_vertices_window +=
        vertices;

    q2gx_particles_draw_calls_window +=
        draw_calls;

    q2gx_particles_invalid_color_window +=
        invalid_colors;

    q2gx_particles_alpha_clamped_window +=
        alpha_clamped;

    q2gx_particles_color_e0_window +=
        color_e0;

    q2gx_particles_color_d0_window +=
        color_d0;

    if (q2gx_particles_frames_window >= 120u)
    {
        unsigned int min_frame =
            q2gx_particles_frames_with_particles_window > 0u
            ?
            q2gx_particles_min_frame_window
            :
            0u;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX PARTICLES 120: "
            "frames=%u "
            "frames_with_particles=%u "
            "particles_total=%u "
            "vertices_total=%u "
            "draw_calls_total=%u "
            "invalid_color_total=%u "
            "alpha_clamped_total=%u "
            "color_e0_total=%u "
            "color_d0_total=%u "
            "min_particles_frame=%u "
            "max_particles_frame=%u "
            "point_sixths=%u "
            "mode=gx_points_palette_alpha_v1\n",
            q2gx_particles_frames_window,
            q2gx_particles_frames_with_particles_window,
            q2gx_particles_count_window,
            q2gx_particles_vertices_window,
            q2gx_particles_draw_calls_window,
            q2gx_particles_invalid_color_window,
            q2gx_particles_alpha_clamped_window,
            q2gx_particles_color_e0_window,
            q2gx_particles_color_d0_window,
            min_frame,
            q2gx_particles_max_frame_window,
            Q2GX_PARTICLE_POINT_SIZE_SIXTHS
        );

        q2gx_particles_frames_window = 0u;
        q2gx_particles_frames_with_particles_window = 0u;
        q2gx_particles_count_window = 0u;
        q2gx_particles_vertices_window = 0u;
        q2gx_particles_draw_calls_window = 0u;
        q2gx_particles_invalid_color_window = 0u;
        q2gx_particles_alpha_clamped_window = 0u;
        q2gx_particles_color_e0_window = 0u;
        q2gx_particles_color_d0_window = 0u;
        q2gx_particles_min_frame_window = 0xffffffffu;
        q2gx_particles_max_frame_window = 0u;
    }
}



static void Q2GX_DrawAliasEntities(refdef_t *fd)
{
    unsigned int entity_index;

    unsigned int entities_drawn = 0u;
    unsigned int weapon_skipped = 0u;
    unsigned int translucent_skipped = 0u;
    unsigned int beam_skipped = 0u;
    unsigned int triangles_drawn = 0u;
    unsigned int vertices_drawn = 0u;
    unsigned int skin_binds = 0u;
    unsigned int tlut_loads = 0u;
    unsigned int invalid_frames = 0u;
    unsigned int invalid_skins = 0u;
    unsigned int custom_skin_fallbacks = 0u;
    unsigned int animated_samples = 0u;

    if (!fd)
        return;

    if (q2gx_alias_translucent_pass)
        ++q2gx_transalias_frames_window;
    else
        ++q2gx_alias_frames_window;

    if (fd->num_entities > 0 && fd->entities)
    {
        for (
            entity_index = 0u;
            entity_index < (unsigned int)fd->num_entities;
            ++entity_index
        )
        {
            entity_t *entity = &fd->entities[entity_index];
            struct model_s *handle;
            q2gx_alias_model_t *model;

            unsigned int frame_index;
            unsigned int old_frame_index;

            q2gx_alias_lerp_t lerp;
            q2gx_alias_transform_t transform;
            q2gx_alias_skin_t *skin;

            qboolean invalid_skin;
            qboolean custom_skin_fallback;
            unsigned int transalias_alpha_u8 = 255u;
            unsigned int triangle_index;

            if (!entity->model)
                continue;

            handle = entity->model;

            if (
                handle->magic != Q2GX_MODEL_HANDLE_MAGIC
                || handle->kind != Q2GX_MODEL_KIND_ALIAS_MD2
            )
            {
                continue;
            }

            model = (q2gx_alias_model_t *)handle;

            if (entity->flags & RF_WEAPONMODEL)
            {
                ++weapon_skipped;
                continue;
            }

            if (entity->flags & RF_BEAM)
            {
                ++beam_skipped;
                continue;
            }

            if (q2gx_alias_translucent_pass)
            {
                f32 transalias_alpha;

                if (!(entity->flags & RF_TRANSLUCENT))
                    continue;

                ++q2gx_transalias_seen_window;

                if (
                    entity->flags
                    &
                    (
                        RF_SHELL_RED
                        |
                        RF_SHELL_GREEN
                        |
                        RF_SHELL_BLUE
                        |
                        RF_SHELL_DOUBLE
                        |
                        RF_SHELL_HALF_DAM
                    )
                )
                {
                    ++q2gx_transalias_shell_skipped_window;
                    continue;
                }

                if (entity->flags & RF_BEAM)
                    continue;

                transalias_alpha = entity->alpha;

                if (transalias_alpha < 0.0f)
                    transalias_alpha = 0.0f;
                else if (transalias_alpha > 1.0f)
                    transalias_alpha = 1.0f;

                transalias_alpha_u8 =
                    (unsigned int)(
                        transalias_alpha * 255.0f + 0.5f
                    );

                if (
                    transalias_alpha_u8
                    <
                    q2gx_transalias_alpha_min_window
                )
                {
                    q2gx_transalias_alpha_min_window =
                        transalias_alpha_u8;
                }

                if (
                    transalias_alpha_u8
                    >
                    q2gx_transalias_alpha_max_window
                )
                {
                    q2gx_transalias_alpha_max_window =
                        transalias_alpha_u8;
                }
            }
            else if (entity->flags & RF_TRANSLUCENT)
            {
                ++translucent_skipped;
                continue;
            }

            if (
                entity->frame < 0
                || (unsigned int)entity->frame >= model->num_frames
            )
            {
                ++invalid_frames;
                frame_index = 0u;
                old_frame_index = 0u;
            }
            else if (
                entity->oldframe < 0
                || (unsigned int)entity->oldframe >= model->num_frames
            )
            {
                ++invalid_frames;
                frame_index = 0u;
                old_frame_index = 0u;
            }
            else
            {
                frame_index = (unsigned int)entity->frame;
                old_frame_index = (unsigned int)entity->oldframe;
            }

            skin =
                Q2GX_SelectAliasSkin(
                    model,
                    entity,
                    &invalid_skin,
                    &custom_skin_fallback
                );

            if (invalid_skin)
                ++invalid_skins;

            if (custom_skin_fallback)
                ++custom_skin_fallbacks;

            if (!skin)
                continue;

            if (
                frame_index != old_frame_index
                || (
                    entity->backlerp > 0.0f
                    && entity->backlerp < 1.0f
                )
            )
            {
                ++animated_samples;
            }

            Q2GX_InitAliasLerp(
                model,
                entity,
                frame_index,
                old_frame_index,
                &lerp
            );

            Q2GX_InitAliasTransform(
                entity,
                &transform
            );

            Q2GX_SetupAlias3D();

            /*
             * Q2GC_TRANSLUCENT_ALIAS_V1D_SHARED_CORE_INPUT_RESTORE
             *
             * Probe J proved that rebuilding the alias input contract
             * fixes the post-viewweapon opaque alias stream.  V1d
             * now layers the complete, separately proven translucent
             * state on that repaired stream, smoke-only.
             */
            if (q2gx_alias_translucent_pass)
            {
                GX_SetTevOp(
                    GX_TEVSTAGE0,
                    GX_MODULATE
                );

                GX_SetBlendMode(
                    GX_BM_BLEND,
                    GX_BL_SRCALPHA,
                    GX_BL_INVSRCALPHA,
                    GX_LO_CLEAR
                );

                GX_SetZMode(
                    GX_TRUE,
                    GX_LEQUAL,
                    GX_FALSE
                );
            }

            GX_LoadTlut(
                &skin->tlut,
                GX_TLUT0
            );

            ++tlut_loads;

            GX_LoadTexObj(
                &skin->texture,
                GX_TEXMAP0
            );

            ++skin_binds;

            GX_Begin(
                GX_TRIANGLES,
                GX_VTXFMT0,
                (u16)(model->num_tris * 3u)
            );

            for (
                triangle_index = 0u;
                triangle_index < model->num_tris;
                ++triangle_index
            )
            {
                const q2gx_alias_triangle_t *triangle =
                    &model->triangles[triangle_index];

                unsigned int corner;

                for (corner = 0u; corner < 3u; ++corner)
                {
                    unsigned int xyz_index =
                        triangle->xyz[corner];

                    unsigned int st_index =
                        triangle->st[corner];

                    const q2gx_alias_st_t *st =
                        &model->st[st_index];

                    f32 local_x, local_y, local_z;
                    f32 world_x, world_y, world_z;

                    Q2GX_LerpAliasVertex(
                        &lerp,
                        xyz_index,
                        &local_x,
                        &local_y,
                        &local_z
                    );

                    Q2GX_TransformAliasPoint(
                        &transform,
                        local_x,
                        local_y,
                        local_z,
                        &world_x,
                        &world_y,
                        &world_z
                    );

                    GX_Position3f32(
                        world_x,
                        world_y,
                        world_z
                    );

                    GX_Color4u8(
                        255u,
                        255u,
                        255u,
                        (u8)(
                            q2gx_alias_translucent_pass
                            ?
                            transalias_alpha_u8
                            :
                            255u
                        )
                    );

                    GX_TexCoord2f32(
                        (f32)st->s / (f32)model->skin_width,
                        (f32)st->t / (f32)model->skin_height
                    );
                }
            }

            GX_End();

            if (q2gx_alias_translucent_pass)
                Q2GX_SetupAlias3D();

            if (
                q2gx_alias_translucent_pass
                &&
                !q2gx_transalias_first_draw_logged
            )
            {
                q2gx_transalias_first_draw_logged = true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX TRANSALIAS FIRST DRAW: "
                    "%s "
                    "frame=%u "
                    "oldframe=%u "
                    "backlerp=%.4f "
                    "tris=%u "
                    "skin=%s "
                    "flags=%d "
                    "alpha=%.4f "
                    "alpha_u8=%u "
                    "state=translucent "
                    "mode=rf_translucent_md2_shared_core_v1d\n",
                    model->name,
                    frame_index,
                    old_frame_index,
                    entity->backlerp,
                    model->num_tris,
                    skin->name,
                    entity->flags,
                    entity->alpha,
                    transalias_alpha_u8
                );
            }

            if (
                q2gx_alias_translucent_pass
                &&
                !q2gx_transalias_nonsmoke_first_draw_logged
                &&
                strcmp(
                    model->name,
                    "models/objects/smoke/tris.md2"
                ) != 0
            )
            {
                q2gx_transalias_nonsmoke_first_draw_logged = true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX TRANSALIAS NONSMOKE FIRST DRAW: "
                    "%s "
                    "frame=%u "
                    "tris=%u "
                    "skin=%s "
                    "flags=%d "
                    "alpha=%.4f "
                    "alpha_u8=%u "
                    "state=translucent "
                    "mode=rf_translucent_md2_shared_core_v1d\n",
                    model->name,
                    frame_index,
                    model->num_tris,
                    skin->name,
                    entity->flags,
                    entity->alpha,
                    transalias_alpha_u8
                );
            }

            ++entities_drawn;
            triangles_drawn += model->num_tris;
            vertices_drawn += model->num_tris * 3u;

            if (!model->first_draw_logged)
            {
                model->first_draw_logged = true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX ALIAS MODEL FIRST DRAW: "
                    "%s frame=%u oldframe=%u backlerp=%.4f "
                    "tris=%u skin=%s origin=%.3f,%.3f,%.3f "
                    "angles=%.3f,%.3f,%.3f flags=%d "
                    "mode=md2_interpolated_rgba8\n",
                    model->name,
                    frame_index,
                    old_frame_index,
                    entity->backlerp,
                    model->num_tris,
                    skin->name,
                    entity->origin[0],
                    entity->origin[1],
                    entity->origin[2],
                    entity->angles[0],
                    entity->angles[1],
                    entity->angles[2],
                    entity->flags
                );
            }
        }
    }

    if (q2gx_alias_translucent_pass)
    {
        q2gx_transalias_drawn_window +=
            entities_drawn;

        q2gx_transalias_triangles_window +=
            triangles_drawn;

        q2gx_transalias_vertices_window +=
            vertices_drawn;

        q2gx_transalias_skin_binds_window +=
            skin_binds;

        q2gx_transalias_tlut_loads_window +=
            tlut_loads;

        q2gx_transalias_invalid_frame_window +=
            invalid_frames;

        q2gx_transalias_invalid_skin_window +=
            invalid_skins;

        if (q2gx_transalias_frames_window >= 120u)
        {
            unsigned int alpha_min =
                q2gx_transalias_drawn_window > 0u
                ?
                q2gx_transalias_alpha_min_window
                :
                0u;

            unsigned int alpha_max =
                q2gx_transalias_drawn_window > 0u
                ?
                q2gx_transalias_alpha_max_window
                :
                0u;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX TRANSALIAS 120: "
                "frames=%u "
                "seen_total=%u "
                "drawn_total=%u "
                "shell_skipped_total=%u "
                "non_smoke_skipped_total=%u "
                "triangles_total=%u "
                "vertices_total=%u "
                "skin_binds_total=%u "
                "tlut_loads_total=%u "
                "invalid_frame_total=%u "
                "invalid_skin_total=%u "
                "alpha_min_u8=%u "
                "alpha_max_u8=%u "
                "state=translucent "
                "mode=rf_translucent_md2_shared_core_v1d\n",
                q2gx_transalias_frames_window,
                q2gx_transalias_seen_window,
                q2gx_transalias_drawn_window,
                q2gx_transalias_shell_skipped_window,
                q2gx_transalias_non_smoke_skipped_window,
                q2gx_transalias_triangles_window,
                q2gx_transalias_vertices_window,
                q2gx_transalias_skin_binds_window,
                q2gx_transalias_tlut_loads_window,
                q2gx_transalias_invalid_frame_window,
                q2gx_transalias_invalid_skin_window,
                alpha_min,
                alpha_max
            );

            q2gx_transalias_frames_window = 0u;
            q2gx_transalias_seen_window = 0u;
            q2gx_transalias_drawn_window = 0u;
            q2gx_transalias_shell_skipped_window = 0u;
            q2gx_transalias_non_smoke_skipped_window = 0u;
            q2gx_transalias_triangles_window = 0u;
            q2gx_transalias_vertices_window = 0u;
            q2gx_transalias_skin_binds_window = 0u;
            q2gx_transalias_tlut_loads_window = 0u;
            q2gx_transalias_invalid_frame_window = 0u;
            q2gx_transalias_invalid_skin_window = 0u;
            q2gx_transalias_alpha_min_window = 255u;
            q2gx_transalias_alpha_max_window = 0u;
        }

        return;
    }


    q2gx_alias_entities_window += entities_drawn;
    q2gx_alias_weapon_skipped_window += weapon_skipped;
    q2gx_alias_translucent_skipped_window += translucent_skipped;
    q2gx_alias_beam_skipped_window += beam_skipped;
    q2gx_alias_triangles_window += triangles_drawn;
    q2gx_alias_vertices_window += vertices_drawn;
    q2gx_alias_skin_binds_window += skin_binds;
    q2gx_alias_tlut_loads_window += tlut_loads;
    q2gx_alias_invalid_frame_window += invalid_frames;
    q2gx_alias_invalid_skin_window += invalid_skins;
    q2gx_alias_custom_skin_fallback_window += custom_skin_fallbacks;
    q2gx_alias_animated_samples_window += animated_samples;

    if (q2gx_alias_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX ALIAS 120: "
            "frames=%u entities_total=%u weapon_skipped_total=%u "
            "translucent_skipped_total=%u beam_skipped_total=%u "
            "triangles_total=%u vertices_total=%u skin_binds_total=%u "
            "tlut_loads_total=%u invalid_frame_total=%u invalid_skin_total=%u "
            "custom_skin_fallback_total=%u animated_samples_total=%u "
            "registered_models=%u registered_skins=%u "
            "mode=md2_world_entities\n",
            q2gx_alias_frames_window,
            q2gx_alias_entities_window,
            q2gx_alias_weapon_skipped_window,
            q2gx_alias_translucent_skipped_window,
            q2gx_alias_beam_skipped_window,
            q2gx_alias_triangles_window,
            q2gx_alias_vertices_window,
            q2gx_alias_skin_binds_window,
            q2gx_alias_tlut_loads_window,
            q2gx_alias_invalid_frame_window,
            q2gx_alias_invalid_skin_window,
            q2gx_alias_custom_skin_fallback_window,
            q2gx_alias_animated_samples_window,
            q2gx_alias_registered_models,
            q2gx_alias_registered_skins
        );

        q2gx_alias_frames_window = 0u;
        q2gx_alias_entities_window = 0u;
        q2gx_alias_weapon_skipped_window = 0u;
        q2gx_alias_translucent_skipped_window = 0u;
        q2gx_alias_beam_skipped_window = 0u;
        q2gx_alias_triangles_window = 0u;
        q2gx_alias_vertices_window = 0u;
        q2gx_alias_skin_binds_window = 0u;
        q2gx_alias_tlut_loads_window = 0u;
        q2gx_alias_invalid_frame_window = 0u;
        q2gx_alias_invalid_skin_window = 0u;
        q2gx_alias_custom_skin_fallback_window = 0u;
        q2gx_alias_animated_samples_window = 0u;
    }
}


/*
 * Q2GC_VIEW_WEAPON_V1
 *
 * Stock Quake II view-weapon semantics recovered from ref_gl:
 *
 *   RF_WEAPONMODEL:
 *     no frustum cull
 *
 *   hand == 2:
 *     suppress the view model
 *
 *   hand == 1:
 *     mirror the perspective projection horizontally
 *
 *   RF_DEPTHHACK:
 *     map the weapon into the nearest 30% of the normal
 *     viewport depth range, then restore normal 3D state.
 *
 * Geometry, skinning and interpolation remain the exact
 * already-proven ALIAS_MD2 core.
 */
static void Q2GX_LoadMirroredViewWeaponProjection(
    refdef_t *fd)
{
    Mtx44 projection;
    f32 aspect;

    if (
        !fd
        ||
        fd->width <= 0
        ||
        fd->height <= 0
    )
    {
        return;
    }

    aspect =
        (f32)fd->width
        /
        (f32)fd->height;

    guPerspective(
        projection,
        fd->fov_y,
        aspect,
        4.0f,
        4096.0f
    );

    /*
     * Stock GL does:
     *
     *   glScalef(-1, 1, 1);
     *   Perspective(...)
     *
     * The perspective used here is symmetric, so negating its
     * X scale produces the same horizontal mirror.
     *
     * Alias V1 currently uses GX_CULL_NONE, so no winding/cull
     * reversal is needed for this correctness milestone.
     */
    projection[0][0] =
        -projection[0][0];

    GX_LoadProjectionMtx(
        projection,
        GX_PERSPECTIVE
    );
}


static void Q2GX_SetViewWeaponDepthRange(
    refdef_t *fd,
    f32 near_depth,
    f32 far_depth)
{
    if (!fd)
        return;

    GX_SetViewport(
        (f32)fd->x,
        (f32)fd->y,
        (f32)fd->width,
        (f32)fd->height,
        near_depth,
        far_depth
    );
}


static void Q2GX_DrawViewWeaponEntities(
    refdef_t *fd)
{
    unsigned int entity_index;

    unsigned int seen = 0u;
    unsigned int drawn = 0u;
    unsigned int hidden_hand2 = 0u;
    unsigned int mirrored_hand1 = 0u;
    unsigned int depthhack_draws = 0u;
    unsigned int triangles = 0u;
    unsigned int vertices = 0u;
    unsigned int skin_binds = 0u;
    unsigned int tlut_loads = 0u;
    unsigned int invalid_frames = 0u;
    unsigned int invalid_skins = 0u;

    int hand_mode;

    if (!fd)
        return;

    ++q2gx_viewweapon_frames_window;

    if (!q2gx_viewweapon_hand)
    {
        q2gx_viewweapon_hand =
            ri.Cvar_Get(
                "hand",
                "0",
                CVAR_USERINFO | CVAR_ARCHIVE
            );
    }

    hand_mode =
        q2gx_viewweapon_hand
        ?
        (int)q2gx_viewweapon_hand->value
        :
        0;

    if (
        hand_mode < 0
        ||
        hand_mode > 2
    )
    {
        hand_mode = 0;
    }

    if (
        fd->num_entities > 0
        &&
        fd->entities
    )
    {
        for (
            entity_index = 0u;
            entity_index < (unsigned int)fd->num_entities;
            ++entity_index
        )
        {
            entity_t *entity =
                &fd->entities[entity_index];

            struct model_s *handle;
            q2gx_alias_model_t *model;

            unsigned int frame_index;
            unsigned int old_frame_index;

            q2gx_alias_lerp_t lerp;
            q2gx_alias_transform_t transform;
            q2gx_alias_skin_t *skin;

            qboolean invalid_skin;
            qboolean custom_skin_fallback;

            unsigned int triangle_index;

            if (!entity->model)
                continue;

            if (!(entity->flags & RF_WEAPONMODEL))
                continue;

            handle = entity->model;

            if (
                handle->magic != Q2GX_MODEL_HANDLE_MAGIC
                ||
                handle->kind != Q2GX_MODEL_KIND_ALIAS_MD2
            )
            {
                continue;
            }

            model =
                (q2gx_alias_model_t *)handle;

            ++seen;

            if (hand_mode == 2)
            {
                ++hidden_hand2;
                continue;
            }

            if (
                entity->frame < 0
                ||
                (unsigned int)entity->frame
                >=
                model->num_frames
            )
            {
                ++invalid_frames;
                frame_index = 0u;
                old_frame_index = 0u;
            }
            else if (
                entity->oldframe < 0
                ||
                (unsigned int)entity->oldframe
                >=
                model->num_frames
            )
            {
                ++invalid_frames;
                frame_index = 0u;
                old_frame_index = 0u;
            }
            else
            {
                frame_index =
                    (unsigned int)entity->frame;

                old_frame_index =
                    (unsigned int)entity->oldframe;
            }

            skin =
                Q2GX_SelectAliasSkin(
                    model,
                    entity,
                    &invalid_skin,
                    &custom_skin_fallback
                );

            if (invalid_skin)
                ++invalid_skins;

            if (!skin)
                continue;

            Q2GX_InitAliasLerp(
                model,
                entity,
                frame_index,
                old_frame_index,
                &lerp
            );

            Q2GX_InitAliasTransform(
                entity,
                &transform
            );

            Q2GX_SetupAlias3D();

            if (hand_mode == 1)
            {
                Q2GX_LoadMirroredViewWeaponProjection(
                    fd
                );

                ++mirrored_hand1;
            }

            if (entity->flags & RF_DEPTHHACK)
            {
                Q2GX_SetViewWeaponDepthRange(
                    fd,
                    0.0f,
                    0.3f
                );

                ++depthhack_draws;
            }

            GX_LoadTlut(
                &skin->tlut,
                GX_TLUT0
            );

            ++tlut_loads;

            GX_LoadTexObj(
                &skin->texture,
                GX_TEXMAP0
            );

            ++skin_binds;

            GX_Begin(
                GX_TRIANGLES,
                GX_VTXFMT0,
                (u16)(model->num_tris * 3u)
            );

            for (
                triangle_index = 0u;
                triangle_index < model->num_tris;
                ++triangle_index
            )
            {
                const q2gx_alias_triangle_t *triangle =
                    &model->triangles[triangle_index];

                unsigned int corner;

                for (
                    corner = 0u;
                    corner < 3u;
                    ++corner
                )
                {
                    unsigned int xyz_index =
                        triangle->xyz[corner];

                    unsigned int st_index =
                        triangle->st[corner];

                    const q2gx_alias_st_t *st =
                        &model->st[st_index];

                    f32 local_x;
                    f32 local_y;
                    f32 local_z;

                    f32 world_x;
                    f32 world_y;
                    f32 world_z;

                    Q2GX_LerpAliasVertex(
                        &lerp,
                        xyz_index,
                        &local_x,
                        &local_y,
                        &local_z
                    );

                    Q2GX_TransformAliasPoint(
                        &transform,
                        local_x,
                        local_y,
                        local_z,
                        &world_x,
                        &world_y,
                        &world_z
                    );

                    GX_Position3f32(
                        world_x,
                        world_y,
                        world_z
                    );

                    GX_Color4u8(
                        255u,
                        255u,
                        255u,
                        255u
                    );

                    GX_TexCoord2f32(
                        (f32)st->s
                        /
                        (f32)model->skin_width,
                        (f32)st->t
                        /
                        (f32)model->skin_height
                    );
                }
            }

            GX_End();

            ++drawn;

            triangles +=
                model->num_tris;

            vertices +=
                model->num_tris * 3u;

            if (!q2gx_viewweapon_first_draw_logged)
            {
                q2gx_viewweapon_first_draw_logged =
                    true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX VIEWWEAPON FIRST DRAW: "
                    "%s "
                    "frame=%u "
                    "oldframe=%u "
                    "backlerp=%.4f "
                    "tris=%u "
                    "skin=%s "
                    "origin=%.3f,%.3f,%.3f "
                    "angles=%.3f,%.3f,%.3f "
                    "flags=%d "
                    "hand=%d "
                    "depthhack=%u "
                    "mode=rf_weaponmodel_ci8\n",
                    model->name,
                    frame_index,
                    old_frame_index,
                    entity->backlerp,
                    model->num_tris,
                    skin->name,
                    entity->origin[0],
                    entity->origin[1],
                    entity->origin[2],
                    entity->angles[0],
                    entity->angles[1],
                    entity->angles[2],
                    entity->flags,
                    hand_mode,
                    (entity->flags & RF_DEPTHHACK)
                    ?
                    1u
                    :
                    0u
                );
            }

            /*
             * Restore exact normal 3D camera/projection/viewport
             * immediately after the special view-model draw.
             */
            Q2GX_SetupWorld3D(
                fd
            );

            Q2GX_SetupAlias3D();
        }
    }

    q2gx_viewweapon_seen_window +=
        seen;

    q2gx_viewweapon_drawn_window +=
        drawn;

    q2gx_viewweapon_hidden_hand2_window +=
        hidden_hand2;

    q2gx_viewweapon_mirrored_hand1_window +=
        mirrored_hand1;

    q2gx_viewweapon_depthhack_window +=
        depthhack_draws;

    q2gx_viewweapon_triangles_window +=
        triangles;

    q2gx_viewweapon_vertices_window +=
        vertices;

    q2gx_viewweapon_skin_binds_window +=
        skin_binds;

    q2gx_viewweapon_tlut_loads_window +=
        tlut_loads;

    q2gx_viewweapon_invalid_frame_window +=
        invalid_frames;

    q2gx_viewweapon_invalid_skin_window +=
        invalid_skins;

    if (q2gx_viewweapon_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX VIEWWEAPON 120: "
            "frames=%u "
            "seen_total=%u "
            "drawn_total=%u "
            "hidden_hand2_total=%u "
            "mirrored_hand1_total=%u "
            "depthhack_draws_total=%u "
            "triangles_total=%u "
            "vertices_total=%u "
            "skin_binds_total=%u "
            "tlut_loads_total=%u "
            "invalid_frame_total=%u "
            "invalid_skin_total=%u "
            "hand=%d "
            "mode=rf_weaponmodel_depthhack\n",
            q2gx_viewweapon_frames_window,
            q2gx_viewweapon_seen_window,
            q2gx_viewweapon_drawn_window,
            q2gx_viewweapon_hidden_hand2_window,
            q2gx_viewweapon_mirrored_hand1_window,
            q2gx_viewweapon_depthhack_window,
            q2gx_viewweapon_triangles_window,
            q2gx_viewweapon_vertices_window,
            q2gx_viewweapon_skin_binds_window,
            q2gx_viewweapon_tlut_loads_window,
            q2gx_viewweapon_invalid_frame_window,
            q2gx_viewweapon_invalid_skin_window,
            hand_mode
        );

        q2gx_viewweapon_frames_window = 0u;
        q2gx_viewweapon_seen_window = 0u;
        q2gx_viewweapon_drawn_window = 0u;
        q2gx_viewweapon_hidden_hand2_window = 0u;
        q2gx_viewweapon_mirrored_hand1_window = 0u;
        q2gx_viewweapon_depthhack_window = 0u;
        q2gx_viewweapon_triangles_window = 0u;
        q2gx_viewweapon_vertices_window = 0u;
        q2gx_viewweapon_skin_binds_window = 0u;
        q2gx_viewweapon_tlut_loads_window = 0u;
        q2gx_viewweapon_invalid_frame_window = 0u;
        q2gx_viewweapon_invalid_skin_window = 0u;
    }
}


static void Q2GX_PrintAliasRegistrationSummary(void)
{
    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX ALIAS REGISTRATION: "
        "models=%u skins=%u md2_source_bytes=%u "
        "skin_ci8_bytes=%u tlut_bytes=%u "
        "mode=md2_alias_cache_ci8\n",
        q2gx_alias_registered_models,
        q2gx_alias_registered_skins,
        q2gx_alias_source_bytes,
        q2gx_alias_skin_bytes,
        q2gx_alias_tlut_bytes
    );
}

typedef struct q2gx_brush_transform_s
{
    f32 origin[3];

    f32 sin_roll;
    f32 cos_roll;

    f32 sin_pitch;
    f32 cos_pitch;

    f32 sin_yaw;
    f32 cos_yaw;
} q2gx_brush_transform_t;


static void Q2GX_InitBrushTransform(
    const entity_t *entity,
    q2gx_brush_transform_t *transform)
{
    const f32 degrees_to_radians =
        0.01745329251994329577f;

    f32 roll;
    f32 pitch;
    f32 yaw;

    memset(
        transform,
        0,
        sizeof(*transform)
    );

    transform->origin[0] = entity->origin[0];
    transform->origin[1] = entity->origin[1];
    transform->origin[2] = entity->origin[2];

    roll = entity->angles[2] * degrees_to_radians;
    pitch = entity->angles[0] * degrees_to_radians;
    yaw = entity->angles[1] * degrees_to_radians;

    transform->sin_roll = sinf(roll);
    transform->cos_roll = cosf(roll);
    transform->sin_pitch = sinf(pitch);
    transform->cos_pitch = cosf(pitch);
    transform->sin_yaw = sinf(yaw);
    transform->cos_yaw = cosf(yaw);
}


static void Q2GX_TransformBrushPoint(
    const q2gx_brush_transform_t *transform,
    const q2gx_world_vertex_t *vertex,
    f32 *world_x,
    f32 *world_y,
    f32 *world_z)
{
    f32 x1;
    f32 y1;
    f32 z1;

    f32 x2;
    f32 y2;
    f32 z2;

    /*
     * Exact stock brush model transform after the historic
     * R_DrawBrushModel pitch/roll sign workaround:
     *
     *   Translate(origin)
     *   Rotate(+yaw,   Z)
     *   Rotate(+pitch, Y)
     *   Rotate(+roll,  X)
     *
     * OpenGL composes this as T * Rz * Ry * Rx, so apply
     * Rx -> Ry -> Rz to each local BSP vertex.
     */
    x1 = vertex->x;

    y1 =
        transform->cos_roll * vertex->y
        -
        transform->sin_roll * vertex->z;

    z1 =
        transform->sin_roll * vertex->y
        +
        transform->cos_roll * vertex->z;

    x2 =
        transform->cos_pitch * x1
        +
        transform->sin_pitch * z1;

    y2 = y1;

    z2 =
        -transform->sin_pitch * x1
        +
        transform->cos_pitch * z1;

    *world_x =
        transform->cos_yaw * x2
        -
        transform->sin_yaw * y2
        +
        transform->origin[0];

    *world_y =
        transform->sin_yaw * x2
        +
        transform->cos_yaw * y2
        +
        transform->origin[1];

    *world_z =
        z2
        +
        transform->origin[2];
}


static qboolean Q2GX_BrushFaceIsVisible(
    const q2gx_world_face_t *face,
    const f32 modelorg[3])
{
    f32 dot;

    dot =
        modelorg[0] * face->normal[0]
        +
        modelorg[1] * face->normal[1]
        +
        modelorg[2] * face->normal[2]
        -
        face->dist;

    if (face->plane_back)
    {
        return
            dot
            <
            -
            Q2GX_BRUSH_BACKFACE_EPSILON;
    }

    return
        dot
        >
        Q2GX_BRUSH_BACKFACE_EPSILON;
}



static qboolean Q2GX_IsPlainTranslucentBrushFace(
    const q2gx_world_face_t *face)
{
    unsigned int flags;

    if (!face)
        return false;

    flags = face->surface_flags;

    if (
        !(
            flags
            &
            (
                Q2GX_SURF_TRANS33
                |
                Q2GX_SURF_TRANS66
            )
        )
    )
    {
        return false;
    }

    if (
        flags
        &
        (
            Q2GX_SURF_WARP
            |
            Q2GX_SURF_FLOWING
            |
            Q2GX_SURF_SKY
            |
            Q2GX_SURF_NODRAW
        )
    )
    {
        return false;
    }

    return true;
}


static void Q2GX_DrawBrushEntities(
    refdef_t *fd)
{
    unsigned int entity_index;

    unsigned int entities_drawn = 0u;
    unsigned int faces_tested = 0u;
    unsigned int backface_rejected = 0u;
    unsigned int visible_faces = 0u;
    unsigned int textured_faces = 0u;
    unsigned int fallback_faces = 0u;
    unsigned int deferred_trans_faces = 0u;
    unsigned int wal_binds = 0u;
    unsigned int vertices_drawn = 0u;

    if (
        !fd
        ||
        fd->num_entities <= 0
        ||
        !fd->entities
        ||
        !q2gx_brush_models
        ||
        q2gx_brush_model_count <= 1u
    )
    {
        return;
    }

    for (
        entity_index = 0u;
        entity_index < (unsigned int)fd->num_entities;
        ++entity_index
    )
    {
        entity_t *entity =
            &fd->entities[
                entity_index
            ];

        struct model_s *handle;
        q2gx_brush_model_t *brush_model;
        q2gx_brush_transform_t transform;
        f32 modelorg[3];
        qboolean rotated;
        unsigned int local_face;

        if (!entity->model)
            continue;

        handle = entity->model;

        if (
            handle->magic != Q2GX_MODEL_HANDLE_MAGIC
            ||
            handle->kind != Q2GX_MODEL_KIND_INLINE_BSP
            ||
            handle->model_index == 0u
            ||
            handle->model_index >= q2gx_brush_model_count
        )
        {
            continue;
        }

        brush_model =
            &q2gx_brush_models[
                handle->model_index
            ];

        ++entities_drawn;

        rotated =
            entity->angles[0] != 0.0f
            ||
            entity->angles[1] != 0.0f
            ||
            entity->angles[2] != 0.0f;

        modelorg[0] = fd->vieworg[0] - entity->origin[0];
        modelorg[1] = fd->vieworg[1] - entity->origin[1];
        modelorg[2] = fd->vieworg[2] - entity->origin[2];

        if (rotated)
        {
            vec3_t temp;
            vec3_t forward;
            vec3_t right;
            vec3_t up;

            temp[0] = modelorg[0];
            temp[1] = modelorg[1];
            temp[2] = modelorg[2];

            AngleVectors(
                entity->angles,
                forward,
                right,
                up
            );

            modelorg[0] = DotProduct(temp, forward);
            modelorg[1] = -DotProduct(temp, right);
            modelorg[2] = DotProduct(temp, up);
        }

        Q2GX_InitBrushTransform(
            entity,
            &transform
        );

        for (
            local_face = 0u;
            local_face < brush_model->face_count;
            ++local_face
        )
        {
            unsigned int face_index =
                brush_model->first_face
                +
                local_face;

            q2gx_world_face_t *face;
            q2gx_world_texinfo_t *texinfo;
            unsigned int vertex_index;
            qboolean use_wal;

            if (face_index >= q2gx_world_face_count)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BRUSH: "
                    "model *%u face overflow %u\n",
                    handle->model_index,
                    face_index
                );

                return;
            }

            face =
                &q2gx_world_faces[
                    face_index
                ];

            ++faces_tested;

            if (!Q2GX_BrushFaceIsVisible(face, modelorg))
            {
                ++backface_rejected;
                continue;
            }

            ++visible_faces;

            texinfo =
                &q2gx_world_texinfos[
                    face->texinfo_index
                ];

            if (Q2GX_IsPlainTranslucentBrushFace(face))
            {
                ++deferred_trans_faces;
                continue;
            }

            use_wal =
                !(
                    face->surface_flags
                    &
                    Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
                )
                &&
                texinfo->next_texinfo < 0
                &&
                texinfo->wal_cache_index >= 0
                &&
                (unsigned int)texinfo->wal_cache_index
                <
                q2gx_world_wal_texture_count;

            if (use_wal)
            {
                Q2GX_BindWorldWALTexture(
                    &q2gx_world_wal_textures[
                        (unsigned int)
                        texinfo->wal_cache_index
                    ]
                );

                ++textured_faces;
                ++wal_binds;
            }
            else
            {
                Q2GX_BindWorldFlatFallback();
                ++fallback_faces;
            }

            if (face->vertex_count > 65535u)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BRUSH: "
                    "face %u too large vertices=%u\n",
                    face_index,
                    face->vertex_count
                );

                return;
            }

            GX_Begin(
                GX_TRIANGLES,
                GX_VTXFMT0,
                (u16)face->vertex_count
            );

            for (
                vertex_index = 0u;
                vertex_index < face->vertex_count;
                ++vertex_index
            )
            {
                const q2gx_world_vertex_t *vertex =
                    &q2gx_world_vertices[
                        face->first_vertex
                        +
                        vertex_index
                    ];

                f32 world_x;
                f32 world_y;
                f32 world_z;

                Q2GX_TransformBrushPoint(
                    &transform,
                    vertex,
                    &world_x,
                    &world_y,
                    &world_z
                );

                GX_Position3f32(
                    world_x,
                    world_y,
                    world_z
                );

                GX_Color4u8(
                    vertex->r,
                    vertex->g,
                    vertex->b,
                    vertex->a
                );

                GX_TexCoord2f32(
                    vertex->s,
                    vertex->t
                );
            }

            GX_End();

            vertices_drawn +=
                face->vertex_count;

            if (!q2gx_brush_first_draw_logged)
            {
                q2gx_brush_first_draw_logged = true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BRUSH FIRST DRAW: "
                    "model=*%u "
                    "first_face=%u "
                    "face_count=%u "
                    "origin=%.3f,%.3f,%.3f "
                    "angles=%.3f,%.3f,%.3f "
                    "textured=%u "
                    "mode=inline_bsp_entities\n",
                    handle->model_index,
                    brush_model->first_face,
                    brush_model->face_count,
                    entity->origin[0],
                    entity->origin[1],
                    entity->origin[2],
                    entity->angles[0],
                    entity->angles[1],
                    entity->angles[2],
                    use_wal ? 1u : 0u
                );
            }
        }
    }

    if (
        visible_faces + backface_rejected
        !=
        faces_tested
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: "
            "face accounting mismatch "
            "tested=%u visible=%u rejected=%u\n",
            faces_tested,
            visible_faces,
            backface_rejected
        );

        return;
    }

    if (
        textured_faces
        +
        fallback_faces
        +
        deferred_trans_faces
        !=
        visible_faces
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: "
            "surface accounting mismatch "
            "visible=%u textured=%u fallback=%u deferred_trans=%u\n",
            visible_faces,
            textured_faces,
            fallback_faces,
            deferred_trans_faces
        );

        return;
    }

    if (wal_binds != textured_faces)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH: "
            "WAL bind accounting mismatch "
            "binds=%u textured=%u\n",
            wal_binds,
            textured_faces
        );

        return;
    }

    q2gx_brush_entities_window += entities_drawn;
    q2gx_brush_faces_tested_window += faces_tested;
    q2gx_brush_backface_rejected_window += backface_rejected;
    q2gx_brush_visible_faces_window += visible_faces;
    q2gx_brush_textured_faces_window += textured_faces;
    q2gx_brush_fallback_faces_window += fallback_faces;
    q2gx_brush_deferred_trans_faces_window += deferred_trans_faces;
    q2gx_brush_wal_binds_window += wal_binds;
    q2gx_brush_vertices_window += vertices_drawn;
}


/*
 * V1b declaration-order fix only:
 * the proven lazy special-WAL loader is defined later in gx_main.c.
 * Keep this experiment in the existing brush-pass location and give C
 * the exact prototype before Q2GX_DrawTranslucentBrushEntities() uses it.
 */
static q2gx_world_wal_texture_t *Q2GX_GetWorldWarpTexture(
    unsigned int texinfo_index);


static int Q2GX_CompareTransBrushBackToFront(
    const void *a_ptr,
    const void *b_ptr)
{
    const q2gx_trans_brush_sort_t *a =
        (const q2gx_trans_brush_sort_t *)a_ptr;

    const q2gx_trans_brush_sort_t *b =
        (const q2gx_trans_brush_sort_t *)b_ptr;

    if (a->depth_sq < b->depth_sq)
        return 1;

    if (a->depth_sq > b->depth_sq)
        return -1;

    if (a->entity_index < b->entity_index)
        return -1;

    if (a->entity_index > b->entity_index)
        return 1;

    if (a->face_index < b->face_index)
        return -1;

    if (a->face_index > b->face_index)
        return 1;

    return 0;
}


static qboolean Q2GX_EnsureTransBrushSortCapacity(
    unsigned int needed)
{
    q2gx_trans_brush_sort_t *new_sort;

    if (needed == 0u)
        return true;

    if (
        q2gx_trans_brush_sort
        &&
        q2gx_trans_brush_sort_capacity >= needed
    )
    {
        return true;
    }

    if (
        needed
        >
        (
            (unsigned int)-1
            /
            (unsigned int)sizeof(*q2gx_trans_brush_sort)
        )
    )
    {
        ++q2gx_trans_brush_alloc_fail_window;
        return false;
    }

    new_sort =
        (q2gx_trans_brush_sort_t *)realloc(
            q2gx_trans_brush_sort,
            needed * sizeof(*q2gx_trans_brush_sort)
        );

    if (!new_sort)
    {
        ++q2gx_trans_brush_alloc_fail_window;
        return false;
    }

    q2gx_trans_brush_sort = new_sort;
    q2gx_trans_brush_sort_capacity = needed;

    return true;
}


static void Q2GX_SetupTranslucentBrush3D(
    refdef_t *fd)
{
    Q2GX_SetupTexturedWorld3D(fd);

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_MODULATE
    );

    GX_SetBlendMode(
        GX_BM_BLEND,
        GX_BL_SRCALPHA,
        GX_BL_INVSRCALPHA,
        GX_LO_CLEAR
    );

    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_FALSE
    );

    GX_SetCullMode(
        GX_CULL_NONE
    );
}


static void Q2GX_DrawTranslucentBrushEntities(
    refdef_t *fd)
{
    unsigned int entity_index;
    unsigned int upper_bound = 0u;
    unsigned int sort_count = 0u;
    unsigned int sorted_index;

    unsigned int candidates = 0u;
    unsigned int drawn = 0u;
    unsigned int alpha33 = 0u;
    unsigned int alpha66 = 0u;
    unsigned int wal_binds = 0u;
    unsigned int vertices = 0u;
    unsigned int invalid = 0u;

    if (
        !fd
        ||
        fd->num_entities <= 0
        ||
        !fd->entities
        ||
        !q2gx_brush_models
        ||
        q2gx_brush_model_count <= 1u
    )
    {
        return;
    }

    ++q2gx_trans_brush_frames_window;

    for (
        entity_index = 0u;
        entity_index < (unsigned int)fd->num_entities;
        ++entity_index
    )
    {
        entity_t *entity =
            &fd->entities[
                entity_index
            ];

        struct model_s *handle;

        if (!entity->model)
            continue;

        handle = entity->model;

        if (
            handle->magic != Q2GX_MODEL_HANDLE_MAGIC
            ||
            handle->kind != Q2GX_MODEL_KIND_INLINE_BSP
            ||
            handle->model_index == 0u
            ||
            handle->model_index >= q2gx_brush_model_count
        )
        {
            continue;
        }

        if (
            q2gx_brush_models[
                handle->model_index
            ].face_count
            >
            (unsigned int)-1 - upper_bound
        )
        {
            ++q2gx_trans_brush_alloc_fail_window;
            return;
        }

        upper_bound +=
            q2gx_brush_models[
                handle->model_index
            ].face_count;
    }

    if (!Q2GX_EnsureTransBrushSortCapacity(upper_bound))
        return;

    for (
        entity_index = 0u;
        entity_index < (unsigned int)fd->num_entities;
        ++entity_index
    )
    {
        entity_t *entity =
            &fd->entities[
                entity_index
            ];

        struct model_s *handle;
        q2gx_brush_model_t *brush_model;
        q2gx_brush_transform_t transform;
        f32 modelorg[3];
        qboolean rotated;
        unsigned int local_face;

        if (!entity->model)
            continue;

        handle = entity->model;

        if (
            handle->magic != Q2GX_MODEL_HANDLE_MAGIC
            ||
            handle->kind != Q2GX_MODEL_KIND_INLINE_BSP
            ||
            handle->model_index == 0u
            ||
            handle->model_index >= q2gx_brush_model_count
        )
        {
            continue;
        }

        brush_model =
            &q2gx_brush_models[
                handle->model_index
            ];

        rotated =
            entity->angles[0] != 0.0f
            ||
            entity->angles[1] != 0.0f
            ||
            entity->angles[2] != 0.0f;

        modelorg[0] = fd->vieworg[0] - entity->origin[0];
        modelorg[1] = fd->vieworg[1] - entity->origin[1];
        modelorg[2] = fd->vieworg[2] - entity->origin[2];

        if (rotated)
        {
            vec3_t temp;
            vec3_t forward;
            vec3_t right;
            vec3_t up;

            temp[0] = modelorg[0];
            temp[1] = modelorg[1];
            temp[2] = modelorg[2];

            AngleVectors(
                entity->angles,
                forward,
                right,
                up
            );

            modelorg[0] = DotProduct(temp, forward);
            modelorg[1] = -DotProduct(temp, right);
            modelorg[2] = DotProduct(temp, up);
        }

        Q2GX_InitBrushTransform(
            entity,
            &transform
        );

        for (
            local_face = 0u;
            local_face < brush_model->face_count;
            ++local_face
        )
        {
            unsigned int face_index =
                brush_model->first_face
                +
                local_face;

            q2gx_world_face_t *face;
            q2gx_world_vertex_t center_vertex;
            f32 world_x;
            f32 world_y;
            f32 world_z;
            f32 dx;
            f32 dy;
            f32 dz;
            unsigned int vertex_index;

            if (face_index >= q2gx_world_face_count)
            {
                ++invalid;
                continue;
            }

            face =
                &q2gx_world_faces[
                    face_index
                ];

            if (!Q2GX_IsPlainTranslucentBrushFace(face))
                continue;

            if (!Q2GX_BrushFaceIsVisible(face, modelorg))
                continue;

            ++candidates;

            if (
                face->vertex_count == 0u
                ||
                sort_count >= q2gx_trans_brush_sort_capacity
            )
            {
                ++invalid;
                continue;
            }

            memset(
                &center_vertex,
                0,
                sizeof(center_vertex)
            );

            for (
                vertex_index = 0u;
                vertex_index < face->vertex_count;
                ++vertex_index
            )
            {
                const q2gx_world_vertex_t *vertex =
                    &q2gx_world_vertices[
                        face->first_vertex
                        +
                        vertex_index
                    ];

                center_vertex.x += vertex->x;
                center_vertex.y += vertex->y;
                center_vertex.z += vertex->z;
            }

            center_vertex.x /= (f32)face->vertex_count;
            center_vertex.y /= (f32)face->vertex_count;
            center_vertex.z /= (f32)face->vertex_count;

            Q2GX_TransformBrushPoint(
                &transform,
                &center_vertex,
                &world_x,
                &world_y,
                &world_z
            );

            dx = world_x - fd->vieworg[0];
            dy = world_y - fd->vieworg[1];
            dz = world_z - fd->vieworg[2];

            q2gx_trans_brush_sort[
                sort_count
            ].entity_index = entity_index;

            q2gx_trans_brush_sort[
                sort_count
            ].face_index = face_index;

            q2gx_trans_brush_sort[
                sort_count
            ].depth_sq =
                dx * dx
                +
                dy * dy
                +
                dz * dz;

            ++sort_count;
        }
    }

    if (sort_count > 1u)
    {
        qsort(
            q2gx_trans_brush_sort,
            sort_count,
            sizeof(*q2gx_trans_brush_sort),
            Q2GX_CompareTransBrushBackToFront
        );
    }

    if (sort_count > 0u)
        Q2GX_SetupTranslucentBrush3D(fd);

    for (
        sorted_index = 0u;
        sorted_index < sort_count;
        ++sorted_index
    )
    {
        q2gx_trans_brush_sort_t *entry =
            &q2gx_trans_brush_sort[
                sorted_index
            ];

        entity_t *entity;
        q2gx_world_face_t *face;
        q2gx_world_texinfo_t *texinfo;
        q2gx_world_wal_texture_t *texture;
        q2gx_brush_transform_t transform;
        unsigned int vertex_index;
        u8 alpha;

        if (
            entry->entity_index
            >=
            (unsigned int)fd->num_entities
            ||
            entry->face_index >= q2gx_world_face_count
        )
        {
            ++invalid;
            continue;
        }

        entity =
            &fd->entities[
                entry->entity_index
            ];

        face =
            &q2gx_world_faces[
                entry->face_index
            ];

        if (
            face->texinfo_index
            >=
            q2gx_world_texinfo_count
        )
        {
            ++invalid;
            continue;
        }

        texinfo =
            &q2gx_world_texinfos[
                face->texinfo_index
            ];

        if (face->surface_flags & Q2GX_SURF_TRANS33)
        {
            alpha = 84u;
            ++alpha33;
        }
        else if (face->surface_flags & Q2GX_SURF_TRANS66)
        {
            alpha = 168u;
            ++alpha66;
        }
        else
        {
            ++invalid;
            continue;
        }

        texture =
            Q2GX_GetWorldWarpTexture(
                face->texinfo_index
            );

        if (!texture)
        {
            ++invalid;
            continue;
        }

        Q2GX_InitBrushTransform(
            entity,
            &transform
        );

        GX_LoadTexObj(
            &texture->texture,
            GX_TEXMAP0
        );

        ++wal_binds;

        if (face->vertex_count > 65535u)
        {
            ++invalid;
            continue;
        }

        GX_Begin(
            GX_TRIANGLES,
            GX_VTXFMT0,
            (u16)face->vertex_count
        );

        for (
            vertex_index = 0u;
            vertex_index < face->vertex_count;
            ++vertex_index
        )
        {
            const q2gx_world_vertex_t *vertex =
                &q2gx_world_vertices[
                    face->first_vertex
                    +
                    vertex_index
                ];

            f32 world_x;
            f32 world_y;
            f32 world_z;

            Q2GX_TransformBrushPoint(
                &transform,
                vertex,
                &world_x,
                &world_y,
                &world_z
            );

            GX_Position3f32(
                world_x,
                world_y,
                world_z
            );

            GX_Color4u8(
                vertex->r,
                vertex->g,
                vertex->b,
                alpha
            );

            GX_TexCoord2f32(
                vertex->s,
                vertex->t
            );
        }

        GX_End();

        ++drawn;

        vertices +=
            face->vertex_count;

        if (!q2gx_trans_brush_first_draw_logged)
        {
            q2gx_trans_brush_first_draw_logged = true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX TRANSBRUSH FIRST DRAW: "
                "entity=%u "
                "face=%u "
                "texture=%s "
                "alpha_u8=%u "
                "flags=0x%02x "
                "depth_write=0 "
                "mode=inline_plain_trans_wal_far_to_near_v1\n",
                entry->entity_index,
                entry->face_index,
                texinfo->texture,
                (unsigned int)alpha,
                face->surface_flags
            );
        }
    }

    q2gx_trans_brush_candidates_window += candidates;
    q2gx_trans_brush_drawn_window += drawn;
    q2gx_trans_brush_alpha33_window += alpha33;
    q2gx_trans_brush_alpha66_window += alpha66;
    q2gx_trans_brush_wal_binds_window += wal_binds;
    q2gx_trans_brush_vertices_window += vertices;
    q2gx_trans_brush_invalid_window += invalid;

    if (q2gx_trans_brush_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TRANSBRUSH 120: "
            "frames=%u "
            "candidates_total=%u "
            "drawn_total=%u "
            "alpha33_total=%u "
            "alpha66_total=%u "
            "wal_binds_total=%u "
            "vertices_total=%u "
            "sort_capacity=%u "
            "alloc_fail_total=%u "
            "invalid_total=%u "
            "mode=inline_plain_trans_wal_far_to_near_v1\n",
            q2gx_trans_brush_frames_window,
            q2gx_trans_brush_candidates_window,
            q2gx_trans_brush_drawn_window,
            q2gx_trans_brush_alpha33_window,
            q2gx_trans_brush_alpha66_window,
            q2gx_trans_brush_wal_binds_window,
            q2gx_trans_brush_vertices_window,
            q2gx_trans_brush_sort_capacity,
            q2gx_trans_brush_alloc_fail_window,
            q2gx_trans_brush_invalid_window
        );

        q2gx_trans_brush_frames_window = 0u;
        q2gx_trans_brush_candidates_window = 0u;
        q2gx_trans_brush_drawn_window = 0u;
        q2gx_trans_brush_alpha33_window = 0u;
        q2gx_trans_brush_alpha66_window = 0u;
        q2gx_trans_brush_wal_binds_window = 0u;
        q2gx_trans_brush_vertices_window = 0u;
        q2gx_trans_brush_alloc_fail_window = 0u;
        q2gx_trans_brush_invalid_window = 0u;
    }
}





static void Q2GX_ClearSkyBounds(void)
{
    unsigned int i;

    for (i = 0u; i < Q2GX_SKY_FACE_COUNT; ++i)
    {
        q2gx_sky_mins[0][i] = 9999.0f;
        q2gx_sky_mins[1][i] = 9999.0f;
        q2gx_sky_maxs[0][i] = -9999.0f;
        q2gx_sky_maxs[1][i] = -9999.0f;
    }
}


static void Q2GX_DrawSkyPolygon(
    unsigned int nump,
    const f32 vecs[][3])
{
    f32 v[3] = {0.0f, 0.0f, 0.0f};
    f32 av[3];
    unsigned int i;
    int axis;

    for (i = 0u; i < nump; ++i)
    {
        v[0] += vecs[i][0];
        v[1] += vecs[i][1];
        v[2] += vecs[i][2];
    }

    av[0] = fabsf(v[0]);
    av[1] = fabsf(v[1]);
    av[2] = fabsf(v[2]);

    if (av[0] > av[1] && av[0] > av[2])
        axis = v[0] < 0.0f ? 1 : 0;
    else if (av[1] > av[2] && av[1] > av[0])
        axis = v[1] < 0.0f ? 3 : 2;
    else
        axis = v[2] < 0.0f ? 5 : 4;

    for (i = 0u; i < nump; ++i)
    {
        int j;
        f32 dv;
        f32 sky_s;
        f32 sky_t;

        j = q2gx_sky_vec_to_st[axis][2];

        if (j > 0)
            dv = vecs[i][j - 1];
        else
            dv = -vecs[i][-j - 1];

        if (dv < 0.001f)
            continue;

        j = q2gx_sky_vec_to_st[axis][0];

        if (j < 0)
            sky_s = -vecs[i][-j - 1] / dv;
        else
            sky_s = vecs[i][j - 1] / dv;

        j = q2gx_sky_vec_to_st[axis][1];

        if (j < 0)
            sky_t = -vecs[i][-j - 1] / dv;
        else
            sky_t = vecs[i][j - 1] / dv;

        if (sky_s < q2gx_sky_mins[0][axis])
            q2gx_sky_mins[0][axis] = sky_s;
        if (sky_t < q2gx_sky_mins[1][axis])
            q2gx_sky_mins[1][axis] = sky_t;
        if (sky_s > q2gx_sky_maxs[0][axis])
            q2gx_sky_maxs[0][axis] = sky_s;
        if (sky_t > q2gx_sky_maxs[1][axis])
            q2gx_sky_maxs[1][axis] = sky_t;
    }
}


static void Q2GX_ClipSkyPolygon(
    unsigned int nump,
    const f32 vecs[][3],
    unsigned int stage)
{
    f32 dists[Q2GX_SKY_MAX_CLIP_VERTS];
    int sides[Q2GX_SKY_MAX_CLIP_VERTS];
    f32 looped[Q2GX_SKY_MAX_CLIP_VERTS][3];
    f32 newv[2][Q2GX_SKY_MAX_CLIP_VERTS][3];
    unsigned int newc[2] = {0u, 0u};
    unsigned int i;
    unsigned int j;
    qboolean front = false;
    qboolean back = false;

    if (nump > Q2GX_SKY_MAX_CLIP_VERTS - 2u)
        return;

    if (stage == 6u)
    {
        Q2GX_DrawSkyPolygon(nump, vecs);
        return;
    }

    for (i = 0u; i < nump; ++i)
    {
        f32 d =
            vecs[i][0] * q2gx_sky_clip[stage][0]
            +
            vecs[i][1] * q2gx_sky_clip[stage][1]
            +
            vecs[i][2] * q2gx_sky_clip[stage][2];

        dists[i] = d;

        if (d > Q2GX_SKY_ON_EPSILON)
        {
            front = true;
            sides[i] = 1;
        }
        else if (d < -Q2GX_SKY_ON_EPSILON)
        {
            back = true;
            sides[i] = -1;
        }
        else
        {
            sides[i] = 0;
        }

        looped[i][0] = vecs[i][0];
        looped[i][1] = vecs[i][1];
        looped[i][2] = vecs[i][2];
    }

    if (!front || !back)
    {
        Q2GX_ClipSkyPolygon(nump, vecs, stage + 1u);
        return;
    }

    sides[nump] = sides[0];
    dists[nump] = dists[0];

    looped[nump][0] = looped[0][0];
    looped[nump][1] = looped[0][1];
    looped[nump][2] = looped[0][2];

    for (i = 0u; i < nump; ++i)
    {
        const f32 *v = looped[i];

        if (sides[i] >= 0)
        {
            newv[0][newc[0]][0] = v[0];
            newv[0][newc[0]][1] = v[1];
            newv[0][newc[0]][2] = v[2];
            ++newc[0];
        }

        if (sides[i] <= 0)
        {
            newv[1][newc[1]][0] = v[0];
            newv[1][newc[1]][1] = v[1];
            newv[1][newc[1]][2] = v[2];
            ++newc[1];
        }

        if (
            sides[i] == 0
            ||
            sides[i + 1u] == 0
            ||
            sides[i + 1u] == sides[i]
        )
        {
            continue;
        }

        {
            f32 d =
                dists[i]
                /
                (dists[i] - dists[i + 1u]);

            for (j = 0u; j < 3u; ++j)
            {
                f32 e =
                    v[j]
                    +
                    d
                    *
                    (
                        looped[i + 1u][j]
                        -
                        v[j]
                    );

                newv[0][newc[0]][j] = e;
                newv[1][newc[1]][j] = e;
            }
        }

        ++newc[0];
        ++newc[1];
    }

    if (newc[0] > 0u)
        Q2GX_ClipSkyPolygon(newc[0], newv[0], stage + 1u);

    if (newc[1] > 0u)
        Q2GX_ClipSkyPolygon(newc[1], newv[1], stage + 1u);
}


static void Q2GX_RotateSkyPoint(
    const refdef_t *fd,
    const f32 in[3],
    f32 out[3])
{
    f32 ax;
    f32 ay;
    f32 az;
    f32 len;

    if (!fd || q2gx_sky_rotate == 0.0f)
    {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }

    ax = q2gx_sky_axis[0];
    ay = q2gx_sky_axis[1];
    az = q2gx_sky_axis[2];

    len = sqrtf(ax * ax + ay * ay + az * az);

    if (len <= 0.0001f)
    {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
        return;
    }

    ax /= len;
    ay /= len;
    az /= len;

    {
        f32 radians =
            fd->time
            *
            q2gx_sky_rotate
            *
            (3.14159265358979323846f / 180.0f);

        f32 c = cosf(radians);
        f32 sn = sinf(radians);

        f32 dot =
            ax * in[0]
            +
            ay * in[1]
            +
            az * in[2];

        f32 cross_x =
            ay * in[2] - az * in[1];

        f32 cross_y =
            az * in[0] - ax * in[2];

        f32 cross_z =
            ax * in[1] - ay * in[0];

        out[0] =
            in[0] * c
            +
            cross_x * sn
            +
            ax * dot * (1.0f - c);

        out[1] =
            in[1] * c
            +
            cross_y * sn
            +
            ay * dot * (1.0f - c);

        out[2] =
            in[2] * c
            +
            cross_z * sn
            +
            az * dot * (1.0f - c);
    }
}


static void Q2GX_EmitSkyVertex(
    refdef_t *fd,
    f32 sky_s,
    f32 sky_t,
    unsigned int axis,
    const struct image_s *image)
{
    f32 b[3];
    f32 local[3];
    f32 rotated[3];

    f32 tex_s;
    f32 tex_t;
    f32 min_s;
    f32 max_s;
    f32 min_t;
    f32 max_t;

    unsigned int j;

    b[0] = sky_s * Q2GX_SKY_DISTANCE;
    b[1] = sky_t * Q2GX_SKY_DISTANCE;
    b[2] = Q2GX_SKY_DISTANCE;

    for (j = 0u; j < 3u; ++j)
    {
        int k =
            q2gx_sky_st_to_vec[axis][j];

        if (k < 0)
            local[j] = -b[-k - 1];
        else
            local[j] = b[k - 1];
    }

    Q2GX_RotateSkyPoint(fd, local, rotated);

    tex_s = (sky_s + 1.0f) * 0.5f;
    tex_t = (sky_t + 1.0f) * 0.5f;

    min_s =
        image && image->width > 0
        ?
        1.0f / (f32)image->width
        :
        1.0f / 256.0f;

    max_s =
        image && image->width > 0
        ?
        ((f32)image->width - 1.0f) / (f32)image->width
        :
        255.0f / 256.0f;

    min_t =
        image && image->height > 0
        ?
        1.0f / (f32)image->height
        :
        1.0f / 256.0f;

    max_t =
        image && image->height > 0
        ?
        ((f32)image->height - 1.0f) / (f32)image->height
        :
        255.0f / 256.0f;

    if (tex_s < min_s)
        tex_s = min_s;
    else if (tex_s > max_s)
        tex_s = max_s;

    if (tex_t < min_t)
        tex_t = min_t;
    else if (tex_t > max_t)
        tex_t = max_t;

    tex_t = 1.0f - tex_t;

    GX_Position3f32(
        fd->vieworg[0] + rotated[0],
        fd->vieworg[1] + rotated[1],
        fd->vieworg[2] + rotated[2]
    );

    GX_Color4u8(255u, 255u, 255u, 255u);

    GX_TexCoord2f32(tex_s, tex_t);
}


static void Q2GX_SetupSky3D(refdef_t *fd)
{
    Q2GX_SetupTexturedWorld3D(fd);

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_REPLACE
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

    GX_SetCullMode(GX_CULL_NONE);
}


static void Q2GX_DrawSkyBox(refdef_t *fd)
{
    unsigned int face_index;
    unsigned int source_faces = 0u;
    unsigned int box_faces = 0u;
    unsigned int vertices = 0u;
    unsigned int axis;

    if (
        !fd
        ||
        !q2gx_sky_ready
        ||
        !q2gx_world_faces
        ||
        !q2gx_world_vertices
    )
    {
        return;
    }

    ++q2gx_sky_frames_window;

    Q2GX_ClearSkyBounds();

    for (
        face_index = q2gx_world_static_first_face;
        face_index
        <
        q2gx_world_static_first_face
        +
        q2gx_world_static_face_count;
        ++face_index
    )
    {
        q2gx_world_face_t *face =
            &q2gx_world_faces[face_index];

        unsigned int tri;

        if (!face->visible_this_frame)
            continue;

        if (!(face->surface_flags & Q2GX_SURF_SKY))
            continue;

        ++source_faces;

        for (
            tri = 0u;
            tri + 2u < face->vertex_count;
            tri += 3u
        )
        {
            f32 vecs[3][3];
            unsigned int corner;

            for (corner = 0u; corner < 3u; ++corner)
            {
                const q2gx_world_vertex_t *vertex =
                    &q2gx_world_vertices[
                        face->first_vertex
                        +
                        tri
                        +
                        corner
                    ];

                vecs[corner][0] =
                    vertex->x - fd->vieworg[0];

                vecs[corner][1] =
                    vertex->y - fd->vieworg[1];

                vecs[corner][2] =
                    vertex->z - fd->vieworg[2];
            }

            Q2GX_ClipSkyPolygon(
                3u,
                vecs,
                0u
            );
        }
    }

    if (source_faces == 0u)
        goto sky_stats;

    if (q2gx_sky_rotate != 0.0f)
    {
        for (axis = 0u; axis < 6u; ++axis)
        {
            q2gx_sky_mins[0][axis] = -1.0f;
            q2gx_sky_mins[1][axis] = -1.0f;
            q2gx_sky_maxs[0][axis] = 1.0f;
            q2gx_sky_maxs[1][axis] = 1.0f;
        }
    }

    Q2GX_SetupSky3D(fd);

    for (axis = 0u; axis < 6u; ++axis)
    {
        unsigned int image_index;
        struct image_s *image;

        if (
            q2gx_sky_mins[0][axis]
            >=
            q2gx_sky_maxs[0][axis]
            ||
            q2gx_sky_mins[1][axis]
            >=
            q2gx_sky_maxs[1][axis]
        )
        {
            continue;
        }

        image_index =
            (unsigned int)
            q2gx_sky_tex_order[axis];

        if (image_index >= 6u)
            continue;

        image = q2gx_sky_images[image_index];

        if (!image)
            continue;

        GX_LoadTexObj(
            &image->texture,
            GX_TEXMAP0
        );

        GX_Begin(
            GX_QUADS,
            GX_VTXFMT0,
            4u
        );

        Q2GX_EmitSkyVertex(
            fd,
            q2gx_sky_mins[0][axis],
            q2gx_sky_mins[1][axis],
            axis,
            image
        );

        Q2GX_EmitSkyVertex(
            fd,
            q2gx_sky_mins[0][axis],
            q2gx_sky_maxs[1][axis],
            axis,
            image
        );

        Q2GX_EmitSkyVertex(
            fd,
            q2gx_sky_maxs[0][axis],
            q2gx_sky_maxs[1][axis],
            axis,
            image
        );

        Q2GX_EmitSkyVertex(
            fd,
            q2gx_sky_maxs[0][axis],
            q2gx_sky_mins[1][axis],
            axis,
            image
        );

        GX_End();

        ++box_faces;
        vertices += 4u;
    }

    Q2GX_SetupTexturedWorld3D(fd);

sky_stats:

    if (source_faces > 0u)
        ++q2gx_sky_frames_with_sky_window;

    q2gx_sky_source_faces_window += source_faces;
    q2gx_sky_box_faces_window += box_faces;
    q2gx_sky_vertices_window += vertices;

    if (
        source_faces > 0u
        &&
        !q2gx_sky_first_draw_logged
    )
    {
        q2gx_sky_first_draw_logged = true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX SKY FIRST DRAW: "
            "name=%s "
            "source_faces=%u "
            "box_faces=%u "
            "vertices=%u "
            "rotate=%.4f "
            "mode=stock_clip_pcx_cube_v1\n",
            q2gx_sky_name,
            source_faces,
            box_faces,
            vertices,
            q2gx_sky_rotate
        );
    }

    if (q2gx_sky_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX SKY 120: "
            "frames=%u "
            "frames_with_sky=%u "
            "source_faces_total=%u "
            "box_faces_total=%u "
            "vertices_total=%u "
            "name=%s "
            "ready=%u "
            "mode=stock_clip_pcx_cube_v1\n",
            q2gx_sky_frames_window,
            q2gx_sky_frames_with_sky_window,
            q2gx_sky_source_faces_window,
            q2gx_sky_box_faces_window,
            q2gx_sky_vertices_window,
            q2gx_sky_name,
            q2gx_sky_ready ? 1u : 0u
        );

        q2gx_sky_frames_window = 0u;
        q2gx_sky_frames_with_sky_window = 0u;
        q2gx_sky_source_faces_window = 0u;
        q2gx_sky_box_faces_window = 0u;
        q2gx_sky_vertices_window = 0u;
    }
}




static qboolean Q2GX_EnsureWorldWarpTextureCache(void)
{
    if (
        q2gx_world_warp_textures
        &&
        q2gx_world_warp_texture_loaded
        &&
        q2gx_world_warp_texture_failed
        &&
        q2gx_world_warp_texture_capacity
        ==
        q2gx_world_texinfo_count
    )
    {
        return true;
    }

    if (q2gx_world_texinfo_count == 0u)
        return false;

    Q2GX_FreeWorldWALTextureArray(
        q2gx_world_warp_textures,
        q2gx_world_warp_texture_capacity
    );

    q2gx_world_warp_textures = NULL;

    if (q2gx_world_warp_texture_loaded)
        free(q2gx_world_warp_texture_loaded);

    if (q2gx_world_warp_texture_failed)
        free(q2gx_world_warp_texture_failed);

    q2gx_world_warp_texture_loaded = NULL;
    q2gx_world_warp_texture_failed = NULL;
    q2gx_world_warp_texture_capacity = 0u;

    q2gx_world_warp_textures =
        calloc(
            q2gx_world_texinfo_count,
            sizeof(*q2gx_world_warp_textures)
        );

    q2gx_world_warp_texture_loaded =
        calloc(
            q2gx_world_texinfo_count,
            sizeof(*q2gx_world_warp_texture_loaded)
        );

    q2gx_world_warp_texture_failed =
        calloc(
            q2gx_world_texinfo_count,
            sizeof(*q2gx_world_warp_texture_failed)
        );

    if (
        !q2gx_world_warp_textures
        ||
        !q2gx_world_warp_texture_loaded
        ||
        !q2gx_world_warp_texture_failed
    )
    {
        Q2GX_FreeWorldWALTextureArray(
            q2gx_world_warp_textures,
            q2gx_world_texinfo_count
        );

        q2gx_world_warp_textures = NULL;

        if (q2gx_world_warp_texture_loaded)
            free(q2gx_world_warp_texture_loaded);

        if (q2gx_world_warp_texture_failed)
            free(q2gx_world_warp_texture_failed);

        q2gx_world_warp_texture_loaded = NULL;
        q2gx_world_warp_texture_failed = NULL;

        return false;
    }

    q2gx_world_warp_texture_capacity =
        q2gx_world_texinfo_count;

    return true;
}


static q2gx_world_wal_texture_t *Q2GX_GetWorldWarpTexture(
    unsigned int texinfo_index)
{
    q2gx_world_texinfo_t *texinfo;

    if (
        texinfo_index
        >=
        q2gx_world_texinfo_count
    )
    {
        return NULL;
    }

    if (!Q2GX_EnsureWorldWarpTextureCache())
        return NULL;

    if (q2gx_world_warp_texture_failed[texinfo_index])
        return NULL;

    if (!q2gx_world_warp_texture_loaded[texinfo_index])
    {
        texinfo =
            &q2gx_world_texinfos[
                texinfo_index
            ];

        if (
            !Q2GX_LoadWorldWALTexture(
                texinfo->texture,
                &q2gx_world_warp_textures[
                    texinfo_index
                ]
            )
        )
        {
            q2gx_world_warp_texture_failed[
                texinfo_index
            ] = true;

            ++q2gx_world_warp_load_fail_window;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX WARP: WAL load failed "
                "texinfo=%u texture=%s\n",
                texinfo_index,
                texinfo->texture
            );

            return NULL;
        }

        q2gx_world_warp_texture_loaded[
            texinfo_index
        ] = true;
    }

    return
        &q2gx_world_warp_textures[
            texinfo_index
        ];
}


/*
 * Q2GC_WORLD_ANIMATED_WAL_V1
 *
 * Resolve a Quake II nexttexinfo animation chain without altering
 * the BSP data. This mirrors stock R_TextureAnimation semantics
 * for the world entity:
 *
 *     frame = time * 2
 *
 * Valid chains may either loop to the base texinfo or terminate
 * with -1.
 */
static unsigned int Q2GX_SelectAnimatedWorldTexinfo(
    unsigned int base_texinfo,
    int frame)
{
    unsigned int count;
    unsigned int index;
    unsigned int step;
    int next;

    if (
        base_texinfo >= q2gx_world_texinfo_count
        ||
        q2gx_world_texinfo_count == 0u
    )
    {
        return base_texinfo;
    }

    count = 1u;
    index = base_texinfo;

    next =
        q2gx_world_texinfos[
            index
        ].next_texinfo;

    while (
        next >= 0
        &&
        (unsigned int)next < q2gx_world_texinfo_count
        &&
        (unsigned int)next != base_texinfo
        &&
        count < q2gx_world_texinfo_count
    )
    {
        index = (unsigned int)next;
        ++count;

        next =
            q2gx_world_texinfos[
                index
            ].next_texinfo;
    }

    if (count <= 1u)
        return base_texinfo;

    if (frame < 0)
        frame = 0;

    step =
        (unsigned int)frame
        %
        count;

    index = base_texinfo;

    while (step > 0u)
    {
        next =
            q2gx_world_texinfos[
                index
            ].next_texinfo;

        if (
            next < 0
            ||
            (unsigned int)next >= q2gx_world_texinfo_count
        )
        {
            return base_texinfo;
        }

        index = (unsigned int)next;
        --step;
    }

    return index;
}


static void Q2GX_SetupOpaqueWarp3D(
    refdef_t *fd)
{
    Q2GX_SetupTexturedWorld3D(fd);

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_REPLACE
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

    GX_SetCullMode(
        GX_CULL_NONE
    );
}


static void Q2GX_EmitWarpLeafPolygon(
    refdef_t *fd,
    const q2gx_world_texinfo_t *texinfo,
    unsigned int numverts,
    const f32 verts[][3],
    u8 alpha,
    unsigned int *subpolys,
    unsigned int *vertices)
{
    unsigned int i;
    f32 scroll = 0.0f;

    if (
        !fd
        ||
        !texinfo
        ||
        numverts < 3u
        ||
        numverts > 65535u
    )
    {
        return;
    }

    if (texinfo->flags & Q2GX_SURF_FLOWING)
    {
        f32 phase =
            fd->time * 0.5f;

        phase -= floorf(phase);

        scroll =
            -64.0f * phase;
    }

    GX_Begin(
        GX_TRIANGLEFAN,
        GX_VTXFMT0,
        (u16)numverts
    );

    for (i = 0u; i < numverts; ++i)
    {
        f32 os =
            verts[i][0] * texinfo->vecs[0][0]
            +
            verts[i][1] * texinfo->vecs[0][1]
            +
            verts[i][2] * texinfo->vecs[0][2]
            +
            texinfo->vecs[0][3];

        f32 ot =
            verts[i][0] * texinfo->vecs[1][0]
            +
            verts[i][1] * texinfo->vecs[1][1]
            +
            verts[i][2] * texinfo->vecs[1][2]
            +
            texinfo->vecs[1][3];

        int s_index =
            ((int)(
                (
                    ot * 0.125f
                    +
                    fd->time
                )
                *
                Q2GX_WARP_TURBSCALE
            ))
            &
            255;

        int t_index =
            ((int)(
                (
                    os * 0.125f
                    +
                    fd->time
                )
                *
                Q2GX_WARP_TURBSCALE
            ))
            &
            255;

        f32 warp_s =
            (
                os
                +
                q2gx_world_warp_sin[s_index]
                +
                scroll
            )
            *
            (1.0f / 64.0f);

        f32 warp_t =
            (
                ot
                +
                q2gx_world_warp_sin[t_index]
            )
            *
            (1.0f / 64.0f);

        GX_Position3f32(
            verts[i][0],
            verts[i][1],
            verts[i][2]
        );

        GX_Color4u8(
            255u,
            255u,
            255u,
            alpha
        );

        GX_TexCoord2f32(
            warp_s,
            warp_t
        );
    }

    GX_End();

    ++(*subpolys);
    *vertices += numverts;
}


static void Q2GX_DrawWarpSubdividedPolygon(
    refdef_t *fd,
    const q2gx_world_texinfo_t *texinfo,
    unsigned int numverts,
    const f32 verts[][3],
    u8 alpha,
    unsigned int *subpolys,
    unsigned int *vertices)
{
    unsigned int axis;

    if (
        numverts < 3u
        ||
        numverts > 60u
    )
    {
        return;
    }

    for (axis = 0u; axis < 3u; ++axis)
    {
        f32 mins = verts[0][axis];
        f32 maxs = verts[0][axis];
        f32 split;
        f32 dist[Q2GX_WARP_MAX_VERTS];
        f32 looped[Q2GX_WARP_MAX_VERTS][3];
        f32 front[Q2GX_WARP_MAX_VERTS][3];
        f32 back[Q2GX_WARP_MAX_VERTS][3];
        unsigned int front_count = 0u;
        unsigned int back_count = 0u;
        unsigned int i;

        for (i = 1u; i < numverts; ++i)
        {
            if (verts[i][axis] < mins)
                mins = verts[i][axis];

            if (verts[i][axis] > maxs)
                maxs = verts[i][axis];
        }

        split =
            Q2GX_WARP_SUBDIVIDE_SIZE
            *
            floorf(
                (
                    (mins + maxs) * 0.5f
                    /
                    Q2GX_WARP_SUBDIVIDE_SIZE
                )
                +
                0.5f
            );

        if (
            maxs - split
            <
            Q2GX_WARP_SUBDIVIDE_EDGE_EPSILON
        )
        {
            continue;
        }

        if (
            split - mins
            <
            Q2GX_WARP_SUBDIVIDE_EDGE_EPSILON
        )
        {
            continue;
        }

        for (i = 0u; i < numverts; ++i)
        {
            dist[i] =
                verts[i][axis] - split;

            looped[i][0] = verts[i][0];
            looped[i][1] = verts[i][1];
            looped[i][2] = verts[i][2];
        }

        dist[numverts] = dist[0];

        looped[numverts][0] = looped[0][0];
        looped[numverts][1] = looped[0][1];
        looped[numverts][2] = looped[0][2];

        for (i = 0u; i < numverts; ++i)
        {
            unsigned int k;

            if (dist[i] >= 0.0f)
            {
                if (
                    front_count
                    >=
                    Q2GX_WARP_MAX_VERTS
                )
                {
                    return;
                }

                front[front_count][0] =
                    looped[i][0];

                front[front_count][1] =
                    looped[i][1];

                front[front_count][2] =
                    looped[i][2];

                ++front_count;
            }

            if (dist[i] <= 0.0f)
            {
                if (
                    back_count
                    >=
                    Q2GX_WARP_MAX_VERTS
                )
                {
                    return;
                }

                back[back_count][0] =
                    looped[i][0];

                back[back_count][1] =
                    looped[i][1];

                back[back_count][2] =
                    looped[i][2];

                ++back_count;
            }

            if (
                dist[i] == 0.0f
                ||
                dist[i + 1u] == 0.0f
                ||
                (
                    (dist[i] > 0.0f)
                    ==
                    (dist[i + 1u] > 0.0f)
                )
            )
            {
                continue;
            }

            if (
                front_count
                >=
                Q2GX_WARP_MAX_VERTS
                ||
                back_count
                >=
                Q2GX_WARP_MAX_VERTS
            )
            {
                return;
            }

            {
                f32 frac =
                    dist[i]
                    /
                    (
                        dist[i]
                        -
                        dist[i + 1u]
                    );

                for (k = 0u; k < 3u; ++k)
                {
                    f32 value =
                        looped[i][k]
                        +
                        frac
                        *
                        (
                            looped[i + 1u][k]
                            -
                            looped[i][k]
                        );

                    front[front_count][k] =
                        value;

                    back[back_count][k] =
                        value;
                }
            }

            ++front_count;
            ++back_count;
        }

        Q2GX_DrawWarpSubdividedPolygon(
            fd,
            texinfo,
            front_count,
            front,
            alpha,
            subpolys,
            vertices
        );

        Q2GX_DrawWarpSubdividedPolygon(
            fd,
            texinfo,
            back_count,
            back,
            alpha,
            subpolys,
            vertices
        );

        return;
    }

    Q2GX_EmitWarpLeafPolygon(
        fd,
        texinfo,
        numverts,
        verts,
        alpha,
        subpolys,
        vertices
    );
}


static void Q2GX_DrawOpaqueWarpWorld(
    refdef_t *fd)
{
    unsigned int local_face;

    unsigned int visible_faces = 0u;
    unsigned int drawn_faces = 0u;
    unsigned int translucent_skipped = 0u;
    unsigned int flowing_faces = 0u;
    unsigned int texture_binds = 0u;
    unsigned int subpolys = 0u;
    unsigned int vertices = 0u;

    if (
        !fd
        ||
        !q2gx_world_faces
        ||
        !q2gx_world_vertices
        ||
        !q2gx_world_texinfos
        ||
        q2gx_world_static_face_count == 0u
    )
    {
        return;
    }

    ++q2gx_world_warp_frames_window;

    for (
        local_face = 0u;
        local_face < q2gx_world_static_face_count;
        ++local_face
    )
    {
        unsigned int face_index =
            q2gx_world_static_first_face
            +
            local_face;

        q2gx_world_face_t *face;
        q2gx_world_texinfo_t *texinfo;
        q2gx_world_wal_texture_t *texture;

        unsigned int original_vertices;
        f32 polygon[Q2GX_WARP_MAX_VERTS][3];
        unsigned int i;

        if (face_index >= q2gx_world_face_count)
            return;

        face =
            &q2gx_world_faces[
                face_index
            ];

        if (!face->visible_this_frame)
            continue;

        if (
            !(
                face->surface_flags
                &
                Q2GX_SURF_WARP
            )
        )
        {
            continue;
        }

        ++visible_faces;

        if (
            face->surface_flags
            &
            (
                Q2GX_SURF_TRANS33
                |
                Q2GX_SURF_TRANS66
            )
        )
        {
            ++translucent_skipped;
            continue;
        }

        if (
            face->texinfo_index
            >=
            q2gx_world_texinfo_count
        )
        {
            continue;
        }

        texinfo =
            &q2gx_world_texinfos[
                face->texinfo_index
            ];

        original_vertices =
            face->triangle_count
            +
            2u;

        if (
            original_vertices < 3u
            ||
            original_vertices > 60u
            ||
            face->vertex_count < 3u
        )
        {
            continue;
        }

        polygon[0][0] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].x;

        polygon[0][1] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].y;

        polygon[0][2] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].z;

        polygon[1][0] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].x;

        polygon[1][1] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].y;

        polygon[1][2] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].z;

        for (i = 2u; i < original_vertices; ++i)
        {
            unsigned int source_offset =
                (i - 2u) * 3u
                +
                2u;

            if (
                source_offset
                >=
                face->vertex_count
            )
            {
                original_vertices = 0u;
                break;
            }

            polygon[i][0] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].x;

            polygon[i][1] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].y;

            polygon[i][2] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].z;
        }

        if (original_vertices == 0u)
            continue;

        texture =
            Q2GX_GetWorldWarpTexture(
                face->texinfo_index
            );

        if (!texture)
            continue;

        Q2GX_SetupOpaqueWarp3D(fd);

        GX_LoadTexObj(
            &texture->texture,
            GX_TEXMAP0
        );

        ++texture_binds;

        Q2GX_DrawWarpSubdividedPolygon(
            fd,
            texinfo,
            original_vertices,
            polygon,
            255u,
            &subpolys,
            &vertices
        );

        ++drawn_faces;

        if (texinfo->flags & Q2GX_SURF_FLOWING)
            ++flowing_faces;

        if (!q2gx_world_warp_first_draw_logged)
        {
            q2gx_world_warp_first_draw_logged = true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX WARP FIRST DRAW: "
                "face=%u "
                "texture=%s "
                "original_vertices=%u "
                "flowing=%u "
                "flags=0x%02x "
                "mode=stock_subdivide_turbsin_wal_v1\n",
                face_index,
                texinfo->texture,
                original_vertices,
                (
                    texinfo->flags
                    &
                    Q2GX_SURF_FLOWING
                )
                ?
                1u
                :
                0u,
                texinfo->flags
            );
        }
    }

    if (drawn_faces > 0u)
        Q2GX_SetupTexturedWorld3D(fd);

    q2gx_world_warp_visible_faces_window += visible_faces;
    q2gx_world_warp_drawn_faces_window += drawn_faces;
    q2gx_world_warp_translucent_skipped_window += translucent_skipped;
    q2gx_world_warp_flowing_faces_window += flowing_faces;
    q2gx_world_warp_texture_binds_window += texture_binds;
    q2gx_world_warp_subpolys_window += subpolys;
    q2gx_world_warp_vertices_window += vertices;

    if (q2gx_world_warp_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX WARP 120: "
            "frames=%u "
            "visible_faces_total=%u "
            "drawn_faces_total=%u "
            "translucent_skipped_total=%u "
            "flowing_faces_total=%u "
            "texture_binds_total=%u "
            "subpolys_total=%u "
            "vertices_total=%u "
            "load_fail_total=%u "
            "mode=stock_subdivide_turbsin_wal_v1\n",
            q2gx_world_warp_frames_window,
            q2gx_world_warp_visible_faces_window,
            q2gx_world_warp_drawn_faces_window,
            q2gx_world_warp_translucent_skipped_window,
            q2gx_world_warp_flowing_faces_window,
            q2gx_world_warp_texture_binds_window,
            q2gx_world_warp_subpolys_window,
            q2gx_world_warp_vertices_window,
            q2gx_world_warp_load_fail_window
        );

        q2gx_world_warp_frames_window = 0u;
        q2gx_world_warp_visible_faces_window = 0u;
        q2gx_world_warp_drawn_faces_window = 0u;
        q2gx_world_warp_translucent_skipped_window = 0u;
        q2gx_world_warp_flowing_faces_window = 0u;
        q2gx_world_warp_texture_binds_window = 0u;
        q2gx_world_warp_subpolys_window = 0u;
        q2gx_world_warp_vertices_window = 0u;
        q2gx_world_warp_load_fail_window = 0u;
    }
}

static qboolean Q2GX_EnsureTransWarpSortBuffer(void)
{
    if (
        q2gx_world_transwarp_sort
        &&
        q2gx_world_transwarp_sort_capacity
        >=
        q2gx_world_static_face_count
    )
    {
        return true;
    }

    if (q2gx_world_transwarp_sort)
    {
        free(q2gx_world_transwarp_sort);
        q2gx_world_transwarp_sort = NULL;
    }

    q2gx_world_transwarp_sort_capacity = 0u;

    if (q2gx_world_static_face_count == 0u)
        return false;

    q2gx_world_transwarp_sort =
        calloc(
            q2gx_world_static_face_count,
            sizeof(*q2gx_world_transwarp_sort)
        );

    if (!q2gx_world_transwarp_sort)
        return false;

    q2gx_world_transwarp_sort_capacity =
        q2gx_world_static_face_count;

    return true;
}


static int Q2GX_CompareTransWarpBackToFront(
    const void *left,
    const void *right)
{
    const q2gx_world_transwarp_sort_t *a = left;
    const q2gx_world_transwarp_sort_t *b = right;

    if (a->depth_sq < b->depth_sq)
        return 1;
    if (a->depth_sq > b->depth_sq)
        return -1;
    if (a->face_index < b->face_index)
        return -1;
    if (a->face_index > b->face_index)
        return 1;
    return 0;
}


static void Q2GX_SetupTranslucentWarp3D(
    refdef_t *fd)
{
    Q2GX_SetupTexturedWorld3D(fd);

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_MODULATE
    );

    GX_SetBlendMode(
        GX_BM_BLEND,
        GX_BL_SRCALPHA,
        GX_BL_INVSRCALPHA,
        GX_LO_CLEAR
    );

    GX_SetZMode(
        GX_TRUE,
        GX_LEQUAL,
        GX_TRUE
    );

    GX_SetCullMode(
        GX_CULL_NONE
    );
}


static void Q2GX_DrawTranslucentWarpWorld(
    refdef_t *fd)
{
    unsigned int local_face;
    unsigned int sort_count = 0u;
    unsigned int sorted_index;

    unsigned int visible_faces = 0u;
    unsigned int drawn_faces = 0u;
    unsigned int alpha33_faces = 0u;
    unsigned int alpha66_faces = 0u;
    unsigned int flowing_faces = 0u;
    unsigned int texture_binds = 0u;
    unsigned int subpolys = 0u;
    unsigned int vertices = 0u;

    if (
        !fd
        ||
        !q2gx_world_faces
        ||
        !q2gx_world_vertices
        ||
        !q2gx_world_texinfos
        ||
        q2gx_world_static_face_count == 0u
    )
    {
        return;
    }

    ++q2gx_world_transwarp_frames_window;

    if (!Q2GX_EnsureTransWarpSortBuffer())
        return;

    for (
        local_face = 0u;
        local_face < q2gx_world_static_face_count;
        ++local_face
    )
    {
        unsigned int face_index =
            q2gx_world_static_first_face
            +
            local_face;

        q2gx_world_face_t *face;
        f32 center[3] = {0.0f, 0.0f, 0.0f};
        f32 dx;
        f32 dy;
        f32 dz;
        unsigned int i;

        if (face_index >= q2gx_world_face_count)
            return;

        face =
            &q2gx_world_faces[
                face_index
            ];

        if (!face->visible_this_frame)
            continue;

        if (
            !(
                face->surface_flags
                &
                Q2GX_SURF_WARP
            )
            ||
            !(
                face->surface_flags
                &
                (
                    Q2GX_SURF_TRANS33
                    |
                    Q2GX_SURF_TRANS66
                )
            )
        )
        {
            continue;
        }

        ++visible_faces;

        if (
            face->vertex_count == 0u
            ||
            sort_count
            >=
            q2gx_world_transwarp_sort_capacity
        )
        {
            continue;
        }

        for (i = 0u; i < face->vertex_count; ++i)
        {
            const q2gx_world_vertex_t *vertex =
                &q2gx_world_vertices[
                    face->first_vertex + i
                ];

            center[0] += vertex->x;
            center[1] += vertex->y;
            center[2] += vertex->z;
        }

        center[0] /= (f32)face->vertex_count;
        center[1] /= (f32)face->vertex_count;
        center[2] /= (f32)face->vertex_count;

        dx = center[0] - fd->vieworg[0];
        dy = center[1] - fd->vieworg[1];
        dz = center[2] - fd->vieworg[2];

        q2gx_world_transwarp_sort[
            sort_count
        ].face_index = face_index;

        q2gx_world_transwarp_sort[
            sort_count
        ].depth_sq =
            dx * dx
            +
            dy * dy
            +
            dz * dz;

        ++sort_count;
    }

    if (sort_count > 1u)
    {
        qsort(
            q2gx_world_transwarp_sort,
            sort_count,
            sizeof(*q2gx_world_transwarp_sort),
            Q2GX_CompareTransWarpBackToFront
        );
    }

    if (sort_count > 0u)
        Q2GX_SetupTranslucentWarp3D(fd);

    for (
        sorted_index = 0u;
        sorted_index < sort_count;
        ++sorted_index
    )
    {
        unsigned int face_index =
            q2gx_world_transwarp_sort[
                sorted_index
            ].face_index;

        q2gx_world_face_t *face =
            &q2gx_world_faces[
                face_index
            ];

        q2gx_world_texinfo_t *texinfo;
        q2gx_world_wal_texture_t *texture;

        unsigned int original_vertices;
        f32 polygon[Q2GX_WARP_MAX_VERTS][3];
        unsigned int i;
        u8 alpha;

        if (
            face->texinfo_index
            >=
            q2gx_world_texinfo_count
        )
        {
            continue;
        }

        texinfo =
            &q2gx_world_texinfos[
                face->texinfo_index
            ];

        if (face->surface_flags & Q2GX_SURF_TRANS33)
        {
            alpha = 84u;
            ++alpha33_faces;
        }
        else if (face->surface_flags & Q2GX_SURF_TRANS66)
        {
            alpha = 168u;
            ++alpha66_faces;
        }
        else
        {
            continue;
        }

        original_vertices =
            face->triangle_count
            +
            2u;

        if (
            original_vertices < 3u
            ||
            original_vertices > 60u
            ||
            face->vertex_count < 3u
        )
        {
            continue;
        }

        polygon[0][0] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].x;
        polygon[0][1] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].y;
        polygon[0][2] =
            q2gx_world_vertices[
                face->first_vertex + 0u
            ].z;

        polygon[1][0] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].x;
        polygon[1][1] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].y;
        polygon[1][2] =
            q2gx_world_vertices[
                face->first_vertex + 1u
            ].z;

        for (i = 2u; i < original_vertices; ++i)
        {
            unsigned int source_offset =
                (i - 2u) * 3u
                +
                2u;

            if (
                source_offset
                >=
                face->vertex_count
            )
            {
                original_vertices = 0u;
                break;
            }

            polygon[i][0] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].x;
            polygon[i][1] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].y;
            polygon[i][2] =
                q2gx_world_vertices[
                    face->first_vertex
                    +
                    source_offset
                ].z;
        }

        if (original_vertices == 0u)
            continue;

        texture =
            Q2GX_GetWorldWarpTexture(
                face->texinfo_index
            );

        if (!texture)
            continue;

        GX_LoadTexObj(
            &texture->texture,
            GX_TEXMAP0
        );

        ++texture_binds;

        Q2GX_DrawWarpSubdividedPolygon(
            fd,
            texinfo,
            original_vertices,
            polygon,
            alpha,
            &subpolys,
            &vertices
        );

        ++drawn_faces;

        if (texinfo->flags & Q2GX_SURF_FLOWING)
            ++flowing_faces;

        if (!q2gx_world_transwarp_first_draw_logged)
        {
            q2gx_world_transwarp_first_draw_logged = true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX TRANSWARP FIRST DRAW: "
                "face=%u "
                "texture=%s "
                "alpha_u8=%u "
                "original_vertices=%u "
                "flowing=%u "
                "flags=0x%02x "
                "mode=stock_alpha_far_to_near_subdivide_turbsin_wal_v1\n",
                face_index,
                texinfo->texture,
                (unsigned int)alpha,
                original_vertices,
                (
                    texinfo->flags
                    &
                    Q2GX_SURF_FLOWING
                )
                ?
                1u
                :
                0u,
                texinfo->flags
            );
        }
    }

    if (drawn_faces > 0u)
        Q2GX_SetupTexturedWorld3D(fd);

    q2gx_world_transwarp_visible_faces_window += visible_faces;
    q2gx_world_transwarp_drawn_faces_window += drawn_faces;
    q2gx_world_transwarp_alpha33_faces_window += alpha33_faces;
    q2gx_world_transwarp_alpha66_faces_window += alpha66_faces;
    q2gx_world_transwarp_flowing_faces_window += flowing_faces;
    q2gx_world_transwarp_texture_binds_window += texture_binds;
    q2gx_world_transwarp_subpolys_window += subpolys;
    q2gx_world_transwarp_vertices_window += vertices;

    if (q2gx_world_transwarp_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TRANSWARP 120: "
            "frames=%u "
            "visible_faces_total=%u "
            "drawn_faces_total=%u "
            "alpha33_faces_total=%u "
            "alpha66_faces_total=%u "
            "flowing_faces_total=%u "
            "texture_binds_total=%u "
            "subpolys_total=%u "
            "vertices_total=%u "
            "mode=stock_alpha_far_to_near_subdivide_turbsin_wal_v1\n",
            q2gx_world_transwarp_frames_window,
            q2gx_world_transwarp_visible_faces_window,
            q2gx_world_transwarp_drawn_faces_window,
            q2gx_world_transwarp_alpha33_faces_window,
            q2gx_world_transwarp_alpha66_faces_window,
            q2gx_world_transwarp_flowing_faces_window,
            q2gx_world_transwarp_texture_binds_window,
            q2gx_world_transwarp_subpolys_window,
            q2gx_world_transwarp_vertices_window
        );

        q2gx_world_transwarp_frames_window = 0u;
        q2gx_world_transwarp_visible_faces_window = 0u;
        q2gx_world_transwarp_drawn_faces_window = 0u;
        q2gx_world_transwarp_alpha33_faces_window = 0u;
        q2gx_world_transwarp_alpha66_faces_window = 0u;
        q2gx_world_transwarp_flowing_faces_window = 0u;
        q2gx_world_transwarp_texture_binds_window = 0u;
        q2gx_world_transwarp_subpolys_window = 0u;
        q2gx_world_transwarp_vertices_window = 0u;
    }
}




static u8 Q2GX_Float01ToU8(f32 value)
{
    if (value <= 0.0f)
        return 0u;

    if (value >= 1.0f)
        return 255u;

    return (u8)(value * 255.0f + 0.5f);
}


/*
 * Q2GC_REFDEF_POLYBLEND_V1
 *
 * Stock ref_gl's R_PolyBlend consumes the already-composed refdef blend
 * after the 3D scene.  Do the same here: no contents-color recomputation
 * in the renderer.
 */


static int Q2GX_UnderwaterSoftTurb(
    int sample,
    int phase)
{
    int index;
    double angle;
    double value;

    index =
        (sample + phase)
        &
        ((int)Q2GX_UNDERWATER_SOFT_CYCLE - 1);

    angle =
        (
            (double)index
            *
            2.0
            *
            3.14159265358979323846
        )
        /
        (double)Q2GX_UNDERWATER_SOFT_CYCLE;

    value =
        (double)Q2GX_UNDERWATER_SOFT_AMP2
        +
        sin(angle)
        *
        (double)Q2GX_UNDERWATER_SOFT_AMP2;

    return (int)value;
}


static qboolean Q2GX_EnsureUnderwaterCopyTexture(void)
{
    if (
        q2gx_underwater_copy_ready
        &&
        q2gx_underwater_copy_buffer
    )
    {
        return true;
    }

    q2gx_underwater_copy_bytes =
        GX_GetTexBufferSize(
            Q2GX_UNDERWATER_COPY_WIDTH,
            Q2GX_UNDERWATER_COPY_HEIGHT,
            GX_TF_RGB565,
            GX_FALSE,
            0u
        );

    if (q2gx_underwater_copy_bytes == 0u)
    {
        ++q2gx_underwater_warp_alloc_fail_window;
        return false;
    }

    q2gx_underwater_copy_buffer =
        memalign(
            32u,
            q2gx_underwater_copy_bytes
        );

    if (!q2gx_underwater_copy_buffer)
    {
        ++q2gx_underwater_warp_alloc_fail_window;
        q2gx_underwater_copy_bytes = 0u;
        return false;
    }

    GX_InitTexObj(
        &q2gx_underwater_copy_texture,
        q2gx_underwater_copy_buffer,
        Q2GX_UNDERWATER_COPY_WIDTH,
        Q2GX_UNDERWATER_COPY_HEIGHT,
        GX_TF_RGB565,
        GX_CLAMP,
        GX_CLAMP,
        GX_FALSE
    );

    GX_InitTexObjFilterMode(
        &q2gx_underwater_copy_texture,
        GX_NEAR,
        GX_NEAR
    );

    q2gx_underwater_copy_ready = true;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX UNDERWATER WARP ALLOC: "
        "width=%u height=%u bytes=%u format=RGB565 "
        "target=gamecube_flipper_gx\n",
        Q2GX_UNDERWATER_COPY_WIDTH,
        Q2GX_UNDERWATER_COPY_HEIGHT,
        (unsigned int)q2gx_underwater_copy_bytes
    );

    return true;
}


static void Q2GX_SetupUnderwaterWarp2D(void)
{
    Q2GX_Setup2D();

    GX_ClearVtxDesc();

    GX_SetVtxDesc(
        GX_VA_POS,
        GX_DIRECT
    );

    GX_SetVtxDesc(
        GX_VA_CLR0,
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
        GX_VA_CLR0,
        GX_CLR_RGBA,
        GX_RGBA8,
        0
    );

    GX_SetVtxAttrFmt(
        GX_VTXFMT0,
        GX_VA_TEX0,
        GX_TEX_ST,
        GX_F32,
        0
    );

    GX_SetNumChans(1);
    GX_SetNumTexGens(1);

    GX_SetTexCoordGen(
        GX_TEXCOORD0,
        GX_TG_MTX2x4,
        GX_TG_TEX0,
        GX_IDENTITY
    );

    GX_SetNumTevStages(1);

    GX_SetTevOrder(
        GX_TEVSTAGE0,
        GX_TEXCOORD0,
        GX_TEXMAP0,
        GX_COLOR0A0
    );

    GX_SetTevOp(
        GX_TEVSTAGE0,
        GX_MODULATE
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
        GX_BL_ONE,
        GX_BL_ZERO,
        GX_LO_CLEAR
    );
}


static void Q2GX_EmitUnderwaterWarpVertex(
    f32 x,
    f32 y,
    int phase)
{
    int dx;
    int dy;
    f32 s;
    f32 t;

    dx =
        Q2GX_UnderwaterSoftTurb(
            (int)y,
            phase
        );

    dy =
        Q2GX_UnderwaterSoftTurb(
            (int)x,
            phase
        );

    s =
        (
            x
            +
            (f32)dx
        )
        /
        (
            (f32)Q2GX_UNDERWATER_COPY_WIDTH
            +
            (f32)(Q2GX_UNDERWATER_SOFT_AMP2 * 2u)
        );

    t =
        (
            y
            +
            (f32)dy
        )
        /
        (
            (f32)Q2GX_UNDERWATER_COPY_HEIGHT
            +
            (f32)(Q2GX_UNDERWATER_SOFT_AMP2 * 2u)
        );

    GX_Position2f32(x, y);
    GX_Color4u8(255u, 255u, 255u, 255u);
    GX_TexCoord2f32(s, t);
}


static void Q2GX_DrawUnderwaterWarp(
    refdef_t *fd)
{
    unsigned int y;
    unsigned int x;
    unsigned int quads = 0u;
    unsigned int vertices = 0u;
    int phase;

    if (!fd)
        return;

    ++q2gx_underwater_warp_frames_window;

    if (
        !(
            fd->rdflags
            &
            RDF_UNDERWATER
        )
    )
    {
        goto report;
    }

    if (
        fd->x != 0
        ||
        fd->y != 0
        ||
        fd->width
        !=
        (int)Q2GX_UNDERWATER_COPY_WIDTH
        ||
        fd->height
        !=
        (int)Q2GX_UNDERWATER_COPY_HEIGHT
    )
    {
        ++q2gx_underwater_warp_bad_refdef_window;
        goto report;
    }

    if (!Q2GX_EnsureUnderwaterCopyTexture())
        goto report;

    GX_SetTexCopySrc(
        0u,
        0u,
        Q2GX_UNDERWATER_COPY_WIDTH,
        Q2GX_UNDERWATER_COPY_HEIGHT
    );

    GX_SetTexCopyDst(
        Q2GX_UNDERWATER_COPY_WIDTH,
        Q2GX_UNDERWATER_COPY_HEIGHT,
        GX_TF_RGB565,
        GX_FALSE
    );

    GX_CopyTex(
        q2gx_underwater_copy_buffer,
        GX_FALSE
    );

    GX_PixModeSync();
    GX_InvalidateTexAll();

    ++q2gx_underwater_warp_copies_window;
    ++q2gx_underwater_warp_active_frames_window;

    Q2GX_SetupUnderwaterWarp2D();

    GX_LoadTexObj(
        &q2gx_underwater_copy_texture,
        GX_TEXMAP0
    );

    phase =
        (
            (int)(
                fd->time
                *
                (f32)Q2GX_UNDERWATER_SOFT_SPEED
            )
        )
        &
        ((int)Q2GX_UNDERWATER_SOFT_CYCLE - 1);

    GX_Begin(
        GX_QUADS,
        GX_VTXFMT0,
        (u16)(
            (
                Q2GX_UNDERWATER_COPY_WIDTH
                /
                Q2GX_UNDERWATER_MESH_STEP
            )
            *
            (
                Q2GX_UNDERWATER_COPY_HEIGHT
                /
                Q2GX_UNDERWATER_MESH_STEP
            )
            *
            4u
        )
    );

    for (
        y = 0u;
        y < Q2GX_UNDERWATER_COPY_HEIGHT;
        y += Q2GX_UNDERWATER_MESH_STEP
    )
    {
        for (
            x = 0u;
            x < Q2GX_UNDERWATER_COPY_WIDTH;
            x += Q2GX_UNDERWATER_MESH_STEP
        )
        {
            f32 x0 = (f32)x;
            f32 y0 = (f32)y;
            f32 x1 =
                (f32)(
                    x
                    +
                    Q2GX_UNDERWATER_MESH_STEP
                );
            f32 y1 =
                (f32)(
                    y
                    +
                    Q2GX_UNDERWATER_MESH_STEP
                );

            Q2GX_EmitUnderwaterWarpVertex(
                x0,
                y0,
                phase
            );

            Q2GX_EmitUnderwaterWarpVertex(
                x1,
                y0,
                phase
            );

            Q2GX_EmitUnderwaterWarpVertex(
                x1,
                y1,
                phase
            );

            Q2GX_EmitUnderwaterWarpVertex(
                x0,
                y1,
                phase
            );

            ++quads;
            vertices += 4u;
        }
    }

    GX_End();

    q2gx_underwater_warp_quads_window +=
        quads;

    q2gx_underwater_warp_vertices_window +=
        vertices;

    if (!q2gx_underwater_warp_first_logged)
    {
        q2gx_underwater_warp_first_logged = true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX UNDERWATER WARP FIRST: "
            "copy=%ux%u "
            "bytes=%u "
            "mesh_step=%u "
            "quads=%u "
            "vertices=%u "
            "cycle=%u "
            "amp2=%u "
            "speed=%u "
            "phase=%d "
            "format=RGB565 "
            "target=gamecube_flipper_gx "
            "mode=efb_copy_softwarp_mesh_v1\n",
            Q2GX_UNDERWATER_COPY_WIDTH,
            Q2GX_UNDERWATER_COPY_HEIGHT,
            (unsigned int)q2gx_underwater_copy_bytes,
            Q2GX_UNDERWATER_MESH_STEP,
            quads,
            vertices,
            Q2GX_UNDERWATER_SOFT_CYCLE,
            Q2GX_UNDERWATER_SOFT_AMP2,
            Q2GX_UNDERWATER_SOFT_SPEED,
            phase
        );
    }

report:
    if (q2gx_underwater_warp_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX UNDERWATER WARP 120: "
            "frames=%u "
            "underwater_frames=%u "
            "copies=%u "
            "quads=%u "
            "vertices=%u "
            "alloc_fail=%u "
            "bad_refdef=%u "
            "bytes=%u "
            "target=gamecube_flipper_gx "
            "mode=efb_copy_softwarp_mesh_v1\n",
            q2gx_underwater_warp_frames_window,
            q2gx_underwater_warp_active_frames_window,
            q2gx_underwater_warp_copies_window,
            q2gx_underwater_warp_quads_window,
            q2gx_underwater_warp_vertices_window,
            q2gx_underwater_warp_alloc_fail_window,
            q2gx_underwater_warp_bad_refdef_window,
            (unsigned int)q2gx_underwater_copy_bytes
        );

        q2gx_underwater_warp_frames_window = 0u;
        q2gx_underwater_warp_active_frames_window = 0u;
        q2gx_underwater_warp_copies_window = 0u;
        q2gx_underwater_warp_quads_window = 0u;
        q2gx_underwater_warp_vertices_window = 0u;
        q2gx_underwater_warp_alloc_fail_window = 0u;
        q2gx_underwater_warp_bad_refdef_window = 0u;
    }
}


static void Q2GX_DrawRefdefPolyBlend(refdef_t *fd)
{
    u8 r;
    u8 g;
    u8 b;
    u8 a;
    qboolean underwater;

    if (!fd)
        return;

    ++q2gx_polyblend_frames_window;

    underwater =
        (fd->rdflags & RDF_UNDERWATER)
        ? true
        : false;

    a = Q2GX_Float01ToU8(fd->blend[3]);

    if (a > 0u)
    {
        r = Q2GX_Float01ToU8(fd->blend[0]);
        g = Q2GX_Float01ToU8(fd->blend[1]);
        b = Q2GX_Float01ToU8(fd->blend[2]);

        Q2GX_Setup2D();

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

        GX_SetCullMode(GX_CULL_NONE);

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

        GX_Begin(
            GX_QUADS,
            GX_VTXFMT0,
            4u
        );

        GX_Position2f32(0.0f, 0.0f);
        GX_Color4u8(r, g, b, a);

        GX_Position2f32(640.0f, 0.0f);
        GX_Color4u8(r, g, b, a);

        GX_Position2f32(640.0f, 480.0f);
        GX_Color4u8(r, g, b, a);

        GX_Position2f32(0.0f, 480.0f);
        GX_Color4u8(r, g, b, a);

        GX_End();

        ++q2gx_polyblend_active_frames_window;
        ++q2gx_polyblend_quads_window;

        if (
            q2gx_polyblend_alpha_min_window == 0u
            ||
            (unsigned int)a < q2gx_polyblend_alpha_min_window
        )
        {
            q2gx_polyblend_alpha_min_window = (unsigned int)a;
        }

        if ((unsigned int)a > q2gx_polyblend_alpha_max_window)
            q2gx_polyblend_alpha_max_window = (unsigned int)a;

        if (underwater)
        {
            ++q2gx_polyblend_underwater_frames_window;

            if (!q2gx_polyblend_underwater_first_logged)
            {
                q2gx_polyblend_underwater_first_logged = true;

                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX POLYBLEND UNDERWATER FIRST: "
                    "rgba_u8=%u,%u,%u,%u "
                    "blend=%.3f,%.3f,%.3f,%.3f "
                    "rdflags=0x%x "
                    "mode=refdef_srcalpha_quad_v1\n",
                    (unsigned int)r,
                    (unsigned int)g,
                    (unsigned int)b,
                    (unsigned int)a,
                    fd->blend[0],
                    fd->blend[1],
                    fd->blend[2],
                    fd->blend[3],
                    fd->rdflags
                );
            }
        }
    }

    if (q2gx_polyblend_frames_window >= 120u)
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX POLYBLEND 120: "
            "frames=%u "
            "active_frames=%u "
            "underwater_frames=%u "
            "quads=%u "
            "alpha_min_u8=%u "
            "alpha_max_u8=%u "
            "mode=refdef_srcalpha_quad_v1\n",
            q2gx_polyblend_frames_window,
            q2gx_polyblend_active_frames_window,
            q2gx_polyblend_underwater_frames_window,
            q2gx_polyblend_quads_window,
            q2gx_polyblend_alpha_min_window,
            q2gx_polyblend_alpha_max_window
        );

        q2gx_polyblend_frames_window = 0u;
        q2gx_polyblend_active_frames_window = 0u;
        q2gx_polyblend_underwater_frames_window = 0u;
        q2gx_polyblend_quads_window = 0u;
        q2gx_polyblend_alpha_min_window = 0u;
        q2gx_polyblend_alpha_max_window = 0u;
    }
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

    if (pvs_faces > q2gx_world_static_face_count)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: marked face count exceeds total\n");
        return;
    }

    pvs_rejected_faces =
        q2gx_world_static_face_count
        -
        pvs_faces;
    backface_rejected_faces = 0u;
    submitted_faces = 0u;
    submitted_triangles = 0u;
    submitted_vertices = 0u;

    for (
        face_index = q2gx_world_static_first_face;
        face_index
        <
        q2gx_world_static_first_face
        +
        q2gx_world_static_face_count;
        ++face_index
    )
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

    if (pvs_faces
        +
        pvs_rejected_faces
        !=
        q2gx_world_static_face_count)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: total face accounting mismatch\n");
        return;
    }

    if (submitted_vertices != submitted_triangles * 3u)
    {
        ri.Con_Printf(PRINT_ALL, "Q2GC REF_GX PVS: triangle accounting mismatch\n");
        return;
    }

    Q2GX_SetupTexturedWorld3D(
        fd
    );


    if (!q2gx_world_texcoord_first_draw_logged)
    {
        q2gx_world_texcoord_first_draw_logged = true;

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TEXCOORD FIRST DRAW: "
            "submitted_faces=%u "
            "submitted_triangles=%u "
            "submitted_vertices=%u "
            "texinfos=%u "
            "checker=64x64 "
            "wrap=repeat "
            "mode=diagnostic_checker\n",
            submitted_faces,
            submitted_triangles,
            submitted_vertices,
            q2gx_world_texinfo_count
        );
    }

{
        unsigned int wal_textured_faces = 0u;
        unsigned int wal_textured_vertices = 0u;

        unsigned int wal_animated_flat_faces = 0u;

/* Q2GC_WORLD_ANIMATED_WAL_V1 */
        unsigned int wal_animated_textured_faces = 0u;
        unsigned int wal_animated_texture_binds = 0u;

        unsigned int wal_special_flat_faces = 0u;
        unsigned int wal_sky_faces = 0u;
        unsigned int wal_opaque_warp_faces = 0u;
        unsigned int wal_translucent_warp_faces = 0u;

        unsigned int wal_visible_textures = 0u;
        unsigned int wal_texture_binds = 0u;
        unsigned int wal_batch_draw_calls = 0u;

        unsigned int texture_index;

        /*
         * Q2GC_WORLD_TEXTURE_BATCHING_V1
         *
         * Static opaque WAL pass:
         * one precomputed face list per unique WAL.
         */
        for (
            texture_index = 0u;
            texture_index < q2gx_world_wal_texture_count;
            ++texture_index
        )
        {
            q2gx_world_wal_texture_t *batch_texture =
                &q2gx_world_wal_textures[
                    texture_index
                ];

            unsigned int visible_vertices = 0u;
            unsigned int visible_faces = 0u;
            unsigned int list_index;

            for (
                list_index = 0u;
                list_index < batch_texture->face_count;
                ++list_index
            )
            {
                unsigned int referenced_face =
                    batch_texture->face_indices[
                        list_index
                    ];

                q2gx_world_face_t *face;

                if (
                    referenced_face
                    >=
                    q2gx_world_face_count
                )
                {
                    ri.Con_Printf(
                        PRINT_ALL,
                        "Q2GC REF_GX BATCH: "
                        "runtime face index overflow "
                        "cache=%u face=%u\n",
                        texture_index,
                        referenced_face
                    );

                    return;
                }

                face =
                    &q2gx_world_faces[
                        referenced_face
                    ];

                if (!face->visible_this_frame)
                    continue;

                if (
                    UINT_MAX
                    -
                    visible_vertices
                    <
                    face->vertex_count
                )
                {
                    ri.Con_Printf(
                        PRINT_ALL,
                        "Q2GC REF_GX BATCH: "
                        "visible vertex overflow "
                        "cache=%u\n",
                        texture_index
                    );

                    return;
                }

                visible_vertices +=
                    face->vertex_count;

                ++visible_faces;
            }

            if (visible_faces == 0u)
                continue;

            Q2GX_BindWorldWALTexture(
                batch_texture
            );

            ++wal_visible_textures;
            ++wal_texture_binds;

            wal_textured_faces +=
                visible_faces;

            wal_textured_vertices +=
                visible_vertices;

            /*
             * Generic chunking retained for other maps.
             * The base1 quarry proved every full WAL batch is
             * <= 4881 vertices, so base1 uses one draw per
             * visible texture.
             */
            list_index = 0u;

            while (
                list_index
                <
                batch_texture->face_count
            )
            {
                unsigned int chunk_start =
                    list_index;

                unsigned int chunk_end =
                    list_index;

                unsigned int chunk_vertices =
                    0u;

                unsigned int emit_index;

                while (
                    chunk_end
                    <
                    batch_texture->face_count
                )
                {
                    unsigned int referenced_face =
                        batch_texture->face_indices[
                            chunk_end
                        ];

                    q2gx_world_face_t *face =
                        &q2gx_world_faces[
                            referenced_face
                        ];

                    if (!face->visible_this_frame)
                    {
                        ++chunk_end;
                        continue;
                    }

                    if (
                        face->vertex_count
                        >
                        65532u
                    )
                    {
                        ri.Con_Printf(
                            PRINT_ALL,
                            "Q2GC REF_GX BATCH: "
                            "single face too large "
                            "face=%u vertices=%u\n",
                            referenced_face,
                            face->vertex_count
                        );

                        return;
                    }

                    if (
                        chunk_vertices > 0u
                        &&
                        chunk_vertices
                        +
                        face->vertex_count
                        >
                        65532u
                    )
                    {
                        break;
                    }

                    chunk_vertices +=
                        face->vertex_count;

                    ++chunk_end;
                }

                if (chunk_vertices == 0u)
                {
                    list_index =
                        chunk_end;

                    continue;
                }

                GX_Begin(
                    GX_TRIANGLES,
                    GX_VTXFMT0,
                    (u16)
                    chunk_vertices
                );

                for (
                    emit_index = chunk_start;
                    emit_index < chunk_end;
                    ++emit_index
                )
                {
                    unsigned int referenced_face =
                        batch_texture->face_indices[
                            emit_index
                        ];

                    q2gx_world_face_t *face =
                        &q2gx_world_faces[
                            referenced_face
                        ];

                    unsigned int vertex_index;

                    if (!face->visible_this_frame)
                        continue;

                    for (
                        vertex_index = 0u;
                        vertex_index < face->vertex_count;
                        ++vertex_index
                    )
                    {
                        const q2gx_world_vertex_t *vertex =
                            &q2gx_world_vertices[
                                face->first_vertex
                                +
                                vertex_index
                            ];

                        GX_Position3f32(
                            vertex->x,
                            vertex->y,
                            vertex->z
                        );

                        GX_Color4u8(
                            vertex->r,
                            vertex->g,
                            vertex->b,
                            vertex->a
                        );

                        GX_TexCoord2f32(
                            vertex->s,
                            vertex->t
                        );
                    }
                }

                GX_End();

                ++wal_batch_draw_calls;

                list_index =
                    chunk_end;
            }
        }

        /*
         * Keep animated/special fallback faces separate and in
         * original BSP face order.
         */
        for (
        face_index = q2gx_world_static_first_face;
        face_index
        <
        q2gx_world_static_first_face
        +
        q2gx_world_static_face_count;
        ++face_index
    )
        {
            q2gx_world_face_t *face =
                &q2gx_world_faces[
                    face_index
                ];

            q2gx_world_texinfo_t *texinfo;
            unsigned int vertex_index;

            if (!face->visible_this_frame)
                continue;

            if (
                face->surface_flags
                &
                Q2GX_SURF_SKY
            )
            {
                ++wal_sky_faces;
                continue;
            }

            if (
                face->surface_flags
                &
                Q2GX_SURF_WARP
            )
            {
                if (
                    face->surface_flags
                    &
                    (
                        Q2GX_SURF_TRANS33
                        |
                        Q2GX_SURF_TRANS66
                    )
                )
                {
                    ++wal_translucent_warp_faces;
                }
                else
                {
                    ++wal_opaque_warp_faces;
                }

                continue;
            }

            texinfo =
                &q2gx_world_texinfos[
                    face->texinfo_index
                ];

            if (
                !(
                    face->surface_flags
                    &
                    Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
                )
                &&
                texinfo->next_texinfo < 0
            )
            {
                continue;
            }

            if (
                face->surface_flags
                &
                Q2GX_WORLD_TEXCOORD_SPECIAL_MASK
            )
            {
                Q2GX_BindWorldFlatFallback();
                ++wal_special_flat_faces;
            }
            else
            {
                unsigned int animated_texinfo_index =
                    Q2GX_SelectAnimatedWorldTexinfo(
                        face->texinfo_index,
                        (int)(fd->time * 2.0f)
                    );

                q2gx_world_wal_texture_t *animated_texture =
                    Q2GX_GetWorldWarpTexture(
                        animated_texinfo_index
                    );

                if (animated_texture)
                {
                    Q2GX_BindWorldWALTexture(
                        animated_texture
                    );

                    ++wal_animated_textured_faces;
                    ++wal_animated_texture_binds;
                }
                else
                {
                    Q2GX_BindWorldFlatFallback();
                    ++wal_animated_flat_faces;
                }
            }

            if (face->vertex_count > 65535u)
            {
                ri.Con_Printf(
                    PRINT_ALL,
                    "Q2GC REF_GX BATCH: "
                    "fallback face %u too large vertices=%u\n",
                    face_index,
                    face->vertex_count
                );

                return;
            }

            GX_Begin(
                GX_TRIANGLES,
                GX_VTXFMT0,
                (u16)
                face->vertex_count
            );

            for (
                vertex_index = 0u;
                vertex_index < face->vertex_count;
                ++vertex_index
            )
            {
                const q2gx_world_vertex_t *vertex =
                    &q2gx_world_vertices[
                        face->first_vertex
                        +
                        vertex_index
                    ];

                GX_Position3f32(
                    vertex->x,
                    vertex->y,
                    vertex->z
                );

                GX_Color4u8(
                    vertex->r,
                    vertex->g,
                    vertex->b,
                    vertex->a
                );

                GX_TexCoord2f32(
                    vertex->s,
                    vertex->t
                );
            }

            GX_End();
        }

        if (
            wal_textured_faces
            +
            wal_animated_textured_faces
            +
            wal_animated_flat_faces
            +
            wal_special_flat_faces
            +
            wal_sky_faces
            +
            wal_opaque_warp_faces
            +
            wal_translucent_warp_faces
            !=
            submitted_faces
        )
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX BATCH: draw accounting mismatch "
                "textured=%u anim_textured=%u "
                "anim_fallback=%u special=%u submitted=%u\n",
                wal_textured_faces,
                wal_animated_textured_faces,
                wal_animated_flat_faces,
                wal_special_flat_faces,
                submitted_faces
            );

            return;
        }

        if (
            wal_texture_binds
            !=
            wal_visible_textures
        )
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX BATCH: "
                "bind/visible-texture mismatch "
                "binds=%u visible=%u\n",
                wal_texture_binds,
                wal_visible_textures
            );

            return;
        }

        if (
            wal_batch_draw_calls
            <
            wal_visible_textures
        )
        {
            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX BATCH: "
                "draw calls below visible textures\n"
            );

            return;
        }

        if (!q2gx_world_wal_first_draw_logged)
        {
            q2gx_world_wal_first_draw_logged =
                true;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX WAL FIRST DRAW: "
                "textured_faces=%u "
                "animated_flat_faces=%u "
                "special_flat_faces=%u "
                "texture_binds=%u "
                "cached_textures=%u "
                "rgba8_bytes=%u "
                "mode=wal_mip0\n",
                wal_textured_faces,
                wal_animated_flat_faces,
                wal_special_flat_faces,
                wal_texture_binds,
                q2gx_world_wal_texture_count,
                q2gx_world_wal_texture_bytes
            );

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX BATCH FIRST DRAW: "
                "textured_faces=%u "
                "textured_vertices=%u "
                "visible_textures=%u "
                "draw_calls=%u "
                "texture_binds=%u "
                "cached_textures=%u "
                "mode=wal_texture_batches\n",
                wal_textured_faces,
                wal_textured_vertices,
                wal_visible_textures,
                wal_batch_draw_calls,
                wal_texture_binds,
                q2gx_world_wal_texture_count
            );
        }

        q2gx_world_wal_textured_faces_window +=
            wal_textured_faces;

        q2gx_world_wal_animated_flat_faces_window +=
            wal_animated_flat_faces;

        q2gx_world_wal_animated_textured_faces_window +=
            wal_animated_textured_faces;

        q2gx_world_wal_animated_texture_binds_window +=
            wal_animated_texture_binds;

        q2gx_world_wal_special_flat_faces_window +=
            wal_special_flat_faces;

        q2gx_world_wal_texture_binds_window +=
            wal_texture_binds;

        q2gx_world_wal_visible_textures_window +=
            wal_visible_textures;

        q2gx_world_wal_batch_draw_calls_window +=
            wal_batch_draw_calls;

        q2gx_world_wal_batch_vertices_window +=
            wal_textured_vertices;
    }

Q2GX_DrawOpaqueWarpWorld(fd);

    Q2GX_DrawSkyBox(fd);

Q2GX_DrawBrushEntities(
        fd
    );

    Q2GX_DrawAliasEntities(
        fd
    );

    Q2GX_DrawViewWeaponEntities(
        fd
    );

    Q2GX_RebuildAliasInputContract();

    q2gx_alias_translucent_pass = true;

    Q2GX_DrawAliasEntities(
        fd
    );

    q2gx_alias_translucent_pass = false;

    /*
     * Q2GC_PARTICLES_V1
     *
     * Stock order places particles after entity rendering.
     * Dynamic lights / alpha surfaces are not native-GX milestones yet,
     * so particles follow the completed opaque/view/translucent entity stack.
     */
    Q2GX_DrawParticles(fd);

    Q2GX_DrawTranslucentBrushEntities(fd);

    Q2GX_DrawTranslucentWarpWorld(fd);

    ++q2gx_world_frames_window;
    q2gx_world_pvs_faces_window += pvs_faces;
    q2gx_world_pvs_rejected_faces_window += pvs_rejected_faces;
    q2gx_world_backface_rejected_faces_window += backface_rejected_faces;
    q2gx_world_submitted_faces_window += submitted_faces;
    q2gx_world_submitted_triangles_window += submitted_triangles;
    q2gx_world_submitted_vertices_window += submitted_vertices;

    q2gx_world_texcoord_vertices_window +=
        submitted_vertices;


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
            q2gx_world_static_face_count,
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
            q2gx_world_static_face_count,
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

                        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BATCH 120: "
            "frames=%u "
            "textured_faces_total=%u "
            "textured_vertices_total=%u "
            "visible_textures_total=%u "
            "draw_calls_total=%u "
            "texture_binds_total=%u "
            "cached_textures=%u "
            "mode=wal_texture_batches\n",
            q2gx_world_frames_window,
            q2gx_world_wal_textured_faces_window,
            q2gx_world_wal_batch_vertices_window,
            q2gx_world_wal_visible_textures_window,
            q2gx_world_wal_batch_draw_calls_window,
            q2gx_world_wal_texture_binds_window,
            q2gx_world_wal_texture_count
        );

ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX WAL 120: "
            "frames=%u "
            "textured_faces_total=%u "
            "animated_flat_faces_total=%u "
            "special_flat_faces_total=%u "
            "texture_binds_total=%u "
            "cached_textures=%u "
            "rgba8_bytes=%u "
            "mode=wal_mip0\n",
            q2gx_world_frames_window,
            q2gx_world_wal_textured_faces_window,
            q2gx_world_wal_animated_flat_faces_window,
            q2gx_world_wal_special_flat_faces_window,
            q2gx_world_wal_texture_binds_window,
            q2gx_world_wal_texture_count,
            q2gx_world_wal_texture_bytes
        );

ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX ANIMWAL 120: "
            "frames=%u "
            "textured_faces_total=%u "
            "fallback_faces_total=%u "
            "texture_binds_total=%u "
            "world_frame=%d "
            "mode=nexttexinfo_time2_v1\n",
            q2gx_world_frames_window,
            q2gx_world_wal_animated_textured_faces_window,
            q2gx_world_wal_animated_flat_faces_window,
            q2gx_world_wal_animated_texture_binds_window,
            (int)(fd->time * 2.0f)
        );

ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX TEXCOORD 120: "
            "frames=%u "
            "streamed_vertices_total=%u "
            "texinfos=%u "
            "checker=64x64 "
            "wrap=repeat "
            "mode=diagnostic_checker\n",
            q2gx_world_frames_window,
            q2gx_world_texcoord_vertices_window,
            q2gx_world_texinfo_count
        );

        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH 120: "
            "frames=%u "
            "entities_total=%u "
            "faces_tested_total=%u "
            "backface_rejected_total=%u "
            "visible_faces_total=%u "
            "textured_faces_total=%u "
            "fallback_faces_total=%u "
            "deferred_trans_faces_total=%u "
            "wal_binds_total=%u "
            "vertices_total=%u "
            "registered_inline=%u "
            "mode=inline_bsp_entities\n",
            q2gx_world_frames_window,
            q2gx_brush_entities_window,
            q2gx_brush_faces_tested_window,
            q2gx_brush_backface_rejected_window,
            q2gx_brush_visible_faces_window,
            q2gx_brush_textured_faces_window,
            q2gx_brush_fallback_faces_window,
            q2gx_brush_deferred_trans_faces_window,
            q2gx_brush_wal_binds_window,
            q2gx_brush_vertices_window,
            q2gx_brush_registered_inline_count
        );

q2gx_world_frames_window = 0u;
        q2gx_world_pvs_faces_window = 0u;
        q2gx_world_pvs_rejected_faces_window = 0u;

        q2gx_world_texcoord_vertices_window = 0u;

        q2gx_world_wal_textured_faces_window = 0u;
        q2gx_world_wal_animated_flat_faces_window = 0u;
        q2gx_world_wal_animated_textured_faces_window = 0u;
        q2gx_world_wal_animated_texture_binds_window = 0u;
        q2gx_world_wal_special_flat_faces_window = 0u;
        q2gx_world_wal_texture_binds_window = 0u;

        q2gx_world_wal_visible_textures_window = 0u;
        q2gx_world_wal_batch_draw_calls_window = 0u;
        q2gx_world_wal_batch_vertices_window = 0u;




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

        q2gx_brush_entities_window = 0u;
        q2gx_brush_faces_tested_window = 0u;
        q2gx_brush_backface_rejected_window = 0u;
        q2gx_brush_visible_faces_window = 0u;
        q2gx_brush_textured_faces_window = 0u;
        q2gx_brush_fallback_faces_window = 0u;
        q2gx_brush_deferred_trans_faces_window = 0u;
        q2gx_brush_wal_binds_window = 0u;
        q2gx_brush_vertices_window = 0u;

    }

    Q2GX_DrawUnderwaterWarp(fd);

    Q2GX_DrawRefdefPolyBlend(fd);

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
    char *end = NULL;
    long model_index;

    if (!name || !name[0])
        return NULL;

    if (name[0] != '*')
    {
        return
            Q2GX_RegisterAliasModel(
                name
            );
    }

    model_index =
        strtol(
            name + 1,
            &end,
            10
        );

    if (
        end == name + 1
        ||
        !end
        ||
        *end != '\0'
        ||
        model_index <= 0
        ||
        (unsigned long)model_index
        >=
        (unsigned long)q2gx_brush_model_count
        ||
        !q2gx_brush_models
    )
    {
        ri.Con_Printf(
            PRINT_ALL,
            "Q2GC REF_GX BRUSH REGISTER: "
            "invalid inline model %s\n",
            name
        );

        return NULL;
    }

    {
        q2gx_brush_model_t *brush_model =
            &q2gx_brush_models[
                (unsigned int)
                model_index
            ];

        if (
            brush_model->handle.magic
            !=
            Q2GX_MODEL_HANDLE_MAGIC
            ||
            brush_model->handle.kind
            !=
            Q2GX_MODEL_KIND_INLINE_BSP
        )
        {
            return NULL;
        }

        if (!brush_model->registered)
        {
            brush_model->registered = true;
            ++q2gx_brush_registered_inline_count;

            ri.Con_Printf(
                PRINT_ALL,
                "Q2GC REF_GX BRUSH REGISTER: "
                "model=*%u "
                "first_face=%u "
                "face_count=%u "
                "mode=inline_bsp_handle\n",
                brush_model->handle.model_index,
                brush_model->first_face,
                brush_model->face_count
            );
        }

        return
            &brush_model->handle;
    }
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
    unsigned int i;
    unsigned int loaded = 0u;

    if (!name)
        name = "";

    strncpy(
        q2gx_sky_name,
        name,
        sizeof(q2gx_sky_name) - 1u
    );

    q2gx_sky_name[
        sizeof(q2gx_sky_name) - 1u
    ] = '\0';

    q2gx_sky_rotate = (f32)rotate;

    if (axis)
    {
        q2gx_sky_axis[0] = axis[0];
        q2gx_sky_axis[1] = axis[1];
        q2gx_sky_axis[2] = axis[2];
    }
    else
    {
        q2gx_sky_axis[0] = 0.0f;
        q2gx_sky_axis[1] = 0.0f;
        q2gx_sky_axis[2] = 1.0f;
    }

    q2gx_sky_ready = false;
    q2gx_sky_first_draw_logged = false;

    for (i = 0u; i < Q2GX_SKY_FACE_COUNT; ++i)
    {
        char image_path[MAX_QPATH + 16];
        int written;

        q2gx_sky_images[i] = NULL;

        written =
            snprintf(
                image_path,
                sizeof(image_path),
                "/env/%s%s.pcx",
                q2gx_sky_name,
                q2gx_sky_suffix[i]
            );

        if (
            written <= 0
            ||
            (size_t)written >= sizeof(image_path)
        )
        {
            continue;
        }

        q2gx_sky_images[i] =
            Q2GX_FindPic(
                image_path
            );

        if (q2gx_sky_images[i])
            ++loaded;
    }

    q2gx_sky_ready =
        loaded == Q2GX_SKY_FACE_COUNT;

    ri.Con_Printf(
        PRINT_ALL,
        "Q2GC REF_GX SKY SET: "
        "name=%s "
        "rotate=%.4f "
        "axis=%.3f,%.3f,%.3f "
        "loaded=%u/6 "
        "ready=%u "
        "source=env_pcx "
        "mode=stock_clip_pcx_cube_v1\n",
        q2gx_sky_name,
        q2gx_sky_rotate,
        q2gx_sky_axis[0],
        q2gx_sky_axis[1],
        q2gx_sky_axis[2],
        loaded,
        q2gx_sky_ready ? 1u : 0u
    );
}


static void Q2GX_EndRegistration(void)
{
    Q2GX_PrintAliasRegistrationSummary();
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
