// Adapted from dmellok/tesserae-device-firmware BLE protocol v2.
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ble_setup.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "power.h"
#include "defaults.h"
#include "config_store.h"
#include "framebuf.h"
#include "epd_driver.h"
#include "maintenance_screen.h"
#include "maintenance_settings.h"
#include "ble_setup_protocol.h"
#include "relay_crypto.h"
#include "ble_wifi.h"

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_setup";

#define BIT_DONE           BIT0
#define BIT_HOST_STOPPED   BIT1
#define BIT_WORKER_STOPPED BIT2
#define COMMAND_QUEUE_LEN  4
#define EVENT_FRAME_MAX    300
#define LOG_ROWS           4
#define TESSERAE_BLE_HARDWARE_CODE 11
#define TESSERAE_DEVICE_MODEL "picpak_4_2"

/* UUID text: 7a5e000N-7b6d-4f8b-9c2e-1d0a5a110001. NimBLE stores 128-bit
 * UUID bytes least-significant first, hence N belongs at byte 12 rather than
 * byte 0 (which is the trailing 01 in the canonical UUID). */
#define TESSERAE_UUID(suffix) \
    BLE_UUID128_INIT(0x01,0x00,0x11,0x5a,0x0a,0x1d,0x2e,0x9c, \
                     0x8b,0x4f,0x6d,0x7b,(suffix),0x00,0x5e,0x7a)

static const ble_uuid128_t s_service_uuid = TESSERAE_UUID(0x01);
static const ble_uuid128_t s_info_uuid    = TESSERAE_UUID(0x02);
static const ble_uuid128_t s_qr_uuid      = TESSERAE_UUID(0x03);
static const ble_uuid128_t s_secure_uuid  = TESSERAE_UUID(0x04);
static const ble_uuid128_t s_event_uuid   = TESSERAE_UUID(0x05);

typedef enum { AUTH_QR = 1, AUTH_PASSKEY = 2 } auth_mode_t;

typedef struct {
    auth_mode_t auth;
    uint32_t generation;
    size_t len;
    char json[BLE_SETUP_MESSAGE_MAX + 1];
} command_t;

typedef struct {
    char ssid[33];
    char password[65];
    uint32_t generation;
    bool ready;
} staged_config_t;

static ble_setup_result_t s_result;
static EventGroupHandle_t s_events;
static QueueHandle_t s_commands;
static TaskHandle_t s_worker;
static atomic_bool s_stopping;
static atomic_uint s_generation;
static uint32_t s_command_generation;
static SemaphoreHandle_t s_lock;
static char s_device_id[32];
// JSON is queued by the worker; only NimBLE's host touches TX framing,
// encryption counters, connection handles, and the readable event cache.
typedef struct {
    auth_mode_t auth;
    uint32_t generation;
    char json[BLE_SETUP_MESSAGE_MAX + 1];
} outgoing_event_t;
static QueueHandle_t s_outgoing;
static atomic_uint s_pending_events;
static struct ble_npl_callout s_tx_callout;
static bool s_tx_initialized;
static outgoing_event_t s_tx;
static bool s_tx_active;
static size_t s_tx_offset, s_tx_limit, s_tx_frame_len;
static uint8_t s_tx_index, s_tx_count, s_tx_retries;
static uint16_t s_tx_message_id;
static uint8_t s_tx_frame[EVENT_FRAME_MAX];
static bool s_notify;
static bool s_nimble_initialized;
static bool s_host_started;
static _Atomic uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_event_handle;
static uint8_t s_addr_type;
static uint32_t s_passkey;
static uint8_t s_secret[BLE_SETUP_KEY_LEN];
static uint8_t s_sid[BLE_SETUP_SID_LEN];
static uint8_t s_connection_nonce[BLE_SETUP_CONN_NONCE_LEN];
static ble_setup_crypto_t s_crypto;
static ble_setup_reassembly_t s_qr_reassembly;
static ble_setup_reassembly_t s_native_reassembly;
static staged_config_t s_staged;
static uint16_t s_out_message_id;
static char s_info[320];
static char s_qr_payload[320];
static uint8_t s_last_event[EVENT_FRAME_MAX];
static size_t s_last_event_len;
static char s_logs[LOG_ROWS][72];
static size_t s_log_count;

static int gap_event(struct ble_gap_event *event, void *arg);

