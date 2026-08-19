// relay.c — Tesserae cloud-relay transport. See relay.h.
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "relay.h"
#include "relay_crypto.h"
#include "relay_wire.h"
#include "config_store.h"
#include "framebuf.h"
#include "wifi_manager.h"
#include "power.h"
#include "board.h"
#include "defaults.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp
#include "esp_attr.h"  // RTC_DATA_ATTR
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "relay";

static char s_pending_etag[80];        // ETag of a frame staged but not yet painted
static char s_advertised_cfg_etag[80]; // config etag advertised by this wake's status
static bool s_have_advertised_cfg;
static bool s_frame_pending;           // framebuf() holds a fresh relay frame this wake

// Consecutive wakes whose device-token requests were answered 401. Since server
// v0.240.0 a revoked/superseded pairing deletes the token record, so 401 from any
// device-token route unambiguously means "this pairing is gone". Require it on TWO
// wakes before acting (a flaky captive portal/middlebox injecting one 401 cannot).
// RTC memory: survives deep sleep (= "across wakes"); a power cycle resets it,
// erring toward staying paired. RTC_DATA_ATTR (unlike main.c's RTC_NOINIT_ATTR
// s_button_event_seq) is zero-initialised by startup on any non-deep-sleep reset,
// so this reset-to-zero is automatic and needs no explicit cold-boot guard.
RTC_DATA_ATTR static uint32_t s_auth_fail_streak;

// One vote per wake: a wake makes up to THREE device-token requests (frame/status/
// config); without this a single revoked wake would score 3 and unpair on the spot.
// RAM, so it resets each boot — exactly the wake boundary being counted.
static bool s_auth_noted_this_wake;

// Fold one device-token response into the streak. 401 counts (once/wake); any 2xx
// or 304 clears it (the relay only serves those to a live token); anything else
// (timeout/5xx/DNS) is left alone — it says nothing about the token.
static void note_auth(int status) {
    if (status == 401) {
        if (s_auth_noted_this_wake) return;
        s_auth_noted_this_wake = true;
        s_auth_fail_streak++;
        ESP_LOGW(TAG, "relay answered 401 (streak %u); pairing may be revoked",
                 (unsigned)s_auth_fail_streak);
    } else if (status == 304 || (status >= 200 && status < 300)) {
        s_auth_fail_streak = 0;
        s_auth_noted_this_wake = false;
    }
}

bool relay_pairing_revoked(void) { return s_auth_fail_streak >= 2; }

void relay_forget_revoked_pairing(void) {
    ESP_LOGW(TAG, "relay pairing revoked; clearing mailbox identity and key");
    config_forget_relay_pairing();
    s_auth_fail_streak = 0;
}

#define RELAY_JSON_MAX  2048
#define RELAY_HTTP_MS   10000

