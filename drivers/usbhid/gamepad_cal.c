/*
 * USB gamepad calibration persistence
 *
 * File format (subset of the in-repo capture logs):
 *
 *   # frank-snes gamepad profile
 *   Source=HID
 *   VID=0x046D
 *   PID=0xC219
 *   A=byte[5]:+0x20
 *   B=byte[5]:+0x40
 *
 * The `A=` / `B=` syntax matches scripts/gen_gamepad_maps.py so a user
 * can commit a learned file into gamepads/ and regenerate the compiled
 * tables. The parser is intentionally lenient: any line that does not
 * match the expected patterns is skipped.
 *
 * SPDX-License-Identifier: MIT
 */

#include "gamepad_cal.h"
#include "usbhid.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CAL_DIR "/nes/gamepads"
#define LINE_MAX 160

// Parse a value of the form "byte[<idx>]:+0xMM" (the `+` signals "bit
// goes from 0 to 1"; `-` would mean "bit clears" — we record both the
// same since the runtime compares via XOR against baseline).
static int parse_bit_spec(const char *s, uint8_t *byte_idx, uint8_t *mask) {
    const char *p = strstr(s, "byte[");
    if (!p) return 0;
    p += 5;
    char *endp = NULL;
    long b = strtol(p, &endp, 10);
    if (!endp || *endp != ']' || b < 0 || b > 31) return 0;
    p = strchr(endp, ':');
    if (!p) return 0;
    p++;
    if (*p == '+' || *p == '-' || *p == '=') p++;
    long m = strtol(p, NULL, 0);
    if (m <= 0 || m > 0xFF) return 0;
    *byte_idx = (uint8_t)b;
    *mask     = (uint8_t)m;
    return 1;
}

static int parse_hex_u16(const char *s, uint16_t *out) {
    const char *p = strchr(s, '=');
    if (!p) return 0;
    long v = strtol(p + 1, NULL, 0);
    if (v < 0 || v > 0xFFFF) return 0;
    *out = (uint16_t)v;
    return 1;
}

// Parses one /snes/gamepads/*.txt file. Returns 1 if a complete A+B
// pair was registered, 0 otherwise.
static int parse_profile_file(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return 0;

    char line[LINE_MAX];
    uint16_t vid = 0, pid = 0;
    usbhid_gp_src_t src = USBHID_GP_SRC_HID;
    uint8_t a_byte = 0, a_mask = 0;
    uint8_t b_byte = 0, b_mask = 0;
    int have_vid = 0, have_pid = 0, have_a = 0, have_b = 0;

    while (f_gets(line, sizeof(line), &f)) {
        // Trim trailing whitespace.
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                       line[len - 1] == ' '  || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') continue;

        if (strncasecmp(line, "VID=", 4) == 0 && parse_hex_u16(line, &vid)) {
            have_vid = 1;
        } else if (strncasecmp(line, "PID=", 4) == 0 && parse_hex_u16(line, &pid)) {
            have_pid = 1;
        } else if (strncasecmp(line, "Source=", 7) == 0) {
            if (strcasecmp(line + 7, "XINPUT") == 0) src = USBHID_GP_SRC_XINPUT;
            else                                      src = USBHID_GP_SRC_HID;
        } else if (strncasecmp(line, "A=", 2) == 0) {
            if (parse_bit_spec(line + 2, &a_byte, &a_mask)) have_a = 1;
        } else if (strncasecmp(line, "B=", 2) == 0) {
            if (parse_bit_spec(line + 2, &b_byte, &b_mask)) have_b = 1;
        }
    }
    f_close(&f);

    if (have_vid && have_pid && have_a && have_b) {
        usbhid_learned_ab_add(src, vid, pid, a_byte, a_mask, b_byte, b_mask);
        printf("gamepad_cal: loaded %s VID=0x%04X PID=0x%04X A=byte[%u]:0x%02X B=byte[%u]:0x%02X\n",
               src == USBHID_GP_SRC_XINPUT ? "XINPUT" : "HID",
               vid, pid, a_byte, a_mask, b_byte, b_mask);
        return 1;
    }
    return 0;
}

void gamepad_cal_load_all(void) {
    DIR dir;
    FRESULT fr = f_opendir(&dir, CAL_DIR);
    if (fr != FR_OK) {
        // Missing directory is expected on a fresh SD card.
        return;
    }
    FILINFO fi;
    for (;;) {
        fr = f_readdir(&dir, &fi);
        if (fr != FR_OK || fi.fname[0] == '\0') break;
        if (fi.fattrib & AM_DIR) continue;
        size_t n = strlen(fi.fname);
        if (n < 5) continue;
        const char *ext = fi.fname + n - 4;
        if (strcasecmp(ext, ".txt") != 0) continue;

        char path[64];
        snprintf(path, sizeof(path), CAL_DIR "/%s", fi.fname);
        parse_profile_file(path);
    }
    f_closedir(&dir);
}

int gamepad_cal_clear(void) {
    int removed = 0;
    DIR dir;
    FRESULT fr = f_opendir(&dir, CAL_DIR);
    if (fr == FR_OK) {
        FILINFO fi;
        for (;;) {
            fr = f_readdir(&dir, &fi);
            if (fr != FR_OK || fi.fname[0] == '\0') break;
            if (fi.fattrib & AM_DIR) continue;
            size_t n = strlen(fi.fname);
            if (n < 5) continue;
            if (strcasecmp(fi.fname + n - 4, ".txt") != 0) continue;
            /* Only touch files that look like our own output so we
             * don't nuke unrelated user files someone parked here. */
            if (strncasecmp(fi.fname, "gamepad_", 8) != 0) continue;

            char path[64];
            snprintf(path, sizeof(path), CAL_DIR "/%s", fi.fname);
            if (f_unlink(path) == FR_OK) removed++;
        }
        f_closedir(&dir);
    }

    /* Drop the in-memory table and reseed any live slots so their menu
     * A/B reverts to the compiled-in map (or triggers the wizard). */
    usbhid_learned_ab_clear();

    printf("gamepad_cal: cleared %d file(s) from " CAL_DIR "\n", removed);
    return removed;
}

int gamepad_cal_save(const char *source,
                     uint16_t vid, uint16_t pid,
                     uint8_t a_byte, uint8_t a_mask,
                     uint8_t b_byte, uint8_t b_mask) {
    // Ensure directory tree exists. Either f_mkdir succeeds or returns
    // FR_EXIST — both are fine; anything else means SD is not writable.
    f_mkdir("/nes");
    f_mkdir(CAL_DIR);

    char path[64];
    snprintf(path, sizeof(path), CAL_DIR "/gamepad_%04X_%04X.txt", vid, pid);

    FIL f;
    FRESULT fr = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("gamepad_cal: f_open(%s) failed (%d)\n", path, fr);
        return 0;
    }

    char buf[256];
    int n = snprintf(buf, sizeof(buf),
                     "# frank-nes gamepad profile\n"
                     "Source=%s\n"
                     "VID=0x%04X\n"
                     "PID=0x%04X\n"
                     "A=byte[%u]:+0x%02X\n"
                     "B=byte[%u]:+0x%02X\n",
                     source, vid, pid,
                     a_byte, a_mask, b_byte, b_mask);
    UINT written = 0;
    f_write(&f, buf, (UINT)n, &written);
    f_close(&f);
    printf("gamepad_cal: saved %s\n", path);
    return written == (UINT)n ? 1 : 0;
}
