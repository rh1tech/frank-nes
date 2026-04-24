/*
 * FRANK NES - VGA HSTX driver, DispHSTX integration (M2 only).
 *
 * Uses DispVMode320x240x8 — 320x240 RGB332 framebuffer scaled 2x to
 * 640x480 by DispHSTX. Core 1 is owned by DispHSTX's VGA ISR. Core 0
 * writes the NES frame into the framebuffer each vsync.
 *
 * The NES frame (256x240 8-bit indexed) is centered in the 320x240
 * framebuffer with 32-pixel black borders on left/right.
 *
 * SPDX-License-Identifier: Unlicense
 */

#include "pico_vga_hstx/video_output.h"
#include "pico_vga_hstx/hstx_pins.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "disphstx.h"
#include "disphstx_vmode_simple.h"
#include "disphstx_vmode.h"

#include <string.h>

#define FB_W 320
#define FB_H 240
#define NES_W 256
#define NES_H 240
#define BORDER_L ((FB_W - NES_W) / 2)   /* 32 */

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

/* RGB332 palette: NES index -> direct RGB332 byte.
 * Just one copy — we only read it from Core 0 during blit, so no race
 * with the VGA ISR on Core 1 which only reads the already-converted
 * framebuffer. */
uint8_t vga_rgb332_palette[256];

/* Pending frame from Core 0, swapped by the copy routine. */
static volatile const uint8_t *vga_pending_pixels;
static volatile long vga_pending_pitch;

/* Static 320x240x8 = 76800-byte framebuffer. Placed in main SRAM so the
 * VGA ISR's DMA can read it at full rate. */
static uint8_t vga_framebuffer[FB_W * FB_H] __attribute__((aligned(4)));
static uint8_t *vga_fb_ptr = vga_framebuffer;

static void paint_black(uint8_t *fb, int w, int h)
{
    memset(fb, 0, w * h);
}

/* Build RGB332 palette: r3g3b2. Input is 16-bit NES palette index into
 * the 512-entry color table. buf_idx is ignored (single palette). */
void vga_hstx_update_palette(int buf_idx, const int16_t *pal, int pal_size,
                             const void *colors_void)
{
    (void)buf_idx;
    typedef struct { uint8_t r, g, b; } rgb_t;
    const rgb_t *colors = (const rgb_t *)colors_void;
    if (!pal || !colors) return;
    for (int i = 0; i < pal_size && i < 256; i++) {
        int idx = pal[i];
        if (idx < 0 || idx >= 512) idx = 0x0F;
        const rgb_t *c = &colors[idx];
        vga_rgb332_palette[i] =
            (c->r & 0xE0) | ((c->g & 0xE0) >> 3) | ((c->b & 0xC0) >> 6);
    }
}

/* Convert the 256-entry rgb565 palette (packed into a 32-bit word with both
 * halves equal) to RGB332 for the DispHSTX framebuffer. Menu/selector code
 * paints into rgb565_palette_32 directly, so this keeps the two in sync. */
void vga_hstx_update_palette_from_rgb565(const uint32_t *pal_rgb565, int count)
{
    if (!pal_rgb565) return;
    if (count > 256) count = 256;
    for (int i = 0; i < count; i++) {
        uint16_t c16 = (uint16_t)(pal_rgb565[i] & 0xFFFF);
        uint8_t r = (uint8_t)(((c16 >> 11) & 0x1F) << 3);
        uint8_t g = (uint8_t)(((c16 >> 5)  & 0x3F) << 2);
        uint8_t b = (uint8_t)(( c16        & 0x1F) << 3);
        vga_rgb332_palette[i] =
            (r & 0xE0) | ((g & 0xE0) >> 3) | ((b & 0xC0) >> 6);
    }
}

/* Called from Core 0 after each frame to blit the NES frame into the
 * framebuffer. Uses the RGB332 palette to convert indexed pixels to
 * direct colour. The DispHSTX VGA ISR reads directly from this buffer;
 * writes racing with ISR reads only tear at pixel granularity, which
 * is imperceptible for the NES's ~8px sprites moving at 60 Hz. */
static void __not_in_flash("vga_blit") vga_blit_nes_frame(const uint8_t *src, long pitch)
{
    if (!vga_fb_ptr || !src) return;
    const uint8_t *pal = vga_rgb332_palette;

    for (int y = 0; y < NES_H; y++) {
        const uint8_t *srow = src + y * pitch;
        uint8_t *drow = vga_fb_ptr + y * FB_W + BORDER_L;
        for (int x = 0; x < NES_W; x += 4) {
            uint32_t p = *(const uint32_t *)(srow + x);
            drow[x + 0] = pal[p & 0xFF];
            drow[x + 1] = pal[(p >> 8) & 0xFF];
            drow[x + 2] = pal[(p >> 16) & 0xFF];
            drow[x + 3] = pal[(p >> 24)];
        }
    }
}

/* Called from Core 0 main emulation loop each frame. */
void vga_hstx_post_frame(const uint8_t *pixels, long pitch)
{
    vga_pending_pixels = pixels;
    vga_pending_pitch = pitch;
}

/* Polled from Core 0's main emulation loop (replaces the DMA-IRQ
 * vsync callback path). Copies whatever frame is pending. */
void vga_hstx_service(void)
{
    const uint8_t *pp = (const uint8_t *)vga_pending_pixels;
    if (pp) {
        long pitch = vga_pending_pitch;
        vga_pending_pixels = NULL;
        vga_blit_nes_frame(pp, pitch);
        video_frame_count++;
        if (vsync_callback) vsync_callback();
    }
}

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;
}

void video_output_set_background_task(video_output_task_fn task) { background_task = task; }
void video_output_set_scanline_callback(video_output_scanline_cb_t cb) { scanline_callback = cb; }
void video_output_set_vsync_callback(video_output_vsync_cb_t cb) { vsync_callback = cb; }
void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate) { (void)sample_rate; }

void video_output_core1_run(void)
{
    /* Not used — DispHSTX claims Core 1 itself via Core1Exec. */
}

/* Bring up DispHSTX on Core 0 before any other peripheral init.
 * Static 320x240 RGB332 framebuffer (scaled 2x to 640x480 by DispHSTX)
 * lives in main SRAM; DispHSTX starts the VGA ISR on Core 1. */
void vga_hstx_start(void)
{
    paint_black(vga_framebuffer, FB_W, FB_H);
    (void)DispVMode320x240x8(DISPHSTX_DISPMODE_VGA, vga_framebuffer);
}
