/*
 * FRANK NES - VGA HSTX driver, DispHSTX integration (M2 only).
 * Test-pattern mode: horizontal stripes, no NES pipeline.
 * SPDX-License-Identifier: Unlicense
 */

#include "pico_vga_hstx/video_output.h"
#include "pico_vga_hstx/hstx_pins.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "disphstx.h"
#include "disphstx_vmode_simple.h"
#include "disphstx_vmode.h"

#include <stdio.h>
#include <string.h>

/* 640x480x8 = 300 KB. DispHSTX works best with its own-allocated
 * buffer — pass NULL to let it malloc the 300 KB framebuffer itself,
 * exactly like the working PicoSDKDemo does. When we're ready to
 * integrate with the NES pipeline we'll feed the buffer ourselves. */
#define FB_W 640
#define FB_H 480

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

static void paint_horizontal_stripes(uint8_t *fb, int w, int h)
{
    static const uint8_t c[8] = {
        0xFF, 0xFC, 0x1F, 0x1C, 0xE3, 0xE0, 0x03, 0x00,
    };
    const uint32_t band_h = h / 8;
    for (uint32_t band = 0; band < 8; band++) {
        memset(&fb[band * band_h * w], c[band], band_h * w);
    }
}

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;
    /* Actual bringup happens in vga_hstx_early_test() called from main
     * before any other peripheral init. See below. */
}

void video_output_set_background_task(video_output_task_fn task) { background_task = task; }
void video_output_set_scanline_callback(video_output_scanline_cb_t cb) { scanline_callback = cb; }
void video_output_set_vsync_callback(video_output_vsync_cb_t cb) { vsync_callback = cb; }
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate) { (void)sample_rate; }

void video_output_core1_run(void)
{
    /* Not used for VGA_HSTX: DispHSTX claims Core 1 itself from Core 0. */
}

/* Early VGA-only bring-up — called from main() before ANY other peripheral
 * init, replicating the PicoSDKDemo flow exactly:
 *   1. DispVMode640x480x8(DISPHSTX_DISPMODE_VGA, NULL)  — DispHSTX allocates
 *      its own 300KB framebuffer via malloc and configures sys_clk to 126
 *      MHz, clk_hstx, HSTX, DMA, and Core 1 VGA ISR.
 *   2. We paint 8 horizontal stripes directly into DispHstxVMode's buffer.
 *
 * If the 8 stripes show stably, the driver is working. */
void vga_hstx_early_test(void)
{
    (void)DispVMode640x480x8(DISPHSTX_DISPMODE_VGA, NULL);
    extern sDispHstxVModeState DispHstxVMode;
    uint8_t *fb = (uint8_t *)DispHstxVMode.strip[0].slot[0].buf;
    if (fb) paint_horizontal_stripes(fb, FB_W, FB_H);
}
