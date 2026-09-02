#include <stdio.h>

#include "ref_soft/r_local.h"
#include "client/keys.h"

#include "quake2.h"

#include <carryhandle/ch_dvd.h>
#include <carryhandle/ch_input.h>
#include <carryhandle/ch_input_physical.h>
#include <carryhandle/ch_time.h>
#include <carryhandle/ch_video.h>


#include <SDL.h>
#include <ogc/system.h>
int _old_mouse_buttonstate = 0;

static int _width = 640;
static int _height = 480;

static SDL_Surface *surface = NULL;



/*****************************************************************************/

static void setupWindow(qboolean fullscreen)
{
  (void)fullscreen;

  if (surface)
  {
    SDL_FreeSurface(surface);
    surface = NULL;
  }

  if (!CH_VideoInit(
          (unsigned int)_width,
          (unsigned int)_height,
          "Quake2Cube"))
  {
    ri.Sys_Error(
        ERR_FATAL,
        "CH_VideoInit failed: %s",
        SDL_GetError());
  }

  surface = SDL_CreateRGBSurface(
      0,
      _width,
      _height,
      8,
      0,
      0,
      0,
      0);

  if (!surface)
  {
    ri.Sys_Error(
        ERR_FATAL,
        "SDL_CreateRGBSurface failed: %s",
        SDL_GetError());
  }

  vid.rowbytes = surface->pitch;
  vid.buffer = surface->pixels;
}


rserr_t SWimp_SetMode( int *pwidth, int *pheight, int mode, qboolean fullscreen )
{
	int width = 0;
  int height = 0;

	ri.Con_Printf( PRINT_ALL, "Initializing OpenGL display\n");

	ri.Con_Printf (PRINT_ALL, "...setting mode %d:", mode );

	if ( !ri.Vid_GetModeInfo( &width, &height, mode ) )
	{
		ri.Con_Printf( PRINT_ALL, " invalid mode\n" );
		return rserr_invalid_mode;
	}

	ri.Con_Printf( PRINT_ALL, " %d %d\n", width, height );


  _width = width;
  _height = height;

  *pwidth = width;
	*pheight = height;

  setupWindow(fullscreen);

	// let the sound and input subsystems know about the new window
	ri.Vid_NewWindow (width, height);

	return rserr_ok;
}

void SWimp_Shutdown( void )
{
  if (surface)
  {
    SDL_FreeSurface(surface);
    surface = NULL;
  }

  CH_VideoShutdown();
  SDL_Quit();
}


int SWimp_Init( void *hInstance, void *wndProc )
{
  (void)hInstance;
  (void)wndProc;

  /*
   * GameCube uses the fixed 640x480 software-renderer mode.
   */
  ri.Cvar_SetValue("sw_mode", 3);

  if (SDL_Init(0) < 0)
  {
    Sys_Error(
        "VID: Couldn't load SDL: %s",
        SDL_GetError());
  }

  setupWindow(false);

  return true;
}

static qboolean SWimp_InitGraphics( qboolean fullscreen )
{
    vid.rowbytes = surface->pitch;
	  vid.buffer = surface->pixels;

    return rserr_ok;
}

void SWimp_SetPalette( const unsigned char *palette )
{
  if (!CH_VideoSetPaletteRGB8(
          palette,
          4u))
  {
    ri.Sys_Error(
        ERR_FATAL,
        "CH_VideoSetPaletteRGB8 failed");
  }
}

void SWimp_BeginFrame( float camera_seperation )
{
}


/*
** GLimp_EndFrame
**
** Responsible for doing a swapbuffers and possibly for other stuff
** as yet to be determined.  Probably better not to make this a GLimp
** function and instead do a call to GLimp_SwapBuffers.
*/
void SWimp_EndFrame (void)
{

  if (!surface ||
      !CH_VideoPresentIndexed8(
          (const uint8_t *)surface->pixels,
          (unsigned int)surface->pitch))
  {
    ri.Sys_Error(
        ERR_FATAL,
        "CH_VideoPresentIndexed8 failed: %s",
        SDL_GetError());
  }
}


void SWimp_AppActivate( qboolean active )
{

}


