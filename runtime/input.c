// Host input: one SDL3 gamepad (buttons, sticks, gyro and accelerometer, rumble) and the keyboard, merged into the
// controller ov_hid.c reports as player 1.
//
// The SDL sensor setup, the SDL-to-Switch motion axes and the 4 ms sensor feed are adapted; the button layout rule and
// the default keys are this port's.
//
// Face buttons follow the labels on the pad: a Nintendo-layout pad (SDL reports B on the south button) maps by position,
// any other pad maps Xbox-style, so the button labelled A (or the south button of a PlayStation pad) is Switch A.
// MTF_PAD_LAYOUT=positional maps every pad by Nintendo position instead.
//
// Keyboard (while this process's window is in front): WASD left stick, arrow keys right stick, IJKL... see keymap
// below; Space shakes the controller (the swing the six-axis sensor reports).
// Contains adapted code under the MIT licence: authors and licence texts in THIRD_PARTY_NOTICES.md.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx_wsys.h"
#include "host.h"
#include "input.h"
#include "platform/platform.h"
#include "stream_host.h"
#include "ui/menu.h"

#define BIT(b) (1ull << (b))
#define TRIGGER_ON 8000   // SDL trigger axis 0..32767: past a quarter counts as ZL / ZR
#define STICK_MAX 32767

static PlatMutex lock = PLAT_MUTEX_INIT;
static SDL_Gamepad *pads[INPUT_MAX_PADS];
#define pad (pads[0])
static SDL_JoystickID pad_ids[INPUT_MAX_PADS];
static char pad_names[INPUT_MAX_PADS][96];
static int started, sdl_ok, sensors_on;
static SDL_JoystickID sensor_pad;
static uint64_t pad_buttons;
static int32_t pad_axes[4];
static uint64_t pad_buttons_arr[INPUT_MAX_PADS];
static int32_t pad_axes_arr[INPUT_MAX_PADS][4];

// ---- keyboard
static const struct { uint8_t key; uint8_t vk; } key_vks[] = {
    {KEY_SPACE, VK_SPACE}, {KEY_TAB, VK_TAB}, {KEY_LSHIFT, VK_LSHIFT}, {KEY_LCTRL, VK_LCONTROL},
    {KEY_UP, VK_UP}, {KEY_DOWN, VK_DOWN}, {KEY_LEFT, VK_LEFT}, {KEY_RIGHT, VK_RIGHT},
    {KEY_MOUSE_L, VK_LBUTTON}, {KEY_MOUSE_R, VK_RBUTTON}, {KEY_MOUSE_M, VK_MBUTTON},
    {KEY_MOUSE_X1, VK_XBUTTON1}, {KEY_MOUSE_X2, VK_XBUTTON2},
    {KEY_ENTER, VK_RETURN}, {KEY_BACKSPACE, VK_BACK}, {KEY_RSHIFT, VK_RSHIFT}, {KEY_RCTRL, VK_RCONTROL},
    {KEY_LALT, VK_LMENU}, {KEY_CAPSLOCK, VK_CAPITAL}, {KEY_INSERT, VK_INSERT}, {KEY_DELETE, VK_DELETE},
    {KEY_HOME, VK_HOME}, {KEY_END, VK_END}, {KEY_PAGEUP, VK_PRIOR}, {KEY_PAGEDOWN, VK_NEXT},
    {KEY_F2, VK_F2}, {KEY_F3, VK_F3}, {KEY_F4, VK_F4}, {KEY_F5, VK_F5},
    {KEY_F6, VK_F6}, {KEY_F7, VK_F7}, {KEY_F9, VK_F9}, {KEY_F12, VK_F12},
    {KEY_NUM_ADD, VK_ADD}, {KEY_NUM_SUB, VK_SUBTRACT}, {KEY_NUM_MUL, VK_MULTIPLY},
    {KEY_NUM_DIV, VK_DIVIDE}, {KEY_NUM_DECIMAL, VK_DECIMAL},
    {KEY_MINUS, VK_OEM_MINUS}, {KEY_EQUALS, VK_OEM_PLUS}, {KEY_LBRACKET, VK_OEM_4}, {KEY_RBRACKET, VK_OEM_6},
    {KEY_SEMICOLON, VK_OEM_1}, {KEY_APOSTROPHE, VK_OEM_7}, {KEY_COMMA, VK_OEM_COMMA}, {KEY_PERIOD, VK_OEM_PERIOD},
    {KEY_SLASH, VK_OEM_2}, {KEY_BACKSLASH, VK_OEM_5}, {KEY_GRAVE, VK_OEM_3},
};

