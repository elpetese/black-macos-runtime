/**
 * keyboard_input.c -- the keyboard as controller port 0. See keyboard_input.h.
 */
#include "keyboard_input.h"
#include "../kernel/f7_instrumentation.h"

#if defined(_WIN32)

void xbox_KeyboardInit(void) {}
int  xbox_KeyboardMergeState(XBOX_GAMEPAD *pad) { (void)pad; return 0; }
void xbox_KeyboardRestoreTerminal(void) {}
int  xbox_KeyboardSlotForKey(unsigned char c) { (void)c; return -1; }
void xbox_KeyboardPressSlot(int slot) { (void)slot; }
void xbox_KeyboardReleaseSlot(int slot) { (void)slot; }
int  xbox_KeyboardAutostartPending(void) { return 0; }
void xbox_KeyboardTitleSeen(void) {}

#else

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* Slot enumeration lives in keyboard_input.h: it is shared with the Cocoa key
 * view in fb_present_posix.m so both the terminal reader and the window drive
 * the same per-slot hold timers below. */

static const struct { const char *name; int slot; } k_names[] = {
    {"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT}, {"right", K_RIGHT},
    {"start", K_START}, {"back", K_BACK},
    {"lthumb", K_LTHUMB}, {"rthumb", K_RTHUMB},
    {"a", K_A}, {"b", K_B}, {"x", K_X}, {"y", K_Y},
    {"black", K_BLACK}, {"white", K_WHITE}, {"lt", K_LT}, {"rt", K_RT},
    {"lsup", K_LS_UP}, {"lsdown", K_LS_DOWN},
    {"lsleft", K_LS_LEFT}, {"lsright", K_LS_RIGHT},
    {"rsup", K_RS_UP}, {"rsdown", K_RS_DOWN},
    {"rsleft", K_RS_LEFT}, {"rsright", K_RS_RIGHT},
};

/* A press from the terminal lasts this long unless auto-repeat renews it. Long
 * enough that a tap survives a 60 Hz poll several times over, short enough that
 * releasing a held key does not feel stuck. */
#define TAP_MS 150u

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t s_until[K_COUNT];      /* monotonic ms the slot is down until */
static int      s_active;
static uint64_t s_epoch;

/* Mouse state for CoD-style look/aim. */
static int s_mouse_dx, s_mouse_dy;
static int s_mouse_buttons;
static int s_mouse_sens;

#define MAX_SCRIPT 64
static struct { uint64_t at, until; int slot; } s_script[MAX_SCRIPT];
static int s_script_len;

/* RECOMP_AUTOSTART: hold the script until the title screen is on screen, then
 * press Start and run the script with its times counted from that press. The
 * title appears anywhere from ~35 to ~60 s into a run, so presses timed from
 * boot landed during loading or a video as often as on the title. */
static int s_autostart;
static int s_autostart_fired;
#define AUTOSTART_SCRIPT_DELAY_MS 1500u   /* script time 0 = this long after Start */

static struct termios s_saved;
static volatile sig_atomic_t s_tty_raw;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void press(int slot, unsigned ms)
{
    uint64_t until = now_ms() + ms;
    pthread_mutex_lock(&s_lock);
    if (s_until[slot] < until)
        s_until[slot] = until;
    pthread_mutex_unlock(&s_lock);
}

void xbox_KeyboardRestoreTerminal(void)
{
    if (s_tty_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &s_saved);
        s_tty_raw = 0;
    }
}

extern void xbox_FramebufferReleaseMouse(void) __attribute__((weak));

static void on_terminate(int sig)
{
    xbox_KeyboardRestoreTerminal();
    if (xbox_FramebufferReleaseMouse)
        xbox_FramebufferReleaseMouse();   /* never leave the cursor frozen */
    signal(sig, SIG_DFL);
    raise(sig);
}

/* ESC [ A..D are the arrows. A bare ESC is ignored rather than guessed at. */
static int escape_slot(void)
{
    unsigned char seq[2];
    if (read(STDIN_FILENO, &seq[0], 1) != 1 || seq[0] != '[')
        return -1;
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
        return -1;
    switch (seq[1]) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    default:  return -1;
    }
}

