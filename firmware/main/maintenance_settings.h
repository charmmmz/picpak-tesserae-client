// PicPak's BLE wire values share the existing portal/NVS waveform numbering.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static inline bool refresh_speed_parse(const char *value, uint8_t *mode) {
    if (!value || !mode) return false;
    if (!strcmp(value, "5s")) *mode = 0;
    else if (!strcmp(value, "10s")) *mode = 1;
    else if (!strcmp(value, "native")) *mode = 2;
    else return false;
    return true;
}

static inline const char *refresh_speed_name(uint8_t mode) {
    switch (mode) {
    case 0: return "5s";
    case 1: return "10s";
    case 2: return "native";
    default: return NULL;
    }
}