static int key_to_vk(int k) {
    if (k >= KEY_A && k <= KEY_Z) return 'A' + (k - KEY_A);
    if (k >= KEY_0 && k <= KEY_9) return '0' + (k - KEY_0);
    if (k >= KEY_NUM0 && k <= KEY_NUM9) return VK_NUMPAD0 + (k - KEY_NUM0);
    for (size_t i = 0; i < sizeof key_vks / sizeof *key_vks; i++)
        if (key_vks[i].key == k) return key_vks[i].vk;
    return 0;
}

static int window_in_front(void) {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static int key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void read_key_down(uint8_t down[KEY_COUNT]) {
    memset(down, 0, KEY_COUNT);
    if (!window_in_front()) return;
    for (int k = KEY_A; k < KEY_COUNT; k++) {
        int vk = key_to_vk(k);
        if (vk && key(vk)) down[k] = 1;
    }
}

static void keyboard_read(uint64_t *buttons, int32_t axes[4]) {
    uint8_t down[KEY_COUNT];
    read_key_down(down);
    *buttons |= input_map_buttons(down);
    int32_t ls[2] = {0}, rs[2] = {0};
    input_map_stick(down, 0, ls);
    input_map_stick(down, 1, rs);
    if (ls[0]) axes[0] = ls[0];
    if (ls[1]) axes[1] = ls[1];
    if (rs[0]) axes[2] = rs[0];
    if (rs[1]) axes[3] = rs[1];
}

int input_shake_held(void) {
    uint8_t down[KEY_COUNT];
    read_key_down(down);
    return input_map_shake(down);
}

// ---- gamepad (under lock)
static int pad_dev_type = INPUT_DEV_XBOX;
static int last_dev = INPUT_DEV_KEYBOARD;
int input_last_device(void) { return __atomic_load_n(&last_dev, __ATOMIC_RELAXED); }

static int pad_type_of(SDL_Gamepad *gp) {
    SDL_GamepadType gt = SDL_GetGamepadType(gp);
    return gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO || gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT ||
                   gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT ||
                   gt == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR
               ? INPUT_DEV_NINTENDO
           : gt == SDL_GAMEPAD_TYPE_PS3 || gt == SDL_GAMEPAD_TYPE_PS4 || gt == SDL_GAMEPAD_TYPE_PS5
               ? INPUT_DEV_PLAYSTATION
               : INPUT_DEV_XBOX;
}

static void open_pad(void) {
    for (int i = 0; i < INPUT_MAX_PADS; i++) {
        if (pads[i] && !SDL_GamepadConnected(pads[i])) {
            rt_log("input: gamepad %d disconnected", i + 1);
            SDL_CloseGamepad(pads[i]);
            pads[i] = NULL;
            pad_ids[i] = 0;
            pad_names[i][0] = 0;
            if (i == 0) {
                sensors_on = 0;
                sensor_pad = 0;
            }
        }
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        for (int j = 0; j < n; j++) {
            int already = 0;
            for (int i = 0; i < INPUT_MAX_PADS; i++) {
                if (pads[i] && pad_ids[i] == ids[j]) { already = 1; break; }
            }
            if (already) continue;
            int slot = -1;
            for (int i = 0; i < INPUT_MAX_PADS; i++) {
                if (!pads[i]) { slot = i; break; }
            }
            if (slot < 0) break;
            SDL_Gamepad *gp = SDL_OpenGamepad(ids[j]);
            if (gp) {
                pads[slot] = gp;
                pad_ids[slot] = ids[j];
                const char *nm = SDL_GetGamepadName(gp);
                snprintf(pad_names[slot], sizeof pad_names[slot], "%s", nm && *nm ? nm : "Controller");
                int dt = pad_type_of(gp);
                if (slot == 0) {
                    pad_dev_type = dt;
                    __atomic_store_n(&last_dev, dt, __ATOMIC_RELAXED);
                }
                rt_log("input: gamepad %d \"%s\"", slot + 1, pad_names[slot]);
            }
        }
    }
    SDL_free(ids);
}

static int nintendo_layout_dev(SDL_Gamepad *gp) {
    const char *e = cfg_get("MTF_PAD_LAYOUT");
    if (e && !strcmp(e, "positional")) return 1;
    return SDL_GetGamepadButtonLabel(gp, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
}

// One stick, SDL axes to Switch axes (SDL: +y down; Switch: +y up), through a radial dead zone of MTF_STICK_DEADZONE
// percent (cfg.c, default 8; 0..50) with the rest of the range stretched to full: a worn pad resting a little off
// centre reads as centred, and the edge still reaches full deflection.
static void stick(int16_t sx, int16_t sy, int32_t *ox, int32_t *oy) {
    static unsigned seen = ~0u;
    static double dz;
    if (seen != cfg_version()) {   // the menu may change it live
        seen = cfg_version();
        int pct = cfg_int("MTF_STICK_DEADZONE");
        dz = (pct < 0 ? 0 : pct > 50 ? 50 : pct) / 100.0 * STICK_MAX;
    }
    double x = sx < -STICK_MAX ? -STICK_MAX : sx, y = sy < -STICK_MAX ? STICK_MAX : -(double)sy;
    double r = sqrt(x * x + y * y);
    if (r <= dz) {
        *ox = *oy = 0;
        return;
    }
    double k = fmin(1.0, (r - dz) / (STICK_MAX - dz)) * STICK_MAX / r;
    *ox = (int32_t)fmax(-STICK_MAX, fmin(STICK_MAX, lround(x * k)));
    *oy = (int32_t)fmax(-STICK_MAX, fmin(STICK_MAX, lround(y * k)));
}

static uint8_t pad_down_live[PAD_COUNT];
static int pad_layout_now = -1;

static void read_pad_down_dev(SDL_Gamepad *gp, uint8_t pdown[PAD_COUNT]) {
    memset(pdown, 0, PAD_COUNT);
    if (!gp) return;
    static const struct { SDL_GamepadButton sdl; uint8_t p; } btns[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, PAD_SOUTH}, {SDL_GAMEPAD_BUTTON_EAST, PAD_EAST},
        {SDL_GAMEPAD_BUTTON_WEST, PAD_WEST}, {SDL_GAMEPAD_BUTTON_NORTH, PAD_NORTH},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, PAD_LSTICK}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, PAD_RSTICK},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, PAD_L}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_R},
        {SDL_GAMEPAD_BUTTON_START, PAD_START}, {SDL_GAMEPAD_BUTTON_BACK, PAD_BACK},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_DPAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_DPAD_UP},
        {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_DPAD_RIGHT}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_DPAD_DOWN},
        {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, PAD_LPADDLE1}, {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, PAD_RPADDLE1},
        {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, PAD_LPADDLE2}, {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, PAD_RPADDLE2},
    };
    for (size_t i = 0; i < sizeof btns / sizeof *btns; i++)
        if (SDL_GetGamepadButton(gp, btns[i].sdl)) pdown[btns[i].p] = 1;
    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > TRIGGER_ON) pdown[PAD_ZL] = 1;
    if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > TRIGGER_ON) pdown[PAD_ZR] = 1;
}

