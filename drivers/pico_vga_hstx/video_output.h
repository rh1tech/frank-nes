/*
 * FRANK NES - NES Emulator for RP2350
 * VGA HSTX driver (M2 platform only). Adapted from pico_hdmi/video_output.
 * SPDX-License-Identifier: Unlicense
 */

#ifndef PICO_VGA_HSTX_VIDEO_OUTPUT_H
#define PICO_VGA_HSTX_VIDEO_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

// ============================================================================
// Video Output Configuration
// ============================================================================
//
// VGA 640x480 @ 60Hz (industry standard timing):
//   Pixel clock: 25.175 MHz
//   HS: negative polarity. VS: negative polarity.
//
// The HSTX output is driven at one 8-bit byte per pixel clock.
// We run the shift register at 4 bytes per 32-bit FIFO word so each word
// produced by the DMA ping-pong command list consumes 4 pixel clocks.

#define MODE_H_FRONT_PORCH 16
#define MODE_H_SYNC_WIDTH 96
#define MODE_H_BACK_PORCH 48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH 10
#define MODE_V_SYNC_WIDTH 2
#define MODE_V_BACK_PORCH 33
#define MODE_V_ACTIVE_LINES 480

#ifndef MODE_HSTX_CLK_DIV
#define MODE_HSTX_CLK_DIV 1
#endif

// HSTX CSR CLKDIV = 5 -> 1 pixel per 5 HSTX clocks.
// clk_hstx 126 MHz / 5 -> 25.2 MHz pixel clock (within VGA tolerance).
#ifndef MODE_HSTX_CSR_CLKDIV
#define MODE_HSTX_CSR_CLKDIV 5
#endif

#define MODE_H_TOTAL_PIXELS (MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH + MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS)
#define MODE_V_TOTAL_LINES (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// Frame dimensions (set via video_output_init)
extern uint16_t frame_width;
extern uint16_t frame_height;

// ============================================================================
// Global State
// ============================================================================

extern volatile uint32_t video_frame_count;

// ============================================================================
// Public Interface
// ============================================================================

typedef void (*video_output_task_fn)(void);
typedef void (*video_output_vsync_cb_t)(void);

/**
 * Scanline Callback:
 * The VGA driver accepts the same RGB565 buffer shape as the HDMI driver so
 * host code (main_pico.c) can reuse its scanline_callback unchanged. The
 * driver internally converts RGB565 -> RGB222 + sync bits per pixel.
 *
 * @param v_scanline The current vertical scanline.
 * @param active_line The current active video line.
 * @param line_buffer Buffer to fill with MODE_H_ACTIVE_PIXELS RGB565 pixels,
 *                    packed as (MODE_H_ACTIVE_PIXELS / 2) uint32_t words.
 */
typedef void (*video_output_scanline_cb_t)(uint32_t v_scanline, uint32_t active_line, uint32_t *line_buffer);

/**
 * Initialize HSTX and DMA for VGA output.
 * @param width  Framebuffer width in pixels (e.g. 640)
 * @param height Framebuffer height in pixels (e.g. 480)
 */
void video_output_init(uint16_t width, uint16_t height);

/** Register the scanline callback. */
void video_output_set_scanline_callback(video_output_scanline_cb_t cb);

/** Register a VSYNC callback, called once per frame at the start of vertical sync. */
void video_output_set_vsync_callback(video_output_vsync_cb_t cb);

/** Register a background task to run in the Core 1 loop. */
void video_output_set_background_task(video_output_task_fn task);

/**
 * Core 1 entry point for video output. Does not return.
 */
void video_output_core1_run(void);

/**
 * No-op on VGA (no HDMI audio). Kept so main_pico.c compiles unchanged.
 */
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate);

#endif // PICO_VGA_HSTX_VIDEO_OUTPUT_H
