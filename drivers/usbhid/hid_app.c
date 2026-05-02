/*
 * USB HID host driver
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 *
 * Fork maintained as part of FRANK NES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/frank-nes
 */

#include "tusb.h"
#include "usbhid.h"
#include "xinput_host.h"
#include "hid_rip.h"
#include <stdio.h>
#include <string.h>

// Only compile if USB Host is enabled
#if CFG_TUH_ENABLED

//--------------------------------------------------------------------
// Internal state
//--------------------------------------------------------------------

#define MAX_REPORT 4

// Per-device, per-instance HID info for generic report parsing
static struct {
    uint8_t report_count;
    tuh_hid_report_info_t report_info[MAX_REPORT];
} hid_info[CFG_TUH_HID];

// Previous keyboard report for detecting key changes
static hid_keyboard_report_t prev_kbd_report = { 0, 0, {0} };

// Previous mouse report for detecting button changes
static hid_mouse_report_t prev_mouse_report = { 0 };

// Accumulated mouse movement
static volatile int16_t cumulative_dx = 0;
static volatile int16_t cumulative_dy = 0;
static volatile int8_t cumulative_wheel = 0;
static volatile uint8_t current_buttons = 0;
static volatile int mouse_has_motion = 0;

//--------------------------------------------------------------------
// Gamepad button map
//
// Different HID gamepads lay out their 8+-byte reports differently.
// Each entry picks a byte index + bitmask for every logical face
// button. D-pad decoding supports three modes:
//   DPAD_AXIS: two analog bytes, 0x00=min 0x7F=centre 0xFF=max
//   DPAD_HAT:  single byte low-nibble hat (0=U, 2=R, 4=D, 6=L, 8=neutral;
//              1/3/5/7 are diagonals)
//   DPAD_BITS: four bits in byte 0 (XInput wButtons low) — UP/DOWN/LEFT/RIGHT
//
// Canonical output bits (what main_pico.c expects):
//   0x0001=A 0x0002=B 0x0004=X 0x0008=Y
//   0x0010=L 0x0020=R 0x0040=Start 0x0080=Select
//--------------------------------------------------------------------

typedef enum {
    DPAD_AXIS = 0,   // analog axes on dpad_x, dpad_y
    DPAD_HAT,        // 8-way hat in byte dpad_x low nibble
    DPAD_BITS,       // UP=0x01 DOWN=0x02 LEFT=0x04 RIGHT=0x08 in byte 0
} dpad_mode_t;

typedef struct {
    uint8_t byte;
    uint8_t mask;
} btn_bit_t;

typedef struct {
    uint16_t vid;
    uint16_t pid;
    dpad_mode_t dpad_mode;
    uint8_t  dpad_x;        // AXIS: X axis byte. HAT: hat byte. BITS: unused.
    uint8_t  dpad_y;        // AXIS: Y axis byte. Otherwise unused.
    btn_bit_t a;
    btn_bit_t b;
    btn_bit_t x;
    btn_bit_t y;
    btn_bit_t l;
    btn_bit_t r;
    btn_bit_t start;
    btn_bit_t select;
} gamepad_map_t;