static void pad_read(void) {
    for (int i = 0; i < INPUT_MAX_PADS; i++) {
        uint64_t b = 0;
        int32_t a[4] = {0};
        if (pads[i]) {
            uint8_t pdown[PAD_COUNT];
            int nin = nintendo_layout_dev(pads[i]);
            int layout = nin ? PAD_LAYOUT_NINTENDO : PAD_LAYOUT_XBOX;
            read_pad_down_dev(pads[i], pdown);
            b = input_map_pad_buttons(pdown, layout);
            stick(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTX), SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_LEFTY), &a[0], &a[1]);
            stick(SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_RIGHTX), SDL_GetGamepadAxis(pads[i], SDL_GAMEPAD_AXIS_RIGHTY), &a[2], &a[3]);
            if (i == 0) {
                memcpy(pad_down_live, pdown, sizeof pad_down_live);
                pad_layout_now = layout;
            }
        } else if (i == 0) {
            memset(pad_down_live, 0, sizeof pad_down_live);
            pad_layout_now = -1;
        }
        pad_buttons_arr[i] = b;
        memcpy(pad_axes_arr[i], a, sizeof a);
    }
    pad_buttons = pad_buttons_arr[0];
    memcpy(pad_axes, pad_axes_arr[0], sizeof pad_axes);
}

// The SDL accelerometer reads the push against gravity in m/s^2 (lying flat: y = +9.81) and the gyro rad/s; Switch
// six-axis axes are x right, y toward the top edge, z out of the face, acceleration as gravity reads (flat: z = -1 G).
// So Switch x = SDL x, y = -SDL z, z = SDL y, with the acceleration negated and scaled to G.
static void motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    acc[0] = -a[0] / SDL_STANDARD_GRAVITY;
    acc[1] = a[2] / SDL_STANDARD_GRAVITY;
    acc[2] = -a[1] / SDL_STANDARD_GRAVITY;
    gyro[0] = g[0];
    gyro[1] = -g[2];
    gyro[2] = g[1];
}

static int sensors_ready(void) {
    if (!pad) return 0;
    SDL_JoystickID id = SDL_GetGamepadID(pad);
    if (id == sensor_pad) return sensors_on;
    sensor_pad = id;
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_SENSOR_UPDATE, false);   // the data updates without an event per sample
    sensors_on = SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO) && SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL) &&
                 SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true) &&
                 SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
    rt_log(sensors_on ? "input: motion sensors on" : "input: the gamepad has no gyro and accelerometer: motion reads a "
                                                      "controller at rest (Space shakes it)");
    return sensors_on;
}