static bool prepare_connection(void)
{
    esp_fill_random(s_connection_nonce, sizeof s_connection_nonce);
    if (!ble_setup_crypto_init(&s_crypto, s_secret, s_sid,
                               s_connection_nonce))
        return false;

    char sid_hex[BLE_SETUP_SID_LEN * 2 + 1];
    snprintf(sid_hex, sizeof sid_hex, "%02x%02x%02x%02x",
             s_sid[0], s_sid[1], s_sid[2], s_sid[3]);
    char nonce_hex[BLE_SETUP_CONN_NONCE_LEN * 2 + 1];
    for (size_t i = 0; i < sizeof s_connection_nonce; i++)
        snprintf(nonce_hex + i * 2, sizeof nonce_hex - i * 2, "%02x",
                 s_connection_nonce[i]);
    const char *device_id = s_device_id;
    int n = snprintf(s_info, sizeof s_info,
                     "{\"protocol\":%u,\"id\":\"%s\",\"sid\":\"%s\","
                     "\"connection_nonce\":\"%s\",\"hardware\":%u,"
                     "\"model\":\"%s\",\"firmware\":\"%s\","
                     "\"mode\":\"%s\"}",
                     (unsigned)BLE_SETUP_PROTOCOL_MAJOR, device_id, sid_hex,
                     nonce_hex, (unsigned)TESSERAE_BLE_HARDWARE_CODE,
                     TESSERAE_DEVICE_MODEL, FW_VERSION,
                     "maintenance");
    if (n <= 0 || (size_t)n >= sizeof s_info) return false;

    s_out_message_id = 0;
    memset(s_last_event, 0, sizeof s_last_event);
    s_last_event_len = 0;
    ble_setup_reassembly_reset(&s_qr_reassembly);
    ble_setup_reassembly_reset(&s_native_reassembly);
    return true;
}

static void add_log(const char *text)
{
    if (!text) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_log_count == LOG_ROWS) {
        memmove(s_logs, s_logs + 1, sizeof s_logs[0] * (LOG_ROWS - 1));
        s_log_count--;
    }
    snprintf(s_logs[s_log_count++], sizeof s_logs[0], "%s", text);
    xSemaphoreGive(s_lock);
}