// Known HID gamepad maps (ported from murmsnes/scripts/gen_gamepad_maps.py output).
static const gamepad_map_t known_hid_maps[] = {
    // gamepad_0079_0006
    {
        .vid = 0x0079, .pid = 0x0006,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_046D_C219
    {
        .vid = 0x046D, .pid = 0xC219,
        .dpad_mode = DPAD_HAT, .dpad_x = 5, .dpad_y = 0,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_081F_E401 (common cheap SNES-clone pad)
    {
        .vid = 0x081F, .pid = 0xE401,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x20 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x10 },
        .y      = { .byte = 5, .mask = 0x80 },
        .l      = { .byte = 6, .mask = 0x01 },
        .r      = { .byte = 6, .mask = 0x02 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_11FF_3331
    {
        .vid = 0x11FF, .pid = 0x3331,
        .dpad_mode = DPAD_AXIS, .dpad_x = 0, .dpad_y = 1,
        .a      = { .byte = 5, .mask = 0x80 },
        .b      = { .byte = 5, .mask = 0x40 },
        .x      = { .byte = 5, .mask = 0x20 },
        .y      = { .byte = 5, .mask = 0x10 },
        .l      = { .byte = 6, .mask = 0x04 },
        .r      = { .byte = 6, .mask = 0x08 },
        .start  = { .byte = 6, .mask = 0x20 },
        .select = { .byte = 6, .mask = 0x10 },
    },
    // gamepad_2563_0575
    {
        .vid = 0x2563, .pid = 0x0575,
        .dpad_mode = DPAD_HAT, .dpad_x = 2, .dpad_y = 0,
        .a      = { .byte = 0, .mask = 0x04 },
        .b      = { .byte = 0, .mask = 0x02 },
        .x      = { .byte = 0, .mask = 0x08 },
        .y      = { .byte = 0, .mask = 0x01 },
        .l      = { .byte = 0, .mask = 0x10 },
        .r      = { .byte = 0, .mask = 0x20 },
        .start  = { .byte = 1, .mask = 0x02 },
        .select = { .byte = 1, .mask = 0x01 },
    },
    // gamepad_FEED_2320
    {
        .vid = 0xFEED, .pid = 0x2320,
        .dpad_mode = DPAD_HAT, .dpad_x = 5, .dpad_y = 0,
        .a      = { .byte = 6, .mask = 0x01 },
        .b      = { .byte = 6, .mask = 0x02 },
        .x      = { .byte = 6, .mask = 0x08 },
        .y      = { .byte = 6, .mask = 0x04 },
        .l      = { .byte = 6, .mask = 0x10 },
        .r      = { .byte = 6, .mask = 0x20 },
        .start  = { .byte = 7, .mask = 0x08 },
        .select = { .byte = 7, .mask = 0x04 },
    },
};

// Fallback layout for unknown HID pads — same bits as the 0x081F/0xE401
// SNES-clone layout, since most cheap pads look like that.
static const gamepad_map_t fallback_hid_map = {
    .vid = 0, .pid = 0,
    .dpad_mode = DPAD_AXIS, .dpad_x = 3, .dpad_y = 4,
    .a      = { .byte = 5, .mask = 0x20 },
    .b      = { .byte = 5, .mask = 0x40 },
    .x      = { .byte = 5, .mask = 0x10 },
    .y      = { .byte = 5, .mask = 0x80 },
    .l      = { .byte = 6, .mask = 0x01 },
    .r      = { .byte = 6, .mask = 0x02 },
    .start  = { .byte = 6, .mask = 0x20 },
    .select = { .byte = 6, .mask = 0x10 },
};

// XInput synthetic frame layout — see tuh_xinput_report_received_cb() below.
//   byte[0] = wButtons low  (DPAD + START/BACK + LS/RS)
//   byte[1] = wButtons high (LB=0x01 RB=0x02 A=0x10 B=0x20 X=0x40 Y=0x80)
static const gamepad_map_t xinput_default_map = {
    .vid = 0, .pid = 0,
    .dpad_mode = DPAD_BITS, .dpad_x = 0xFF, .dpad_y = 0xFF,
    .a      = { .byte = 1, .mask = 0x10 },
    .b      = { .byte = 1, .mask = 0x20 },
    .x      = { .byte = 1, .mask = 0x40 },
    .y      = { .byte = 1, .mask = 0x80 },
    .l      = { .byte = 1, .mask = 0x01 },
    .r      = { .byte = 1, .mask = 0x02 },
    .start  = { .byte = 0, .mask = 0x10 },
    .select = { .byte = 0, .mask = 0x20 },
};

static const gamepad_map_t known_xinput_maps[] = {
    // gamepad_045E_028E (Xbox 360 wired controller).
    // Microsoft uses A=bit4/B=bit5/X=bit6/Y=bit7 in wButtons-high after
    // XInput button normalisation, which after synth[1] becomes the
    // bitmask below.
    {
        .vid = 0x045E, .pid = 0x028E,
        .dpad_mode = DPAD_BITS, .dpad_x = 0xFF, .dpad_y = 0xFF,
        .a      = { .byte = 1, .mask = 0x10 },
        .b      = { .byte = 1, .mask = 0x20 },
        .x      = { .byte = 1, .mask = 0x40 },
        .y      = { .byte = 1, .mask = 0x80 },
        .l      = { .byte = 1, .mask = 0x01 },
        .r      = { .byte = 1, .mask = 0x02 },
        .start  = { .byte = 0, .mask = 0x10 },
        .select = { .byte = 0, .mask = 0x20 },
    },
    // gamepad_046D_C21F (Logitech F710 in XInput mode)
    {
        .vid = 0x046D, .pid = 0xC21F,
        .dpad_mode = DPAD_BITS, .dpad_x = 0xFF, .dpad_y = 0xFF,
        .a      = { .byte = 1, .mask = 0x10 },
        .b      = { .byte = 1, .mask = 0x20 },
        .x      = { .byte = 1, .mask = 0x40 },
        .y      = { .byte = 1, .mask = 0x80 },
        .l      = { .byte = 1, .mask = 0x01 },
        .r      = { .byte = 1, .mask = 0x02 },
        .start  = { .byte = 0, .mask = 0x10 },
        .select = { .byte = 0, .mask = 0x20 },
    },
};
static const size_t known_xinput_map_count = sizeof(known_xinput_maps) / sizeof(known_xinput_maps[0]);

static const gamepad_map_t *find_hid_map(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < sizeof(known_hid_maps) / sizeof(known_hid_maps[0]); i++) {
        if (known_hid_maps[i].vid == vid && known_hid_maps[i].pid == pid)
            return &known_hid_maps[i];
    }
    return &fallback_hid_map;
}

static const gamepad_map_t *find_xinput_map(uint16_t vid, uint16_t pid) {
    for (size_t i = 0; i < known_xinput_map_count; i++) {
        if (known_xinput_maps[i].vid == vid && known_xinput_maps[i].pid == pid)
            return &known_xinput_maps[i];
    }
    return &xinput_default_map;
}

// Gamepad state — two slots for two USB gamepads
#define MAX_GAMEPADS 2

// Max report length we track for calibration (covers every known pad).
#define GP_RAW_MAX_LEN 32

typedef enum {
    GP_SRC_NONE = 0,
    GP_SRC_HID,
    GP_SRC_XINPUT,
} gp_source_t;

typedef struct {
    volatile int8_t axis_x;
    volatile int8_t axis_y;
    volatile uint8_t dpad;
    volatile uint16_t buttons;
    volatile int connected;
    uint8_t dev_addr;
    uint8_t instance;
    gp_source_t source;
    const gamepad_map_t *map;

    // Calibration / learn-mode support.
    uint16_t vid;
    uint16_t pid;
    volatile int needs_calibration;
    uint8_t raw_report[GP_RAW_MAX_LEN];
    uint8_t raw_len;
    uint8_t raw_baseline[GP_RAW_MAX_LEN];
    int     raw_baseline_valid;
    btn_bit_t menu_a;
    btn_bit_t menu_b;
    uint8_t   ab_valid;     // bit0=A taught, bit1=B taught
} gamepad_slot_t;

static gamepad_slot_t gamepad_slots[MAX_GAMEPADS] = {0};

// Runtime table of learned menu-A/B profiles, populated at boot by
// usbhid_learned_ab_add() (called for each /nes/gamepads/*.txt file).
typedef struct {
    uint16_t vid;
    uint16_t pid;
    gp_source_t source;
    btn_bit_t a;
    btn_bit_t b;
} learned_ab_t;

#define MAX_LEARNED_AB 16
static learned_ab_t learned_ab_table[MAX_LEARNED_AB];
static size_t learned_ab_count = 0;

// Session-seen table: (vid, pid, source) tuples that have already been
// through the calibration wizard since power-on.
#define MAX_SESSION_SEEN 8
typedef struct {
    uint16_t vid;
    uint16_t pid;
    gp_source_t source;
} session_seen_t;
static session_seen_t session_seen[MAX_SESSION_SEEN];
static size_t session_seen_count = 0;

static bool session_seen_contains(uint16_t vid, uint16_t pid, gp_source_t src) {
    for (size_t i = 0; i < session_seen_count; i++) {
        if (session_seen[i].vid == vid && session_seen[i].pid == pid &&
            session_seen[i].source == src)
            return true;
    }
    return false;
}

static void session_seen_add(uint16_t vid, uint16_t pid, gp_source_t src) {
    if (session_seen_contains(vid, pid, src)) return;
    if (session_seen_count >= MAX_SESSION_SEEN) return;
    session_seen[session_seen_count].vid = vid;
    session_seen[session_seen_count].pid = pid;
    session_seen[session_seen_count].source = src;
    session_seen_count++;
}

static const learned_ab_t *find_learned_ab(uint16_t vid, uint16_t pid, gp_source_t src) {
    for (size_t i = 0; i < learned_ab_count; i++) {
        if (learned_ab_table[i].vid == vid && learned_ab_table[i].pid == pid &&
            learned_ab_table[i].source == src)
            return &learned_ab_table[i];
    }
    return NULL;
}

static void seed_menu_ab(int slot, uint16_t vid, uint16_t pid,
                         gp_source_t src, const gamepad_map_t *compiled_map,
                         const gamepad_map_t *fallback) {
    const learned_ab_t *p = find_learned_ab(vid, pid, src);
    if (p) {
        gamepad_slots[slot].menu_a = p->a;
        gamepad_slots[slot].menu_b = p->b;
        gamepad_slots[slot].ab_valid = 0x03;
        return;
    }
    if (compiled_map && compiled_map != fallback) {
        gamepad_slots[slot].menu_a = compiled_map->a;
        gamepad_slots[slot].menu_b = compiled_map->b;
        gamepad_slots[slot].ab_valid = 0x03;
    }
}

static int find_or_alloc_gamepad_slot(uint8_t dev_addr, uint8_t inst, gp_source_t src) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected && gamepad_slots[i].dev_addr == dev_addr &&
            gamepad_slots[i].instance == inst && gamepad_slots[i].source == src)
            return i;
    }
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (!gamepad_slots[i].connected) {
            gamepad_slots[i].dev_addr = dev_addr;
            gamepad_slots[i].instance = inst;
            gamepad_slots[i].source = src;
            return i;
        }
    }
    return -1;
}