// 250 Hz: gamepads update, the buttons and sticks are sampled, and the newest motion sample goes into the fusion.
static void input_main(void *arg) {
    (void)arg;
    for (int tick = 0;; tick++) {
        float a[3], g[3];
        int have = 0;
        static unsigned map_ver = ~0u;
    unsigned ver = cfg_version();
    if (map_ver != ver) {
        map_ver = ver;
        input_map_configure();
    }
    plat_mutex_lock(&lock);
        SDL_PumpEvents();   // hot-plug
        SDL_UpdateGamepads();
        if (tick % 50 == 0) open_pad();
        pad_read();
        have = sensors_ready() && SDL_GetGamepadSensorData(pad, SDL_SENSOR_ACCEL, a, 3) &&
               SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, g, 3);
        plat_mutex_unlock(&lock);
        if (have && cfg_bool("MTF_MOTION")) {
            double acc[3], gyro[3];
            motion_axes(a, g, acc, gyro);
            motion_feed(motion_global(), acc, gyro, kern_now_ns());
        }
        plat_wait_ns(4000000);
    }
}

static void start(void) {
    if (started) return;
    started = 1;
    if (cfg_get("MTF_INPUT") && !strcmp(cfg_get("MTF_INPUT"), "none")) {
        rt_log("input: off (MTF_INPUT=none)");
        return;
    }
    const char *rl_pad = getenv("RECOMPLAUNCHER_PAD");
    if (rl_pad) {
        if (!strcmp(rl_pad, "xbox")) __atomic_store_n(&last_dev, INPUT_DEV_XBOX, __ATOMIC_RELAXED);
        else if (!strcmp(rl_pad, "playstation")) __atomic_store_n(&last_dev, INPUT_DEV_PLAYSTATION, __ATOMIC_RELAXED);
    }
    const char *rl = getenv("RECOMPLAUNCHER");
    if (!rl || strcmp(rl, "1")) SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    sdl_ok = SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_SENSOR);
    if (!sdl_ok) rt_log("input: SDL gamepad init failed (%s): keyboard only", SDL_GetError());
    else open_pad();
    if (sdl_ok && !plat_thread_spawn(input_main, NULL, "input", 0)) rt_fatal("input: cannot start the input thread");
    rt_log("input: keyboard WASD / arrows sticks, K A, J B, I X, U Y, Q L, E R, Z ZL, C ZR, Enter +, Backspace -, "
           "Space shake%s", pad ? "; gamepad open" : "; no gamepad yet");
}