int ConvertToQuakeKey(unsigned int keysym)
{
  int key;

  key = 0;
  switch(keysym) {
  case SDLK_KP_9:            key = K_KP_PGUP; break;
  case SDLK_PAGEUP:        key = K_PGUP; break;

  case SDLK_KP_3:            key = K_KP_PGDN; break;
  case SDLK_PAGEDOWN:        key = K_PGDN; break;

  case SDLK_KP_7:            key = K_KP_HOME; break;
  case SDLK_HOME:            key = K_HOME; break;

  case SDLK_KP_1:            key = K_KP_END; break;
  case SDLK_END:            key = K_END; break;

  case SDLK_KP_4:            key = K_KP_LEFTARROW; break;
  case SDLK_LEFT:            key = K_LEFTARROW; break;

  case SDLK_KP_6:            key = K_KP_RIGHTARROW; break;
  case SDLK_RIGHT:        key = K_RIGHTARROW; break;

  case SDLK_KP_2:            key = K_KP_DOWNARROW; break;
  case SDLK_DOWN:            key = K_DOWNARROW; break;

  case SDLK_KP_8:            key = K_KP_UPARROW; break;
  case SDLK_UP:            key = K_UPARROW; break;

  case SDLK_ESCAPE:        key = K_ESCAPE; break;

  case SDLK_KP_ENTER:        key = K_KP_ENTER; break;
  case SDLK_RETURN:        key = K_ENTER; break;

  case SDLK_TAB:            key = K_TAB; break;

  case SDLK_F1:            key = K_F1; break;
  case SDLK_F2:            key = K_F2; break;
  case SDLK_F3:            key = K_F3; break;
  case SDLK_F4:            key = K_F4; break;
  case SDLK_F5:            key = K_F5; break;
  case SDLK_F6:            key = K_F6; break;
  case SDLK_F7:            key = K_F7; break;
  case SDLK_F8:            key = K_F8; break;
  case SDLK_F9:            key = K_F9; break;
  case SDLK_F10:            key = K_F10; break;
  case SDLK_F11:            key = K_F11; break;
  case SDLK_F12:            key = K_F12; break;

  case SDLK_BACKSPACE:        key = K_BACKSPACE; break;

  case SDLK_KP_PERIOD:        key = K_KP_DEL; break;
  case SDLK_DELETE:        key = K_DEL; break;

  case SDLK_PAUSE:        key = K_PAUSE; break;

  case SDLK_LSHIFT:
  case SDLK_RSHIFT:        key = K_SHIFT; break;

  case SDLK_LCTRL:
  case SDLK_RCTRL:        key = K_CTRL; break;

  case SDLK_LGUI:
  case SDLK_RGUI:
  case SDLK_LALT:
  case SDLK_RALT:            key = K_ALT; break;

  case SDLK_KP_5:            key = K_KP_5; break;

  case SDLK_INSERT:        key = K_INS; break;
  case SDLK_KP_0:            key = K_KP_INS; break;

  case SDLK_KP_MULTIPLY:        key = '*'; break;
  case SDLK_KP_PLUS:        key = K_KP_PLUS; break;
  case SDLK_KP_MINUS:        key = K_KP_MINUS; break;
  case SDLK_KP_DIVIDE:        key = K_KP_SLASH; break;

  default: /* assuming that the other sdl keys are mapped to ascii */
    if (keysym < 128)
      key = keysym;
    break;
  }

  return key;
}

#define GC_STICK_PRESS_THRESHOLD   24
#define GC_STICK_RELEASE_THRESHOLD 12
#define GC_CSTICK_PRESS_THRESHOLD   CH_INPUT_CSTICK_DEADZONE
#define GC_CSTICK_RELEASE_THRESHOLD 16


/*
 * Q2GC_DISTINCT_DIRECTION_KEYS
 *
 * Separate event state for each physical directional control.
 *
 * NEVER merge:
 *
 *     D-pad
 *     main stick
 *     C-stick
 */
static bool gc_dpad_up;
static bool gc_dpad_down;
static bool gc_dpad_left;
static bool gc_dpad_right;

static bool gc_stick_up;
static bool gc_stick_down;
static bool gc_stick_left;
static bool gc_stick_right;

static bool gc_cstick_up;
static bool gc_cstick_down;
static bool gc_cstick_left;
static bool gc_cstick_right;


/*
 * Q2GC_TRUE_ANALOG_PROVIDER
 *
 * CarryHandle gives us native signed GameCube stick axes.
 * Keep the latest snapshot here so Quake's command builder can consume
 * continuously variable movement instead of fake keyboard presses.
 */

static CH_PadState gc_analog_pad;
static bool gc_analog_pad_valid;
static bool gc_gameplay_capture;





/*
 * Native GameCube axis contract consumed by client/cl_input.c.
 *
 * move_x:
 *   -1 = strafe left
 *   +1 = strafe right
 *
 * move_y:
 *   -1 = backwards
 *   +1 = forwards
 *
 * look_x:
 *   -1 = turn left
 *   +1 = turn right
 *
 * look_y:
 *   -1 = look down
 *   +1 = look up
 */
