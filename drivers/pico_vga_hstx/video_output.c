/*
 * FRANK NES - NES Emulator for RP2350
 * VGA HSTX driver (M2 platform only).
 *
 * Adapted from drivers/pico_hdmi/video_output.c by stripping HDMI-specific
 * features (TMDS encoding, Data Islands, audio, AVI InfoFrame) and routing
 * the HSTX output as plain 8-bit parallel video with HS/VS sync.
 *
 * Pin layout (matches drivers/hdmi_pio/vga.c, GPIO 12..19):
 *   GPIO 12..17 = RGB222 (B0 B1 G0 G1 R0 R1)
 *   GPIO 18     = HS (active low)
 *   GPIO 19     = VS (active low)
 *
 * HSTX configuration: identical to the HDMI driver for clocking, so that
 * clk_hstx / CLKDIV = 25.2 MHz pixel clock. Differences vs HDMI:
 *   - No TMDS lane expansion. Every BIT register maps 1 GPIO -> 1 shift
 *     register bit (identity mapping of low 8 bits).
 *   - All command-list entries are RAW. Active data uses HSTX_CMD_RAW; the
 *     pixel buffer stores one pixel byte per 32-bit FIFO word (in low 8 bits).
 *   - No Data Islands, no ACR, no AVI InfoFrame, no audio.
 *
 * SPDX-License-Identifier: Unlicense
 */

#include "pico_vga_hstx/video_output.h"
#include "pico_vga_hstx/hstx_pins.h"

#include "pico/stdlib.h"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include <string.h>

// ============================================================================
// HSTX command words
// ============================================================================

#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ============================================================================
// VGA sync-state patterns (low 8 bits of each FIFO word)
// ============================================================================
//
// HS and VS are active low. Quiescent (both idle) = 0xC0.
// These bytes are replicated into all 4 bytes of each 32-bit FIFO word so
// the HSTX output is stable whichever byte the shift register currently
// presents to SEL_P/SEL_N.

#define VGA_BYTE_IDLE     ((uint8_t)((1u << VGA_HSTX_HS_BIT) | (1u << VGA_HSTX_VS_BIT)))  // 0xC0
#define VGA_BYTE_HSYNC    ((uint8_t)((0u << VGA_HSTX_HS_BIT) | (1u << VGA_HSTX_VS_BIT)))  // 0x80
#define VGA_BYTE_VSYNC    ((uint8_t)((1u << VGA_HSTX_HS_BIT) | (0u << VGA_HSTX_VS_BIT)))  // 0x40
#define VGA_BYTE_VHSYNC   ((uint8_t)((0u << VGA_HSTX_HS_BIT) | (0u << VGA_HSTX_VS_BIT)))  // 0x00

#define WORD4(b)          (((uint32_t)(b)) * 0x01010101u)

#define SYNC_IDLE         WORD4(VGA_BYTE_IDLE)
#define SYNC_HSYNC        WORD4(VGA_BYTE_HSYNC)
#define SYNC_VSYNC        WORD4(VGA_BYTE_VSYNC)
#define SYNC_VHSYNC       WORD4(VGA_BYTE_VHSYNC)

// ============================================================================
// State
// ============================================================================

uint16_t frame_width = 0;
uint16_t frame_height = 0;
volatile uint32_t video_frame_count = 0;

// RGB565 pixels written by the scanline callback (per-slot scratch buffer).
// One per ring slot so the producer and IRQ consumer never touch the same
// slot at the same time.
#define LINE_RING_SLOTS 4
static uint32_t line_buffer[LINE_RING_SLOTS][MODE_H_ACTIVE_PIXELS / 2]
    __attribute__((aligned(4)));