static int find_gamepad_slot_by_dev(uint8_t dev_addr, gp_source_t src) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected && gamepad_slots[i].dev_addr == dev_addr &&
            gamepad_slots[i].source == src)
            return i;
    }
    return -1;
}

// Device connection state
static volatile int keyboard_connected = 0;
static volatile int mouse_connected = 0;

// Key action queue (for detecting press/release)
#define KEY_ACTION_QUEUE_SIZE 32
typedef struct {
    uint8_t keycode;
    int down;
} key_action_t;

static key_action_t key_action_queue[KEY_ACTION_QUEUE_SIZE];
static volatile int key_action_head = 0;
static volatile int key_action_tail = 0;

// Raw character queue for text input (search dialog)
#define USB_RAW_CHAR_QUEUE_SIZE 16
static uint8_t usb_raw_char_queue[USB_RAW_CHAR_QUEUE_SIZE];
static volatile int usb_raw_char_head = 0;
static volatile int usb_raw_char_tail = 0;

static void usb_raw_char_push(uint8_t ch) {
    int next = (usb_raw_char_head + 1) & (USB_RAW_CHAR_QUEUE_SIZE - 1);
    if (next != usb_raw_char_tail) {
        usb_raw_char_queue[usb_raw_char_head] = ch;
        usb_raw_char_head = next;
    }
}

static uint8_t usb_hid_to_ascii(uint8_t code, uint8_t modifier) {
    int shift = (modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;
    if (code >= 0x04 && code <= 0x1D)
        return shift ? ('A' + (code - 0x04)) : ('a' + (code - 0x04));
    if (code >= 0x1E && code <= 0x26)
        return '1' + (code - 0x1E);
    if (code == 0x27) return '0';
    if (code == 0x2C) return ' ';
    if (code == 0x2A) return '\b';
    return 0;
}

//--------------------------------------------------------------------
// Internal functions
//--------------------------------------------------------------------

static void queue_key_action(uint8_t keycode, int down) {
    int next_head = (key_action_head + 1) % KEY_ACTION_QUEUE_SIZE;
    if (next_head != key_action_tail) {
        key_action_queue[key_action_head].keycode = keycode;
        key_action_queue[key_action_head].down = down;
        key_action_head = next_head;
    }
}

static int find_keycode_in_report(hid_keyboard_report_t const *report, uint8_t keycode) {
    for (int i = 0; i < 6; i++) {
        if (report->keycode[i] == keycode) return 1;
    }
    return 0;
}

// Forward declarations
static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report);
static void process_mouse_report(hid_mouse_report_t const *report);
static void process_gamepad_report(int slot, uint8_t const *report, uint16_t len);
static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len);

//--------------------------------------------------------------------
// Process keyboard report
//--------------------------------------------------------------------

