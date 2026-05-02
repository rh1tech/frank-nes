/*
 * USB gamepad calibration persistence
 *
 * Saves/loads learned menu-A/B profiles to SD at /nes/gamepads/VID_PID.txt.
 * Format is a compatible subset of the in-repo capture logs so an advanced
 * user can commit their learned file back to the project and regenerate the
 * compiled-in known_hid_maps / known_xinput_maps tables.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef GAMEPAD_CAL_H
#define GAMEPAD_CAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Scan /nes/gamepads/ at boot and register every VID/PID A/B profile
 *  we can parse via usbhid_learned_ab_add(). Missing directory is OK. */
void gamepad_cal_load_all(void);

/** Write a learned profile to /nes/gamepads/gamepad_VVVV_PPPP.txt.
 *  Returns true on success. Source must be "HID" or "XINPUT". */
int gamepad_cal_save(const char *source,
                     uint16_t vid, uint16_t pid,
                     uint8_t a_byte, uint8_t a_mask,
                     uint8_t b_byte, uint8_t b_mask);

/** Delete every gamepad_*.txt file in /snes/gamepads/ and clear the
 *  in-memory learned-profile table. Returns the number of files
 *  removed (0 if the directory was absent or empty). */
int gamepad_cal_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* GAMEPAD_CAL_H */
