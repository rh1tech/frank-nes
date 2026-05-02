/*
 * USB HID host driver
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 *
 * Fork maintained as part of FRANK NES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 */

#ifndef USBHID_H
#define USBHID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// USB HID State
//--------------------------------------------------------------------

// USB keyboard state accessible from wrapper
typedef struct {
    uint8_t keycode[6];     // Currently pressed keys (HID keycodes)
    uint8_t modifier;       // Modifier keys (shift, ctrl, alt, etc.)
    int has_key;            // Non-zero if a key event is pending
} usbhid_keyboard_state_t;

// USB mouse state
typedef struct {
    int16_t dx;             // Accumulated X movement
    int16_t dy;             // Accumulated Y movement
    int8_t wheel;           // Wheel movement
    uint8_t buttons;        // Button state (bit 0=left, 1=right, 2=middle)
    int has_motion;         // Non-zero if motion/button change occurred
} usbhid_mouse_state_t;

// USB gamepad state
typedef struct {
    int8_t axis_x;          // Left stick X: -127 to 127
    int8_t axis_y;          // Left stick Y: -127 to 127
    uint8_t dpad;           // D-pad: bit 0=up, 1=down, 2=left, 3=right
    uint16_t buttons;       // Buttons: bit 0=A, 1=B, 2=X, 3=Y, 4=L, 5=R, 6=Start, 7=Select
    int connected;          // Non-zero if gamepad is connected
} usbhid_gamepad_state_t;

//--------------------------------------------------------------------
// API Functions
//--------------------------------------------------------------------

void usbhid_init(void);
void usbhid_task(void);

int usbhid_keyboard_connected(void);
int usbhid_mouse_connected(void);

void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state);
void usbhid_get_mouse_state(usbhid_mouse_state_t *state);
int usbhid_get_key_action(uint8_t *keycode, int *down);

/**
 * Get keyboard state as bitmask (same format as PS/2 keyboard)
 * Uses same KBD_STATE_* constants from ps2kbd_wrapper.h
 */
uint16_t usbhid_get_kbd_state(void);

/** Non-zero while Ctrl+Alt+Del are all currently held on a USB keyboard.
 *  Del is not exposed in the KBD_STATE bitmask; this chord is only used
 *  to soft-reset a running ROM. */
int usbhid_ctrl_alt_del_pressed(void);

/** Pop the next queued raw ASCII character from USB keyboard input.
 *  Returns a-z / A-Z (shift-aware), 0-9, space, or '\b' for Backspace.
 *  Returns -1 when empty. Mirrors ps2kbd_get_raw_char(). */
int usbhid_get_raw_char(void);

/** Check if a USB gamepad is connected (any slot) */
int usbhid_gamepad_connected(void);

/** Get combined gamepad state (all connected gamepads merged) */
void usbhid_get_gamepad_state(usbhid_gamepad_state_t *state);

/** Check if USB gamepad at specific slot (0 or 1) is connected */
int usbhid_gamepad_connected_idx(int idx);

/** Get gamepad state for specific slot (0 or 1) */
void usbhid_get_gamepad_state_idx(int idx, usbhid_gamepad_state_t *state);

//--------------------------------------------------------------------
// Menu-A/B calibration wizard support
//
// Rationale: in-game input is remappable via the settings menu, but the
// menus themselves ask the user to press "A" and "B". If the fallback
// layout doesn't match the physical pad, those menu prompts are wrong.
// The wizard below only teaches menu A/B — in-game decoding is
// untouched.
//--------------------------------------------------------------------

typedef enum {
    USBHID_GP_SRC_NONE = 0,
    USBHID_GP_SRC_HID,
    USBHID_GP_SRC_XINPUT,
} usbhid_gp_src_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    usbhid_gp_src_t source;
    uint8_t report_len;         // Effective length of raw report bytes
    uint8_t baseline[32];       // Resting state (first seen report)
    uint8_t raw[32];            // Most recent report
} usbhid_gamepad_raw_info_t;

/** Non-zero if a newly connected pad in this slot has no compiled-in or
 *  saved menu-A/B profile and needs the wizard. */
int usbhid_gamepad_needs_calibration(int idx);

/** Clear the needs_calibration flag (wizard finished or user cancelled).
 *  This also records the slot's VID/PID in the session-seen table so the
 *  wizard won't re-prompt if the pad is unplugged and replugged during
 *  the same power-on session. Rebooting re-arms the wizard. */
void usbhid_gamepad_clear_calibration(int idx);

/** Copy raw report info out for the wizard. Returns 0 if no raw report
 *  has been captured yet (pad just mounted, not sampled). */
int usbhid_gamepad_get_raw_info(int idx, usbhid_gamepad_raw_info_t *out);

/** Scan for the first report byte whose current value differs from the
 *  resting baseline. Returns 1 with *byte_idx / *mask filled in when
 *  exactly one bit changed; returns 0 otherwise (no change, or multi-bit
 *  change — wizard should wait until user releases and re-presses). */
int usbhid_gamepad_find_pressed_bit(int idx, uint8_t *byte_idx, uint8_t *mask);

/** Record a learned menu-A/B assignment on the given slot. Called by the
 *  wizard once the user has confirmed both buttons. Does NOT persist. */
void usbhid_gamepad_set_menu_ab(int idx,
                                uint8_t a_byte, uint8_t a_mask,
                                uint8_t b_byte, uint8_t b_mask);

/** Register a learned menu-A/B profile for a VID/PID at boot (called
 *  once per gamepads/VID_PID.txt file loaded from SD). Future mounts of
 *  this pad will use this profile automatically. Returns 0 on success,
 *  -1 if the internal table is full. */
int usbhid_learned_ab_add(usbhid_gp_src_t source,
                          uint16_t vid, uint16_t pid,
                          uint8_t a_byte, uint8_t a_mask,
                          uint8_t b_byte, uint8_t b_mask);

/** Returns 1 if either of this slot's menu-A/B bits is currently set in
 *  the latest raw report, with out_a / out_b each set to 0 or 1. */
int usbhid_gamepad_get_menu_ab(int idx, int *out_a, int *out_b);

/** Drop every learned menu-A/B profile (in-memory table) and re-seed
 *  any connected slots from the compiled-in maps. Slots whose pads have
 *  no compiled map get needs_calibration=1 so the wizard re-prompts. */
void usbhid_learned_ab_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* USBHID_H */
