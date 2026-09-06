// Wi-Fi operations owned only by the bounded BLE maintenance session.
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#define WIFI_SCAN_MAX_NETWORKS 12
typedef struct { char ssid[33]; int rssi; bool secure; } wifi_network_t;
typedef bool (*ble_wifi_cancelled_t)(void);
esp_err_t ble_wifi_scan(wifi_network_t *out, size_t cap, size_t *count,
                       ble_wifi_cancelled_t cancelled);
esp_err_t ble_wifi_connect(const char *ssid, const char *password,
                          ble_wifi_cancelled_t cancelled);
bool ble_wifi_get_ip(char *out, size_t cap);
int ble_wifi_rssi(void);
void ble_wifi_stop(void);
