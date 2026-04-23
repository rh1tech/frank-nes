/*
 * FRANK NES - NES Emulator for RP2350
 * Board configuration: DV (Pimoroni Pico DV)
 *
 * PIO-HDMI only (HDMI_BASE_PIN=6). No TV/VGA.
 */

#ifndef BOARD_DV_H
#define BOARD_DV_H

/* PS/2 Keyboard */
#define KBD_CLOCK_PIN 0
#define KBD_DATA_PIN  1
#define PS2_PIN_CLK   0
#define PS2_PIN_DATA  1

/* SD Card (PIO-SPI) */
#define SDCARD_PIN_SPI0_SCK  5
#define SDCARD_PIN_SPI0_MOSI 18
#define SDCARD_PIN_SPI0_MISO 19
#define SDCARD_PIN_SPI0_CS   22

/* NES Gamepad */
#define NESPAD_CLK_PIN   14
#define NESPAD_LATCH_PIN 15
#define NESPAD_DATA_PIN  20

/* I2S Audio */
#define HAS_I2S 1
#define I2S_DATA_PIN       26
#define I2S_CLOCK_PIN_BASE 27

/* PWM Audio */
#define PWM_PIN0 26
#define PWM_PIN1 27

/* No TV/VGA */

/* PSRAM on GP47 */
#define PSRAM_CS_PIN_RP2350B 47
#define PSRAM_CS_PIN_RP2350A 47

/* No UART logging (GPIO 0 is KBD) */
#define NO_UART_LOGGING 1

/* Video: PIO-HDMI only (GPIO 6-13) */
#define HDMI_BASE_PIN 6
#define VGA_BASE_PIN  6

#endif /* BOARD_DV_H */