static int key_slot(unsigned char c)
{
    switch (c) {
    case '\r': case '\n':      return K_START;
    case '\t': case 0x7F: case 0x08: return K_BACK;
    /* The layout the Critical Hour port used, so both games play the same:
     *   Space / E  A          C  B          R  X (reload)     Q  Y (weapon)
     *   F  Black   G  White   Z  right-stick click
     *   1 2 3 4    d-pad up / right / down / left
     * plus V = left trigger and X = B kept as extras (no clash), WASD left
     * stick, I/J/K/L right stick, H left-stick click, Shift = left-stick click
     * (run; handled by the window), arrows d-pad, Enter Start, Tab Back. */
    case ' ':                  return K_A;
    case 'e': case 'E':        return K_A;
    case 'c': case 'C':        return K_B;
    case 'x': case 'X':        return K_B;
    case 'r': case 'R':        return K_X;
    case 'q': case 'Q':        return K_Y;
    case 'f': case 'F':        return K_BLACK;
    case 'g': case 'G':        return K_WHITE;
    case 'v': case 'V':        return K_LT;
    case 'z': case 'Z':        return K_RTHUMB;
    case '1':                  return K_UP;
    case '2':                  return K_RIGHT;
    case '3':                  return K_DOWN;
    case '4':                  return K_LEFT;
    case 'w': case 'W':        return K_LS_UP;
    case 'a': case 'A':        return K_LS_LEFT;
    case 's': case 'S':        return K_LS_DOWN;
    case 'd': case 'D':        return K_LS_RIGHT;
    case 'h': case 'H':        return K_LTHUMB;
    case 'i': case 'I':        return K_RS_UP;
    case 'j': case 'J':        return K_RS_LEFT;
    case 'k': case 'K':        return K_RS_DOWN;
    case 'l': case 'L':        return K_RS_RIGHT;
    case '\x1B':               return K_BACK;
    default:                   return -1;
    }
}

static void *tty_thread(void *unused)
{
    unsigned char c;
    (void)unused;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        int slot = (c == 0x1B) ? escape_slot() : key_slot(c);
        if (slot >= 0)
            press(slot, TAP_MS);
    }
    return NULL;
}

static void start_tty(void)
{
    const char *opt = getenv("RECOMP_KEYBOARD");
    struct termios raw;
    pthread_t th;

    if ((opt && strcmp(opt, "0") == 0) || !isatty(STDIN_FILENO))
        return;
    if (tcgetattr(STDIN_FILENO, &s_saved) != 0)
        return;

    /* Non-canonical and no echo, but ISIG stays on: Ctrl+C still stops the
     * process, and the handler below puts the terminal back first. */
    raw = s_saved;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_iflag &= ~(tcflag_t)(ICRNL | IXON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return;
    s_tty_raw = 1;
    atexit(xbox_KeyboardRestoreTerminal);
    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);
    signal(SIGHUP, on_terminate);

    if (pthread_create(&th, NULL, tty_thread, NULL) != 0) {
        xbox_KeyboardRestoreTerminal();
        return;
    }
    pthread_detach(th);
    s_active = 1;
    fprintf(stderr, "  Keyboard: WASD move, mouse or IJKL look, click = fire, "
                    "right click = aim, wheel = weapon, Space/E A, C B, "
                    "R reload, Q weapon, F Black, G White, Shift run, "
                    "1-4 d-pad, Enter Start, Esc = release mouse + Start\n");
}

static int slot_named(const char *name, size_t len)
{
    size_t i;
    for (i = 0; i < sizeof k_names / sizeof k_names[0]; i++)
        if (strlen(k_names[i].name) == len
         && strncmp(k_names[i].name, name, len) == 0)
            return k_names[i].slot;
    return -1;
}

/* "ms:button[+button...][:hold_ms]" entries, comma separated. */
static void load_script(void)
{
    const char *p = getenv("RECOMP_INPUT_SCRIPT");

    while (p && *p) {
        char *end;
        unsigned long at = strtoul(p, &end, 10);
        unsigned long hold = TAP_MS;
        const char *names, *names_end, *q;

        if (end == p || *end != ':')
            break;
        names = end + 1;
        names_end = names + strcspn(names, ":,");
        if (*names_end == ':')
            hold = strtoul(names_end + 1, NULL, 10);

        for (q = names; q < names_end; ) {
            size_t len = strcspn(q, "+:,");
            int slot = slot_named(q, len);
            if (slot < 0) {
                fprintf(stderr, "  Keyboard: unknown button \"%.*s\" in "
                                "RECOMP_INPUT_SCRIPT\n", (int)len, q);
            } else if (s_script_len < MAX_SCRIPT) {
                /* Relative until xbox_KeyboardTitleSeen() rebases them. */
                uint64_t base = s_autostart ? 0 : s_epoch;
                s_script[s_script_len].at    = base + at;
                s_script[s_script_len].until = base + at + hold;
                s_script[s_script_len].slot  = slot;
                s_script_len++;
            }
            q += len;
            if (*q == '+')
                q++;
        }
        p = strchr(names_end, ',');
        if (p)
            p++;
    }
    if (s_script_len) {
        s_active = 1;
        fprintf(stderr, "  Keyboard: %d scripted press(es) loaded\n",
                s_script_len);
    }
}