static void finish(ble_setup_result_t result)
{
    // Terminal events are queued as well. Let the host finish their frames
    // before teardown; disconnect cancels delivery and the session still ends.
    if (xTaskGetCurrentTaskHandle() == s_worker) {
        for (unsigned i = 0; i < 300 && atomic_load(&s_pending_events); i++) {
            if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_stopping) break;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    s_result = result;
    xEventGroupSetBits(s_events, BIT_DONE);
}

static bool copy_json_string(const cJSON *root, const char *name,
                             char *out, size_t cap, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return !required;
    size_t n = strlen(item->valuestring);
    if (n >= cap) return false;
    memcpy(out, item->valuestring, n + 1);
    return true;
}

static bool command_cancelled(void)
{
    return s_stopping || s_conn == BLE_HS_CONN_HANDLE_NONE ||
           s_command_generation != atomic_load(&s_generation);
}

static size_t event_payload_limit(auth_mode_t auth)
{
    uint16_t mtu = s_conn == BLE_HS_CONN_HANDLE_NONE ? 23 : ble_att_mtu(s_conn);
    size_t frame = mtu > 3 ? (size_t)mtu - 3 : 20;
    size_t overhead = 1 + BLE_SETUP_CHUNK_HEADER;
    if (auth == AUTH_QR) overhead += 4 + BLE_SETUP_TAG_LEN;
    return frame > overhead ? frame - overhead : 1;
}

static void drop_tx(void)
{
    if (s_tx_active) atomic_fetch_sub(&s_pending_events, 1);
    s_tx_active = false;
    s_tx_frame_len = 0;
    memset(&s_tx, 0, sizeof s_tx);
    memset(s_tx_frame, 0, sizeof s_tx_frame);
}

static void pump_tx(struct ble_npl_event *event)
{
    (void)event;
    if (s_stopping) return;
    if (!s_tx_active) {
        if (xQueueReceive(s_outgoing, &s_tx, 0) != pdTRUE) return;
        s_tx_active = true;
        s_tx_offset = s_tx_index = s_tx_frame_len = s_tx_retries = 0;
        s_tx_limit = event_payload_limit(s_tx.auth);
        s_tx_count = (uint8_t)((strlen(s_tx.json) + s_tx_limit - 1) / s_tx_limit);
        s_tx_message_id = ++s_out_message_id;
    }
    uint16_t conn = s_conn;
    bool allowed = conn != BLE_HS_CONN_HANDLE_NONE && s_notify &&
                   s_tx.generation == atomic_load(&s_generation);
    if (allowed && s_tx.auth == AUTH_PASSKEY) {
        struct ble_gap_conn_desc desc;
        allowed = ble_gap_conn_find(conn, &desc) == 0 &&
                  desc.sec_state.encrypted && desc.sec_state.authenticated;
    }
    if (allowed && s_tx.auth == AUTH_QR && ble_att_mtu(conn) < 30) allowed = false;
    if (!allowed) {
        drop_tx();
    } else {
        size_t part = strlen(s_tx.json) - s_tx_offset;
        if (part > s_tx_limit) part = s_tx_limit;
        if (!s_tx_frame_len) {
            uint8_t plain[EVENT_FRAME_MAX];
            size_t plain_len = 0;
            bool encoded = ble_setup_chunk_encode(s_tx_message_id, s_tx_index,
                s_tx_count, (const uint8_t *)s_tx.json + s_tx_offset, part,
                plain, sizeof plain, &plain_len);
            if (encoded && s_tx.auth == AUTH_QR)
                encoded = ble_setup_seal(&s_crypto, BLE_SETUP_DIR_DEVICE_TO_APP,
                    plain, plain_len, s_tx_frame, sizeof s_tx_frame, &s_tx_frame_len);
            else if (encoded) {
                s_tx_frame[0] = BLE_SETUP_FRAME_NATIVE;
                memcpy(s_tx_frame + 1, plain, plain_len);
                s_tx_frame_len = plain_len + 1;
            }
            if (!encoded) { drop_tx(); goto schedule; }
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(s_tx_frame, (uint16_t)s_tx_frame_len);
        int rc = om ? ble_gatts_notify_custom(conn, s_event_handle, om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            memcpy(s_last_event, s_tx_frame, s_tx_frame_len);
            s_last_event_len = s_tx_frame_len;
            s_tx_frame_len = 0;
            s_tx_offset += part;
            s_tx_retries = 0;
            if (++s_tx_index == s_tx_count) drop_tx();
        } else if (++s_tx_retries >= 30) {
            ESP_LOGW(TAG, "notification delivery failed: %d", rc);
            drop_tx();
        }
    }
schedule:
    if (s_tx_active || uxQueueMessagesWaiting(s_outgoing))
        ble_npl_callout_reset(&s_tx_callout, ble_npl_time_ms_to_ticks32(15));
}

static void send_event(auth_mode_t auth, const char *json)
{
    if (!json || command_cancelled() || strlen(json) > BLE_SETUP_MESSAGE_MAX) return;
    outgoing_event_t event = { .auth = auth, .generation = s_command_generation };
    snprintf(event.json, sizeof event.json, "%s", json);
    atomic_fetch_add(&s_pending_events, 1);
    if (xQueueSend(s_outgoing, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
        atomic_fetch_sub(&s_pending_events, 1);
        ESP_LOGW(TAG, "BLE event queue full");
        return;
    }
    ble_npl_callout_reset(&s_tx_callout, ble_npl_time_ms_to_ticks32(15));
}

static void send_simple(auth_mode_t auth, const char *event, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", event);
    if (message) cJSON_AddStringToObject(root, "message", message);
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (encoded) { send_event(auth, encoded); free(encoded); }
}

static void send_diagnostics(auth_mode_t auth)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "event", "diagnostics");
    cJSON_AddStringToObject(root, "firmware", FW_VERSION);
    cJSON_AddStringToObject(root, "model", TESSERAE_DEVICE_MODEL);
    cJSON_AddNumberToObject(root, "battery_mv", power_battery_mv());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "reset_reason", esp_reset_reason());
    char ssid[33] = {0}, password[65] = {0}, ip[48] = {0}, server[160] = {0};
    bool has_wifi = config_get_wifi(ssid, sizeof ssid, password, sizeof password);
    memset(password, 0, sizeof password);
    config_get_server_url(server, sizeof server);
    bool has_server = server[0] || config_relay_configured();
    if (config_get_transport(1) == 0) {
        char user[64], pass[128];
        config_get_mqtt(server, sizeof server, user, sizeof user, pass, sizeof pass);
        has_server = has_server || server[0];
        memset(pass, 0, sizeof pass);
    }
    cJSON_AddBoolToObject(root, "wifi_configured", has_wifi);
    cJSON_AddBoolToObject(root, "server_configured", has_server);
    cJSON_AddStringToObject(root, "refresh_speed",
        refresh_speed_name(config_get_waveform(DEFAULT_WAVEFORM)));
    cJSON_AddNumberToObject(root, "rssi", ble_wifi_rssi());
    if (has_wifi) cJSON_AddStringToObject(root, "ssid", ssid);
    else cJSON_AddNullToObject(root, "ssid");
    if (ble_wifi_get_ip(ip, sizeof ip)) cJSON_AddStringToObject(root, "ip", ip);
    else cJSON_AddNullToObject(root, "ip");
    cJSON *logs = cJSON_AddArrayToObject(root, "logs");
    char recent_logs[LOG_ROWS][72];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t log_count = s_log_count;
    memcpy(recent_logs, s_logs, sizeof recent_logs);
    xSemaphoreGive(s_lock);
    for (size_t i = 0; logs && i < log_count; i++)
        cJSON_AddItemToArray(logs, cJSON_CreateString(recent_logs[i]));
    char *encoded = cJSON_PrintUnformatted(root);
    while (encoded && strlen(encoded) > BLE_SETUP_MESSAGE_MAX && cJSON_GetArraySize(logs)) {
        free(encoded);
        cJSON_DeleteItemFromArray(logs, 0);
        encoded = cJSON_PrintUnformatted(root);
    }
    cJSON_Delete(root);
    if (encoded) { send_event(auth, encoded); free(encoded); }
}

