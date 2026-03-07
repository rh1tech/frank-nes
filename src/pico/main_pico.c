/*
 * murmnes - NES Emulator for RP2350
 *
 * Core 0: NES emulation + audio queue feeding
 * Core 1: HDMI video output via HSTX
 */

#include "cart.h"
#include "cpu.h"
#include "ppu.h"
#include "cpu_ppu_interface.h"
#include "cpu_mapper_interface.h"

#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/hstx_packet.h"
#include "pico_hdmi/video_output.h"

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "hardware/clocks.h"
#include "hardware/vreg.h"

#include "nespad.h"

#include <stdio.h>
#include <string.h>

// ============================================================================
// Configuration
// ============================================================================

#define FRAME_WIDTH 640
#define FRAME_HEIGHT 480

// NES framebuffer (defined in ppu.c)
extern uint32_t pixels[256 * 240];

// NES controller button masks
#define A_BUTTON      0x01U
#define B_BUTTON      0x02U
#define SELECT_BUTTON 0x04U
#define START_BUTTON  0x08U
#define UP_BUTTON     0x10U
#define DOWN_BUTTON   0x20U
#define LEFT_BUTTON   0x40U
#define RIGHT_BUTTON  0x80U

// ============================================================================
// Audio — feed silence to keep HDMI data island queue stable
// ============================================================================

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

// ============================================================================
// HDMI Scanline Callback (Core 1 DMA ISR context)
// ============================================================================

// 480p: NES 256x240 doubled to 512x480, centered with 64px black borders
void __not_in_flash("scanline") scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;
    uint32_t nes_line = active_line < 480 ? active_line / 2 : 0;
    const uint32_t *src = &pixels[nes_line * 256];
    // Write all 320 uint32_t words (640 pixels)
    for (int i = 0; i < 320; i++) {
        if (i < 32 || i >= 288) {
            dst[i] = 0; // Black border
        } else {
            uint32_t p = src[i - 32];
            uint32_t c = ((p >> 8) & 0xF800) | ((p >> 5) & 0x07E0) | ((p >> 3) & 0x001F);
            dst[i] = c | (c << 16);
        }
    }
}

// ============================================================================
// CPU-PPU Timing (duplicated from emu.c to avoid SDL2 dependency)
// ============================================================================

static void ppu_vblank_warmup_seq(const Cpu6502 *cpu)
{
    static unsigned count = 0;
    if (!count) {
        clear_ppu_status_vblank_bit(cpu->cpu_ppu_io);
        ++count;
    } else if ((count == 1) && cpu->cycle >= 27383) {
        set_ppu_status_vblank_bit(cpu->cpu_ppu_io);
        ++count;
    } else if ((count == 2) && cpu->cycle >= 57164) {
        set_ppu_status_vblank_bit(cpu->cpu_ppu_io);
        ++count;
    }
}

static void cpu_ppu_buffered_writes(CpuPpuShare *cpu_ppu_io, Cpu6502 *cpu)
{
    if (cpu_ppu_io->buffer_write) {
        --cpu_ppu_io->buffer_counter;
        if (cpu_ppu_io->buffer_address == 0x2001 && (cpu_ppu_io->buffer_value & 0x08)) {
            if (cpu_ppu_io->buffer_counter == 3)
                cpu_ppu_io->bg_early_enable_mask = true;
        }
        if (cpu_ppu_io->buffer_address == 0x2001 && !(cpu_ppu_io->buffer_value & 0x08)) {
            if (cpu_ppu_io->buffer_counter == 3)
                cpu_ppu_io->bg_early_disable_mask = true;
        }
        if (!cpu_ppu_io->buffer_counter) {
            write_ppu_reg(cpu_ppu_io->buffer_address, cpu_ppu_io->buffer_value, cpu);
            cpu_ppu_io->buffer_write = false;
            cpu_ppu_io->buffer_counter = 6;
            cpu_ppu_io->bg_early_enable_mask = false;
            cpu_ppu_io->bg_early_disable_mask = false;
        }
    }
}

static inline void clock_nes(Cpu6502 *cpu, Ppu2C02 *ppu)
{
    clock_cpu(cpu);
    ppu_vblank_warmup_seq(cpu);
    cpu_ppu_buffered_writes(ppu->cpu_ppu_io, cpu);
    clock_ppu(ppu);
    cpu_ppu_buffered_writes(ppu->cpu_ppu_io, cpu);
    clock_ppu(ppu);
    cpu_ppu_buffered_writes(ppu->cpu_ppu_io, cpu);
    clock_ppu(ppu);
}

// ============================================================================
// NES Gamepad
// ============================================================================