void QG_GetGamepadAxes(
    float *move_x,
    float *move_y,
    float *look_x,
    float *look_y)
{
    if (move_x)
        *move_x = 0.0f;

    if (move_y)
        *move_y = 0.0f;

    if (look_x)
        *look_x = 0.0f;

    if (look_y)
        *look_y = 0.0f;

    if (!gc_analog_pad_valid)
    {
        return;
    }

    /*
     * CarryHandle owns physical-axis selection,
     * deadzones and rescaling.
     *
     * D-pad is deliberately NOT involved.
     */
    if (move_x)
    {
        *move_x =
            CH_InputAnalogAxisFloat(
                &gc_analog_pad,
                CH_PHYSICAL_STICK_LEFT,
                CH_PHYSICAL_STICK_RIGHT
            );
    }

    if (move_y)
    {
        *move_y =
            CH_InputAnalogAxisFloat(
                &gc_analog_pad,
                CH_PHYSICAL_STICK_DOWN,
                CH_PHYSICAL_STICK_UP
            );
    }

    if (look_x)
    {
        *look_x =
            CH_InputAnalogAxisFloat(
                &gc_analog_pad,
                CH_PHYSICAL_CSTICK_LEFT,
                CH_PHYSICAL_CSTICK_RIGHT
            );
    }

    if (look_y)
    {
        *look_y =
            CH_InputAnalogAxisFloat(
                &gc_analog_pad,
                CH_PHYSICAL_CSTICK_DOWN,
                CH_PHYSICAL_CSTICK_UP
            );
    }
}



static void GC_SendLogicalKey(
    bool held,
    bool *was_held,
    int quake_key)
{
    if (held == *was_held)
    {
        return;
    }

    *was_held = held;

    Quake2_SendKey(
        quake_key,
        held);
}


static void GC_SendPadButton(
    const CH_PadState *pad,
    uint16_t button,
    int quake_key)
{
    if ((pad->buttons_down & button) != 0)
    {
        Quake2_SendKey(
            quake_key,
            true);
    }

    if ((pad->buttons_up & button) != 0)
    {
        Quake2_SendKey(
            quake_key,
            false);
    }
}


void HandleInput(void)
{
    SDL_Event event;
    CH_PadState pad;

    /*
     * Keep SDL's event pump alive for the video backend,
     * but GameCube gameplay/menu input comes from CarryHandle.
     */
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
        {
            ri.Cmd_ExecuteText(
                EXEC_NOW,
                "quit");
        }
    }

    CH_InputPoll();

    gc_analog_pad_valid = false;

    if (!CH_InputGetPad(
            0,
            &pad) ||
        !pad.connected)
    {
        return;
    }

    gc_analog_pad =
        pad;

    gc_analog_pad_valid =
        true;




    /*
     * Q2GC_DISTINCT_DIRECTION_KEYS
     *
     * Preserve each physical direction as its own Quake key.
     *
     * IMPORTANT:
     *
     * There are NO K_UPARROW / K_DOWNARROW / K_LEFTARROW /
     * K_RIGHTARROW events here anymore.
     *
     * Ordinary menu navigation converts D-pad/main-stick keys to
     * arrows later, inside Default_MenuKey().
     *
     * Customize Controls therefore sees the original physical key.
     */

    /* --------------------------------------------------------- */
    /* DIGITAL D-PAD                                             */
    /* --------------------------------------------------------- */

    GC_SendLogicalKey(
        CH_InputPhysicalHeld(
            &pad,
            CH_PHYSICAL_DPAD_UP),
        &gc_dpad_up,
        K_GC_DPAD_UP);

    GC_SendLogicalKey(
        CH_InputPhysicalHeld(
            &pad,
            CH_PHYSICAL_DPAD_DOWN),
        &gc_dpad_down,
        K_GC_DPAD_DOWN);

    GC_SendLogicalKey(
        CH_InputPhysicalHeld(
            &pad,
            CH_PHYSICAL_DPAD_LEFT),
        &gc_dpad_left,
        K_GC_DPAD_LEFT);

    GC_SendLogicalKey(
        CH_InputPhysicalHeld(
            &pad,
            CH_PHYSICAL_DPAD_RIGHT),
        &gc_dpad_right,
        K_GC_DPAD_RIGHT);


    /* --------------------------------------------------------- */
    /* MAIN ANALOGUE STICK — digital identity for menus/binding  */
    /* --------------------------------------------------------- */

    {
        bool held;

        held =
            gc_stick_up
                ? pad.stick_y >
                    GC_STICK_RELEASE_THRESHOLD
                : pad.stick_y >
                    GC_STICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_stick_up,
            K_GC_STICK_UP);


        held =
            gc_stick_down
                ? pad.stick_y <
                    -GC_STICK_RELEASE_THRESHOLD
                : pad.stick_y <
                    -GC_STICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_stick_down,
            K_GC_STICK_DOWN);


        held =
            gc_stick_left
                ? pad.stick_x <
                    -GC_STICK_RELEASE_THRESHOLD
                : pad.stick_x <
                    -GC_STICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_stick_left,
            K_GC_STICK_LEFT);


        held =
            gc_stick_right
                ? pad.stick_x >
                    GC_STICK_RELEASE_THRESHOLD
                : pad.stick_x >
                    GC_STICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_stick_right,
            K_GC_STICK_RIGHT);
    }


    /* --------------------------------------------------------- */
    /* C-STICK — distinct identity, NEVER menu arrows            */
    /* --------------------------------------------------------- */

    {
        bool held;

        held =
            gc_cstick_up
                ? pad.cstick_y >
                    GC_CSTICK_RELEASE_THRESHOLD
                : pad.cstick_y >
                    GC_CSTICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_cstick_up,
            K_GC_CSTICK_UP);


        held =
            gc_cstick_down
                ? pad.cstick_y <
                    -GC_CSTICK_RELEASE_THRESHOLD
                : pad.cstick_y <
                    -GC_CSTICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_cstick_down,
            K_GC_CSTICK_DOWN);


        held =
            gc_cstick_left
                ? pad.cstick_x <
                    -GC_CSTICK_RELEASE_THRESHOLD
                : pad.cstick_x <
                    -GC_CSTICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_cstick_left,
            K_GC_CSTICK_LEFT);


        held =
            gc_cstick_right
                ? pad.cstick_x >
                    GC_CSTICK_RELEASE_THRESHOLD
                : pad.cstick_x >
                    GC_CSTICK_PRESS_THRESHOLD;

        GC_SendLogicalKey(
            held,
            &gc_cstick_right,
            K_GC_CSTICK_RIGHT);
    }


    /*
     * Expose GameCube buttons as Quake joystick keys.
     * Gameplay actions remain entirely user-bindable through
     * Quake II's existing Customize Controls menu.
     */
    GC_SendPadButton(&pad, CH_PAD_BUTTON_A, K_JOY1);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_B, K_JOY2);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_X, K_JOY3);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_Y, K_JOY4);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_L, K_AUX1);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_R, K_AUX2);
    GC_SendPadButton(&pad, CH_PAD_BUTTON_Z, K_AUX3);

    /*
     * Start remains the system/menu button rather than a
     * configurable gameplay action.
     */
    GC_SendPadButton(&pad, CH_PAD_BUTTON_START, K_ESCAPE);
}