// MTF_INPUT_SCRIPT=<seconds>:<button>[+<button>...][/<hold ms>],... presses buttons at those times after start (for
// unattended test runs), e.g. "25:A,27:A,40:PLUS/200". Buttons: A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LS RS.
static uint64_t script_buttons(void) {
    static struct { uint64_t at_ns, until_ns, mask; } ev[64];
    static int n = -1;
    static uint64_t t0;
    if (n < 0) {
        n = 0;
        t0 = plat_now_ns();
        const char *e = cfg_get("MTF_INPUT_SCRIPT");
        static const struct { const char *name; int bit; } names[] = {
            {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LS", B_LS}, {"RS", B_RS}, {"L", B_L}, {"R", B_R},
            {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP},
            {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}};
        for (const char *p = e; p && *p && n < 64;) {
            char *q;
            double at = strtod(p, &q);
            if (q == p || *q != ':') break;
            p = q + 1;
            uint64_t mask = 0, hold = 150;
            for (;;) {
                size_t len = strcspn(p, "+/,");
                for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                    if (strlen(names[i].name) == len && !strncmp(p, names[i].name, len)) mask |= BIT(names[i].bit);
                p += len;
                if (*p != '+') break;
                p++;
            }
            if (*p == '/') hold = strtoull(p + 1, (char **)&p, 10);
            ev[n].at_ns = (uint64_t)(at * 1e9);
            ev[n].until_ns = ev[n].at_ns + hold * 1000000ull;
            ev[n].mask = mask;
            n++;
            if (*p == ',') p++;
        }
        if (n) rt_log("input: %d scripted presses (MTF_INPUT_SCRIPT)", n);
    }
    uint64_t now = plat_now_ns() - t0, b = 0;
    for (int i = 0; i < n; i++)
        if (now >= ev[i].at_ns && now < ev[i].until_ns) b |= ev[i].mask;
    return b;
}

// MTF_INPUT_FILE=<path>: lines appended to a text file are carried out in order, one at a time, for runs a test harness
// drives step by step (it writes a line, waits, looks at a shot). The file is read every 50 ms; a partial last line
// waits for its end of line. Lines:
//   <button>[+<button>...][/<hold ms>]   a press (default 120 ms): the buttons of MTF_INPUT_SCRIPT, and SL SR SU SD,
//                                        the left stick held left, right, up or down
//   WAIT <ms>                           nothing pressed for that long
//   SHOT <file>                         the next presented picture as a PNG (relative: the working directory)
//   SHOTS <n> <prefix>                  the next n presented pictures, one per frame: <prefix>-00.png ...
//   CFG <NAME>=<value>                  a live setting for this run (cfg_set), not saved unless the menu saves
// Each press is followed by 80 ms with nothing pressed, so that two presses of one button are two presses.
typedef struct { uint64_t mask; int sx, sy; uint32_t ms; int kind; char arg[200]; } FileCmd;   // kind 0 press, 1 wait,
                                                                                                    // 2 shot, 3 shots (ms: n)
static PlatMutex file_lock = PLAT_MUTEX_INIT;

static int parse_file_cmd(const char *s, FileCmd *c) {
    static const struct { const char *name; int bit; } names[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"LS", B_LS}, {"RS", B_RS}, {"L", B_L}, {"R", B_R},
        {"ZL", B_ZL}, {"ZR", B_ZR}, {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP},
        {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}};
    memset(c, 0, sizeof *c);
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || *s == '#') return 0;
    if (!strncmp(s, "WAIT ", 5)) {
        c->kind = 1, c->ms = (uint32_t)strtoul(s + 5, NULL, 10);
        return 1;
    }
    if (!strncmp(s, "CFG ", 4)) {
        c->kind = 4;
        snprintf(c->arg, sizeof c->arg, "%s", s + 4);
        return strchr(c->arg, '=') != NULL;
    }
    if (!strncmp(s, "SHOTS ", 6)) {
        char *rest;
        c->kind = 3, c->ms = (uint32_t)strtoul(s + 6, &rest, 10);
        while (*rest == ' ') rest++;
        snprintf(c->arg, sizeof c->arg, "%s", rest);
        return c->ms > 0 && c->arg[0] != 0;
    }
    if (!strncmp(s, "SHOT ", 5)) {
        c->kind = 2;
        snprintf(c->arg, sizeof c->arg, "%s", s + 5);
        return c->arg[0] != 0;
    }
    c->ms = 120;
    for (;;) {
        size_t len = strcspn(s, "+/ ");
        int known = 0;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == len && !strncmp(s, names[i].name, len)) c->mask |= BIT(names[i].bit), known = 1;
        if (len == 2 && s[0] == 'S' && strchr("LRUD", s[1]))
            known = 1, c->sx = s[1] == 'L' ? -1 : s[1] == 'R' ? 1 : c->sx, c->sy = s[1] == 'U' ? 1 : s[1] == 'D' ? -1 : c->sy;
        if (!known) return -1;
        s += len;
        if (*s != '+') break;
        s++;
    }
    if (*s == '/') c->ms = (uint32_t)strtoul(s + 1, NULL, 10);
    return 1;
}

static void file_input(uint64_t *buttons, int32_t axes[4]) {
    static int state = -1;   // -1 not read yet, 0 off, 1 on
    static const char *path;
    static long off;
    static uint64_t next_read, press_end, cmd_end;
    static uint64_t mask;
    static int sx, sy;
    static FileCmd q[64];
    static int qh, qn;
    if (state == 0) return;
    plat_mutex_lock(&file_lock);
    if (state < 0) {
        path = cfg_get("MTF_INPUT_FILE");
        state = path && *path;
        if (state) rt_log("input: commands from %s (MTF_INPUT_FILE)", path);
    }
    uint64_t now = plat_now_ns();
    if (state && now >= next_read) {
        next_read = now + 50000000ull;
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            if (size < off) off = 0;   // the file was started again
            if (size > off && size - off < 65536) {
                static char buf[65536];   // file_lock held
                fseek(f, off, SEEK_SET);
                size_t n = fread(buf, 1, (size_t)(size - off), f);
                size_t done = 0;
                for (size_t i = 0; i < n; i++) {
                    if (buf[i] != '\n') continue;
                    buf[i] = 0;
                    if (i > done && buf[i - 1] == '\r') buf[i - 1] = 0;
                    FileCmd c;
                    int r = parse_file_cmd(buf + done, &c);
                    if (r < 0) rt_log("input: MTF_INPUT_FILE line not understood: %s", buf + done);
                    else if (r && qn < 64) q[(qh + qn++) % 64] = c;
                    done = i + 1;
                }
                off += (long)done;
            }
            fclose(f);
        }
    }
    if (now >= cmd_end && qn) {
        FileCmd *c = &q[qh];
        qh = (qh + 1) % 64, qn--;
        mask = 0, sx = sy = 0;
        if (c->kind == 0) {
            mask = c->mask, sx = c->sx, sy = c->sy;
            press_end = now + (uint64_t)c->ms * 1000000ull;
            cmd_end = press_end + 80000000ull;
        } else if (c->kind == 1) {
            press_end = now, cmd_end = now + (uint64_t)c->ms * 1000000ull;
        } else if (c->kind == 2) {
            gfx_shot_request(c->arg);
            press_end = cmd_end = now;
        } else if (c->kind == 4) {
            char *eq = strchr(c->arg, '=');
            *eq = 0;
            cfg_set(c->arg, eq + 1);
            rt_log("input: CFG %s = %s", c->arg, eq + 1);
            press_end = cmd_end = now;
        } else {
            gfx_shots_request(c->arg, (int)c->ms);
            press_end = cmd_end = now;
        }
    }
    if (now < press_end) {
        *buttons |= mask;
        if (sx) axes[0] = sx * STICK_MAX;
        if (sy) axes[1] = sy * STICK_MAX;
    }
    plat_mutex_unlock(&file_lock);
}