static void run_scan(auth_mode_t auth)
{
    send_simple(auth, "scan_started", NULL);
    wifi_network_t networks[WIFI_SCAN_MAX_NETWORKS];
    size_t count = 0;
    esp_err_t err = ble_wifi_scan(networks, WIFI_SCAN_MAX_NETWORKS, &count, command_cancelled);
    if (err != ESP_OK) {
        add_log("Wi-Fi scan failed");
        send_simple(auth, "error", "Wi-Fi scan failed");
        return;
    }
    char logline[72];
    snprintf(logline, sizeof logline, "Wi-Fi scan found %u networks", (unsigned)count);
    add_log(logline);
    for (size_t i = 0; i < count; i++) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "event", "network");
        cJSON_AddStringToObject(root, "ssid", networks[i].ssid);
        cJSON_AddNumberToObject(root, "rssi", networks[i].rssi);
        cJSON_AddBoolToObject(root, "secure", networks[i].secure);
        char *encoded = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (encoded) { send_event(auth, encoded); free(encoded); }
    }
    send_simple(auth, "scan_complete", NULL);
}

static void stage_config(auth_mode_t auth, const cJSON *root)
{
    staged_config_t next = { .generation = s_command_generation };
    char server[160] = {0}, pairing[16] = {0};
    const cJSON *preserve = cJSON_GetObjectItemCaseSensitive(root, "preserve_server");
    if (!copy_json_string(root, "ssid", next.ssid, sizeof next.ssid, true) ||
        !copy_json_string(root, "password", next.password, sizeof next.password, true) ||
        !copy_json_string(root, "server_url", server, sizeof server, false) ||
        !copy_json_string(root, "pairing_code", pairing, sizeof pairing, false) ||
        !next.ssid[0] || server[0] || pairing[0] ||
        (preserve && !cJSON_IsTrue(preserve))) {
        memset(&next, 0, sizeof next);
        send_simple(auth, "error", "Use AP setup to change the server. BLE repairs Wi-Fi only.");
        return;
    }
    next.ready = true;
    s_staged = next;
    memset(&next, 0, sizeof next);
    add_log("Wi-Fi staged in memory");
    send_simple(auth, "staged", NULL);
}

static void apply_config(auth_mode_t auth)
{
    if (!s_staged.ready || s_staged.generation != s_command_generation) {
        send_simple(auth, "error", "Stage Wi-Fi first");
        return;
    }
    send_simple(auth, "testing_wifi", NULL);
    esp_err_t err = ble_wifi_connect(s_staged.ssid, s_staged.password, command_cancelled);
    if (command_cancelled()) { memset(&s_staged, 0, sizeof s_staged); return; }
    if (err != ESP_OK) {
        add_log("Wi-Fi test failed; saved settings unchanged");
        send_simple(auth, "wifi_failed", "Could not connect to that Wi-Fi network");
        return;
    }
    send_simple(auth, "wifi_connected", NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (command_cancelled()) {
        xSemaphoreGive(s_lock);
        memset(&s_staged, 0, sizeof s_staged);
        return;
    }
    err = config_save_wifi(s_staged.ssid, s_staged.password);
    xSemaphoreGive(s_lock);
    memset(&s_staged, 0, sizeof s_staged);
    if (err != ESP_OK) {
        send_simple(auth, "error", "Could not save Wi-Fi settings");
        return;
    }
    add_log("Wi-Fi saved; server settings preserved");
    send_simple(auth, "configured", "Wi-Fi saved. Restarting display");
    vTaskDelay(pdMS_TO_TICKS(250));
    finish(BLE_SETUP_RESULT_CONFIGURED);
}

static void set_refresh_speed(auth_mode_t auth, const cJSON *root)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "value");
    uint8_t mode;
    if (!cJSON_IsString(value) || !refresh_speed_parse(value->valuestring, &mode)) {
        send_simple(auth, "error", "Unsupported refresh speed");
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (command_cancelled()) { xSemaphoreGive(s_lock); return; }
    esp_err_t err = config_save_waveform(mode);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        send_simple(auth, "error", "Could not save refresh speed");
        return;
    }
    char event[72];
    snprintf(event, sizeof event, "{\"event\":\"refresh_speed\",\"value\":\"%s\"}",
             refresh_speed_name(mode));
    add_log("Refresh speed saved; applies on next refresh");
    send_event(auth, event);
}

