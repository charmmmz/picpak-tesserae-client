// PicPak BLE photo v1: authenticated, bounded, stop-and-wait transfers.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define BLE_PHOTO_BYTES 30000u
#define BLE_PHOTO_CHUNK_BYTES 192u
typedef struct {
    uint32_t id, offset, generation;
    bool active;
    uint8_t digest[32];
} ble_photo_t;
bool ble_photo_begin(ble_photo_t *state, uint32_t id, uint32_t length,
                     const uint8_t digest[32], uint32_t generation);
// Duplicate chunks are acknowledged only when their contents match.
bool ble_photo_write(ble_photo_t *state, uint8_t *buffer, const uint8_t *packet,
                     size_t length, uint32_t generation);
bool ble_photo_complete(const ble_photo_t *state, uint32_t id, uint32_t generation);