#define STICK_ACTIVE 8000   // past the drift of a worn stick
static uint64_t last_active_ns;

uint64_t input_idle_ns(void) { return plat_now_ns() - __atomic_load_n(&last_active_ns, __ATOMIC_RELAXED); }

void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    static unsigned map_ver = ~0u;
    unsigned ver = cfg_version();
    if (map_ver != ver) {
        map_ver = ver;
        input_map_configure();
    }
    plat_mutex_lock(&lock);
    start();
    uint64_t b = pad_buttons;
    int32_t a[4];
    memcpy(a, pad_axes, sizeof a);
    int dt = pad_dev_type;
    plat_mutex_unlock(&lock);
    static uint64_t prev_b, prev_kb;
    static int32_t prev_a[4];
    uint64_t new_pad = b & ~prev_b;
    prev_b = b;
    int pad_moved = new_pad != 0;
    for (int i = 0; i < 4; i++) {
        int32_t d = a[i] - prev_a[i];
        if ((d > 4000 || d < -4000) && (a[i] > STICK_ACTIVE || a[i] < -STICK_ACTIVE)) {
            pad_moved = 1;
            prev_a[i] = a[i];
        } else if (a[i] <= STICK_ACTIVE && a[i] >= -STICK_ACTIVE) {
            prev_a[i] = a[i];
        }
    }
    uint64_t kb = 0;
    int32_t ka[4] = {0};
    keyboard_read(&kb, ka);
    uint64_t new_kb = kb & ~prev_kb;
    prev_kb = kb;
    int kb_moved = new_kb != 0;
    for (int i = 0; i < 4; i++) kb_moved |= ka[i] > STICK_ACTIVE || ka[i] < -STICK_ACTIVE;
    int old_dev = __atomic_load_n(&last_dev, __ATOMIC_RELAXED);
    int new_dev = kb_moved ? INPUT_DEV_KEYBOARD : (pad_moved ? dt : old_dev);
    if (new_dev != old_dev) {
        __atomic_store_n(&last_dev, new_dev, __ATOMIC_RELAXED);
        rt_log("input: device %d -> %d (kb=%d pad=%d)", old_dev, new_dev, kb_moved, pad_moved);
    }
    extern void ov_font_tick(void);
    ov_font_tick();
    b |= kb;
    for (int i = 0; i < 4; i++)
        if (ka[i]) a[i] = ka[i];
    // the menu's combination, held by the player (not the script): an edge toggles the menu
    static int combo_was, held_off, was_open;
    int combo = (b & (BIT(B_PLUS) | BIT(B_MINUS))) == (BIT(B_PLUS) | BIT(B_MINUS));
    if (combo && !combo_was) menu_toggle();
    combo_was = combo;
    int open = menu_is_open();
    if (open != was_open) held_off = 1;   // opened or closed: what is held now belongs to the menu
    was_open = open;
    if (held_off && !b) held_off = 0;
    b |= script_buttons();
    file_input(&b, a);
    int moved = b != 0;   // someone is playing (input_idle_ns)
    for (int i = 0; i < 4; i++) moved |= a[i] > STICK_ACTIVE || a[i] < -STICK_ACTIVE;
    if (moved) __atomic_store_n(&last_active_ns, plat_now_ns(), __ATOMIC_RELAXED);
    if (open || held_off) b = 0, memset(a, 0, sizeof a);
    *buttons = b;
    *lx = a[0], *ly = a[1], *rx = a[2], *ry = a[3];
}