void xbox_KeyboardInit(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    s_epoch = now_ms();
    s_autostart = getenv("RECOMP_AUTOSTART") != NULL;
    {
        /* Stick counts (of 32767) per pixel of mouse motion. The old default,
         * 4, made a 10-pixel flick 0.1% of the stick -- no visible turn.
         * RECOMP_MOUSE_SENS gives the value directly; the older
         * RECOMP_MOUSE_SENSITIVITY (1..20) still works, scaled so that its
         * old default 4 is the new default. 1800 is what Critical Hour used. */
        const char *direct = getenv("RECOMP_MOUSE_SENS");
        const char *legacy = getenv("RECOMP_MOUSE_SENSITIVITY");
        s_mouse_sens = direct ? atoi(direct)
                     : legacy ? atoi(legacy) * 450 : 1800;
        if (s_mouse_sens < 50) s_mouse_sens = 50;
        if (s_mouse_sens > 12000) s_mouse_sens = 12000;
    }
    load_script();
    if (s_autostart) {
        s_active = 1;
        fprintf(stderr, "  Keyboard: RECOMP_AUTOSTART -- Start on the title screen, "
                        "script counted from there\n");
    }
    start_tty();
}

/* The title can be visible before it accepts input (it fades in, and its UI
 * script arms Start a little later), so a press that is ignored is repeated
 * while the title is still there -- a few times, then the script takes over. */
#define AUTOSTART_RETRY_MS 5000u
#define AUTOSTART_MAX_PRESSES 5
static int      s_autostart_presses;
static uint64_t s_autostart_last;

int xbox_KeyboardAutostartPending(void)
{
    return s_autostart && s_autostart_presses < AUTOSTART_MAX_PRESSES
        && (!s_autostart_presses || now_ms() - s_autostart_last >= AUTOSTART_RETRY_MS);
}

void xbox_KeyboardTitleSeen(void)
{
    uint64_t t;
    int i;

    if (!xbox_KeyboardAutostartPending())
        return;
    t = now_ms();
    pthread_mutex_lock(&s_lock);
    s_until[K_START] = t + 200u;
    s_autostart_last = t;
    if (!s_autostart_fired) {
        s_autostart_fired = 1;
        for (i = 0; i < s_script_len; i++) {
            s_script[i].at    += t + AUTOSTART_SCRIPT_DELAY_MS;
            s_script[i].until += t + AUTOSTART_SCRIPT_DELAY_MS;
        }
    }
    s_autostart_presses++;
    pthread_mutex_unlock(&s_lock);
    fprintf(stderr, "  Keyboard: title screen detected at +%llu ms -- pressing Start\n",
            (unsigned long long)(t - s_epoch));
    fflush(stderr);
}