// FIFO-ready pixel data: 1 pixel per 32-bit word (byte replicated to all 4
// bytes so SEL_P sees the same byte regardless of rotation phase).
//
// Producer/consumer synchronize via monotonically advancing sequence
// numbers. Each sequence number names one line-doubled producer line (NES
// line × 2 = VGA active line pair). Slot index = seq % LINE_RING_SLOTS.
//
// pixel_ring_seq[slot] holds the sequence number of the data currently in
// that slot, or (uint32_t)-1 if empty. The consumer matches on strict
// equality, so stale data can never be mistaken for a fresh line.
//
// Critically, NEITHER counter is ever reset — the IRQ used to clear ring
// state at vsync start, which races with the producer's three-write
// commit sequence and occasionally left the ring in a wedged state where
// the consumer served black indefinitely until the monitor lost sync.
// With monotonic counters the producer catches up to the consumer after
// any stall by skipping forward, no cross-core reset needed.
static uint32_t pixel_ring[LINE_RING_SLOTS][MODE_H_ACTIVE_PIXELS]
    __attribute__((aligned(4)));
static volatile uint32_t pixel_ring_seq[LINE_RING_SLOTS];
static volatile uint32_t produce_seq;  // producer's next seq to fill
static volatile uint32_t consume_seq;  // consumer's next seq to display

// Number of producer lines per frame (480 VGA active / 2 doubling).
#define PRODUCER_LINES_PER_FRAME (MODE_V_ACTIVE_LINES / 2)

// Solid-black filler used when the ring underflows.
static uint32_t black_line[MODE_H_ACTIVE_PIXELS] __attribute__((aligned(4)));

static uint32_t v_scanline = 2;
static bool vactive_cmdlist_posted = false;
static bool dma_pong = false;

static video_output_task_fn background_task = NULL;
static video_output_scanline_cb_t scanline_callback = NULL;
static video_output_vsync_cb_t vsync_callback = NULL;

#define DMACH_PING 0
#define DMACH_PONG 1

// ============================================================================
// Command Lists
// ============================================================================

// VBlank, VS inactive: idle front porch, HS pulse, idle back porch + "active".
static uint32_t vblank_line_vsync_off[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,                     SYNC_IDLE,   HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,                      SYNC_HSYNC,  HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                                                  SYNC_IDLE,   HSTX_CMD_NOP
};

// VBlank, VS active: VS asserted throughout, HS pulse as usual.
static uint32_t vblank_line_vsync_on[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,                     SYNC_VSYNC,  HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,                      SYNC_VHSYNC, HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS),
                                                                  SYNC_VSYNC,  HSTX_CMD_NOP
};

// Active line header (front porch, HS pulse, back porch, then RAW pixels).
// The RAW command is followed by DMA-fed pixel data (next DMA transfer).
static uint32_t vactive_line_header[] = {
    HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH,                     SYNC_IDLE,   HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH,                      SYNC_HSYNC,  HSTX_CMD_NOP,
    HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH,                      SYNC_IDLE,
    HSTX_CMD_RAW | MODE_H_ACTIVE_PIXELS
};

// ============================================================================
// RGB565 -> VGA byte conversion
// ============================================================================
//
// Packed two pixels per uint32_t (the HDMI driver's line_buffer convention).
//   word[15: 0] = pixel 2k   (RGB565 in low halfword)
//   word[31:16] = pixel 2k+1 (RGB565 in high halfword)
//
// RGB565 layout: RRRRR GGGGGG BBBBB
// Take top 2 bits of each channel, OR with VGA_BYTE_IDLE (HS=VS=1).

static inline uint32_t __scratch_x("vga") rgb565_to_vga_word(uint16_t rgb565)
{
    uint32_t r = (rgb565 >> 14) & 0x3;
    uint32_t g = (rgb565 >> 9) & 0x3;
    uint32_t b = (rgb565 >> 3) & 0x3;
    uint8_t byte = (uint8_t)(VGA_BYTE_IDLE | (r << 4) | (g << 2) | b);
    return WORD4(byte);
}