static void process_command(const command_t *command)
{
    if (command_cancelled()) return;
    cJSON *root = cJSON_ParseWithLength(command->json, command->len);
    if (!root) { send_simple(command->auth, "error", "Invalid command JSON"); return; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op) || !op->valuestring) {
        cJSON_Delete(root);
        send_simple(command->auth, "error", "Missing operation");
        return;
    }
    ESP_LOGI(TAG, "command: %s", op->valuestring);
    if (strcmp(op->valuestring, "scan") == 0) run_scan(command->auth);
    else if (strcmp(op->valuestring, "stage") == 0) stage_config(command->auth, root);
    else if (strcmp(op->valuestring, "apply") == 0) apply_config(command->auth);
    else if (strcmp(op->valuestring, "diagnostics") == 0) send_diagnostics(command->auth);
    else if (strcmp(op->valuestring, "set_refresh_speed") == 0) set_refresh_speed(command->auth, root);
    else if (strcmp(op->valuestring, "reboot") == 0) {
        send_simple(command->auth, "rebooting", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_REBOOT);
    } else if (strcmp(op->valuestring, "clear_wifi") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (command_cancelled()) { xSemaphoreGive(s_lock); cJSON_Delete(root); return; }
        esp_err_t err = config_clear_wifi();
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            send_simple(command->auth, "error", "Could not clear Wi-Fi settings");
            cJSON_Delete(root); return;
        }
        send_simple(command->auth, "clearing_wifi", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_CLEAR_WIFI);
    } else if (strcmp(op->valuestring, "factory_reset") == 0) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (command_cancelled()) { xSemaphoreGive(s_lock); cJSON_Delete(root); return; }
        esp_err_t err = config_factory_reset();
        xSemaphoreGive(s_lock);
        if (err != ESP_OK) {
            send_simple(command->auth, "error", "Could not reset settings");
            cJSON_Delete(root); return;
        }
        send_simple(command->auth, "factory_resetting", NULL);
        vTaskDelay(pdMS_TO_TICKS(250));
        finish(BLE_SETUP_RESULT_FACTORY_RESET);
    } else send_simple(command->auth, "error", "Unsupported operation");
    cJSON_Delete(root);
}

static void worker_task(void *arg)
{
    (void)arg;
    command_t command;

    while (!s_stopping) {
        if (xQueueReceive(s_commands, &command, pdMS_TO_TICKS(250)) == pdTRUE) {
            s_command_generation = command.generation;
            process_command(&command);
        }
        memset(&command, 0, sizeof command);
    }
    xEventGroupSetBits(s_events, BIT_WORKER_STOPPED);
    vTaskDelete(NULL);
}