int xbox_KeyboardMergeState(XBOX_GAMEPAD *pad)
{
    static const WORD digital[] = {
        [K_UP] = XBOX_GAMEPAD_DPAD_UP, [K_DOWN] = XBOX_GAMEPAD_DPAD_DOWN,
        [K_LEFT] = XBOX_GAMEPAD_DPAD_LEFT, [K_RIGHT] = XBOX_GAMEPAD_DPAD_RIGHT,
        [K_START] = XBOX_GAMEPAD_START, [K_BACK] = XBOX_GAMEPAD_BACK,
        [K_LTHUMB] = XBOX_GAMEPAD_LEFT_THUMB,
        [K_RTHUMB] = XBOX_GAMEPAD_RIGHT_THUMB,
    };
    static const int analog[] = {
        [K_A] = XBOX_BUTTON_A, [K_B] = XBOX_BUTTON_B,
        [K_X] = XBOX_BUTTON_X, [K_Y] = XBOX_BUTTON_Y,
        [K_BLACK] = XBOX_BUTTON_BLACK, [K_WHITE] = XBOX_BUTTON_WHITE,
        [K_LT] = XBOX_BUTTON_LTRIGGER, [K_RT] = XBOX_BUTTON_RTRIGGER,
    };
    uint64_t t;
    int down[K_COUNT] = {0};
    int i;

    if (!s_active || !pad)
        return 0;

    t = now_ms();
    /* RECOMP_INPUT_FILE=<path>: live input for driving a run from outside.
     * The file holds lines "button[+button...] hold_ms" (same names as
     * RECOMP_INPUT_SCRIPT); each poll that finds it presses those buttons
     * from now and deletes it. */
    {
        static const char *path = (const char *)1;
        if (path == (const char *)1)
            path = getenv("RECOMP_INPUT_FILE");
        if (path) {
            FILE *f = fopen(path, "r");
            if (f) {
                char line[256];
                while (fgets(line, sizeof line, f)) {
                    char *sp = strrchr(line, ' ');
                    unsigned long hold = TAP_MS;
                    char *q = line;
                    if (sp) { hold = strtoul(sp + 1, NULL, 10); *sp = 0; }
                    line[strcspn(line, "\r\n")] = 0;
                    while (*q) {
                        size_t len = strcspn(q, "+");
                        int slot = slot_named(q, len);
                        if (slot >= 0) {
                            pthread_mutex_lock(&s_lock);
                            s_until[slot] = t + hold;
                            pthread_mutex_unlock(&s_lock);
                        } else if (len) {
                            fprintf(stderr, "  Keyboard: unknown button \"%.*s\" in RECOMP_INPUT_FILE\n", (int)len, q);
                        }
                        q += len;
                        if (*q == '+') q++;
                    }
                }
                fclose(f);
                unlink(path);
            }
        }
    }
    pthread_mutex_lock(&s_lock);
    for (i = 0; i < K_COUNT; i++)
        down[i] = s_until[i] > t;
    pthread_mutex_unlock(&s_lock);

    for (i = 0; i < s_script_len && (!s_autostart || s_autostart_fired); i++) {
        if (s_script[i].at <= t && t < s_script[i].until) {
            if (!down[s_script[i].slot]) {
                static unsigned announced[MAX_SCRIPT];
                if (!announced[i]++) {
                    fprintf(stderr, "  Keyboard: script press %s at +%llu ms\n",
                            k_names[s_script[i].slot].name,
                            (unsigned long long)(t - s_epoch));
                    fflush(stderr);
                }
            }
            down[s_script[i].slot] = 1;
        }
    }

    for (i = K_UP; i <= K_RTHUMB; i++)
        if (down[i])
            pad->wButtons |= digital[i];
    for (i = K_A; i <= K_RT; i++)
        if (down[i])
            pad->bAnalogButtons[analog[i]] = 255;

    g_f7_input_poll_count++;

    /* Mouse buttons: left = fire (RT), right = aim (LT). */
    if (s_mouse_buttons & (1 << K_MOUSE_LEFT))
        pad->bAnalogButtons[XBOX_BUTTON_RTRIGGER] = 255;
    if (s_mouse_buttons & (1 << K_MOUSE_RIGHT))
        pad->bAnalogButtons[XBOX_BUTTON_LTRIGGER] = 255;
    /* Wheel = Y (change weapon), a brief press per notch, as in Critical Hour. */
    if (s_mouse_buttons & ((1 << K_MOUSE_WHEEL_UP) | (1 << K_MOUSE_WHEEL_DOWN)))
        pad->bAnalogButtons[XBOX_BUTTON_Y] = 255;

    if (down[K_LS_UP])    pad->sThumbLY = 32767;
    if (down[K_LS_DOWN])  pad->sThumbLY = -32768;
    if (down[K_LS_LEFT])  pad->sThumbLX = -32768;
    if (down[K_LS_RIGHT]) pad->sThumbLX = 32767;

    /* The right-stick slots were named ("rsup"...) and mapped from keys but
     * never applied, so only the mouse could look around. */
    if (down[K_RS_UP])    pad->sThumbRY = 32767;
    if (down[K_RS_DOWN])  pad->sThumbRY = -32768;
    if (down[K_RS_LEFT])  pad->sThumbRX = -32768;
    if (down[K_RS_RIGHT]) pad->sThumbRX = 32767;

    /* Mouse look: right stick from accumulated mouse delta. */
    if (s_mouse_dx != 0 || s_mouse_dy != 0) {
        int sens = s_mouse_sens > 0 ? s_mouse_sens : 1800;
        /* Start every nonzero motion just past the game's stick dead zone.
         * Without it a slow mouse -- a few pixels per poll, a few percent of
         * the stick -- was inside the dead zone and turned nothing, while a
         * fast one saturated: look felt heavy. RECOMP_MOUSE_DEADZONE sets it
         * (default 8689, the XDK's right-stick dead zone; 0 = off). */
        static int dz = -1;
        if (dz < 0) {
            const char *e = getenv("RECOMP_MOUSE_DEADZONE");
            dz = e ? atoi(e) : 8689;
            if (dz < 0) dz = 0;
            if (dz > 20000) dz = 20000;
        }
        int rx = s_mouse_dx * sens;
        int ry = -s_mouse_dy * sens;
        if (rx) rx += rx > 0 ? dz : -dz;
        if (ry) ry += ry > 0 ? dz : -dz;
        if (rx > 32767) rx = 32767;
        if (rx < -32768) rx = -32768;
        if (ry > 32767) ry = 32767;
        if (ry < -32768) ry = -32768;
        pad->sThumbRX = (short)rx;
        pad->sThumbRY = (short)ry;
        s_mouse_dx = s_mouse_dy = 0;
    }

    return 1;
}

