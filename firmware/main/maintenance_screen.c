// Local QR/passkey screen; no network or extra framebuffer allocation.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "maintenance_screen.h"
#include "board.h"
#include "fb2bpp.h"
#include "qrcodegen.h"
#include "font8x8_basic.h"
#include <stdio.h>
#include <string.h>

static void pixel(uint8_t *fb, int x, int y) {
    if (x < 0 || x >= EPD_W || y < 0 || y >= EPD_H) return;
    size_t i = (size_t)(EPD_H - 1 - y) * (EPD_W / 4) + x / 4;
    fb[i] &= (uint8_t)~(3u << (6 - 2 * (x % 4))); // black
}

static void text(uint8_t *fb, int x, int y, const char *s, int scale) {
    for (; *s; s++, x += 8 * scale) {
        const char *glyph = font8x8_basic[(unsigned char)*s & 0x7f];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((glyph[row] >> col) & 1)
                    for (int yy = 0; yy < scale; yy++)
                        for (int xx = 0; xx < scale; xx++)
                            pixel(fb, x + col * scale + xx, y + row * scale + yy);
    }
}

void maintenance_screen_closed(uint8_t *fb) {
    fb_fill(fb, 1);
    text(fb, 24, 78, "Bluetooth closed", 2);
    text(fb, 24, 126, "Hold the button for 3 seconds", 1);
    text(fb, 24, 146, "and release to reconnect.", 1);
    text(fb, 24, 188, "The display will check for its", 1);
    text(fb, 24, 208, "next image when Wi-Fi returns.", 1);
}

bool maintenance_screen_render(uint8_t *fb, const char *payload, uint32_t passkey) {
    if (!fb || !payload || passkey > 999999) return false;
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    if (!qrcodegen_encodeText(payload, tmp, qr, qrcodegen_Ecc_MEDIUM,
                              1, 10, qrcodegen_Mask_AUTO, true)) return false;
    fb_fill(fb, 1);
    text(fb, 16, 18, "Bluetooth", 2);
    text(fb, 16, 42, "Maintenance", 2);
    text(fb, 16, 84, "Open Tesserae", 1);
    text(fb, 16, 102, "on your iPhone.", 1);
    text(fb, 16, 134, "Scan this code", 1);
    text(fb, 16, 152, "or enter:", 1);
    char code[7];
    snprintf(code, sizeof code, "%06lu", (unsigned long)passkey);
    text(fb, 16, 180, code, 3);
    text(fb, 16, 242, "Closes after 5 minutes", 1);
    text(fb, 16, 264, "Wi-Fi is not required", 1);
    int n = qrcodegen_getSize(qr);
    int scale = 180 / (n + 8); // four-module quiet zone, within right column
    if (scale < 1) return false;
    int left = 210 + (180 - n * scale) / 2;
    int top = (240 - n * scale) / 2;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y))
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        pixel(fb, left + x * scale + dx, top + y * scale + dy);
    return true;
}