static const char *player_cfg_key(int player) {
    static const char *const keys[INPUT_MAX_PLAYERS] = {
        "MTF_P1_INPUT", "MTF_P2_INPUT", "MTF_P3_INPUT", "MTF_P4_INPUT",
    };
    return (player >= 0 && player < INPUT_MAX_PLAYERS) ? keys[player] : NULL;
}

void input_list_pads(InputPadList *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    plat_mutex_lock(&lock);
    start();
    for (int i = 0; i < INPUT_MAX_PADS; i++) {
        if (pads[i]) {
            out->connected[i] = 1;
            out->count++;
            snprintf(out->names[i], sizeof out->names[i], "%s", pad_names[i]);
        }
    }
    plat_mutex_unlock(&lock);
}

int input_player_connected(int player) {
    if (player < 0 || player >= INPUT_MAX_PLAYERS) return 0;
    if (stream_host_remote_connected(player)) return 1;
    static int mp_logged;
    int mp_on = cfg_bool("MTF_MULTIPLAYER_INPUT");
    if (!mp_logged) {
        mp_logged = 1;
        rt_log("input: multiplayer input %s (MTF_MULTIPLAYER_INPUT)", mp_on ? "on" : "off");
    }
    if (!mp_on) return player == 0;
    const char *src = cfg_get(player_cfg_key(player));
    if (!src || !*src) return player == 0;
    if (!strcmp(src, "none")) return 0;
    if (!strcmp(src, "auto") || !strcmp(src, "keyboard")) return 1;
    if (!strncmp(src, "pad:", 4)) {
        int idx = atoi(src + 4);
        if (idx < 0 || idx >= INPUT_MAX_PADS) return 0;
        plat_mutex_lock(&lock);
        start();
        int conn = pads[idx] != NULL;
        plat_mutex_unlock(&lock);
        return conn;
    }
    return 0;
}

int input_connected_count(void) {
    int count = 0;
    for (int i = 0; i < INPUT_MAX_PLAYERS; i++)
        if (input_player_connected(i)) count++;
    return count > 0 ? count : 1;
}

void input_poll_player(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    if (stream_host_remote_poll(player, buttons, lx, ly, rx, ry)) {
        if (menu_is_open()) { *buttons = 0; *lx = *ly = *rx = *ry = 0; }
        return;
    }
    if (!cfg_bool("MTF_MULTIPLAYER_INPUT")) {
        if (player == 0) input_poll(buttons, lx, ly, rx, ry);
        else { *buttons = 0; *lx = *ly = *rx = *ry = 0; }
        return;
    }
    const char *src = player_cfg_key(player) ? cfg_get(player_cfg_key(player)) : "none";
    if (!src || !*src) src = player == 0 ? "auto" : "none";
    if (!strcmp(src, "auto") && player == 0) {
        input_poll(buttons, lx, ly, rx, ry);
        return;
    }
    uint64_t b = 0;
    int32_t a[4] = {0};
    if (!strcmp(src, "keyboard")) {
        keyboard_read(&b, a);
    } else if (!strncmp(src, "pad:", 4)) {
        int idx = atoi(src + 4);
        if (idx >= 0 && idx < INPUT_MAX_PADS) {
            plat_mutex_lock(&lock);
            start();
            if (pads[idx]) {
                b = pad_buttons_arr[idx];
                memcpy(a, pad_axes_arr[idx], sizeof a);
            }
            plat_mutex_unlock(&lock);
        }
    }
    if (menu_is_open()) {
        b = 0;
        memset(a, 0, sizeof a);
    }
    *buttons = b;
    *lx = a[0], *ly = a[1], *rx = a[2], *ry = a[3];
}

int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly) {
    static unsigned map_ver = ~0u;
    unsigned ver = cfg_version();
    if (map_ver != ver) {
        map_ver = ver;
        input_map_configure();
    }
    plat_mutex_lock(&lock);
    start();
    uint64_t b = pad_buttons;
    int32_t a[4];
    memcpy(a, pad_axes, sizeof a);
    int have = pad != NULL;
    plat_mutex_unlock(&lock);
    keyboard_read(&b, a);
    *buttons = b, *lx = a[0], *ly = a[1];
    return have;
}

// MTF_RUMBLE_STRENGTH (percent, 0 off) scales the game's amplitudes after MTF_RUMBLE_CURVE (an exponent: under 1
// lifts the weak pulses a pad would not make felt), both read at every call (the menu changes them live).
static float rumble_shape(float a, double curve, double gain) {
    a = a < 0 ? 0 : a > 1 ? 1 : a;
    double v = a > 0 ? pow(a, curve) * gain : 0;
    return v > 1 ? 1.0f : (float)v;
}

