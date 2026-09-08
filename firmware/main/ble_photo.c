// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ble_photo.h"
#include <string.h>
static uint32_t read32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
bool ble_photo_begin(ble_photo_t *s, uint32_t id, uint32_t length,
                     const uint8_t digest[32], uint32_t generation) {
    if (!id || length != BLE_PHOTO_BYTES || !digest) return false;
    *s = (ble_photo_t){.id = id, .generation = generation, .active = true};
    memcpy(s->digest, digest, 32);
    return true;
}
bool ble_photo_write(ble_photo_t *s, uint8_t *buffer, const uint8_t *packet,
                     size_t length, uint32_t generation) {
    if (!s->active || s->generation != generation || length <= 8 ||
        length > 8 + BLE_PHOTO_CHUNK_BYTES || read32(packet) != s->id) return false;
    uint32_t offset = read32(packet + 4);
    size_t count = length - 8;
    if (offset > s->offset || offset > BLE_PHOTO_BYTES ||
        count > BLE_PHOTO_BYTES - offset) return false;
    if (offset < s->offset)
        return count <= s->offset - offset && !memcmp(buffer + offset, packet + 8, count);
    memcpy(buffer + offset, packet + 8, count);
    s->offset += count;
    return true;
}
bool ble_photo_complete(const ble_photo_t *s, uint32_t id, uint32_t generation) {
    return s->active && id == s->id && generation == s->generation &&
           s->offset == BLE_PHOTO_BYTES;
}