static void process_kbd_report(hid_keyboard_report_t const *report, hid_keyboard_report_t const *prev_report) {
    uint8_t released_mods = prev_report->modifier & ~(report->modifier);
    uint8_t pressed_mods = report->modifier & ~(prev_report->modifier);

    if (released_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
        queue_key_action(0xE1, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
        queue_key_action(0xE1, 1);
    if (released_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        queue_key_action(0xE0, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        queue_key_action(0xE0, 1);
    if (released_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        queue_key_action(0xE2, 0);
    if (pressed_mods & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        queue_key_action(0xE2, 1);

    for (int i = 0; i < 6; i++) {
        uint8_t keycode = prev_report->keycode[i];
        if (keycode && !find_keycode_in_report(report, keycode))
            queue_key_action(keycode, 0);
    }
    for (int i = 0; i < 6; i++) {
        uint8_t keycode = report->keycode[i];
        if (keycode && !find_keycode_in_report(prev_report, keycode)) {
            queue_key_action(keycode, 1);
            uint8_t ch = usb_hid_to_ascii(keycode, report->modifier);
            if (ch) usb_raw_char_push(ch);
        }
    }
}

//--------------------------------------------------------------------
// Process mouse report
//--------------------------------------------------------------------

static void process_mouse_report(hid_mouse_report_t const *report) {
    // Y axis inverted for games that expect positive = forward
    cumulative_dx += report->x;
    cumulative_dy += -report->y;
    cumulative_wheel += report->wheel;
    current_buttons = report->buttons & 0x07;
    if (report->x != 0 || report->y != 0) mouse_has_motion = 1;
    prev_mouse_report = *report;
}

//--------------------------------------------------------------------
// Process gamepad report (per-slot)
//--------------------------------------------------------------------

static inline uint16_t eval_btn(const btn_bit_t *b, uint8_t const *report, uint16_t len, uint16_t out_mask) {
    if (b->byte >= len) return 0;
    return (report[b->byte] & b->mask) ? out_mask : 0;
}

static void process_gamepad_report(int slot, uint8_t const *report, uint16_t len) {
    if (slot < 0 || slot >= MAX_GAMEPADS || report == NULL || len < 2) return;

    gamepad_slot_t *gp = &gamepad_slots[slot];
    const gamepad_map_t *m = gp->map ? gp->map : &fallback_hid_map;

    // Snapshot raw report for the calibration wizard.
    uint8_t copy_len = len > GP_RAW_MAX_LEN ? GP_RAW_MAX_LEN : (uint8_t)len;
    memcpy(gp->raw_report, report, copy_len);
    gp->raw_len = copy_len;
    if (!gp->raw_baseline_valid) {
        memcpy(gp->raw_baseline, report, copy_len);
        if (copy_len < GP_RAW_MAX_LEN)
            memset(gp->raw_baseline + copy_len, 0, GP_RAW_MAX_LEN - copy_len);
        gp->raw_baseline_valid = 1;
    }

    // D-pad decoding depends on the map's dpad_mode.
    uint8_t dpad = 0;
    switch (m->dpad_mode) {
        case DPAD_AXIS:
            if (m->dpad_x < len && m->dpad_y < len) {
                if (report[m->dpad_x] < 0x40) dpad |= 0x04; // Left
                if (report[m->dpad_x] > 0xC0) dpad |= 0x08; // Right
                if (report[m->dpad_y] < 0x40) dpad |= 0x01; // Up
                if (report[m->dpad_y] > 0xC0) dpad |= 0x02; // Down
            }
            break;
        case DPAD_HAT:
            if (m->dpad_x < len) {
                // 8-way hat in the low nibble: 0=U, 1=UR, 2=R, 3=DR,
                // 4=D, 5=DL, 6=L, 7=UL, 8=neutral.
                uint8_t h = report[m->dpad_x] & 0x0F;
                switch (h) {
                    case 0: dpad = 0x01; break;                 // U
                    case 1: dpad = 0x01 | 0x08; break;          // U+R
                    case 2: dpad = 0x08; break;                 // R
                    case 3: dpad = 0x02 | 0x08; break;          // D+R
                    case 4: dpad = 0x02; break;                 // D
                    case 5: dpad = 0x02 | 0x04; break;          // D+L
                    case 6: dpad = 0x04; break;                 // L
                    case 7: dpad = 0x01 | 0x04; break;          // U+L
                    default: dpad = 0; break;                   // neutral/invalid
                }
            }
            break;
        case DPAD_BITS:
            // XInput wButtons low byte: UP=0x01 DOWN=0x02 LEFT=0x04 RIGHT=0x08
            if (len >= 1) {
                if (report[0] & 0x01) dpad |= 0x01;
                if (report[0] & 0x02) dpad |= 0x02;
                if (report[0] & 0x04) dpad |= 0x04;
                if (report[0] & 0x08) dpad |= 0x08;
            }
            break;
    }
    gp->dpad = dpad;

    uint16_t buttons = 0;
    buttons |= eval_btn(&m->a,      report, len, 0x0001);
    buttons |= eval_btn(&m->b,      report, len, 0x0002);
    buttons |= eval_btn(&m->x,      report, len, 0x0004);
    buttons |= eval_btn(&m->y,      report, len, 0x0008);
    buttons |= eval_btn(&m->l,      report, len, 0x0010);
    buttons |= eval_btn(&m->r,      report, len, 0x0020);
    buttons |= eval_btn(&m->start,  report, len, 0x0040);
    buttons |= eval_btn(&m->select, report, len, 0x0080);
    gp->buttons = buttons;
}

//--------------------------------------------------------------------
// Process generic HID report
//--------------------------------------------------------------------

static void process_generic_report(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)dev_addr;
    if (instance >= CFG_TUH_HID || report == NULL || len == 0) return;

    uint8_t const rpt_count = hid_info[instance].report_count;
    if (rpt_count == 0 || rpt_count > MAX_REPORT) return;

    tuh_hid_report_info_t *rpt_info_arr = hid_info[instance].report_info;
    tuh_hid_report_info_t *rpt_info = NULL;

    if (rpt_count == 1 && rpt_info_arr[0].report_id == 0) {
        rpt_info = &rpt_info_arr[0];
    } else {
        uint8_t const rpt_id = report[0];
        for (uint8_t i = 0; i < rpt_count && i < MAX_REPORT; i++) {
            if (rpt_id == rpt_info_arr[i].report_id) {
                rpt_info = &rpt_info_arr[i];
                break;
            }
        }
        report++;
        len--;
    }

    if (!rpt_info) return;

    if (rpt_info->usage_page == HID_USAGE_PAGE_DESKTOP) {
        switch (rpt_info->usage) {
            case HID_USAGE_DESKTOP_KEYBOARD:
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
                break;
            case HID_USAGE_DESKTOP_MOUSE:
                if (len >= 3) {
                    hid_mouse_report_t padded = {0};
                    memcpy(&padded, report,
                           len < sizeof(padded) ? len : sizeof(padded));
                    process_mouse_report(&padded);
                }
                break;
            case HID_USAGE_DESKTOP_GAMEPAD:
            case HID_USAGE_DESKTOP_JOYSTICK: {
                int slot = find_or_alloc_gamepad_slot(dev_addr, instance, GP_SRC_HID);
                if (slot >= 0) {
                    gamepad_slots[slot].connected = 1;
                    process_gamepad_report(slot, report, len);
                }
                break;
            }
            default:
                break;
        }
    }
}

//--------------------------------------------------------------------
// XInput class driver registration.
//
// TinyUSB looks up extra host class drivers via usbh_app_driver_get_cb().
// Xbox 360 / Xbox One / XboxOG controllers do not expose a HID interface,
// so without this callback they would never enumerate.
//--------------------------------------------------------------------

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count) {
    *driver_count = 1;
    return &usbh_xinput_driver;
}

//--------------------------------------------------------------------
// XInput callbacks
//
// Build a stable 8-byte synthetic report from xinput_gamepad_t so the
// rest of the pipeline (process_gamepad_report → per-device button
// map) works the same way as HID pads.
//
//   byte[0] = wButtons low  (DPAD_UP/DOWN/LEFT/RIGHT + START/BACK + LS/RS)
//   byte[1] = wButtons high (LB=0x01 RB=0x02 GUIDE=0x04 SHARE=0x08
//                            A=0x10 B=0x20 X=0x40 Y=0x80)
//   byte[2] = bLeftTrigger  (quantised on/off)
//   byte[3] = bRightTrigger (quantised on/off)
//   byte[4..7] = stick axes quantised to three bins
//
// Stick quantisation matters: at rest an Xbox pad drifts ~±2000 LSB;
// a raw high-byte snapshot would change 60x/sec and break debouncing.
//--------------------------------------------------------------------

#define USBHID_XINPUT_REPORT_LEN 8

static inline uint8_t xinput_axis_quantise(int16_t v) {
    const int16_t threshold = 0x2000;   // ~25% deflection
    if (v >  threshold) return 0x7F;
    if (v < -threshold) return 0x80;
    return 0x00;
}

static inline uint8_t xinput_trigger_quantise(uint8_t v) {
    return v > 0x20 ? 0xFF : 0x00;
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xid_itf) {
    printf("XInput mounted: dev_addr=%u inst=%u type=%u\n",
           dev_addr, instance, (unsigned)xid_itf->type);

    int slot = find_or_alloc_gamepad_slot(dev_addr, instance, GP_SRC_XINPUT);
    if (slot < 0) {
        printf("  -> slots full, ignored\n");
        tuh_xinput_receive_report(dev_addr, instance);
        return;
    }
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);
    gamepad_slots[slot].connected = 1;
    gamepad_slots[slot].vid = vid;
    gamepad_slots[slot].pid = pid;
    gamepad_slots[slot].map = find_xinput_map(vid, pid);
    gamepad_slots[slot].buttons = 0;
    gamepad_slots[slot].dpad = 0;
    gamepad_slots[slot].raw_len = 0;
    gamepad_slots[slot].raw_baseline_valid = 0;
    gamepad_slots[slot].ab_valid = 0;
    seed_menu_ab(slot, vid, pid, GP_SRC_XINPUT,
                 gamepad_slots[slot].map, &xinput_default_map);
    bool have_sd_profile = find_learned_ab(vid, pid, GP_SRC_XINPUT) != NULL;
    gamepad_slots[slot].needs_calibration =
        (have_sd_profile || session_seen_contains(vid, pid, GP_SRC_XINPUT)) ? 0 : 1;
    printf("  -> VID=0x%04X PID=0x%04X (%s)\n", vid, pid,
           have_sd_profile ? "sd-profile" :
           gamepad_slots[slot].needs_calibration ? "wizard-pending" : "seen-this-session");

    // Light the player-1 quadrant LED on 360 pads; XBone ignores this.
    tuh_xinput_set_led(dev_addr, instance, 1, true);
    tuh_xinput_receive_report(dev_addr, instance);
    printf("  -> XINPUT gamepad assigned to slot %d\n", slot);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected &&
            gamepad_slots[i].source == GP_SRC_XINPUT &&
            gamepad_slots[i].dev_addr == dev_addr &&
            gamepad_slots[i].instance == instance) {
            printf("XInput unmounted: gamepad slot %d disconnected\n", i);
            gamepad_slots[i].connected = 0;
            gamepad_slots[i].buttons = 0;
            gamepad_slots[i].dpad = 0;
            gamepad_slots[i].axis_x = 0;
            gamepad_slots[i].axis_y = 0;
            gamepad_slots[i].dev_addr = 0;
            gamepad_slots[i].source = GP_SRC_NONE;
            gamepad_slots[i].map = NULL;
        }
    }
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                   xinputh_interface_t const *xid_itf,
                                   uint16_t len) {
    (void)len;
    int slot = find_gamepad_slot_by_dev(dev_addr, GP_SRC_XINPUT);
    if (slot >= 0 && xid_itf) {
        const xinput_gamepad_t *p = &xid_itf->pad;
        uint8_t synth[USBHID_XINPUT_REPORT_LEN];
        synth[0] = (uint8_t)(p->wButtons & 0xFF);
        synth[1] = (uint8_t)((p->wButtons >> 8) & 0xFF);
        synth[2] = xinput_trigger_quantise(p->bLeftTrigger);
        synth[3] = xinput_trigger_quantise(p->bRightTrigger);
        synth[4] = xinput_axis_quantise(p->sThumbLX);
        synth[5] = xinput_axis_quantise(p->sThumbLY);
        synth[6] = xinput_axis_quantise(p->sThumbRX);
        synth[7] = xinput_axis_quantise(p->sThumbRY);
        process_gamepad_report(slot, synth, sizeof(synth));
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// TinyUSB HID Callbacks
//--------------------------------------------------------------------

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *desc_report, uint16_t desc_len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    printf("USB HID mounted: dev_addr=%d, instance=%d, protocol=%d\n", dev_addr, instance, itf_protocol);

    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 1;
        printf("  -> Keyboard detected\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 1;
        printf("  -> Mouse detected\n");
    }

    if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        printf("  -> Generic HID device (protocol=NONE)\n");
        if (instance >= CFG_TUH_HID) {
            printf("  -> Instance %d out of range (max %d)\n", instance, CFG_TUH_HID);
            tuh_hid_receive_report(dev_addr, instance);
            return;
        }

        hid_info[instance].report_count = tuh_hid_parse_report_descriptor(
            hid_info[instance].report_info, MAX_REPORT, desc_report, desc_len);
        printf("  -> Parsed %d reports from descriptor (stock tusb)\n",
               hid_info[instance].report_count);

        // Fallback: some pads ship descriptors tinyusb trips on.
        // Re-parse via the fruit-bat/pico-hid-host RIP.
        if (hid_info[instance].report_count == 0) {
            tuh_hid_report_info_plus_t rip_info[MAX_REPORT];
            uint8_t n = tuh_hid_parse_report_descriptor_plus(
                rip_info, MAX_REPORT, desc_report, desc_len);
            if (n > 0 && n <= MAX_REPORT) {
                for (uint8_t i = 0; i < n; i++) {
                    hid_info[instance].report_info[i].report_id  = rip_info[i].report_id;
                    hid_info[instance].report_info[i].usage      = (uint8_t)rip_info[i].usage;
                    hid_info[instance].report_info[i].usage_page = rip_info[i].usage_page;
                }
                hid_info[instance].report_count = n;
                printf("  -> RIP fallback recovered %d reports\n", n);
            }
        }

        bool is_gamepad = false;
        bool is_mouse = false;
        bool is_keyboard = false;
        for (uint8_t i = 0; i < hid_info[instance].report_count && i < MAX_REPORT; i++) {
            printf("  -> Report %d: usage_page=0x%02X, usage=0x%02X\n",
                   i, hid_info[instance].report_info[i].usage_page,
                   hid_info[instance].report_info[i].usage);
            if (hid_info[instance].report_info[i].usage_page == HID_USAGE_PAGE_DESKTOP) {
                uint8_t usage = hid_info[instance].report_info[i].usage;
                if (usage == HID_USAGE_DESKTOP_GAMEPAD || usage == HID_USAGE_DESKTOP_JOYSTICK)
                    is_gamepad = true;
                else if (usage == HID_USAGE_DESKTOP_MOUSE)
                    is_mouse = true;
                else if (usage == HID_USAGE_DESKTOP_KEYBOARD)
                    is_keyboard = true;
            }
        }

        if (is_mouse) {
            mouse_connected = 1;
            printf("  -> MOUSE detected via descriptor\n");
        }
        if (is_keyboard) {
            keyboard_connected = 1;
            printf("  -> KEYBOARD detected via descriptor\n");
        }

        if (!is_gamepad && !is_mouse && !is_keyboard &&
            hid_info[instance].report_count == 0) {
            printf("  -> No reports parsed, assuming gamepad\n");
            is_gamepad = true;
        }

        if (is_gamepad) {
            int slot = find_or_alloc_gamepad_slot(dev_addr, instance, GP_SRC_HID);
            if (slot >= 0) {
                uint16_t vid = 0, pid = 0;
                tuh_vid_pid_get(dev_addr, &vid, &pid);
                gamepad_slots[slot].connected = 1;
                gamepad_slots[slot].vid = vid;
                gamepad_slots[slot].pid = pid;
                gamepad_slots[slot].map = find_hid_map(vid, pid);
                gamepad_slots[slot].raw_len = 0;
                gamepad_slots[slot].raw_baseline_valid = 0;
                gamepad_slots[slot].ab_valid = 0;
                seed_menu_ab(slot, vid, pid, GP_SRC_HID,
                             gamepad_slots[slot].map, &fallback_hid_map);
                bool have_sd_profile = find_learned_ab(vid, pid, GP_SRC_HID) != NULL;
                gamepad_slots[slot].needs_calibration =
                    (have_sd_profile || session_seen_contains(vid, pid, GP_SRC_HID))
                        ? 0 : 1;
                printf("  -> GAMEPAD slot %d VID=0x%04X PID=0x%04X (%s)\n",
                       slot, vid, pid,
                       have_sd_profile ? "sd-profile" :
                       gamepad_slots[slot].needs_calibration ? "wizard-pending" : "seen-this-session");
            } else {
                printf("  -> GAMEPAD slots full, ignored\n");
            }
        }
    }

    tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        keyboard_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        mouse_connected = 0;
    } else if (itf_protocol == HID_ITF_PROTOCOL_NONE) {
        int slot = find_gamepad_slot_by_dev(dev_addr, GP_SRC_HID);
        if (slot >= 0) {
            printf("USB HID unmounted: gamepad slot %d disconnected\n", slot);
            gamepad_slots[slot].connected = 0;
            gamepad_slots[slot].buttons = 0;
            gamepad_slots[slot].dpad = 0;
            gamepad_slots[slot].axis_x = 0;
            gamepad_slots[slot].axis_y = 0;
            gamepad_slots[slot].dev_addr = 0;
            gamepad_slots[slot].source = GP_SRC_NONE;
            gamepad_slots[slot].map = NULL;
        }
    }
}

