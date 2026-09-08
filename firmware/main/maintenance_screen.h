// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Render into the existing 400x300 bottom-up packed BWRY framebuffer.
bool maintenance_screen_render(uint8_t *fb, const char *qr_payload, uint32_t passkey);
void maintenance_screen_closed(uint8_t *fb);
void maintenance_screen_photo_ready(uint8_t *fb);
