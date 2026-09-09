// manual_core.h — pure Manual (Bluetooth) photo-mode wake decision (host-testable).
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
//
// One decision per wake in Manual mode, given the low-battery gate's verdict and the wake
// context. Manual mode wakes on a button press (user present) or a 24 h low-power poll
// (battery check only). This decides what to paint and whether to bring up the BLE radio;
// the caller always re-arms the 24 h poll afterward (a brownout reset is deferred earlier,
// before the gate runs, so it never reaches here). Pure: no hardware, no NVS, unit-testable.
#pragma once
#include <stdbool.h>
#include "lowbatt_core.h"

typedef enum {
    MANUAL_PAINT_NONE = 0,   // leave the panel as-is
    MANUAL_PAINT_LOWBATT,    // paint the "battery low — charge" splash (just went low)
    MANUAL_PAINT_READY,      // paint the "ready for photos" screen (just recovered)
} manual_paint_t;

typedef struct {
    manual_paint_t paint;    // what to repaint before sleeping
    bool           run_photo;// bring up BLE and advertise for a photo transfer
} manual_decision_t;

// Decide the Manual-mode action for this wake. Pure: no side effects.
//   user_wake   a button/tap/refresh wake (a human is present), vs. the 24 h poll timer.
//   gate        the low-battery gate's verdict for this wake (NORMAL / ARM / STAY_LOW).
//   was_locked  whether the low-battery lock was set BEFORE the gate ran (to detect recovery).
static inline manual_decision_t manual_decide(bool user_wake, lowbatt_action_t gate,
                                              bool was_locked) {
    manual_decision_t d = { MANUAL_PAINT_NONE, false };
    switch (gate) {
        case LOWBATT_ARM:
            d.paint = MANUAL_PAINT_LOWBATT;   // just went low: warn once, never run the radio
            break;
        case LOWBATT_STAY_LOW:
            break;                             // still low: keep polling, no repaint, no radio
        case LOWBATT_NORMAL:
        default:
            if (was_locked) d.paint = MANUAL_PAINT_READY;   // recovered: clear the charge splash
            if (user_wake)  d.run_photo = true;             // a human is here: advertise for a photo
            break;
    }
    return d;
}
