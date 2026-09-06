// Boot/wake button timing, independent of GPIO for boundary testing.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>

// Hold windows, classified on release (see button_release_gesture):
//   tap         < 5 s   -> deck next / show latest
//   refresh   5-10 s    -> re-render in place
//   maintenance 10-20 s -> Companion BLE session
//   provision  >= 20 s  -> captive portal
// Maintenance sits ABOVE refresh (longer hold) so the short deck-next tap has a
// wide window and an over-held refresh can never fall into a BLE session.
#define BTN_REFRESH_HOLD_MS 5000
#define BTN_MAINTENANCE_HOLD_MS 10000
#define PROVISION_HOLD_MS 20000

typedef enum {
    BTN_GESTURE_NONE = 0,
    BTN_GESTURE_TAP,
    BTN_GESTURE_REFRESH,
    BTN_GESTURE_PROVISION,
    BTN_GESTURE_MAINTENANCE,
} btn_gesture_t;

static inline btn_gesture_t button_release_gesture(uint32_t held_ms) {
    if (held_ms >= PROVISION_HOLD_MS) return BTN_GESTURE_PROVISION;
    if (held_ms >= BTN_MAINTENANCE_HOLD_MS) return BTN_GESTURE_MAINTENANCE;
    if (held_ms >= BTN_REFRESH_HOLD_MS) return BTN_GESTURE_REFRESH;
    return BTN_GESTURE_TAP;
}
