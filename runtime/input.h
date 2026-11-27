// Host input for the game's controller (input.c): one SDL3 gamepad, with its gyro and accelerometer, and the keyboard
// as a second source, merged into one Switch controller for player 1 (ov_hid.c serves it to nn::hid).
#pragma once
#include <stdint.h>

// Switch NpadButton bits: A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14
// Down=15. Sticks are -32767..32767, up is +y. Any thread; starts the input thread on the first call.
// While the in-game menu is open (ui/menu.h) the game gets a released controller; Minus and Plus held together (Enter
// and Backspace on the keyboard) open and close it, and the buttons of that press reach the game only once released.
void input_poll(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
enum { INPUT_MAX_PADS = 4, INPUT_MAX_PLAYERS = 4 };
typedef struct {
    int count;
    int connected[INPUT_MAX_PADS];
    char names[INPUT_MAX_PADS][96];
} InputPadList;
void input_list_pads(InputPadList *out);
int input_player_connected(int player);
int input_connected_count(void);
void input_poll_player(int player, uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
uint64_t input_idle_ns(void);   // since a button was last held or a stick last pushed
enum { INPUT_DEV_KEYBOARD, INPUT_DEV_NINTENDO, INPUT_DEV_XBOX, INPUT_DEV_PLAYSTATION };
int input_last_device(void);    // last active input device (keyboard or connected gamepad type)
#include "input_map.h"
#ifndef B_LS
#define B_LS B_STICK_L
#define B_RS B_STICK_R
#endif
enum { INPUT_CAPTURE_NONE, INPUT_CAPTURE_KEY, INPUT_CAPTURE_PAD };
enum { PAD_LABEL_NONE, PAD_LABEL_A, PAD_LABEL_B, PAD_LABEL_X, PAD_LABEL_Y,
       PAD_LABEL_CROSS, PAD_LABEL_CIRCLE, PAD_LABEL_SQUARE, PAD_LABEL_TRIANGLE };
typedef struct { int type; char name[128]; } InputPadInfo;
int input_capture(int reset, int *id_out);
int input_capture_for(int player, int reset, int *id_out);
uint32_t input_key_char(int key);
int input_pad_label(int pad);
int input_pad_layout(void);
int input_pad_info(InputPadInfo *out);
// The controller as the menu reads it (gamepad and keyboard, no script): 1 when a gamepad is connected.
int input_pad_read(uint64_t *buttons, int32_t *lx, int32_t *ly);
// The shake key (Space) is held: ov_hid.c adds the side-to-side swing the six-axis sensor reports for it.
int input_shake_held(void);
// Rumble on the gamepad: low (heavy) and high (light) motor 0..1 for ms milliseconds. 1 if a gamepad took it.
int input_rumble(float low, float high, uint32_t ms);
// A gamepad is connected (for nn::hid's vibration handles and the log).

// motion_fusion.c: nn::hid::SixAxisSensorState and the sensor fusion that produces it.
typedef struct {
    int64_t delta_ns, sampling;
    float acc[3], gyro[3], angle[3], dir[9];   // G, revolutions/s, revolutions, controller axes in the reference frame
    uint32_t attributes, reserved;             // bit 0 connected
} MotionState;
_Static_assert(sizeof(MotionState) == 0x60, "SixAxisSensorState ABI");
typedef struct MotionFusion MotionFusion;
MotionFusion *motion_new(void);
MotionFusion *motion_global(void);   // the gamepad's, fed by input.c
void motion_feed(MotionFusion *m, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
int motion_read(MotionFusion *m, MotionState *out, int max, uint64_t now_ns);   // newest first; 0 = none in 200 ms

// The game window: the cursor is released around a fullscreen switch (no-op: this port does not capture the mouse).
void input_cursor_suspend(int on);