static __attribute__((noinline)) void __scratch_x("vga") convert_line_rgb565_to_vga(const uint32_t *src, uint32_t *dst)
{
    for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++) {
        uint32_t w = src[i];
        dst[2 * i + 0] = rgb565_to_vga_word((uint16_t)(w & 0xFFFF));
        dst[2 * i + 1] = rgb565_to_vga_word((uint16_t)(w >> 16));
    }
}

#ifdef VGA_HSTX_TEST_PATTERN
// Paint 8 vertical color bars (SMPTE-style) into the active-pixel buffer.
// Byte format: HS=1 VS=1 (idle) | RR GG BB. RGB colors are RGB222 so each
// channel value is in 0..3. Use max-brightness primaries:
//   white, yellow, cyan, green, magenta, red, blue, black
static __attribute__((noinline)) void __scratch_x("vga") paint_test_pattern(uint32_t *dst, uint32_t active_line)
{
    (void)active_line;
    static const uint8_t bar_rgb222[8] = {
        (3 << 4) | (3 << 2) | 3, // white
        (3 << 4) | (3 << 2) | 0, // yellow
        (0 << 4) | (3 << 2) | 3, // cyan
        (0 << 4) | (3 << 2) | 0, // green
        (3 << 4) | (0 << 2) | 3, // magenta
        (3 << 4) | (0 << 2) | 0, // red
        (0 << 4) | (0 << 2) | 3, // blue
        (0 << 4) | (0 << 2) | 0, // black
    };
    const uint32_t bar_width = MODE_H_ACTIVE_PIXELS / 8;
    for (uint32_t bar = 0; bar < 8; bar++) {
        uint8_t byte = (uint8_t)(VGA_BYTE_IDLE | bar_rgb222[bar]);
        uint32_t word = WORD4(byte);
        uint32_t start = bar * bar_width;
        uint32_t end = start + bar_width;
        for (uint32_t x = start; x < end; x++)
            dst[x] = word;
    }
}
#endif

// ============================================================================
// Scanline state
// ============================================================================

typedef struct {
    bool vsync_active;
    bool front_porch;
    bool back_porch;
    bool active_video;
    uint32_t active_line;
} scanline_state_t;

static inline void __scratch_x("vga") get_scanline_state(uint32_t line, scanline_state_t *state)
{
    state->vsync_active = (line >= MODE_V_FRONT_PORCH && line < (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH));
    state->front_porch = (line < MODE_V_FRONT_PORCH);
    state->back_porch = (line >= MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH &&
                        line < MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH);
    state->active_video = (!state->vsync_active && !state->front_porch && !state->back_porch);
    state->active_line = state->active_video ? (line - (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES)) : 0;
}

// ============================================================================
// DMA IRQ handler
// ============================================================================

