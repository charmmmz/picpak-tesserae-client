// epd_tempcomp.h — pure temperature->power-correction curve (host-testable, no ESP deps).
//
// Derives a small power-correction step from (die - ref) with an asymmetric slope:
//
//     delta = die_c - ref_c
//     raw   = (delta >= 0) ? delta/8    (warm side, gentler slope)
//                          : delta/6    (cold side, steeper slope)   [asymmetric]
//     corr  = -raw
//     corr  = clamp(corr, -clamp, +clamp)
//
// SIGN: we apply the correction to pwr[2] = VSPL, which the SSD2683 datasheet
// (Rev 0.20, PWR cmd 0x01) shows is a source drive-HIGH voltage: higher VSPL =
// stronger push = more development. Cold ink is sluggish and develops lighter, so it
// needs MORE drive -> VSPL must go UP when cold. Hence corr is POSITIVE when cold,
// NEGATIVE when warm. The steeper ÷6 slope lands on the cold side, so cold is
// compensated more aggressively.
// (Bench-confirmed: raising pwr[2] darkens/adds contrast; +15 clearly darker.)
//
// Integer division truncates toward zero. We drive a single fixed reference (no
// per-MAC NVS calibration blob), so absolute accuracy is looser, but the relative
// cold-vs-warm response — the part that matters for the ink — holds.
//
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#pragma once

static inline int epd_tempcomp_corr(int die_c, int ref_c, int clamp) {
    int delta = die_c - ref_c;
    int raw   = (delta >= 0) ? (delta / 8) : (delta / 6);
    int corr  = -raw;   // invert: pwr[2]=VSPL is a drive rail — cold needs MORE drive (up)
    if (corr >  clamp) corr =  clamp;
    if (corr < -clamp) corr = -clamp;
    return corr;
}
