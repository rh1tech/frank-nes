/*
 * USB HID host driver
 * Based on TinyUSB HID host example
 * SPDX-License-Identifier: MIT
 *
 * Fork maintained as part of MurmNES by Mikhail Matveev.
 * https://rh1.tech | https://github.com/rh1tech/murmnes
 */

#include "usbhid.h"
#include "usbhid_wrapper.h"
#include <stdint.h>
#include <stdbool.h>

// External Genesis button state (from gwenesis_io.c)
extern unsigned char button_state[];

/* Genesis Button mapping in button_state byte:
   Bit 7 6 5 4 3 2 1 0
       S A C B R L D U
   
   Start, A, C, B, Right, Left, Down, Up
   0 = pressed, 1 = released
*/

// Button bit definitions
#define BTN_UP    0x01
#define BTN_DOWN  0x02
#define BTN_LEFT  0x04
#define BTN_RIGHT 0x08
#define BTN_B     0x10
#define BTN_C     0x20
#define BTN_A     0x40
#define BTN_START 0x80

// Current button state for USB keyboard (active low)
static uint8_t usb_buttons = 0xFF;

//--------------------------------------------------------------------
// HID Keycode to Genesis Button Mapping
//--------------------------------------------------------------------

static void process_key(uint8_t hid_keycode, bool pressed) {
    uint8_t mask = 0;
    
    switch (hid_keycode) {
        // Arrow keys for D-pad
        case 0x52: mask = BTN_UP;    break;  // Up arrow
        case 0x51: mask = BTN_DOWN;  break;  // Down arrow
        case 0x50: mask = BTN_LEFT;  break;  // Left arrow
        case 0x4F: mask = BTN_RIGHT; break;  // Right arrow
        
        // WASD alternative for D-pad
        case 0x1A: mask = BTN_UP;    break;  // W
        case 0x16: mask = BTN_DOWN;  break;  // S
        case 0x04: mask = BTN_LEFT;  break;  // A
        case 0x07: mask = BTN_RIGHT; break;  // D
        
        // Action buttons
        case 0x1D: mask = BTN_A;     break;  // Z key -> A
        case 0x1B: mask = BTN_B;     break;  // X key -> B
        case 0x06: mask = BTN_C;     break;  // C key -> C
        
        case 0x28: mask = BTN_START; break;  // Enter -> Start
        case 0x2C: mask = BTN_START; break;  // Space -> Start (alternative)
        
        default: return;  // Unknown key, ignore
    }
    
    if (pressed) {
        usb_buttons &= ~mask;  // Clear bit (active low)
    } else {
        usb_buttons |= mask;   // Set bit (released)
    }
}

//--------------------------------------------------------------------
// USB HID Wrapper API
//--------------------------------------------------------------------

void usbhid_wrapper_init(void) {
    usb_buttons = 0xFF;  // All buttons released
    usbhid_init();
}

void usbhid_wrapper_poll(void) {
    usbhid_keyboard_state_t current_keys;
    
    // Run USB task
    usbhid_task();
    
    // Get keyboard state
    usbhid_get_keyboard_state(&current_keys);
    
    // Reset to all released
    usb_buttons = 0xFF;
    
    // Check each key slot
    for (int i = 0; i < 6; i++) {
        if (current_keys.keycode[i] != 0) {
            process_key(current_keys.keycode[i], true);
        }
    }
    
    // Check modifiers (Ctrl, Shift, Alt)
    if (current_keys.modifier & 0x11) {  // Ctrl
        usb_buttons &= ~BTN_A;
    }
    if (current_keys.modifier & 0x22) {  // Shift  
        usb_buttons &= ~BTN_B;
    }
    if (current_keys.modifier & 0x44) {  // Alt
        usb_buttons &= ~BTN_C;
    }
}

void usbhid_wrapper_update_buttons(void) {
    // Merge USB buttons with player 1 button state
    button_state[0] &= usb_buttons;
}