static int report_debug_counter = 0;

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const *report, uint16_t len) {
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (report && len >= sizeof(hid_keyboard_report_t)) {
                process_kbd_report((hid_keyboard_report_t const *)report, &prev_kbd_report);
                prev_kbd_report = *(hid_keyboard_report_t const *)report;
            }
            break;
        case HID_ITF_PROTOCOL_MOUSE:
            // Boot-protocol mice send 3 bytes (buttons,x,y); some add
            // wheel (4); hid_mouse_report_t is 5. Pad short reports.
            if (report && len >= 3) {
                hid_mouse_report_t padded = {0};
                memcpy(&padded, report, len < sizeof(padded) ? len : sizeof(padded));
                process_mouse_report(&padded);
            }
            break;
        default:
            if (report && len >= 2) {
                if (report_debug_counter < 5) {
                    printf("Generic HID report (dev=%d, len=%d): ", dev_addr, len);
                    for (int i = 0; i < len && i < 16; i++) printf("%02X ", report[i]);
                    printf("\n");
                    report_debug_counter++;
                }
                int slot = find_gamepad_slot_by_dev(dev_addr, GP_SRC_HID);
                if (slot >= 0) {
                    gamepad_slots[slot].connected = 1;
                    process_gamepad_report(slot, report, len);
                } else {
                    process_generic_report(dev_addr, instance, report, len);
                }
            }
            break;
    }

    tuh_hid_receive_report(dev_addr, instance);
}

