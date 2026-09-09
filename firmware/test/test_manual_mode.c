// test_manual_mode.c — host unit test for the pure Manual-mode wake decision (manual_core.h).
// cc firmware/test/test_manual_mode.c -Ifirmware/main -o /tmp/t && /tmp/t
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "manual_core.h"

int main(void) {
    manual_decision_t d;

    // Healthy battery + a user (button) wake -> advertise for a photo, no repaint.
    d = manual_decide(/*user_wake*/true, LOWBATT_NORMAL, /*was_locked*/false);
    assert(d.run_photo && d.paint == MANUAL_PAINT_NONE);

    // Healthy battery + the 24 h poll timer (no user) -> just checked the cell, do nothing.
    d = manual_decide(/*user_wake*/false, LOWBATT_NORMAL, /*was_locked*/false);
    assert(!d.run_photo && d.paint == MANUAL_PAINT_NONE);

    // Battery just went low (ARM) -> paint the charge splash, never bring up the radio,
    // even on a user wake (a weak cell must not run a BLE transfer).
    d = manual_decide(/*user_wake*/true, LOWBATT_ARM, /*was_locked*/false);
    assert(!d.run_photo && d.paint == MANUAL_PAINT_LOWBATT);

    // Still low on the next poll (STAY_LOW) -> keep polling, no repaint, no radio.
    d = manual_decide(/*user_wake*/false, LOWBATT_STAY_LOW, /*was_locked*/true);
    assert(!d.run_photo && d.paint == MANUAL_PAINT_NONE);

    // Recovered after charging (was locked, now NORMAL) + a user wake -> clear the splash
    // with the "ready" screen AND advertise so a photo can be sent right away.
    d = manual_decide(/*user_wake*/true, LOWBATT_NORMAL, /*was_locked*/true);
    assert(d.run_photo && d.paint == MANUAL_PAINT_READY);

    // Recovered on the 24 h poll (no user present) -> clear the splash to "ready" so the
    // user sees it recovered, but do not advertise (nobody is here to send a photo).
    d = manual_decide(/*user_wake*/false, LOWBATT_NORMAL, /*was_locked*/true);
    assert(!d.run_photo && d.paint == MANUAL_PAINT_READY);

    printf("test_manual_mode: all assertions passed\n");
    return 0;
}