// ---- small JSON HTTP helper ---------------------------------------------
// Pairing and status are tiny JSON round trips. Frames/config go through
// relay_get_sealed() (streamed into a sized malloc). crt bundle is attached only
// for https:// (attaching it on plain http mis-configures the client) -- matches
// rest_handler.c, and lets a self-hosted http:// Worker work for bench testing.
typedef struct { char *buf; size_t cap; size_t len; bool overflow; } rx_t;
static esp_err_t on_evt(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rx_t *rx = e->user_data;
    if (!rx || !rx->buf) return ESP_OK;
    if (rx->len + e->data_len >= rx->cap) { rx->overflow = true; return ESP_OK; }
    memcpy(rx->buf + rx->len, e->data, e->data_len);
    rx->len += e->data_len; rx->buf[rx->len] = '\0';
    return ESP_OK;
}
// One small JSON request. body==NULL means GET. Returns HTTP status or <0.
static int relay_json(const char *url, const char *method, const char *body,
                      const char *bearer, char *out, size_t out_cap) {
    if (out && out_cap) out[0] = '\0';
    rx_t rx = { .buf = out, .cap = out_cap, .len = 0, .overflow = false };
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = on_evt, .user_data = &rx,
        .timeout_ms = RELAY_HTTP_MS, .buffer_size = 1024, .buffer_size_tx = 1024,
    };
    if (strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    esp_http_client_set_method(cli, strcmp(method, "POST") == 0 ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (bearer && bearer[0]) {
        char auth[300]; snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    if (body) {
        esp_http_client_set_header(cli, "Content-Type", "application/json");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK && status <= 0) {
        ESP_LOGW(TAG, "%s %s: %s", method, url, esp_err_to_name(err));
        return -1;
    }
    if (rx.overflow) ESP_LOGW(TAG, "response truncated (> %u)", (unsigned)out_cap);
    return status;
}

// Capture the response ETag. esp_http_client_get_header() reads only REQUEST
// headers, so the RESPONSE ETag has to come through the ON_HEADER event (which
// fetch_headers() dispatches) — same pattern as rest_handler.c. Without this the
// stored ETag stays empty, no If-None-Match is ever sent, and every wake 200s
// and repaints the same frame instead of 304-ing.
typedef struct { char *etag; size_t cap; } relay_hdr_ctx_t;
static esp_err_t relay_hdr_evt(esp_http_client_event_t *e) {
    if (e->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    relay_hdr_ctx_t *h = e->user_data;
    if (h && h->etag && e->header_key && strcasecmp(e->header_key, "ETag") == 0)
        strlcpy(h->etag, e->header_value ? e->header_value : "", h->cap);
    return ESP_OK;
}

// Conditional GET of a sealed blob. On 200, mallocs *buf (caller frees), sets
// *len and captures the ETag. Returns HTTP status, or <0 on transport error.
static int relay_get_sealed(const char *url, const char *bearer,
                            const char *in_etag, char *out_etag, size_t etag_cap,
                            uint8_t **buf, size_t *len) {
    *buf = NULL; *len = 0; if (out_etag && etag_cap) out_etag[0] = '\0';
    relay_hdr_ctx_t hc = { .etag = out_etag, .cap = etag_cap };
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = RELAY_HTTP_MS, .buffer_size = 2048,
        .event_handler = relay_hdr_evt, .user_data = &hc,
    };
    if (strncmp(url, "https://", 8) == 0) cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;
    if (bearer && bearer[0]) {
        char auth[300]; snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    if (in_etag && in_etag[0]) esp_http_client_set_header(cli, "If-None-Match", in_etag);
    int status = -1;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        int clen = esp_http_client_fetch_headers(cli);   // content-length, or <0
        status = esp_http_client_get_status_code(cli);
        if (status == 200 && clen > 0) {
            uint8_t *b = malloc((size_t)clen);
            if (b) {
                int got = 0, r;
                while (got < clen && (r = esp_http_client_read(cli, (char *)b + got, clen - got)) > 0)
                    got += r;
                if (got == clen) { *buf = b; *len = (size_t)clen; }
                else { free(b); status = -1; }
            } else status = -1;
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return status;
}

// ---- state ---------------------------------------------------------------
bool relay_ready(void) { return config_relay_ready(); }
bool relay_pairing_pending(void) {
    return config_relay_configured() && !config_relay_ready();
}

// ---- pairing -------------------------------------------------------------
// Submit our public key for the operator's code. 404 means the code is unknown
// or expired -- terminal, not something to retry into a lockout.
static relay_pair_result_t pair_submit(const char *relay_url, const char *relay_code) {
    uint8_t priv[RELAY_PRIV_LEN], pub[RELAY_PUB_LEN];
    relay_keypair(priv, pub);

    char pub_b64[RELAY_B64_KEY_CAP];
    if (!relay_b64url_encode(pub_b64, sizeof pub_b64, pub, sizeof pub))
        return RELAY_PAIR_ERROR;

    // model/gamut self-report is deliberately omitted (NULL, NULL): PicPak's BWRY
    // gamut is not a relay palette id, and the server already knows the device
    // from its pairing slot. Home fills only what the slot left blank.
    char body[512];
    if (!relay_build_pair_body(body, sizeof body, relay_code, pub_b64,
                               EPD_W, EPD_H, NULL, NULL)) {
        ESP_LOGE(TAG, "could not build the pair request body");
        return RELAY_PAIR_ERROR;
    }

    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair", relay_url);
    char resp[RELAY_JSON_MAX];
    int st = relay_json(url, "POST", body, NULL, resp, sizeof resp);

    if (st == 404) {
        ESP_LOGW(TAG, "pairing code rejected (expired or unknown); clearing");
        config_clear_relay();
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) {
        ESP_LOGW(TAG, "POST /v1/pair -> %d", st);
        return RELAY_PAIR_ERROR;
    }

    // Persist the private key BEFORE reporting progress: pairing polls can span
    // deep sleeps, and losing the scalar would strand the single-use code.
    config_set_relay_priv(priv);
    ESP_LOGI(TAG, "pairing submitted (%dx%d); waiting for the home instance", EPD_W, EPD_H);
    return RELAY_PAIR_WAITING;
}

// Poll for completion and, on "ready", derive + persist the frame key. The poll
// has no expiry, so it may span as many wakes as home needs.
static relay_pair_result_t pair_poll(const char *relay_url, const char *relay_code) {
    char url[256];
    snprintf(url, sizeof url, "%s/v1/pair/%s", relay_url, relay_code);
    char resp[RELAY_JSON_MAX];
    int st = relay_json(url, "GET", NULL, NULL, resp, sizeof resp);

    if (st == 404) {
        ESP_LOGW(TAG, "pairing code expired before completion; clearing");
        config_clear_relay();
        return RELAY_PAIR_EXPIRED;
    }
    if (st < 200 || st >= 300) return RELAY_PAIR_ERROR;

    relay_pairing_t pr;
    switch (relay_parse_ready(resp, strlen(resp), &pr)) {
    case RELAY_READY_PENDING:
        return RELAY_PAIR_WAITING;      // home has not completed it yet
    case RELAY_READY_MALFORMED:
        ESP_LOGE(TAG, "pairing response malformed or missing a field");
        return RELAY_PAIR_ERROR;
    default:
        break;
    }

    uint8_t priv[RELAY_PRIV_LEN];
    if (!config_get_relay_priv(priv)) {
        // Scalar gone (NVS wiped mid-pair). The code is single-use, so start over.
        ESP_LOGE(TAG, "no panel private key held; re-pair required");
        config_clear_relay();
        return RELAY_PAIR_EXPIRED;
    }

    uint8_t key[RELAY_KEY_LEN];
    if (!relay_derive_key(key, priv, pr.home_pub)) {
        ESP_LOGE(TAG, "frame key derivation failed (degenerate peer key)");
        return RELAY_PAIR_ERROR;
    }

    config_set_relay_paired(pr.install_id, pr.device_id, pr.device_token, key);
    config_set_relay_etag("");          // new mailbox: nothing to match
    memset(key, 0, sizeof key);
    ESP_LOGI(TAG, "paired: install=%s device=%s", pr.install_id, pr.device_id);
    return RELAY_PAIR_DONE;
}

relay_pair_result_t relay_pair_step(void) {
    char url[160], code[40];
    config_get_relay_url(url, sizeof url);
    config_get_relay_code(code, sizeof code);
    if (!url[0] || !code[0]) return RELAY_PAIR_IDLE;
    if (config_relay_ready()) return RELAY_PAIR_IDLE;
    uint8_t priv[RELAY_PRIV_LEN];
    return config_get_relay_priv(priv) ? pair_poll(url, code) : pair_submit(url, code);
}

// ---- frames --------------------------------------------------------------
// Fetch + unseal the current frame, staging plaintext into framebuf(). Sets
// s_frame_pending on a new frame. Returns true if framebuf() was updated.
static bool relay_fetch_frame_into_framebuf(void) {
    s_frame_pending = false;
    if (!config_relay_ready()) return false;
    char url[320], base[160], install[64], device[64], token[256], etag_in[80];
    config_get_relay_url(base, sizeof base);
    config_get_relay_install(install, sizeof install);
    config_get_relay_device(device, sizeof device);
    config_get_relay_token(token, sizeof token);
    config_get_relay_etag(etag_in, sizeof etag_in);
    if (!relay_mailbox_url(url, sizeof url, base, install, device, "frame")) {
        ESP_LOGE(TAG, "cannot build frame URL"); return false;
    }
    uint8_t key[RELAY_KEY_LEN];
    if (!config_get_relay_key(key)) return false;

    uint8_t *blob = NULL; size_t blob_len = 0; char etag_out[80];
    int st = relay_get_sealed(url, token, etag_in, etag_out, sizeof etag_out, &blob, &blob_len);
    note_auth(st);   // folds 401 into the 2-wake revocation streak; 2xx/304 clears it
    if (st == 304) { ESP_LOGI(TAG, "relay frame unchanged (304)"); return false; }
    if (st == 204) { ESP_LOGI(TAG, "relay has no frame yet (204)"); return false; }
    if (st != 200 || !blob) {
        ESP_LOGW(TAG, "relay frame fetch failed (http %d)", st);
        if (blob) free(blob);
        return false;   // revocation is handled by relay_pairing_revoked() (2 wakes)
    }

    uint8_t *plain = NULL; size_t plain_len = 0;
    if (!relay_unseal(blob, blob_len, key, &plain, &plain_len)) {
        // A failed GCM tag must never be painted: the mailbox does not match our
        // key. Drop the ETag so the next poll refetches, never 304s onto it.
        ESP_LOGE(TAG, "sealed frame failed authentication (%u bytes); dropping",
                 (unsigned)blob_len);
        free(blob);
        config_set_relay_etag("");
        return false;
    }
    if (plain_len != EPD_FB_BYTES) {
        ESP_LOGE(TAG, "relay frame is %u bytes, panel expects %u; dropping",
                 (unsigned)plain_len, (unsigned)EPD_FB_BYTES);
        free(blob);
        config_set_relay_etag("");
        return false;
    }
    memcpy(framebuf(), plain, EPD_FB_BYTES);
    free(blob);
    strlcpy(s_pending_etag, etag_out, sizeof s_pending_etag);
    s_frame_pending = true;
    ESP_LOGI(TAG, "relay frame %u bytes -> painting (etag %s)",
             (unsigned)plain_len, etag_out[0] ? etag_out : "(none)");
    return true;
}

const uint8_t *relay_pending_frame(void) { return s_frame_pending ? framebuf() : NULL; }
bool relay_poll_frame(void) { return relay_fetch_frame_into_framebuf(); }
void relay_frame_painted(void) {
    if (s_pending_etag[0]) { config_set_relay_etag(s_pending_etag); s_pending_etag[0] = '\0'; }
    s_frame_pending = false;
}

// ---- status --------------------------------------------------------------
static void relay_post_status(const char *button, uint32_t button_event_id) {
    if (!config_relay_ready()) return;
    char base[160], install[64], device[64], token[256], url[320];
    config_get_relay_url(base, sizeof base);
    config_get_relay_install(install, sizeof install);
    config_get_relay_device(device, sizeof device);
    config_get_relay_token(token, sizeof token);
    if (!relay_mailbox_url(url, sizeof url, base, install, device, "status")) return;

    char ip[16] = {0};
    wifi_get_sta_ip(ip, sizeof ip);
    int rssi = wifi_rssi();
    int mv = power_battery_mv();
    // A press rides the status body: a relay panel's frame GET terminates at the
    // relay, so REST's ?button= never reaches home. button_event_id is REQUIRED
    // here (the server's time-window dedup is unreliable over a polled relay).
    char body[512];
    if (!relay_build_status_body(body, sizeof body, device, FW_VERSION,
                                 EPD_W, EPD_H, ip, rssi,
                                 mv, mv > 0 ? power_battery_pct(mv) : 0,
                                 (button && button[0]) ? button : NULL,
                                 button_event_id)) {
        ESP_LOGE(TAG, "could not build the relay status body");
        return;
    }
    if (button && button[0])
        ESP_LOGI(TAG, "reporting button '%s' (event %lu) over the relay",
                 button, (unsigned long)button_event_id);

    char resp[RELAY_JSON_MAX];
    int st = relay_json(url, "POST", body, token, resp, sizeof resp);
    note_auth(st);
    if (st < 200 || st >= 300) { ESP_LOGW(TAG, "status post -> %d", st); return; }

    // The response advertises the current config etag when one exists, so posting
    // status doubles as the change notification (relay_sync_config can then skip
    // its request when nothing moved). Absent simply leaves no hint.
    s_advertised_cfg_etag[0] = '\0';
    s_have_advertised_cfg = relay_parse_config_etag(resp, strlen(resp),
                              s_advertised_cfg_etag, sizeof s_advertised_cfg_etag);
}

// ---- device config -------------------------------------------------------
static void relay_sync_config(void) {
    if (!config_relay_ready()) return;
    char cfg_etag[80];
    config_get_relay_config_etag(cfg_etag, sizeof cfg_etag);
    // Cheapest path: this wake's status advertised exactly the etag we hold.
    if (s_have_advertised_cfg && cfg_etag[0] &&
        strcmp(s_advertised_cfg_etag, cfg_etag) == 0) return;

    char base[160], install[64], device[64], token[256], url[320];
    config_get_relay_url(base, sizeof base);
    config_get_relay_install(install, sizeof install);
    config_get_relay_device(device, sizeof device);
    config_get_relay_token(token, sizeof token);
    if (!relay_mailbox_url(url, sizeof url, base, install, device, "config")) return;

    uint8_t *blob = NULL; size_t blob_len = 0; char etag_out[80];
    int st = relay_get_sealed(url, token, cfg_etag, etag_out, sizeof etag_out, &blob, &blob_len);
    note_auth(st);
    if (st == 304 || st == 204) { if (blob) free(blob); return; }
    if (st != 200 || !blob) { if (blob) free(blob); return; }

    uint8_t key[RELAY_KEY_LEN];
    if (!config_get_relay_key(key)) { free(blob); return; }
    uint8_t *plain = NULL; size_t plain_len = 0;
    if (!relay_unseal(blob, blob_len, key, &plain, &plain_len)) {
        ESP_LOGE(TAG, "sealed config failed authentication; dropping");
        free(blob);
        config_set_relay_config_etag("");   // refetch next wake
        return;
    }
    relay_devcfg_t dc;
    bool parsed = relay_parse_config((const char *)plain, plain_len, &dc);
    free(blob);
    if (!parsed) { ESP_LOGW(TAG, "relay config is not a JSON object; ignoring"); return; }

    // This deep-sleep board adopts only sleep_interval_s (no button_wake_s /
    // always_on). Absent (-1) means keep what we have.
    if (dc.sleep_interval_s > 0)
        config_set_sleep_s((uint32_t)dc.sleep_interval_s);
    config_set_relay_config_etag(etag_out);   // store last, always
    ESP_LOGI(TAG, "relay config applied (etag %s): sleep=%lds",
             etag_out[0] ? etag_out : "(none)", (long)dc.sleep_interval_s);
}

// ---- wake cycle ----------------------------------------------------------
int relay_run_loop(const char *button, uint32_t button_event_id) {
    relay_fetch_frame_into_framebuf();          // stages into framebuf() + s_frame_pending
    relay_post_status(button, button_event_id); // carries the pending button (if any)
    relay_sync_config();                        // may adopt sleep_interval_s
    return config_get_sleep_s(SLEEP_INTERVAL_DEFAULT_S);   // re-read after adoption
}