//--------------------------------------------------------------------
// Public API
//--------------------------------------------------------------------

void usbhid_init(void) {
    tuh_init(BOARD_TUH_RHPORT);
    memset(&prev_kbd_report, 0, sizeof(prev_kbd_report));
    memset(&prev_mouse_report, 0, sizeof(prev_mouse_report));
    cumulative_dx = 0;
    cumulative_dy = 0;
    cumulative_wheel = 0;
    current_buttons = 0;
    mouse_has_motion = 0;
    key_action_head = 0;
    key_action_tail = 0;
}

void usbhid_task(void) {
    tuh_task();
}

int usbhid_keyboard_connected(void) { return keyboard_connected; }
int usbhid_mouse_connected(void) { return mouse_connected; }

void usbhid_get_keyboard_state(usbhid_keyboard_state_t *state) {
    if (state) {
        memcpy(state->keycode, prev_kbd_report.keycode, 6);
        state->modifier = prev_kbd_report.modifier;
        state->has_key = (key_action_head != key_action_tail);
    }
}

void usbhid_get_mouse_state(usbhid_mouse_state_t *state) {
    if (state) {
        state->dx = cumulative_dx;
        state->dy = cumulative_dy;
        state->wheel = cumulative_wheel;
        state->buttons = current_buttons;
        state->has_motion = mouse_has_motion;
        cumulative_dx = 0;
        cumulative_dy = 0;
        cumulative_wheel = 0;
        mouse_has_motion = 0;
    }
}

