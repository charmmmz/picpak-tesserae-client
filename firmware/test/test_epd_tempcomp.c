// test_epd_tempcomp.c — host unit test for the temperature->power-correction curve.
// Verifies the ÷8 / ÷6 asymmetric slope and the INVERTED sign we apply to
// our VSPL drive byte: cold -> byte UP (more drive), warm -> byte DOWN. See epd_tempcomp.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include "epd_tempcomp.h"

int main(void) {
    const int REF = 25, CL = 6;

    // At reference: no correction.
    assert(epd_tempcomp_corr(25, REF, CL) == 0);

    // Warm side (delta>=0, ÷8): byte goes DOWN (less drive; hot ink over-develops).
    assert(epd_tempcomp_corr(33, REF, CL) == -1);   //  -(8/8)
    assert(epd_tempcomp_corr(26, REF, CL) == 0);    //  -(1/8) -> 0
    assert(epd_tempcomp_corr(49, REF, CL) == -3);   //  -(24/8)

    // Cold side (delta<0, ÷6): byte goes UP (more drive; cold ink is sluggish), steeper.
    assert(epd_tempcomp_corr(19, REF, CL) == 1);    //  -(-6/6)
    assert(epd_tempcomp_corr(24, REF, CL) == 0);    //  -(-1/6) -> 0 (toward zero)
    assert(epd_tempcomp_corr(13, REF, CL) == 2);    //  -(-12/6)

    // Asymmetry: same |delta|=24 is -3 warm but +4 cold (cold compensated harder).
    assert(epd_tempcomp_corr(49, REF, CL) == -3);
    assert(epd_tempcomp_corr(1,  REF, CL) == 4);

    // Clamp both directions.
    assert(epd_tempcomp_corr(200,  REF, CL) == -CL);  // very warm -> clamp down
    assert(epd_tempcomp_corr(-200, REF, CL) ==  CL);  // very cold -> clamp up

    printf("PASS\n");
    return 0;
}
