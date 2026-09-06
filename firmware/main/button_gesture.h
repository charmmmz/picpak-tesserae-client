// Boot/wake button timing, independent of GPIO for boundary testing.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdint.h>

#define BTN_MAINTENANCE_HOLD_MS 3000
#define BTN_REFRESH_HOLD_MS 5000
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
    if (held_ms >= BTN_REFRESH_HOLD_MS) return BTN_GESTURE_REFRESH;
    if (held_ms >= BTN_MAINTENANCE_HOLD_MS) return BTN_GESTURE_MAINTENANCE;
    return BTN_GESTURE_TAP;
}