static int accept_frame(auth_mode_t auth, struct os_mbuf *om)
{
    size_t frame_len = OS_MBUF_PKTLEN(om);
    uint8_t frame[BLE_SETUP_MESSAGE_MAX + 32];
    if (frame_len == 0 || frame_len > sizeof frame ||
        os_mbuf_copydata(om, 0, (int)frame_len, frame) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    uint8_t plain[BLE_SETUP_MESSAGE_MAX + 8]; size_t plain_len = 0;
    ble_setup_reassembly_t *reassembly = NULL;
    if (auth == AUTH_QR) {
        if (!ble_setup_open(&s_crypto, BLE_SETUP_DIR_APP_TO_DEVICE,
                            frame, frame_len, plain, sizeof plain, &plain_len))
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        reassembly = &s_qr_reassembly;
    } else {
        if (frame[0] != BLE_SETUP_FRAME_NATIVE || frame_len < 2)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        memcpy(plain, frame + 1, frame_len - 1);
        plain_len = frame_len - 1;
        reassembly = &s_native_reassembly;
    }

    ble_setup_chunk_t chunk;
    if (!ble_setup_chunk_decode(plain, plain_len, &chunk))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    bool valid = false;
    bool complete = ble_setup_reassembly_push(reassembly, &chunk, &valid);
    if (!valid) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (complete) {
        command_t command = { .auth = auth, .generation = atomic_load(&s_generation), .len = reassembly->len };
        memcpy(command.json, reassembly->bytes, reassembly->len + 1);
        ble_setup_reassembly_reset(reassembly);
        if (xQueueSend(s_commands, &command, 0) != pdTRUE)
            return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uintptr_t which = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && which == 1) {
        int rc = os_mbuf_append(ctxt->om, s_info, strlen(s_info));
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && which == 4) {
        if (s_last_event_len && s_last_event[0] == BLE_SETUP_FRAME_NATIVE) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(conn_handle, &desc) != 0 ||
                !desc.sec_state.encrypted || !desc.sec_state.authenticated)
                return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }
        int rc = os_mbuf_append(ctxt->om, s_last_event, s_last_event_len);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 2) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int rc = accept_frame(AUTH_QR, ctxt->om);
        xSemaphoreGive(s_lock);
        return rc;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 3)
        return accept_frame(AUTH_PASSKEY, ctxt->om);
    (void)conn_handle; (void)attr_handle;
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_services[] = {{
    .type = BLE_GATT_SVC_TYPE_PRIMARY,
    .uuid = &s_service_uuid.u,
    .characteristics = (struct ble_gatt_chr_def[]) {{
        .uuid = &s_info_uuid.u, .access_cb = gatt_access, .arg = (void *)1,
        .flags = BLE_GATT_CHR_F_READ,
    }, {
        .uuid = &s_qr_uuid.u, .access_cb = gatt_access, .arg = (void *)2,
        .flags = BLE_GATT_CHR_F_WRITE,
    }, {
        .uuid = &s_secure_uuid.u, .access_cb = gatt_access, .arg = (void *)3,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    }, {
        .uuid = &s_event_uuid.u, .access_cb = gatt_access, .arg = (void *)4,
        .val_handle = &s_event_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    }, {0}}
}, {0}};

