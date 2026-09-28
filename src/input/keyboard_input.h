/**
 * Keyboard as controller port 0, for hosts without a pad or a window.
 *
 * Two sources, merged into whatever the real pad reports:
 *
 *   - The terminal. When stdin is a TTY and RECOMP_KEYBOARD is not "0", the
 *     terminal is switched to non-canonical, no-echo input and each key held
 *     presses its button. A terminal only reports key *presses* (and their
 *     auto-repeat), never releases, so a button stays down for a short hold
 *     after the last byte for that key; holding the key keeps it down once
 *     auto-repeat starts.
 *
 *   - A script, for runs nobody is typing into. RECOMP_INPUT_SCRIPT is a comma
 *     list of `ms:button[+button...][:hold_ms]`, with ms counted from
 *     xbox_KeyboardInit, e.g. "40000:start,45000:a:300,47000:down".
 *
 * Terminal keys:
 *   arrows        D-pad              Enter        Start
 *   Tab / Bksp   Back               Space        A
 *   x            B                  q            X
 *   e            Y                  v / c        left / right trigger
 *   1 / 2        White / Black      w a s d       left stick
 *   i j k l      right stick        f / h        L3 / R3
 *
 * Button names for the script: up down left right start back a b x y white
 * black lt rt lthumb rthumb, and lsup lsdown lsleft lsright for the stick.
 */
#ifndef XBOX_KEYBOARD_INPUT_H
#define XBOX_KEYBOARD_INPUT_H

#include "xinput_xbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Slot indices a key can press: the D-pad / face buttons / sticks. The numeric
 * values drive the per-slot hold timers in keyboard_input.c and are exposed so
 * a host window key-event handler can name a slot. Keep in sync with that file.
 */
enum {
    K_UP, K_DOWN, K_LEFT, K_RIGHT, K_START, K_BACK, K_LTHUMB, K_RTHUMB,
    K_A, K_B, K_X, K_Y, K_BLACK, K_WHITE, K_LT, K_RT,
    K_LS_UP, K_LS_DOWN, K_LS_LEFT, K_LS_RIGHT,
    K_RS_UP, K_RS_DOWN, K_RS_LEFT, K_RS_RIGHT,
    K_COUNT
};

/* Mouse button indices for mouse-as-controller support. */
enum {
    K_MOUSE_LEFT = 0,
    K_MOUSE_RIGHT,
    K_MOUSE_WHEEL_UP,
    K_MOUSE_WHEEL_DOWN,
    K_MOUSE_BUTTON_COUNT
};

/* Start the terminal reader and load the script. Safe to call more than once. */
void xbox_KeyboardInit(void);

/* OR the keyboard's current state into *pad. Returns 1 while a keyboard source
 * is active (so port 0 counts as connected even with no pad), 0 otherwise. */
int xbox_KeyboardMergeState(XBOX_GAMEPAD *pad);

/* Put the terminal back the way it was. Async-signal-safe, for crash handlers;
 * also registered with atexit and on SIGINT/SIGTERM. */
void xbox_KeyboardRestoreTerminal(void);

/* Translate a printable key character -- the byte read from the terminal, or
 * the glyph of an NSEvent -- to a slot, or -1 if the key is not bound. Reuses
 * the terminal mapping so the window and the terminal agree on bindings. */
int xbox_KeyboardSlotForKey(unsigned char c);

/* Translate a host virtual key code to a slot (macOS only; used by the Cocoa
 * key view for arrows, which carry no glyph). Returns -1 if unbound. */
#if defined(__APPLE__)
int xbox_KeyboardSlotForKeyCode(unsigned short keycode);
#endif

/* Press/release a slot from a window-system event handler (thread-safe).
 * A press holds the button until the next release so a key held in the window
 * stays down exactly as the user holds it; this marks the keyboard source
 * active even when stdin is not a TTY (e.g. launched from a GUI). */
void xbox_KeyboardPressSlot(int slot);
void xbox_KeyboardReleaseSlot(int slot);
void xbox_KeyboardReleaseAll(void);

/* Mouse as controller input (CoD-style). Called from the window event handler. */
void xbox_MouseSetDelta(int dx, int dy);
void xbox_MousePressButton(int button);
void xbox_MouseReleaseButton(int button);
void xbox_MouseReleaseAll(void);

/* RECOMP_AUTOSTART support: nonzero while waiting for the title screen, and
 * the call the presenter makes when it sees it (presses Start once and starts
 * RECOMP_INPUT_SCRIPT's clock). */
int  xbox_KeyboardAutostartPending(void);
void xbox_KeyboardTitleSeen(void);

#ifdef __cplusplus
}
#endif

#endif