int xbox_KeyboardSlotForKey(unsigned char c)
{
    return key_slot(c);
}

#if defined(__APPLE__)
/* macOS virtual key codes (kVK_*) for the arrow keys, so the Cocoa key view
 * can drive the d-pad without relying on the terminal. Values are stable
 * across SDKs; matched by numeric literal to avoid pulling HIToolbox into the
 * keyboard_input.c translation unit. */
int xbox_KeyboardSlotForKeyCode(unsigned short keycode)
{
    switch (keycode) {
    case 0x7E: return K_UP;    /* kVK_UpArrow    */
    case 0x7F: return K_DOWN;  /* kVK_DownArrow  */
    case 0x7B: return K_LEFT;  /* kVK_LeftArrow  */
    case 0x7C: return K_RIGHT; /* kVK_RightArrow */
    default:   return -1;
    }
}
#endif

void xbox_KeyboardPressSlot(int slot)
{
    if (slot < 0 || slot >= K_COUNT)
        return;
    /* Held until the matching release; key repeat keeps re-pressing. */
    s_active = 1;
    pthread_mutex_lock(&s_lock);
    s_until[slot] = UINT64_MAX;
    pthread_mutex_unlock(&s_lock);
}

/* Every held key up at once. The window only hears keyUp while it has the
 * focus: a key held through Cmd-Tab or a screenshot never got one, and stayed
 * down for the rest of the session -- a look axis pinned, the view spinning. */
void xbox_KeyboardReleaseAll(void)
{
    uint64_t t = now_ms();
    int i;

    pthread_mutex_lock(&s_lock);
    for (i = 0; i < K_COUNT; i++)
        if (s_until[i] > t)
            s_until[i] = t;
    pthread_mutex_unlock(&s_lock);
}

void xbox_KeyboardReleaseSlot(int slot)
{
    if (slot < 0 || slot >= K_COUNT)
        return;
    s_active = 1;
    pthread_mutex_lock(&s_lock);
    s_until[slot] = now_ms();   /* expire any held press immediately */
    pthread_mutex_unlock(&s_lock);
}

/* ── Mouse input (CoD-style) ─────────────────────────────────── */

void xbox_MouseSetDelta(int dx, int dy)
{
    s_active = 1;   /* motion alone is input: the pad must report as present */
    pthread_mutex_lock(&s_lock);
    s_mouse_dx += dx;
    s_mouse_dy += dy;
    pthread_mutex_unlock(&s_lock);
}

void xbox_MousePressButton(int button)
{
    if (button < 0 || button >= K_MOUSE_BUTTON_COUNT)
        return;
    s_active = 1;
    pthread_mutex_lock(&s_lock);
    s_mouse_buttons |= (1 << button);
    pthread_mutex_unlock(&s_lock);
}

void xbox_MouseReleaseButton(int button)
{
    if (button < 0 || button >= K_MOUSE_BUTTON_COUNT)
        return;
    pthread_mutex_lock(&s_lock);
    s_mouse_buttons &= ~(1 << button);
    pthread_mutex_unlock(&s_lock);
}

void xbox_MouseReleaseAll(void)
{
    pthread_mutex_lock(&s_lock);
    s_mouse_buttons = 0;
    s_mouse_dx = s_mouse_dy = 0;
    pthread_mutex_unlock(&s_lock);
}

#endif
