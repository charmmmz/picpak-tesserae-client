#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ble_photo.h"
static void put32(uint8_t *p, uint32_t v) {
    p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v;
}
int main(void) {
    ble_photo_t s = {0}; uint8_t buffer[BLE_PHOTO_BYTES] = {0}, digest[32] = {1};
    uint8_t packet[8 + BLE_PHOTO_CHUNK_BYTES] = {0};
    assert(!ble_photo_begin(&s, 0, BLE_PHOTO_BYTES, digest, 7));
    assert(!ble_photo_begin(&s, 1, BLE_PHOTO_BYTES+1, digest, 7));
    assert(ble_photo_begin(&s, 0x01020304, BLE_PHOTO_BYTES, digest, 7));
    put32(packet, s.id); put32(packet+4, 1);
    assert(!ble_photo_write(&s, buffer, packet, sizeof packet, 7)); // missing first byte
    assert(ble_photo_is_gap(&s, packet, sizeof packet, 7));
    assert(!ble_photo_is_gap(&s, packet, sizeof packet, 8));
    assert(!ble_photo_is_gap(&s, packet, sizeof packet + 1, 7));
    assert(s.offset == 0);
    put32(packet+4, 0);
    assert(!ble_photo_write(&s, buffer, packet, sizeof packet, 8)); // new connection
    assert(!ble_photo_write(&s, buffer, packet, 8, 7));
    memset(packet+8, 0x1b, BLE_PHOTO_CHUNK_BYTES); // B/W/Y/R packed vector
    assert(ble_photo_write(&s, buffer, packet, sizeof packet, 7));
    assert(s.offset == 192 && buffer[0] == 0x1b);
    assert(ble_photo_write(&s, buffer, packet, sizeof packet, 7)); // lost ACK retry
    assert(s.offset == 192);
    packet[8] ^= 1;
    assert(!ble_photo_write(&s, buffer, packet, sizeof packet, 7)); // changed duplicate
    packet[8] ^= 1;
    assert(!ble_photo_complete(&s, s.id, 7));
    while (s.offset < BLE_PHOTO_BYTES) {
        put32(packet+4, s.offset);
        size_t count = BLE_PHOTO_BYTES-s.offset;
        if (count > BLE_PHOTO_CHUNK_BYTES) count = BLE_PHOTO_CHUNK_BYTES;
        assert(ble_photo_write(&s, buffer, packet, 8+count, 7));
    }
    assert(ble_photo_complete(&s, 0x01020304, 7));
    assert(!ble_photo_complete(&s, s.id, 8));
    assert(!ble_photo_complete(&s, 1, 7));
    put32(packet+4, UINT32_MAX);
    assert(!ble_photo_write(&s, buffer, packet, sizeof packet, 7));
    assert(!ble_photo_is_gap(&s, packet, sizeof packet, 7));
    put32(packet+4, BLE_PHOTO_BYTES);
    assert(!ble_photo_write(&s, buffer, packet, sizeof packet, 7));

    // MTU-aware short chunks remain valid under the 192-byte maximum. A lost
    // command yields an unchanged cumulative offset, then a barrier/resend
    // resumes at exactly that offset; skipped packets must never be copied.
    assert(ble_photo_begin(&s, 9, BLE_PHOTO_BYTES, digest, 10));
    memset(buffer, 0xee, sizeof buffer);
    put32(packet, 9); put32(packet + 4, 0);
    memset(packet + 8, 0x42, BLE_PHOTO_CHUNK_BYTES);
    assert(ble_photo_write(&s, buffer, packet, 8 + 119, 10));
    put32(packet + 4, 238);
    assert(ble_photo_is_gap(&s, packet, 8 + 119, 10));
    assert(!ble_photo_write(&s, buffer, packet, 8 + 119, 10));
    assert(s.offset == 119 && buffer[238] == 0xee);
    put32(packet, 10);
    assert(!ble_photo_is_gap(&s, packet, 8 + 119, 10));
    put32(packet, 9);
    while (s.offset < BLE_PHOTO_BYTES) {
        size_t count = BLE_PHOTO_BYTES - s.offset;
        if (count > 119) count = 119;
        put32(packet + 4, s.offset);
        assert(ble_photo_write(&s, buffer, packet, 8 + count, 10));
    }
    put32(packet + 4, 119);
    assert(ble_photo_write(&s, buffer, packet, 8 + 119, 10));
    packet[8] ^= 1;
    assert(!ble_photo_write(&s, buffer, packet, 8 + 119, 10));
    assert(!ble_photo_is_gap(&s, packet, 8 + 119, 10));
    assert(ble_photo_complete(&s, 9, 10));
    for (size_t i = 0; i < sizeof buffer; i++) assert(buffer[i] == 0x42);
    puts("BLE photo: bounds, dynamic chunks, gaps/retry, full frame and generation isolation passed");
}
