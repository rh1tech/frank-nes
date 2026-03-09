/*
 * MurmNES - NES Emulator for RP2350
 * Copyright (c) 2026 Mikhail Matveev <xtreme@rh1.tech>
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 * SPDX-License-Identifier: MIT
 */

#include "board_config.h"
#include "ps2kbd_wrapper.h"
#include "ps2kbd_mrmltr.h"

// Simple ring buffer for key events (avoids std::queue to save RAM)
#define EVENT_QUEUE_SIZE 16

struct KeyEvent {
    uint8_t pressed;
    uint8_t key;
};

static KeyEvent event_queue[EVENT_QUEUE_SIZE];
static volatile uint8_t queue_head = 0;  // Next write position
static volatile uint8_t queue_tail = 0;  // Next read position

static inline bool queue_empty(void) {
    return queue_head == queue_tail;
}

static inline bool queue_full(void) {
    return ((queue_head + 1) & (EVENT_QUEUE_SIZE - 1)) == queue_tail;
}

static void queue_push(uint8_t pressed, uint8_t key) {
    if (!queue_full()) {
        event_queue[queue_head].pressed = pressed;
        event_queue[queue_head].key = key;
        queue_head = (queue_head + 1) & (EVENT_QUEUE_SIZE - 1);
    }
}

static bool queue_pop(uint8_t* pressed, uint8_t* key) {
    if (queue_empty()) return false;
    *pressed = event_queue[queue_tail].pressed;
    *key = event_queue[queue_tail].key;
    queue_tail = (queue_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    return true;
}

// Genesis key codes returned by hid_to_genesis()
// These are translated to button presses in main.c
#define GENESIS_KEY_UP     0x01
#define GENESIS_KEY_DOWN   0x02
#define GENESIS_KEY_LEFT   0x03
#define GENESIS_KEY_RIGHT  0x04
#define GENESIS_KEY_A      0x05
#define GENESIS_KEY_B      0x06
#define GENESIS_KEY_C      0x07
#define GENESIS_KEY_START  0x08
#define GENESIS_KEY_X      0x09
#define GENESIS_KEY_Y      0x0A
#define GENESIS_KEY_Z      0x0B
#define GENESIS_KEY_MODE   0x0C
#define GENESIS_KEY_SELECT 0x0D
#define GENESIS_KEY_ESC    0x0E

// HID to Genesis key mapping
// Key mapping:
//   Arrow keys -> D-pad (Up/Down/Left/Right)
//   A, S, D    -> Genesis A, B, C buttons
//   Q, W, E    -> Genesis X, Y, Z buttons (6-button mode)
//   Enter      -> Start
//   Space      -> Select
//   Alt        -> Mode (6-button)
//   ESC        -> Settings menu
// Returns 0 if no mapping
static unsigned char hid_to_genesis(uint8_t code) {
    switch (code) {
        // Arrow keys -> D-pad
        case 0x52: return GENESIS_KEY_UP;     // Up arrow
        case 0x51: return GENESIS_KEY_DOWN;   // Down arrow
        case 0x50: return GENESIS_KEY_LEFT;   // Left arrow
        case 0x4F: return GENESIS_KEY_RIGHT;  // Right arrow
        
        // A, S, D -> Genesis A, B, C
        case 0x04: return GENESIS_KEY_A;      // A key
        case 0x16: return GENESIS_KEY_B;      // S key
        case 0x07: return GENESIS_KEY_C;      // D key
        
        // Q, W, E -> Genesis X, Y, Z (6-button)
        case 0x14: return GENESIS_KEY_X;      // Q key
        case 0x1A: return GENESIS_KEY_Y;      // W key
        case 0x08: return GENESIS_KEY_Z;      // E key
        
        // Start = Enter or Keypad Enter
        case 0x28: return GENESIS_KEY_START;  // Enter
        case 0x58: return GENESIS_KEY_START;  // Keypad Enter
        
        // Select = Space
        case 0x2C: return GENESIS_KEY_SELECT; // Space
        
        // Mode button (Left Alt or Right Alt)
        case 0xE2: return GENESIS_KEY_MODE;   // Left Alt
        case 0xE6: return GENESIS_KEY_MODE;   // Right Alt
        
        // ESC = Settings menu / Back
        case 0x29: return GENESIS_KEY_ESC;    // Escape
        
        default: return 0;
    }
}

static void key_handler(hid_keyboard_report_t *curr, hid_keyboard_report_t *prev) {
    // Check keys - new key presses
    for (int i = 0; i < 6; i++) {
        if (curr->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (prev->keycode[j] == curr->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unsigned char k = hid_to_genesis(curr->keycode[i]);
                if (k) queue_push(1, k);
            }
        }
    }

    // Check keys - key releases
    for (int i = 0; i < 6; i++) {
        if (prev->keycode[i] != 0) {
            bool found = false;
            for (int j = 0; j < 6; j++) {
                if (curr->keycode[j] == prev->keycode[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                unsigned char k = hid_to_genesis(prev->keycode[i]);
                if (k) queue_push(0, k);
            }
        }
    }
}

static Ps2Kbd_Mrmltr* kbd = nullptr;
static volatile uint16_t g_kbd_state = 0;  // Global keyboard state bitmask

// Convert key code to state bit
static uint16_t key_to_state_bit(uint8_t key) {
    switch (key) {
        case GENESIS_KEY_UP:     return KBD_STATE_UP;
        case GENESIS_KEY_DOWN:   return KBD_STATE_DOWN;
        case GENESIS_KEY_LEFT:   return KBD_STATE_LEFT;
        case GENESIS_KEY_RIGHT:  return KBD_STATE_RIGHT;
        case GENESIS_KEY_A:      return KBD_STATE_A;
        case GENESIS_KEY_B:      return KBD_STATE_B;
        case GENESIS_KEY_C:      return KBD_STATE_C;
        case GENESIS_KEY_START:  return KBD_STATE_START;
        case GENESIS_KEY_X:      return KBD_STATE_X;
        case GENESIS_KEY_Y:      return KBD_STATE_Y;
        case GENESIS_KEY_Z:      return KBD_STATE_Z;
        case GENESIS_KEY_MODE:   return KBD_STATE_MODE;
        case GENESIS_KEY_SELECT: return KBD_STATE_SELECT;
        case GENESIS_KEY_ESC:    return KBD_STATE_ESC;
        default: return 0;
    }
}

extern "C" void ps2kbd_init(void) {
    kbd = new Ps2Kbd_Mrmltr(pio0, PS2_PIN_CLK, key_handler);
    kbd->init_gpio();
    g_kbd_state = 0;
}

extern "C" void ps2kbd_tick(void) {
    if (kbd) kbd->tick();
    
    // Update global state from queued events (peek, don't consume)
    // Process all events and update g_kbd_state
    uint8_t p, k;
    while (queue_pop(&p, &k)) {
        uint16_t bit = key_to_state_bit(k);
        if (bit) {
            if (p) {
                g_kbd_state |= bit;
            } else {
                g_kbd_state &= ~bit;
            }
        }
    }
}

extern "C" int ps2kbd_get_key(int* pressed, unsigned char* key) {
    // This function is now deprecated - use ps2kbd_get_state() instead
    // But keep it for compatibility - always returns 0 since we consume in tick()
    (void)pressed;
    (void)key;
    return 0;
}

extern "C" uint16_t ps2kbd_get_state(void) {
    return g_kbd_state;
}

