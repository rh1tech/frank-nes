/*
 * FRANK NES - NES Emulator for RP2350
 * VGA HSTX driver pin map (M2 platform only).
 * SPDX-License-Identifier: Unlicense
 */

#ifndef PICO_VGA_HSTX_PINS_H
#define PICO_VGA_HSTX_PINS_H

// =============================================================================
// VGA HSTX Output Pins (GPIO 12-19)
// =============================================================================
// The M2 board shares the HDMI connector's 8 GPIOs (12-19) with a VGA output
// when using an HDMI-to-VGA ribbon. Layout matches drivers/hdmi_pio/vga.c:
//
//   GPIO 12 -> B0  (bit 0)
//   GPIO 13 -> B1  (bit 1)
//   GPIO 14 -> G0  (bit 2)
//   GPIO 15 -> G1  (bit 3)
//   GPIO 16 -> R0  (bit 4)
//   GPIO 17 -> R1  (bit 5)
//   GPIO 18 -> HS  (bit 6)
//   GPIO 19 -> VS  (bit 7)
//
// RGB is 2 bits per channel (RGB222 / 6-bit). HS and VS are active low.
#define VGA_HSTX_PIN_BASE 12
#define VGA_HSTX_PIN_COUNT 8

// Byte bit positions (for building line patterns and sync constants)
#define VGA_HSTX_HS_BIT 6
#define VGA_HSTX_VS_BIT 7

#endif // PICO_VGA_HSTX_PINS_H