void __scratch_x("vga") dma_irq_handler(void)
{
    uint32_t ch_num = dma_pong ? DMACH_PONG : DMACH_PING;
    dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
    dma_hw->intr = 1U << ch_num;
    dma_pong = !dma_pong;

    scanline_state_t s;
    get_scanline_state(v_scanline, &s);

    if (s.vsync_active) {
        ch->read_addr = (uintptr_t)vblank_line_vsync_on;
        ch->transfer_count = count_of(vblank_line_vsync_on);
        if (v_scanline == MODE_V_FRONT_PORCH) {
            video_frame_count++;
            if (vsync_callback)
                vsync_callback();
        }
    } else if (s.active_video && !vactive_cmdlist_posted) {
        // Post the active-line header. The pixel buffer for this line has
        // already been converted by the background task (or we fall back to
        // black on underflow).
        ch->read_addr = (uintptr_t)vactive_line_header;
        ch->transfer_count = count_of(vactive_line_header);
        vactive_cmdlist_posted = true;
    } else if (s.active_video && vactive_cmdlist_posted) {
        // Follow-up DMA for the active pixel data.
        //
        // Each converted line is displayed twice (line-doubled). We consume
        // the slot whose sequence number matches our expected consume_seq;
        // on the odd VGA line we then advance consume_seq so the next pair
        // picks up the next producer line. On underflow (producer hasn't
        // filled it yet) we fall back to black_line AND advance consume_seq
        // anyway, so the consumer never sticks waiting for a slot the
        // producer has already skipped past.
        uint32_t *pixbuf = black_line;
        uint32_t want = consume_seq;
        uint32_t slot = want % LINE_RING_SLOTS;
        if (pixel_ring_seq[slot] == want) {
            pixbuf = pixel_ring[slot];
        }
        if (s.active_line & 1) {
            // odd VGA line: advance to next producer line next time
            consume_seq = want + 1;
        }
        ch->read_addr = (uintptr_t)pixbuf;
        ch->transfer_count = MODE_H_ACTIVE_PIXELS;
        vactive_cmdlist_posted = false;
    } else {
        // Blanking (front porch or back porch), VS inactive.
        ch->read_addr = (uintptr_t)vblank_line_vsync_off;
        ch->transfer_count = count_of(vblank_line_vsync_off);
    }

    if (!vactive_cmdlist_posted)
        v_scanline = (v_scanline + 1) % MODE_V_TOTAL_LINES;
}

// ============================================================================
// Public interface
// ============================================================================

void video_output_init(uint16_t width, uint16_t height)
{
    frame_width = width;
    frame_height = height;

    // Run clk_hstx at clk_sys (divider 1). With clk_sys = 252 MHz this gives
    // clk_hstx = 252 MHz, which combined with CSR.CLKDIV=4 and
    // CSR.N_SHIFTS = phase_repeats yields the 25.2 MHz VGA pixel clock for
    // 640x480 @ 60 Hz (phase_repeats = clk_hstx / pixelclock = 10).
    uint32_t sys_freq = clock_get_hz(clk_sys);
    clock_configure_int_divider(clk_hstx, 0, CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, sys_freq, 1);

    dma_channel_claim(DMACH_PING);
    dma_channel_claim(DMACH_PONG);

    // Solid-black fallback line for ring underflows.
    for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS; i++)
        black_line[i] = SYNC_IDLE;

    // Ring state: mark every slot empty (sequence number that can never be
    // requested). Producer and consumer both start at 0.
    for (uint32_t i = 0; i < LINE_RING_SLOTS; i++)
        pixel_ring_seq[i] = (uint32_t)-1;
    produce_seq = 0;
    consume_seq = 0;
}

// Producer: convert the next scheduled active line if ring has space. Called
// from Core 1's background loop on every iteration. Does nothing if the ring
// is full; jumps forward if the consumer has raced ahead of us (stall
// recovery — keeps the consumer/producer seqs aligned to mod-4 slots).
static __attribute__((noinline)) void __scratch_x("vga") producer_task(void)
{
    uint32_t p_seq = produce_seq;
    uint32_t c_seq = consume_seq;

    // If the consumer has moved past us (we stalled long enough that several
    // scanlines passed without their slot being filled), jump forward to
    // c_seq. The consumer will have already served black for the missed
    // lines; now we need to make sure we don't re-fill old slots with data
    // destined for scanlines that are already gone.
    if ((int32_t)(c_seq - p_seq) > 0) {
        p_seq = c_seq;
    }

    // Don't get more than LINE_RING_SLOTS-1 ahead (leaves one slot free so
    // the slot the consumer is currently reading won't be overwritten).
    if ((int32_t)(p_seq - c_seq) >= LINE_RING_SLOTS)
        return;

    // Render modulo the frame: seq % PRODUCER_LINES_PER_FRAME gives the NES
    // line to pull from. The consumer's seq matches because both march at
    // the same rate (one increment per active-line pair).
    uint32_t active_line = p_seq % PRODUCER_LINES_PER_FRAME;
    uint32_t slot = p_seq % LINE_RING_SLOTS;
    uint32_t *line16 = line_buffer[slot];

#if defined(VGA_HSTX_TEST_PATTERN)
    (void)active_line;
    paint_test_pattern(pixel_ring[slot], active_line);
#elif defined(VGA_HSTX_NOOP_ACTIVE)
    (void)line16;
    memcpy(pixel_ring[slot], black_line, sizeof(black_line));
#elif defined(VGA_HSTX_RING_TEST)
    // Diagnostic: producer writes solid magenta via the ring, bypassing
    // both the scanline callback and the RGB565 conversion. Verifies that
    // the ring and IRQ consumer are wired correctly.
    (void)line16;
    (void)active_line;
    uint8_t magenta = (uint8_t)(VGA_BYTE_IDLE | (3 << 4) | (0 << 2) | 3);
    uint32_t word = WORD4(magenta);
    for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS; i++)
        pixel_ring[slot][i] = word;