static void advertise(void)
{
    /* UUID (16) + payload (10) + AD overhead (2) + flags AD (3) exactly fills
     * the 31-byte legacy advertisement without moving identity into the scan
     * response. See docs/ble-setup-protocol.md. */
    uint8_t service_data[26];
    memcpy(service_data, s_service_uuid.value, 16);
    service_data[16] = BLE_SETUP_PROTOCOL_MAJOR;
    service_data[17] = 0x02;
    service_data[18] = TESSERAE_BLE_HARDWARE_CODE;
    uint8_t mac[6] = {0}; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    memcpy(service_data + 19, mac + 3, 3);
    memcpy(service_data + 22, s_sid, sizeof s_sid);

    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.svc_data_uuid128 = service_data;
    fields.svc_data_uuid128_len = sizeof service_data;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv data failed: %d", rc); return; }

    /* The primary packet is nearly full with authenticated session metadata.
     * Put the discoverable service UUID in the scan response so iOS can use a
     * service-filtered scan, alongside a deliberately short local name. */
    char name[16];
    snprintf(name, sizeof name, "Tes-%02X%02X%02X", mac[3], mac[4], mac[5]);
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = &s_service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    response.name = (const uint8_t *)name;
    response.name_len = (uint8_t)strlen(name);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc != 0) { ESP_LOGE(TAG, "scan response failed: %d", rc); return; }

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "advertise failed: %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0) advertise();
    else ESP_LOGE(TAG, "address inference failed: %d", rc);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();

    /* The task that owns NimBLE must remain alive long enough to report that
     * the host loop has exited. The main setup task then deletes this suspended
     * task through nimble_port_freertos_deinit() before deinitializing NimBLE.
     * Calling freertos_deinit() here deletes the current task, so statements
     * after it never execute and the old teardown raced a live host task. */
    xEventGroupSetBits(s_events, BIT_HOST_STOPPED);
    vTaskSuspend(NULL);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            UBaseType_t stack_before = uxTaskGetStackHighWaterMark(NULL);
            ESP_LOGI(TAG, "preparing connection crypto (host stack low-watermark=%u)",
                     (unsigned)stack_before);
            atomic_fetch_add(&s_generation, 1);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            bool prepared = prepare_connection();
            xSemaphoreGive(s_lock);
            if (!prepared) {
                ESP_LOGE(TAG, "could not prepare BLE connection crypto");
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                finish(BLE_SETUP_RESULT_ERROR);
                return 0;
            }
            UBaseType_t stack_after = uxTaskGetStackHighWaterMark(NULL);
            add_log("Phone connected over BLE");
            ESP_LOGI(TAG,
                     "phone connected over BLE (handle=%u, host stack low-watermark=%u)",
                     s_conn, (unsigned)stack_after);
        } else {
            ESP_LOGW(TAG, "BLE connection failed (status=%d)", event->connect.status);
            if (!s_stopping) advertise();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected from BLE (reason=%d)",
                 event->disconnect.reason);
        atomic_fetch_add(&s_generation, 1);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_conn = BLE_HS_CONN_HANDLE_NONE; s_notify = false;
        /* Native-mode events rely on the encrypted BLE link. Never leave the
         * final plaintext event readable to a later unauthenticated connection. */
        memset(s_last_event, 0, sizeof s_last_event);
        s_last_event_len = 0;
        memset(s_info, 0, sizeof s_info);
        memset(s_connection_nonce, 0, sizeof s_connection_nonce);
        memset(&s_crypto, 0, sizeof s_crypto);
        ble_setup_reassembly_reset(&s_qr_reassembly);
        ble_setup_reassembly_reset(&s_native_reassembly);
        xSemaphoreGive(s_lock);
        if (!s_stopping) advertise();
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_event_handle)
            s_notify = event->subscribe.cur_notify;
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        if (event->passkey.params.action != BLE_SM_IOACT_DISP) return 0;
        struct ble_sm_io io = { .action = BLE_SM_IOACT_DISP, .passkey = s_passkey };
        return ble_sm_inject_io(event->passkey.conn_handle, &io);
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static bool prepare_session(void)
{
    esp_fill_random(s_secret, sizeof s_secret);
    esp_fill_random(s_sid, sizeof s_sid);
    s_passkey = 100000u + esp_random() % 900000u;
    char key[RELAY_B64_KEY_CAP];
    if (!relay_b64url_encode(key, sizeof key, s_secret, sizeof s_secret)) return false;
    char sid_hex[9];
    snprintf(sid_hex, sizeof sid_hex, "%02x%02x%02x%02x",
             s_sid[0], s_sid[1], s_sid[2], s_sid[3]);
    const char *device_id = s_device_id;
    int n = snprintf(s_qr_payload, sizeof s_qr_payload,
                     "tesserae://setup?v=%u&id=%s&sid=%s&key=%s",
                     (unsigned)BLE_SETUP_PROTOCOL_MAJOR,
                     device_id, sid_hex, key);
    return n > 0 && (size_t)n < sizeof s_qr_payload;
}

static bool start_ble(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err)); return false; }
    s_nimble_initialized = true;
    if (ble_npl_callout_init(&s_tx_callout, nimble_port_get_dflt_eventq(), pump_tx, NULL) != 0)
        return false;
    s_tx_initialized = true;
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_services) != 0 || ble_gatts_add_svcs(s_services) != 0) {
        return false; // caller owns complete callout/controller cleanup
    }
    ble_svc_gap_device_name_set("Tesserae");
    nimble_port_freertos_init(host_task);
    s_host_started = true;
    return true;
}

static void stop_ble(void)
{
    s_stopping = true;

    /* Stop application work before dismantling the GATT host. This prevents a
     * scan or configuration command from sending through NimBLE while its
     * queues and mutexes are being released. */
    if (s_worker) {
        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_WORKER_STOPPED, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(5000));
        if (!(bits & BIT_WORKER_STOPPED)) {
            ESP_LOGE(TAG, "BLE worker did not stop; restarting instead of unsafe teardown");
            esp_restart();
        }
        s_worker = NULL;
    }

    if (s_nimble_initialized && s_host_started) {
        /* nimble_port_stop() already terminates active links and advertising.
         * Manually terminating first raced its asynchronous disconnect with
         * ble_hs_stop(), which returned EALREADY and left the host task live
         * while the caller deinitialized its event queue. */
        if (s_tx_initialized) ble_npl_callout_stop(&s_tx_callout);
        int rc = nimble_port_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "NimBLE host stop failed (%d); restarting safely", rc);
            esp_restart();
        }

        EventBits_t bits = xEventGroupWaitBits(
            s_events, BIT_HOST_STOPPED, pdFALSE, pdTRUE,
            pdMS_TO_TICKS(3000));
        if (!(bits & BIT_HOST_STOPPED)) {
            ESP_LOGE(TAG, "NimBLE host task did not exit; restarting safely");
            esp_restart();
        }

        nimble_port_freertos_deinit();
        s_host_started = false;
    }
    if (s_nimble_initialized) {
        if (s_tx_initialized) {
            ble_npl_callout_deinit(&s_tx_callout);
            s_tx_initialized = false;
        }
        esp_err_t err = nimble_port_deinit();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NimBLE deinit failed: %s; restarting safely",
                     esp_err_to_name(err));
            esp_restart();
        }
    }
    s_nimble_initialized = false;
    s_host_started = false;
    ESP_LOGI(TAG, "BLE host stopped cleanly");
}

