/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include "../kernel/f7_instrumentation.h"
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include "keyboard_input.h"

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];

/* Open up to XBOX_MAX_CONTROLLERS attached game controllers. */
static void open_controllers(void)
{
    int slot = 0;
    int num_joysticks = SDL_NumJoysticks();
    /* RECOMP_BACKGROUND: a scripted run must not see a pad someone is holding. */
    if (getenv("RECOMP_BACKGROUND")) {
        fprintf(stderr, "  [INPUT] RECOMP_BACKGROUND: host controllers ignored\n");
        return;
    }
    fprintf(stderr, "  [INPUT] SDL sees %d joystick(s)\n", num_joysticks);
    fflush(stderr);
    for (int i = 0; i < num_joysticks && slot < XBOX_MAX_CONTROLLERS; i++) {
        int is_gc = SDL_IsGameController(i);
        const char *name = SDL_JoystickNameForIndex(i);
        fprintf(stderr, "  [INPUT] joystick %d: name=\"%s\" is_gamecontroller=%d\n",
                i, name ? name : "(null)", is_gc);
        fflush(stderr);
        if (!is_gc)
            continue;
        if (!g_pads[slot]) {
            g_pads[slot] = SDL_GameControllerOpen(i);
            g_controller_connected[slot] = (g_pads[slot] != NULL);
        }
        slot++;
    }
    fprintf(stderr, "  [INPUT] opened %d controller(s)\n", slot);
    fflush(stderr);
}

/* Controllers are found on the thread that polls them.
 *
 * SDL2 on macOS schedules its HID manager on the run loop of the thread that
 * initialised the joystick subsystem, and services it from whichever thread
 * calls the update. Initialising here on the game's main thread while the
 * USB controller model polled from its own thread meant that run loop never
 * ran: no device was ever enumerated, and a PS4 pad plugged in before launch
 * or after read as "SDL sees 0 joystick(s)". It also only looked once. So:
 * initialise on the first poll, update on every poll, and rescan whenever the
 * count changes -- which is also what makes hot-plugging work. */
static void input_rescan(void);

static void input_poll_host(void)
{
    static int ready;
    static int last_count = -1;
    static uint32_t last_ms;
    uint32_t now;
    int count;

    if (getenv("RECOMP_BACKGROUND"))
        return;
    if (!ready) {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
            fprintf(stderr, "  [INPUT] SDL game controller init failed: %s\n", SDL_GetError());
            fflush(stderr);
        }
        ready = 1;
    }
    SDL_GameControllerUpdate();
    /* Nothing reads SDL's event queue here; keep it from filling. */
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    now = SDL_GetTicks();
    if (last_count >= 0 && now - last_ms < 500)
        return;
    last_ms = now;
    count = SDL_NumJoysticks();
    if (count != last_count) {
        last_count = count;
        input_rescan();
    }
}

static void input_rescan(void)
{
    for (int i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        if (g_pads[i] && !SDL_GameControllerGetAttached(g_pads[i])) {
            SDL_GameControllerClose(g_pads[i]);
            g_pads[i] = NULL;
            g_controller_connected[i] = FALSE;
        }
    }
    /* No hand-written mappings: SDL's own database covers the DualShock 4
     * (macOS names it "Wireless Controller") and the DualSense, and the
     * mapping this used to add for them left the sticks unassigned and put
     * the triggers on stick axes. */
    open_controllers();
}

void xbox_InputInit(void)
{
    /* Host controllers are set up on the polling thread (input_poll_host). */
    xbox_KeyboardInit();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (dwPort == 0)
        input_poll_host();

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        /* No pad: the keyboard, when it is on, is port 0 by itself. */
        if (dwPort == 0) {
            memset(pState, 0, sizeof(XBOX_INPUT_STATE));
            if (xbox_KeyboardMergeState(&pState->Gamepad)) {
                pState->dwPacketNumber = ++g_packet[dwPort];
                return ERROR_SUCCESS;
            }
        }
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    /* Port 0 is counted by xbox_KeyboardMergeState below, which every port-0
     * poll goes through; counting here too doubled the poll total. */
    if (dwPort != 0)
        g_f7_input_poll_count++;

    /* Read the pad through SDL's controller mapping, not raw joystick
     * indices: raw button numbers differ per model, and on a PS4 pad the old
     * raw layout reported a digital button held on every poll, which kept
     * Black's title screen from seeing a fresh Start press. */
    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    static const struct { int sdl; int xbox; } analog[] = {
        { SDL_CONTROLLER_BUTTON_A,             XBOX_BUTTON_A },
        { SDL_CONTROLLER_BUTTON_B,             XBOX_BUTTON_B },
        { SDL_CONTROLLER_BUTTON_X,             XBOX_BUTTON_X },
        { SDL_CONTROLLER_BUTTON_Y,             XBOX_BUTTON_Y },
        /* The Xbox's Black and White buttons sit where the shoulders are. */
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XBOX_BUTTON_BLACK },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  XBOX_BUTTON_WHITE },
    };
    int any_analog = 0;
    for (size_t i = 0; i < sizeof analog / sizeof analog[0]; i++) {
        BYTE v = SDL_GameControllerGetButton(c, analog[i].sdl) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[analog[i].xbox] = v;
        any_analog |= v;
    }
    /* SDL triggers run 0..32767. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL's Y axes grow downward, the Xbox's upward; -1 - v keeps the full
     * range without overflowing at -32768. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    if (btn || any_analog) g_f7_input_nonzero_buttons++;

    if (getenv("RECOMP_INPUT_DEBUG")) {
        static uint32_t shown;
        if (shown++ < 50) {
            const XBOX_GAMEPAD *g = &pState->Gamepad;
            fprintf(stderr, "  [INPUT] pad btn=0x%04X A=%u B=%u X=%u Y=%u Blk=%u Wht=%u LT=%u RT=%u "
                    "LX=%d LY=%d RX=%d RY=%d\n",
                    btn, g->bAnalogButtons[XBOX_BUTTON_A], g->bAnalogButtons[XBOX_BUTTON_B],
                    g->bAnalogButtons[XBOX_BUTTON_X], g->bAnalogButtons[XBOX_BUTTON_Y],
                    g->bAnalogButtons[XBOX_BUTTON_BLACK], g->bAnalogButtons[XBOX_BUTTON_WHITE],
                    g->bAnalogButtons[XBOX_BUTTON_LTRIGGER], g->bAnalogButtons[XBOX_BUTTON_RTRIGGER],
                    g->sThumbLX, g->sThumbLY, g->sThumbRX, g->sThumbRY);
            fflush(stderr);
        }
    }

    /* A pad and the keyboard together: either one presses the button. */
    if (dwPort == 0)
        xbox_KeyboardMergeState(&pState->Gamepad);

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
