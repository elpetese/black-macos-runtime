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

void xbox_InputInit(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (SDL_IsGameController(0)) {
        const char *name = SDL_JoystickNameForIndex(0);
        if (name && strstr(name, "DualSense")) {
            const char *mapping =
                "050000004c050000cc09000000000000,"
                "PlayStation Controller,"
                "a:b1,b:b2,back:b8,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,"
                "guide:b10,leftshoulder:b4,leftstick:b11,lefttrigger:a2,"
                "rightshoulder:b5,rightstick:b12,righttrigger:a5,start:b9,x:b0,y:b3,"
                "platform:Mac OS X,";
            SDL_GameControllerAddMapping(mapping);
            fprintf(stderr, "  [INPUT] applied DualSense mapping\n");
            fflush(stderr);
        }
    }
    open_controllers();
    xbox_KeyboardInit();
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

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

    g_f7_input_poll_count++;

    SDL_Joystick *joy = SDL_GameControllerGetJoystick(c);
    WORD btn = 0;
    if (joy) {
        if (SDL_JoystickGetButton(joy, 4))  btn |= XBOX_GAMEPAD_DPAD_UP;
        if (SDL_JoystickGetButton(joy, 6))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
        if (SDL_JoystickGetButton(joy, 7))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
        if (SDL_JoystickGetButton(joy, 5))  btn |= XBOX_GAMEPAD_DPAD_RIGHT;
        if (SDL_JoystickGetButton(joy, 9))  btn |= XBOX_GAMEPAD_START;
        if (SDL_JoystickGetButton(joy, 8))  btn |= XBOX_GAMEPAD_BACK;
        if (SDL_JoystickGetButton(joy, 10)) btn |= XBOX_GAMEPAD_LEFT_THUMB;
        if (SDL_JoystickGetButton(joy, 11)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    }
    pState->Gamepad.wButtons = btn;

    if (btn) g_f7_input_nonzero_buttons++;

    if (joy) {
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
            SDL_JoystickGetButton(joy, 1) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
            SDL_JoystickGetButton(joy, 2) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
            SDL_JoystickGetButton(joy, 0) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
            SDL_JoystickGetButton(joy, 3) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
            SDL_JoystickGetButton(joy, 6) ? 255 : 0;
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
            SDL_JoystickGetButton(joy, 7) ? 255 : 0;

        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
            (BYTE)(SDL_JoystickGetAxis(joy, 4) >> 7);
        pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
            (BYTE)(SDL_JoystickGetAxis(joy, 5) >> 7);

        pState->Gamepad.sThumbLX = SDL_JoystickGetAxis(joy, 0);
        pState->Gamepad.sThumbLY =
            (SHORT)(-1 - SDL_JoystickGetAxis(joy, 1));
        pState->Gamepad.sThumbRX = SDL_JoystickGetAxis(joy, 3);
        pState->Gamepad.sThumbRY =
            (SHORT)(-1 - SDL_JoystickGetAxis(joy, 4));
    }

    if (getenv("RECOMP_INPUT_DEBUG")) {
        static uint32_t shown;
        if (shown++ < 50) {
            uint64_t now = 0;
            if (joy) {
                int sdl_a = SDL_JoystickGetButton(joy, 1);
                int sdl_b = SDL_JoystickGetButton(joy, 2);
                int sdl_x = SDL_JoystickGetButton(joy, 0);
                int sdl_y = SDL_JoystickGetButton(joy, 3);
                int sdl_back = SDL_JoystickGetButton(joy, 8);
                int sdl_guide = SDL_JoystickGetButton(joy, 10);
                int sdl_start = SDL_JoystickGetButton(joy, 9);
                int sdl_lstick = SDL_JoystickGetButton(joy, 11);
                int sdl_rstick = SDL_JoystickGetButton(joy, 12);
                int sdl_lb = SDL_JoystickGetButton(joy, 4);
                int sdl_rb = SDL_JoystickGetButton(joy, 5);
                int sdl_dpad_up = SDL_JoystickGetButton(joy, 13);
                int sdl_dpad_down = SDL_JoystickGetButton(joy, 14);
                int sdl_dpad_left = SDL_JoystickGetButton(joy, 15);
                int sdl_dpad_right = SDL_JoystickGetButton(joy, 16);
                int lt = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER];
                int rt = pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER];
                fprintf(stderr, "  [INPUT] raw btn=0x%04X A=%d B=%d X=%d Y=%d LB=%d RB=%d LT=%d RT=%d "
                        "up=%d down=%d left=%d right=%d back=%d guide=%d start=%d ls=%d rs=%d "
                        "LX=%d LY=%d RX=%d RY=%d\n",
                        btn,
                        sdl_a, sdl_b, sdl_x, sdl_y,
                        sdl_lb, sdl_rb, lt, rt,
                        sdl_dpad_up, sdl_dpad_down, sdl_dpad_left, sdl_dpad_right,
                        sdl_back, sdl_guide, sdl_start,
                        sdl_lstick, sdl_rstick,
                        pState->Gamepad.sThumbLX, pState->Gamepad.sThumbLY,
                        pState->Gamepad.sThumbRX, pState->Gamepad.sThumbRY);
            } else {
                fprintf(stderr, "  [INPUT] raw btn=0x%04X no joystick\n", btn);
            }
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
