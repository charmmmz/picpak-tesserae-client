/* Bounded BLE setup and maintenance service. */
#pragma once

#include <stdint.h>

typedef enum {
    BLE_SETUP_RESULT_TIMEOUT = 0,
    BLE_SETUP_RESULT_CONFIGURED,
    BLE_SETUP_RESULT_REBOOT,
    BLE_SETUP_RESULT_CLEAR_WIFI,
    BLE_SETUP_RESULT_FACTORY_RESET,
    BLE_SETUP_RESULT_ERROR,
    BLE_SETUP_RESULT_CANCELLED,
} ble_setup_result_t;

/* Starts advertising, paints the QR/passkey screen, and serves one bounded
 * session. Wi-Fi changes are saved only after joining successfully; server
 * settings are preserved even when the server is unreachable. */
ble_setup_result_t ble_setup_run(uint32_t timeout_s);