int usbhid_get_key_action(uint8_t *keycode, int *down) {
    if (key_action_head == key_action_tail) return 0;
    *keycode = key_action_queue[key_action_tail].keycode;
    *down = key_action_queue[key_action_tail].down;
    key_action_tail = (key_action_tail + 1) % KEY_ACTION_QUEUE_SIZE;
    return 1;
}

// Convert HID keycode to keyboard state bit (NES layout — matches
// ps2kbd_wrapper.h KBD_STATE_* constants).
static uint16_t hid_to_kbd_state_bit(uint8_t keycode) {
    switch (keycode) {
        // Arrow keys -> D-pad
        case 0x52: return 0x0001; // Up    -> KBD_STATE_UP
        case 0x51: return 0x0002; // Down  -> KBD_STATE_DOWN
        case 0x50: return 0x0004; // Left  -> KBD_STATE_LEFT
        case 0x4F: return 0x0008; // Right -> KBD_STATE_RIGHT

        // WASD alternative for D-pad
        case 0x1A: return 0x0001; // W -> UP
        case 0x16: return 0x0002; // S -> DOWN
        case 0x04: return 0x0004; // A -> LEFT
        case 0x07: return 0x0008; // D -> RIGHT

        // X = A, Z = B (standard NES keyboard convention)
        case 0x1B: return 0x0010; // X -> KBD_STATE_A
        case 0x1D: return 0x0020; // Z -> KBD_STATE_B

        // Start = Enter or Keypad Enter, Select = Space
        case 0x28: return 0x0080; // Enter       -> KBD_STATE_START
        case 0x58: return 0x0080; // Keypad Enter -> KBD_STATE_START
        case 0x2C: return 0x0040; // Space       -> KBD_STATE_SELECT

        // Menu / navigation
        case 0x29: return 0x0100; // Escape -> KBD_STATE_ESC
        case 0x3C: return 0x8000; // F3     -> KBD_STATE_F3
        case 0x44: return 0x0400; // F11    -> KBD_STATE_F11
        case 0x45: return 0x0200; // F12    -> KBD_STATE_F12
        case 0x4B: return 0x0800; // PgUp   -> KBD_STATE_PGUP
        case 0x4E: return 0x1000; // PgDn   -> KBD_STATE_PGDN
        case 0x4A: return 0x2000; // Home   -> KBD_STATE_HOME
        case 0x4D: return 0x4000; // End    -> KBD_STATE_END

        default: return 0;
    }
}

uint16_t usbhid_get_kbd_state(void) {
    uint16_t state = 0;
    for (int i = 0; i < 6; i++) {
        if (prev_kbd_report.keycode[i] != 0)
            state |= hid_to_kbd_state_bit(prev_kbd_report.keycode[i]);
    }
    return state;
}

int usbhid_get_raw_char(void) {
    if (usb_raw_char_head == usb_raw_char_tail) return -1;
    uint8_t ch = usb_raw_char_queue[usb_raw_char_tail];
    usb_raw_char_tail = (usb_raw_char_tail + 1) & (USB_RAW_CHAR_QUEUE_SIZE - 1);
    return ch;
}

int usbhid_ctrl_alt_del_pressed(void) {
    /* tinyusb modifier masks:
     *   LEFTCTRL=0x01, RIGHTCTRL=0x10, LEFTALT=0x04, RIGHTALT=0x40. */
    uint8_t m = prev_kbd_report.modifier;
    bool ctrl = (m & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL)) != 0;
    bool alt  = (m & (KEYBOARD_MODIFIER_LEFTALT  | KEYBOARD_MODIFIER_RIGHTALT))  != 0;
    if (!ctrl || !alt) return 0;
    for (int i = 0; i < 6; i++) {
        if (prev_kbd_report.keycode[i] == 0x4C) return 1;  // HID Delete
    }
    return 0;
}

// Per-slot gamepad API
int usbhid_gamepad_connected_idx(int idx) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return 0;
    return gamepad_slots[idx].connected;
}

void usbhid_get_gamepad_state_idx(int idx, usbhid_gamepad_state_t *state) {
    if (!state) return;
    if (idx < 0 || idx >= MAX_GAMEPADS || !gamepad_slots[idx].connected) {
        memset(state, 0, sizeof(*state));
        return;
    }
    state->axis_x = gamepad_slots[idx].axis_x;
    state->axis_y = gamepad_slots[idx].axis_y;
    state->dpad = gamepad_slots[idx].dpad;
    state->buttons = gamepad_slots[idx].buttons;
    state->connected = gamepad_slots[idx].connected;
}

int usbhid_gamepad_connected(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected) return 1;
    }
    return 0;
}

void usbhid_get_gamepad_state(usbhid_gamepad_state_t *state) {
    if (!state) return;
    memset(state, 0, sizeof(*state));
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (gamepad_slots[i].connected) {
            state->dpad |= gamepad_slots[i].dpad;
            state->buttons |= gamepad_slots[i].buttons;
            state->connected = 1;
        }
    }
}