#elif defined(VGA_HSTX_CONVERT_TEST)
    // Diagnostic: fill line16 with a known RGB565 constant (red), then
    // run the actual conversion function. Verifies convert_line_rgb565_to_vga
    // in isolation from scanline_callback.
    (void)active_line;
    for (uint32_t i = 0; i < MODE_H_ACTIVE_PIXELS / 2; i++)
        line16[i] = 0xF800F800u;  // two pure red RGB565 pixels packed
    convert_line_rgb565_to_vga(line16, pixel_ring[slot]);
#else
    // Line doubled: producer's `active_line` (0..239) maps to VGA active
    // lines `active_line*2` and `active_line*2+1`. Feed the callback the
    // first of the two since existing scanline callbacks divide active_line
    // by 2 to get their source line.
    //
    // Zero the whole line_buffer first so any pixels the host callback
    // doesn't touch (e.g. the 64 left/right border pixels around a 256-wide
    // NES frame) render as solid black instead of leaking stale contents
    // from the previous use of this ring slot.
    memset(line16, 0, MODE_H_ACTIVE_PIXELS * sizeof(uint16_t));
    uint32_t vga_active_line = active_line * 2;
    uint32_t v_total_scanline = vga_active_line + (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES);
    if (scanline_callback) {
        scanline_callback(v_total_scanline, vga_active_line, line16);
    }
    convert_line_rgb565_to_vga(line16, pixel_ring[slot]);
#endif

    // Publish: write the sequence number LAST, and after a memory barrier,
    // so the consumer observing pixel_ring_seq[slot] == want is guaranteed
    // to see fully-written pixel data in pixel_ring[slot].
    __compiler_memory_barrier();
    pixel_ring_seq[slot] = p_seq;
    produce_seq = p_seq + 1;
}

void video_output_set_background_task(video_output_task_fn task)
{
    background_task = task;
}

void video_output_set_scanline_callback(video_output_scanline_cb_t cb)
{
    scanline_callback = cb;
}

void video_output_set_vsync_callback(video_output_vsync_cb_t cb)
{
    vsync_callback = cb;
}

void pico_hdmi_set_audio_sample_rate(uint32_t sample_rate)
{
    (void)sample_rate;  // VGA has no audio path.
}

