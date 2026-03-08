/*
 * murmnes - NES Emulator for RP2350
 * QuickNES core with HDMI output via HSTX
 */

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "hardware/clocks.h"

#include <stdio.h>
#include <string.h>

#include "quicknes.h"

/* 16KB stack in main SRAM — scratch_y (4KB) is too small for QuickNES */
static uint8_t big_stack[16384] __attribute__((aligned(8)));
static void real_main(void);

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

#define NES_WIDTH 256
#define NES_HEIGHT 240
#define SAMPLE_RATE 44100


/* ROM embedded in flash by CMake (objcopy) */
#ifdef HAS_NES_ROM
extern const uint8_t nes_rom_data[];
extern const uint8_t nes_rom_end[];
#endif

/* Palette lookup: NES indexed pixel -> RGB565 */
static uint16_t rgb565_palette[256];

/* Pointer to current frame pixels (set after each emulate_frame) */
static const uint8_t *frame_pixels;
static long frame_pitch;

/* Vsync flag — set by Core 1 DMA ISR, cleared by Core 0 after emulating */
static volatile uint32_t vsync_flag;

static void __not_in_flash("vsync") vsync_cb(void)
{
    vsync_flag = 1;
    __sev(); /* wake Core 0 from WFE */
}

/* Audio */
static int audio_frame_counter = 0;

static void feed_audio(void)
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

/* Build RGB565 palette from QuickNES frame palette + color table */
static void update_palette(void)
{
    int pal_size = 0;
    const int16_t *pal = qnes_get_palette(&pal_size);
    const qnes_rgb_t *colors = qnes_get_color_table();

    if (!pal || !colors)
        return;

    for (int i = 0; i < pal_size && i < 256; i++) {
        int idx = pal[i];
        if (idx < 0 || idx >= 512)
            idx = 0x0F; /* black */
        const qnes_rgb_t *c = &colors[idx];
        rgb565_palette[i] = ((c->r & 0xF8) << 8) | ((c->g & 0xFC) << 3) | (c->b >> 3);
    }
}

/* Scanline callback: convert indexed pixels to RGB565, doubled to 640x480
 * Runs on Core 1 DMA ISR — must be in RAM, no flash access */
void __not_in_flash("scanline") scanline_callback(
    uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    /* Each NES line is doubled vertically (480 / 240 = 2) */
    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;
    const uint8_t *src = frame_pixels + nes_line * frame_pitch;

    /* Left border: 64 pixels = 32 words of black (640 - 256*2 = 128, half = 64) */
    for (int i = 0; i < 32; i++)
        dst[i] = 0;

    /* NES pixels: each pixel doubled horizontally = 512 pixels = 256 words */
    for (int x = 0; x < NES_WIDTH; x++) {
        uint16_t c = rgb565_palette[src[x]];
        dst[32 + x] = c | (c << 16);
    }

    /* Right border */
    for (int i = 288; i < 320; i++)
        dst[i] = 0;
}

/* Generate test pattern when no ROM is loaded */
static uint8_t test_pixels[NES_WIDTH * NES_HEIGHT];

static void generate_test_pattern(void)
{
    /* Simple color bars using direct RGB565 palette */
    static const uint16_t bars[] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    for (int i = 0; i < 256; i++)
        rgb565_palette[i] = bars[i % 8];

    for (int y = 0; y < NES_HEIGHT; y++) {
        for (int x = 0; x < NES_WIDTH; x++) {
            test_pixels[y * NES_WIDTH + x] = (x >> 5) % 8;
        }
    }

    frame_pixels = test_pixels;
    frame_pitch = NES_WIDTH;
}

static void error_loop(const char *msg)
{
    printf("ERROR: %s\n", msg);
    generate_test_pattern();
    while (1) { feed_audio(); sleep_ms(16); }
}

/* Stack watermark: paint stack with 0xDEADBEEF, later check how much was used */
static void paint_stack(void)
{
    volatile uint32_t sp;
    __asm volatile ("MOV %0, SP" : "=r" (sp));
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t *end = (uint32_t *)(sp - 256);
    while (p < end)
        *p++ = 0xDEADBEEF;
}

static uint32_t check_stack_free(void)
{
    uint32_t *p = (uint32_t *)big_stack;
    uint32_t count = 0;
    while (*p == 0xDEADBEEF) {
        p++;
        count += 4;
    }
    return count;
}

