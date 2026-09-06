#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "button_gesture.h"
#include "maintenance_settings.h"
#include "maintenance_screen.h"
#include "board.h"

int main(int argc, char **argv) {
    assert(button_release_gesture(0) == BTN_GESTURE_TAP);
    assert(button_release_gesture(2999) == BTN_GESTURE_TAP);
    assert(button_release_gesture(3000) == BTN_GESTURE_MAINTENANCE);
    assert(button_release_gesture(4999) == BTN_GESTURE_MAINTENANCE);
    assert(button_release_gesture(5000) == BTN_GESTURE_REFRESH);
    assert(button_release_gesture(19999) == BTN_GESTURE_REFRESH);
    assert(button_release_gesture(20000) == BTN_GESTURE_PROVISION);
    const char *names[] = {"5s", "10s", "native"};
    for (uint8_t i = 0; i < 3; i++) {
        uint8_t mode = 255;
        assert(refresh_speed_parse(names[i], &mode) && mode == i);
        assert(strcmp(refresh_speed_name(mode), names[i]) == 0);
    }
    uint8_t mode = 1;
    assert(!refresh_speed_parse("5", &mode) && mode == 1);
    assert(!refresh_speed_parse("", &mode));
    assert(!refresh_speed_parse(NULL, &mode));
    assert(!refresh_speed_parse("native", NULL));
    assert(refresh_speed_name(3) == NULL);

    uint8_t guarded[EPD_FB_BYTES + 2];
    memset(guarded, 0xaf, sizeof guarded);
    const char *payload = "tesserae://setup?v=2&id=picpak-010203040506&sid=12345678&key=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    assert(maintenance_screen_render(guarded + 1, payload, 123456));
    assert(guarded[0] == 0xaf && guarded[sizeof guarded - 1] == 0xaf);
    size_t black = 0;
    for (size_t i = 1; i <= EPD_FB_BYTES; i++) {
        for (int shift = 0; shift < 8; shift += 2) {
            int color = (guarded[i] >> shift) & 3;
            assert(color == 0 || color == 1);
            if (!color) black++;
        }
    }
    assert(black > 5000 && black < 40000);
    if (argc == 2) {
        FILE *out = fopen(argv[1], "wb"); assert(out);
        assert(fwrite(guarded + 1, 1, EPD_FB_BYTES, out) == EPD_FB_BYTES);
        fclose(out);
    }
    assert(!maintenance_screen_render(NULL, payload, 123456));
    assert(!maintenance_screen_render(guarded + 1, NULL, 123456));
    assert(!maintenance_screen_render(guarded + 1, payload, 1000000));
    maintenance_screen_closed(guarded + 1);
    assert(guarded[0] == 0xaf && guarded[sizeof guarded - 1] == 0xaf);
    // The former QR area is cleared, so a closed session cannot be scanned.
    for (int y = 70; y < 100; y++)
        for (int x = 310; x < 390; x++) {
            size_t i = 1 + (EPD_H - 1 - y) * (EPD_W / 4) + x / 4;
            assert(((guarded[i] >> (6 - 2 * (x % 4))) & 3) == 1);
        }
    puts("maintenance: gesture boundaries, settings values and screen passed");
    return 0;
}