static void poll_nespad_input(Cpu6502 *cpu)
{
    nespad_read();
    uint8_t b = 0;
    if (nespad_state & DPAD_A)      b |= A_BUTTON;
    if (nespad_state & DPAD_B)      b |= B_BUTTON;
    if (nespad_state & DPAD_SELECT) b |= SELECT_BUTTON;
    if (nespad_state & DPAD_START)  b |= START_BUTTON;
    if (nespad_state & DPAD_UP)     b |= UP_BUTTON;
    if (nespad_state & DPAD_DOWN)   b |= DOWN_BUTTON;
    if (nespad_state & DPAD_LEFT)   b |= LEFT_BUTTON;
    if (nespad_state & DPAD_RIGHT)  b |= RIGHT_BUTTON;
    cpu->player_1_controller = b;

    b = 0;
    if (nespad_state2 & DPAD_A)      b |= A_BUTTON;
    if (nespad_state2 & DPAD_B)      b |= B_BUTTON;
    if (nespad_state2 & DPAD_SELECT) b |= SELECT_BUTTON;
    if (nespad_state2 & DPAD_START)  b |= START_BUTTON;
    if (nespad_state2 & DPAD_UP)     b |= UP_BUTTON;
    if (nespad_state2 & DPAD_DOWN)   b |= DOWN_BUTTON;
    if (nespad_state2 & DPAD_LEFT)   b |= LEFT_BUTTON;
    if (nespad_state2 & DPAD_RIGHT)  b |= RIGHT_BUTTON;
    cpu->player_2_controller = b;
}

// ============================================================================
// ROM Loading
// ============================================================================

#ifdef HAS_EMBEDDED_ROM
#include "embedded_rom.h"
#endif

// ============================================================================
// Main (Core 0)
// ============================================================================

int main(void)
{
    // 252 MHz with HSTX_CLK_DIV=2 keeps pixel clock at 25.2 MHz
    set_sys_clock_khz(252000, true);

    stdio_init_all();
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    sleep_ms(1000);

    // Initialize HDMI (must follow bouncing_box pattern exactly)
    hstx_di_queue_init();
    video_output_init(FRAME_WIDTH, FRAME_HEIGHT);
    video_output_set_scanline_callback(scanline_callback);
    generate_silence();
    multicore_launch_core1(video_output_core1_run);
    sleep_ms(100);

    // Wait for USB serial
    for (int i = 0; i < 50; i++) {
        if (stdio_usb_connected()) break;
        sleep_ms(100);
    }

    printf("\n=== murmnes - NES Emulator for RP2350 ===\n");
    printf("sys_clk: %lu Hz\n", (unsigned long)clock_get_hz(clk_sys));

    // Initialize NES gamepad
    uint32_t cpu_khz = clock_get_hz(clk_sys) / 1000;
    if (nespad_begin(cpu_khz, NESPAD_GPIO_CLK, NESPAD_GPIO_DATA, NESPAD_GPIO_LATCH)) {
        printf("NES gamepad OK\n");
    }

    // Initialize NES emulator
    Cartridge *cart = cart_allocator();
    CpuMapperShare *cpu_mapper = cpu_mapper_allocator();
    CpuPpuShare *cpu_ppu = cpu_ppu_io_allocator();
    Cpu6502 *cpu = cpu_allocator();
    Ppu2C02 *ppu = ppu_allocator();

    if (!cart || !cpu_mapper || !cpu_ppu || !cpu || !ppu) {
        printf("FATAL: alloc failed\n");
        while (1) tight_loop_contents();
    }

    cart_init(cart);
    cpu_mapper_init(cpu_mapper, cart);
    cpu_ppu_io_init(cpu_ppu);
    map_ppu_data_to_cpu_ppu_io(cpu_ppu, ppu);
    cpu_init(cpu, 0xC000, cpu_ppu, cpu_mapper);
    ppu_init(ppu, cpu_ppu);

    // Load ROM
    bool rom_loaded = false;
#ifdef HAS_EMBEDDED_ROM
    printf("Loading ROM (%u bytes)...\n", (unsigned)nes_rom_size);
    if (parse_nes_cart_from_buffer(cart, nes_rom_data, nes_rom_size, cpu, ppu) == 0) {
        rom_loaded = true;
        printf("ROM OK\n");
    } else {
        printf("ROM parse failed\n");
    }
#endif

    if (!rom_loaded) {
        printf("No ROM - showing test pattern\n");
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 256; x++)
                pixels[y * 256 + x] = 0xFF000000 | (x << 16) | (y << 8) | ((x+y) & 0xFF);
        while (1) { generate_silence(); tight_loop_contents(); }
    }

    init_pc(cpu);
    printf("Emulation starting (PC=0x%04X)\n", cpu->PC);

    // Main emulation loop
    uint32_t nes_frames = 0;
    uint32_t clocks = 0;
    uint32_t last_report = to_ms_since_boot(get_absolute_time());
    while (1) {
        // Run NES in tight batch (reduce loop overhead)
        for (int batch = 0; batch < 1000; batch++) {
            clock_nes(cpu, ppu);

            if (ppu->scanline == 241 && ppu->cycle == 0) {
                nes_frames++;
                generate_silence();
                poll_nespad_input(cpu);
            }
        }
        clocks += 1000;

        // Print stats every 2 seconds
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_report >= 2000) {
            uint32_t dt = now - last_report;
            printf("NES fps=%lu  clk/s=%lu\n",
                   (unsigned long)(nes_frames * 1000 / dt),
                   (unsigned long)((uint64_t)clocks * 1000 / dt));
            nes_frames = 0;
            clocks = 0;
            last_report = now;
        }
    }

    return 0;
}