int QG_Milliseconds(void)
{
    static uint64_t origin;
    static bool initialized;
    uint64_t now = CH_TimeMilliseconds();

    if (!initialized)
    {
        origin = now;
        initialized = true;
    }

    return (int)(now - origin);
}


void QG_GetMouseDiff(
    int *dx,
    int *dy)
{
    *dx = 0;
    *dy = 0;
}


void QG_CaptureMouse(void)
{
    /*
     * QuakeGeneric calls this when gameplay owns pointer-style input.
     *
     * On GameCube we don't have a mouse to capture, but this is the
     * perfect platform boundary for switching the main stick from
     * digital menu arrows to true analogue gameplay movement.
     */
    gc_gameplay_capture = true;
}


void QG_ReleaseMouse(void)
{
    /*
     * QuakeGeneric releases the mouse when menus own input.
     *
     * Re-enable main-stick -> menu-arrow synthesis.
     */
    gc_gameplay_capture = false;
}


int main(int argc, char **argv)
{
    SYS_STDIO_Report(true);
    int time;
    int oldtime;
    int newtime;

    /*
     * GameCube has no useful process command line.
     *
     * Quake's own filesystem remains untouched: setting basedir
     * to dvd: makes its existing baseq2 lookup become:
     *
     *     dvd:/baseq2
     */
    char *gamecube_argv[] =
    {
        "quake2",
        "+set",
        "basedir",
        "dvd:",
        NULL
    };

    (void)argc;
    (void)argv;

    if (!CH_DVDMount())
    {
        fprintf(
            stderr,
            "Quake2Generic: failed to mount dvd:/\n");

        return 1;
    }

    if (!CH_InputInit())
    {
        fprintf(
            stderr,
            "Quake2Generic: failed to initialize controller input\n");

        return 1;
    }

    Quake2_Init(
        4,
        gamecube_argv);

    oldtime =
        Quake2_Milliseconds();

    while (1)
    {
        HandleInput();

        do
        {
            newtime =
                Quake2_Milliseconds();

            time =
                newtime -
                oldtime;
        }
        while (time < 1);

        Quake2_Frame(
            time);

        oldtime =
            newtime;
    }

    return 0;
}