static void scrub_session(void)
{
    if (s_commands) {
        command_t pending;
        while (xQueueReceive(s_commands, &pending, 0) == pdTRUE)
            memset(&pending, 0, sizeof pending);
        vQueueDelete(s_commands);
        s_commands = NULL;
    }
    if (s_outgoing) { vQueueDelete(s_outgoing); s_outgoing = NULL; }
    memset(&s_tx, 0, sizeof s_tx);
    memset(s_tx_frame, 0, sizeof s_tx_frame);
    s_tx_active = false;
    atomic_store(&s_pending_events, 0);
    if (s_events) { vEventGroupDelete(s_events); s_events = NULL; }
    if (s_lock) { vSemaphoreDelete(s_lock); s_lock = NULL; }
    memset(s_secret, 0, sizeof s_secret);
    memset(s_connection_nonce, 0, sizeof s_connection_nonce);
    memset(&s_crypto, 0, sizeof s_crypto);
    memset(&s_staged, 0, sizeof s_staged);
    memset(&s_qr_reassembly, 0, sizeof s_qr_reassembly);
    memset(&s_native_reassembly, 0, sizeof s_native_reassembly);
    memset(s_qr_payload, 0, sizeof s_qr_payload);
    memset(s_info, 0, sizeof s_info);
    s_passkey = 0;
    memset(s_last_event, 0, sizeof s_last_event);
    memset(s_logs, 0, sizeof s_logs);
}

ble_setup_result_t ble_setup_run(uint32_t timeout_s)
{
    s_result = BLE_SETUP_RESULT_TIMEOUT;
    s_stopping = false; s_notify = false; s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_nimble_initialized = false; s_host_started = false;
    s_worker = NULL; s_out_message_id = 0; s_last_event_len = 0; s_log_count = 0;
    memset(&s_staged, 0, sizeof s_staged);
    ble_setup_reassembly_reset(&s_qr_reassembly);
    ble_setup_reassembly_reset(&s_native_reassembly);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof s_device_id, "picpak-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    s_commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    s_outgoing = xQueueCreate(6, sizeof(outgoing_event_t));
    if (!s_lock || !s_events || !s_commands || !s_outgoing || !prepare_session()) {
        scrub_session();
        return BLE_SETUP_RESULT_ERROR;
    }
    add_log("BLE maintenance started");

    // Finish the panel refresh before advertising: the QR must match this session,
    // and display/radio peaks should not overlap on PicPak's marginal supply.
    if (!maintenance_screen_render(framebuf(), s_qr_payload, s_passkey) ||
        epd_init() != ESP_OK) {
        scrub_session();
        return BLE_SETUP_RESULT_ERROR;
    }
    config_clear_frame_ref();
    epd_display(framebuf());
    epd_sleep();

    if (xTaskCreate(worker_task, "ble_setup_work", 7168, NULL, 5, &s_worker) != pdPASS ||
        !start_ble()) {
        s_result = BLE_SETUP_RESULT_ERROR;
        goto cleanup;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_DONE, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_s * 1000u));
    if (!(bits & BIT_DONE)) s_result = BLE_SETUP_RESULT_TIMEOUT;
cleanup:
    stop_ble();
    ble_wifi_stop();
    scrub_session();
    if (s_result == BLE_SETUP_RESULT_TIMEOUT || s_result == BLE_SETUP_RESULT_ERROR ||
        s_result == BLE_SETUP_RESULT_REBOOT) {
        // Do not leave an expired QR on the screen if the server is offline.
        maintenance_screen_closed(framebuf());
        if (epd_init() == ESP_OK) { epd_display(framebuf()); epd_sleep(); }
    }
    return s_result;
}
