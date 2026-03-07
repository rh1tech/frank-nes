/*
 * murmnes - NES Emulator for RP2350
 * Test pattern display
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "hardware/clocks.h"

#include <stdio.h>

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

// Test pattern framebuffer (ARGB format)
static uint32_t pixels[256 * 240];

// Audio
static int audio_frame_counter = 0;

static void generate_silence(void)
{
    while (hstx_di_queue_get_level() < 200) {
        audio_sample_t samples[4] = {0};
        hstx_packet_t packet;
        audio_frame_counter = hstx_packet_set_audio_samples(&packet, samples, 4, audio_frame_counter);
        hstx_data_island_t island;
        hstx_encode_data_island(&island, &packet, false, true);
        hstx_di_queue_push(&island);
    }
}

// Scanline callback — reads from static pixels[], converts ARGB to RGB565
void __not_in_flash("scanline") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;
    const uint32_t *src = &pixels[nes_line * 256];
    for (int i = 0; i < 32; i++) dst[i] = 0;
    for (int x = 0; x < 256; x++) {
        uint32_t p = src[x];
        uint32_t c = ((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F);
        dst[32 + x] = c | (c << 16);
    }
    for (int i = 288; i < 320; i++) dst[i] = 0;
}

int main(void)
{
    set_sys_clock_khz(252000, true);
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    sleep_ms(1000);

    // Fill test pattern
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 256; x++) {
            uint8_t r = x;
            uint8_t g = y;
            uint8_t b = (x + y) & 0xFF;
            pixels[y * 256 + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    // Init HDMI
    hstx_di_queue_init();
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    video_output_set_scanline_callback(scanline_callback);
    generate_silence();
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    for (int i = 0; i < 50; i++) {
        if (stdio_usb_connected()) break;
        sleep_ms(100);
    }
    printf("\n=== murmnes - Test Pattern ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    while (1) {
        generate_silence();
        sleep_ms(16);
    }

    return 0;
}