/* HardFault handler — store fault info, pump USB to flush, blink LED */
static volatile uint32_t fault_pc, fault_lr, fault_cfsr, fault_mmfar, fault_bfar;
static volatile bool fault_occurred = false;

void __attribute__((naked)) isr_hardfault(void)
{
    __asm volatile (
        "MRS r0, MSP\n"
        "B hardfault_handler_c\n"
    );
}

void __attribute__((used)) hardfault_handler_c(uint32_t *stack)
{
    fault_pc = stack[6];
    fault_lr = stack[5];
    fault_cfsr = *(volatile uint32_t *)0xE000ED28;
    fault_mmfar = *(volatile uint32_t *)0xE000ED34;
    fault_bfar = *(volatile uint32_t *)0xE000ED38;
    fault_occurred = true;

    /* Re-enable interrupts so USB can flush */
    __asm volatile ("CPSIE i");

    for (int attempt = 0; attempt < 20; attempt++) {
        printf("!FAULT! PC=%08lx LR=%08lx CFSR=%08lx MMFAR=%08lx BFAR=%08lx stk=%lu\n",
               (unsigned long)fault_pc, (unsigned long)fault_lr,
               (unsigned long)fault_cfsr, (unsigned long)fault_mmfar,
               (unsigned long)fault_bfar, (unsigned long)check_stack_free());
        for (volatile int d = 0; d < 2000000; d++) {} /* busy delay */
    }

    while (1) {
        gpio_put(PICO_DEFAULT_LED_PIN, 1);
        for (volatile int d = 0; d < 500000; d++) {}
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        for (volatile int d = 0; d < 500000; d++) {}
    }
}

int main(void)
{
    /* Switch to large stack before doing anything else */
    __asm volatile ("MSR MSP, %0" :: "r" (big_stack + sizeof(big_stack)));
    real_main();
    __builtin_unreachable();
}

static void real_main(void)
{
    set_sys_clock_khz(252000, true);
    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    paint_stack();
    sleep_ms(500);

    /* Show test pattern immediately so HDMI has valid pixel data from first scanline */
    generate_test_pattern();

    /* Init HDMI in DVI mode (no audio packets needed — more stable link) */
    hstx_di_queue_init();
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    video_output_set_dvi_mode(true);
    video_output_set_scanline_callback(scanline_callback);
    video_output_set_vsync_callback(vsync_cb);
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    /* Wait for USB serial */
    for (int i = 0; i < 50; i++) {
        if (stdio_usb_connected()) break;
        sleep_ms(100);
        feed_audio();
    }
    printf("\n=== murmnes (QuickNES) ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    feed_audio();

    /* Init QuickNES */
    printf("qnes_init...\n");
    if (qnes_init(SAMPLE_RATE) != 0)
        error_loop("qnes_init failed");
    printf("qnes_init OK\n");

    feed_audio();

#ifdef HAS_NES_ROM
    long rom_size = (long)(nes_rom_end - nes_rom_data);
    printf("ROM size: %ld bytes\n", rom_size);
    printf("ROM addr: %p\n", (const void *)nes_rom_data);

    feed_audio();

    printf("qnes_load_rom...\n");
    if (qnes_load_rom(nes_rom_data, rom_size) != 0)
        error_loop("qnes_load_rom failed");
    printf("ROM loaded OK\n");

    feed_audio();

    /*
     * Main emulation loop — vsync-synchronized.
     *
     * Timing: vsync fires at start of vblank (~1.4ms before active video).
     * We start emulate_frame() immediately → PPU renders top-to-bottom in ~2ms.
     * By the time active video reads line 0, the PPU is already past line ~170.
     * Result: no tearing without double buffering.
     */
    uint32_t frame_count = 0;

    while (1) {
        /* Wait for vsync (vblank start), with timeout fallback */
        for (int wait = 0; !vsync_flag && wait < 20000; wait++)
            __wfe();
        vsync_flag = 0;

        /* Emulate one NES frame — PPU writes pixels top-to-bottom */
        qnes_emulate_frame(0, 0);

        /* Update palette and pixel pointer (atomic pointer swap) */
        update_palette();
        frame_pitch = 272;
        frame_pixels = qnes_get_pixels();

        frame_count++;
        if (frame_count % 300 == 0) {
            printf("f%lu stk=%lu\n",
                   (unsigned long)frame_count,
                   (unsigned long)check_stack_free());
        }
    }
#else
    printf("No ROM embedded. Showing test pattern.\n");
    generate_test_pattern();
    while (1) {
        feed_audio();
        sleep_ms(16);
    }
#endif
}