void video_output_core1_run(void)
{
    // HSTX core config for VGA 640x480 @ 60 Hz (quakegeneric dvi_hstx reference):
    //
    //   clk_hstx = 252 MHz (direct from clk_sys)
    //   CSR.CLKDIV = 4      → serial output bit rate = clk_hstx / 4 = 63 MHz
    //   CSR.N_SHIFTS = 10   → pop FIFO every 10 shifts → pixel clock = 63 / (10/... )
    //                         actual: pixel rate = clk_hstx / (CLKDIV * N_SHIFTS / CSR.SHIFT-stride)
    //                         empirically 25.2 MHz pixel clock with these values.
    //   CSR.SHIFT = 16      → shift register rotates by 16 each serial cycle.
    //
    // Pin mapping uses SEL_P=i, SEL_N=i+8: first half of the HSTX clock
    // period drives bit i, second half drives bit i+8. Because the shift
    // register rotates by 16 per serial cycle, bits 0..15 of each pixel's
    // 32-bit word encode the output pattern for that whole pixel period.
    //
    // The sync bytes we emit are replicated across all four bytes of the
    // 32-bit word (see WORD4), so every byte position presents the same
    // HS/VS/RGB bits to the output regardless of rotation phase.
    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr = HSTX_CTRL_CSR_EXPAND_EN_BITS |
                        (4u  << HSTX_CTRL_CSR_CLKDIV_LSB) |
                        (10u << HSTX_CTRL_CSR_N_SHIFTS_LSB) |
                        (16u << HSTX_CTRL_CSR_SHIFT_LSB) |
                        HSTX_CTRL_CSR_EN_BITS;

    // Command expander: RAW / RAW_REPEAT emit one 32-bit word per pixel.
    hstx_ctrl_hw->expand_shift =
        (1u << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB) |
        (0u << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB) |
        (1u << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB) |
        (0u << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB);

    // Pin -> shift register bit mapping. Each GPIO selects bit n for the
    // first half of the HSTX serial period and bit n+8 for the second half.
    // (See quakegeneric drivers/dvi_hstx/dvi.c :: vga_configure_hstx_output.)
    //   GPIO 12 = B0 -> bits 0 / 8
    //   GPIO 13 = B1 -> bits 1 / 9
    //   GPIO 14 = G0 -> bits 2 / 10
    //   GPIO 15 = G1 -> bits 3 / 11
    //   GPIO 16 = R0 -> bits 4 / 12
    //   GPIO 17 = R1 -> bits 5 / 13
    //   GPIO 18 = HS -> bits 6 / 14
    //   GPIO 19 = VS -> bits 7 / 15
    for (uint32_t i = 0; i < VGA_HSTX_PIN_COUNT; i++) {
        hstx_ctrl_hw->bit[i] = (i << HSTX_CTRL_BIT0_SEL_P_LSB) |
                               ((i + 8) << HSTX_CTRL_BIT0_SEL_N_LSB);
    }

    for (uint32_t i = 0; i < VGA_HSTX_PIN_COUNT; i++) {
        gpio_set_function(VGA_HSTX_PIN_BASE + i, 0);  // HSTX function
    }

    // DMA: ping-pong chained channels feeding HSTX FIFO via DREQ_HSTX.
    // Both marked high-priority so the HSTX FIFO refill always wins bus
    // arbitration against any DMA Core 0 might run (SD card, I2S audio).
    dma_channel_config c = dma_channel_get_default_config(DMACH_PING);
    channel_config_set_chain_to(&c, DMACH_PONG);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(DMACH_PING, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    c = dma_channel_get_default_config(DMACH_PONG);
    channel_config_set_chain_to(&c, DMACH_PING);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(DMACH_PONG, &c, &hstx_fifo_hw->fifo, vblank_line_vsync_off,
                          count_of(vblank_line_vsync_off), false);

    dma_hw->ints0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    dma_hw->inte0 = (1U << DMACH_PING) | (1U << DMACH_PONG);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_priority(DMA_IRQ_0, 0);
    irq_set_enabled(DMA_IRQ_0, true);

    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_W_BITS | BUSCTRL_BUS_PRIORITY_DMA_R_BITS |
                            BUSCTRL_BUS_PRIORITY_PROC1_BITS;
    dma_channel_start(DMACH_PING);

    while (1) {
        // Pre-convert the next active line ahead of the DMA IRQ so the ISR
        // stays short. Runs as fast as Core 1 can; throttled by the ring
        // being full.
        producer_task();
        if (background_task) {
            background_task();
        }
    }
}
