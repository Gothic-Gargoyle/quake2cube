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
    (void)name;
    return NULL;
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
    (void)name;

    if (w)
        *w = 0;

    if (h)
        *h = 0;
}


static void Q2GX_DrawPic(
    int x,
    int y,
    char *name)
{
    (void)x;
    (void)y;
    (void)name;
}


static void Q2GX_DrawStretchPic(
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


static void Q2GX_DrawChar(
    int x,
    int y,
    int c)
{
    (void)x;
    (void)y;
    (void)c;
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

    /*
     * Q2GC_DRAWFILL_PALETTE_SELFTEST
     *
     * Milestone test intentionally kept in this commit.
     *
     * Every Quake II palette index 0..255 is drawn exactly once.
     */
    {
        int row;
        int column;

        for (row = 0;
             row < 16;
             ++row)
        {
            for (column = 0;
                 column < 16;
                 ++column)
            {
                Q2GX_DrawFill(
                    64 + column * 32,
                    48 + row * 24,
                    32,
                    24,
                    row * 16 + column
                );
            }
        }
    }
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

        q2gx_frame_count = 0u;
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