int input_rumble(float low, float high, uint32_t ms) {
    double gain = cfg_double("MTF_RUMBLE_STRENGTH") / 100.0, curve = cfg_double("MTF_RUMBLE_CURVE");
    if (gain <= 0) return 0;
    if (curve < 0.2 || curve > 1.5) curve = 1;
    low = rumble_shape(low, curve, gain);
    high = rumble_shape(high, curve, gain);
    static unsigned map_ver = ~0u;
    unsigned ver = cfg_version();
    if (map_ver != ver) {
        map_ver = ver;
        input_map_configure();
    }
    plat_mutex_lock(&lock);
    int ok = pad && SDL_RumbleGamepad(pad, (Uint16)(low * 65535.0f), (Uint16)(high * 65535.0f), ms);
    plat_mutex_unlock(&lock);
    return ok;
}

// ---- the game window's mouse: this port reads no mouse, so the window keeps the cursor
int input_window_msg(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT *res) {
    (void)h, (void)w, (void)l, (void)res;
    if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN)
        __atomic_store_n(&last_dev, INPUT_DEV_KEYBOARD, __ATOMIC_RELAXED);
    return 0;
}
void input_cursor_suspend(int on) { (void)on; }

uint32_t input_key_char(int k) {
    if (k < KEY_MINUS || k > KEY_GRAVE) return 0;
    int vk = key_to_vk(k);
    if (!vk) return 0;
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    BYTE ks[256] = {0};
    WCHAR out[4] = {0};
    if (ToUnicode((UINT)vk, sc, ks, out, 4, 0) == 1 && out[0] >= 0x21 && out[0] != 0x7f) return out[0];
    return 0;
}

int input_pad_layout(void) { return pad_layout_now; }

int input_pad_info(InputPadInfo *out) {
    plat_mutex_lock(&lock);
    int ok = pad != NULL;
    if (ok && out) {
        out->type = (int)SDL_GetGamepadType(pad);
        const char *nm = SDL_GetGamepadName(pad);
        snprintf(out->name, sizeof out->name, "%s", nm ? nm : "");
    }
    plat_mutex_unlock(&lock);
    return ok;
}

int input_pad_label(int p) {
    if (p < PAD_SOUTH || p > PAD_NORTH) return PAD_LABEL_NONE;
    plat_mutex_lock(&lock);
    int lbl = PAD_LABEL_NONE;
    if (pad) {
        static const SDL_GamepadButton b[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
                                               SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
        SDL_GamepadButtonLabel sl = SDL_GetGamepadButtonLabel(pad, b[p - PAD_SOUTH]);
        switch (sl) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: lbl = PAD_LABEL_A; break;
        case SDL_GAMEPAD_BUTTON_LABEL_B: lbl = PAD_LABEL_B; break;
        case SDL_GAMEPAD_BUTTON_LABEL_X: lbl = PAD_LABEL_X; break;
        case SDL_GAMEPAD_BUTTON_LABEL_Y: lbl = PAD_LABEL_Y; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS: lbl = PAD_LABEL_CROSS; break;
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: lbl = PAD_LABEL_CIRCLE; break;
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: lbl = PAD_LABEL_SQUARE; break;
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: lbl = PAD_LABEL_TRIANGLE; break;
        default: break;
        }
    }
    plat_mutex_unlock(&lock);
    return lbl;
}

int input_capture(int reset, int *id_out) {
    static uint8_t prev_k[KEY_COUNT], prev_p[PAD_COUNT];
    uint8_t cur_k[KEY_COUNT], cur_p[PAD_COUNT];
    read_key_down(cur_k);
    plat_mutex_lock(&lock);
    memcpy(cur_p, pad_down_live, sizeof cur_p);
    plat_mutex_unlock(&lock);
    if (reset) {
        memcpy(prev_k, cur_k, sizeof prev_k);
        memcpy(prev_p, cur_p, sizeof prev_p);
        return INPUT_CAPTURE_NONE;
    }
    for (int k = KEY_A; k < KEY_COUNT; k++) {
        if (cur_k[k] && !prev_k[k]) {
            memcpy(prev_k, cur_k, sizeof prev_k);
            if (id_out) *id_out = k;
            return INPUT_CAPTURE_KEY;
        }
    }
    for (int p = PAD_SOUTH; p < PAD_COUNT; p++) {
        if (cur_p[p] && !prev_p[p]) {
            memcpy(prev_p, cur_p, sizeof prev_p);
            if (id_out) *id_out = p;
            return INPUT_CAPTURE_PAD;
        }
    }
    memcpy(prev_k, cur_k, sizeof prev_k);
    memcpy(prev_p, cur_p, sizeof prev_p);
    return INPUT_CAPTURE_NONE;
}