//--------------------------------------------------------------------
// Menu-A/B calibration API
//--------------------------------------------------------------------

int usbhid_gamepad_needs_calibration(int idx) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return 0;
    if (!gamepad_slots[idx].connected) return 0;
    return gamepad_slots[idx].needs_calibration;
}

void usbhid_gamepad_clear_calibration(int idx) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return;
    gamepad_slot_t *gp = &gamepad_slots[idx];
    gp->needs_calibration = 0;
    if (gp->connected && gp->source != GP_SRC_NONE)
        session_seen_add(gp->vid, gp->pid, gp->source);
}

int usbhid_gamepad_get_raw_info(int idx, usbhid_gamepad_raw_info_t *out) {
    if (!out || idx < 0 || idx >= MAX_GAMEPADS) return 0;
    gamepad_slot_t *gp = &gamepad_slots[idx];
    if (!gp->connected || !gp->raw_baseline_valid || gp->raw_len == 0) return 0;
    out->vid = gp->vid;
    out->pid = gp->pid;
    out->source = (gp->source == GP_SRC_XINPUT) ? USBHID_GP_SRC_XINPUT :
                  (gp->source == GP_SRC_HID)    ? USBHID_GP_SRC_HID    :
                                                   USBHID_GP_SRC_NONE;
    out->report_len = gp->raw_len;
    memcpy(out->baseline, gp->raw_baseline, sizeof(out->baseline));
    memcpy(out->raw, gp->raw_report, sizeof(out->raw));
    return 1;
}

int usbhid_gamepad_find_pressed_bit(int idx, uint8_t *byte_idx, uint8_t *mask) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return 0;
    gamepad_slot_t *gp = &gamepad_slots[idx];
    if (!gp->connected || !gp->raw_baseline_valid || gp->raw_len == 0) return 0;

    int found_byte = -1;
    uint8_t found_mask = 0;
    int popcount = 0;
    for (uint8_t b = 0; b < gp->raw_len; b++) {
        uint8_t diff = gp->raw_report[b] ^ gp->raw_baseline[b];
        if (!diff) continue;
        // Skip analog-stick drift in the centre band.
        uint8_t base = gp->raw_baseline[b];
        uint8_t cur  = gp->raw_report[b];
        if (base >= 0x60 && base <= 0xA0 && cur >= 0x60 && cur <= 0xA0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (diff & (1u << bit)) {
                if (found_byte < 0) {
                    found_byte = b;
                    found_mask = 1u << bit;
                }
                popcount++;
            }
        }
    }
    if (popcount != 1) return 0;
    if (byte_idx) *byte_idx = (uint8_t)found_byte;
    if (mask)     *mask     = found_mask;
    return 1;
}

void usbhid_gamepad_set_menu_ab(int idx,
                                uint8_t a_byte, uint8_t a_mask,
                                uint8_t b_byte, uint8_t b_mask) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return;
    gamepad_slots[idx].menu_a.byte = a_byte;
    gamepad_slots[idx].menu_a.mask = a_mask;
    gamepad_slots[idx].menu_b.byte = b_byte;
    gamepad_slots[idx].menu_b.mask = b_mask;
    gamepad_slots[idx].ab_valid = 0x03;
}

int usbhid_learned_ab_add(usbhid_gp_src_t source,
                          uint16_t vid, uint16_t pid,
                          uint8_t a_byte, uint8_t a_mask,
                          uint8_t b_byte, uint8_t b_mask) {
    if (learned_ab_count >= MAX_LEARNED_AB) return -1;
    gp_source_t src = (source == USBHID_GP_SRC_XINPUT) ? GP_SRC_XINPUT :
                      (source == USBHID_GP_SRC_HID)    ? GP_SRC_HID    :
                                                          GP_SRC_NONE;
    for (size_t i = 0; i < learned_ab_count; i++) {
        if (learned_ab_table[i].vid == vid && learned_ab_table[i].pid == pid &&
            learned_ab_table[i].source == src) {
            learned_ab_table[i].a.byte = a_byte;
            learned_ab_table[i].a.mask = a_mask;
            learned_ab_table[i].b.byte = b_byte;
            learned_ab_table[i].b.mask = b_mask;
            return 0;
        }
    }
    learned_ab_table[learned_ab_count].vid = vid;
    learned_ab_table[learned_ab_count].pid = pid;
    learned_ab_table[learned_ab_count].source = src;
    learned_ab_table[learned_ab_count].a.byte = a_byte;
    learned_ab_table[learned_ab_count].a.mask = a_mask;
    learned_ab_table[learned_ab_count].b.byte = b_byte;
    learned_ab_table[learned_ab_count].b.mask = b_mask;
    learned_ab_count++;
    return 0;
}

void usbhid_learned_ab_clear(void) {
    learned_ab_count = 0;
    session_seen_count = 0;
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        gamepad_slot_t *gp = &gamepad_slots[i];
        if (!gp->connected) continue;
        gp->ab_valid = 0;
        const gamepad_map_t *fallback = (gp->source == GP_SRC_XINPUT)
            ? &xinput_default_map : &fallback_hid_map;
        seed_menu_ab(i, gp->vid, gp->pid, gp->source, gp->map, fallback);
        gp->needs_calibration = 1;
    }
}

int usbhid_gamepad_get_menu_ab(int idx, int *out_a, int *out_b) {
    if (idx < 0 || idx >= MAX_GAMEPADS) return 0;
    gamepad_slot_t *gp = &gamepad_slots[idx];
    if (!gp->connected || !gp->ab_valid || gp->raw_len == 0) {
        if (out_a) *out_a = 0;
        if (out_b) *out_b = 0;
        return 0;
    }
    int a = 0, b = 0;
    if ((gp->ab_valid & 0x01) && gp->menu_a.byte < gp->raw_len)
        a = (gp->raw_report[gp->menu_a.byte] & gp->menu_a.mask) ? 1 : 0;
    if ((gp->ab_valid & 0x02) && gp->menu_b.byte < gp->raw_len)
        b = (gp->raw_report[gp->menu_b.byte] & gp->menu_b.mask) ? 1 : 0;
    if (out_a) *out_a = a;
    if (out_b) *out_b = b;
    return 1;
}

#endif // CFG_TUH_ENABLED
