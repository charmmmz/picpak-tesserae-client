// epd_driver.c — PicPak UC81xx-class e-paper SPI driver
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include "epd_driver.h"
#include "epd_init_seq.h"
#include "epd_lut_5s.h"
#include "epd_lut_10s.h"
#include "epd_tempcomp.h"
#include "config_store.h"
#include "defaults.h"
#include "board.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <limits.h>

static const char *TAG = "epd";

// --- Temperature compensation (EXPERIMENTAL, branch feat/epd-tempcomp) --------
// Nudges the panel PWR (0x01) drive voltage by a small signed step derived from
// the C3 on-die temperature. See epd_tempcomp.h. Safe by design: only the 5s/10s
// vendor-LUT path, only a source-drive byte (never flags/gate), clamped to ±6,
// and a no-op at the reference temperature.
#ifndef EPD_TEMPCOMP
#define EPD_TEMPCOMP         1   // master on/off for the temp->PWR nudge
#endif
#ifndef EPD_TEMPCOMP_REF_C
#define EPD_TEMPCOMP_REF_C   30  // reference die temp (deg C); corr==0 here. Set to the
                                 // observed room-temp die reading (~30C, incl. self-heat)
                                 // so normal indoor use sits at the baseline look.
#endif
#ifndef EPD_TEMPCOMP_CLAMP
#define EPD_TEMPCOMP_CLAMP   6   // max |correction| in LSB
#endif
#ifndef EPD_TEMPCOMP_TARGET_IDX
#define EPD_TEMPCOMP_TARGET_IDX 2 // pwr[] byte to nudge: 2=VDH 3=VDL 4=VDHR (never 0=flags/1=gate)
#endif
#ifndef EPD_TEMPCOMP_FORCE
#define EPD_TEMPCOMP_FORCE   0   // bench override: if nonzero, use this corr regardless of temp
#endif

#if EPD_TEMPCOMP
#include "driver/temperature_sensor.h"
// Read the ESP32-C3 on-die temperature sensor once. Returns whole deg C, or
// INT_MIN on failure (caller then applies no correction).
static int epd_read_die_temp_c(void) {
    temperature_sensor_handle_t h = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &h) != ESP_OK) return INT_MIN;
    int out = INT_MIN;
    if (temperature_sensor_enable(h) == ESP_OK) {
        float t = 0;
        if (temperature_sensor_get_celsius(h, &t) == ESP_OK)
            out = (int)(t >= 0 ? t + 0.5f : t - 0.5f);
        temperature_sensor_disable(h);
    }
    temperature_sensor_uninstall(h);
    return out;
}
#endif
static spi_device_handle_t s_spi;
static bool s_spi_ready = false;   // SPI bus set up once per boot; epd_init re-callable

// UC81xx BUSY is active-low: panel is busy while the line reads 0.
static void epd_wait_busy(void) {
    int guard = 0;
    while (gpio_get_level(PIN_EPD_BUSY) == 0 && guard++ < 4000)
        vTaskDelay(pdMS_TO_TICKS(10));   // up to ~40 s guard
    if (guard >= 4000) ESP_LOGW(TAG, "wait_busy timeout");
}

static void epd_cmd(uint8_t c) {
    gpio_set_level(PIN_EPD_DC, 0);       // DC low = command
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    spi_device_polling_transmit(s_spi, &t);
}

static void epd_data(const uint8_t *d, int n) {
    if (n <= 0) return;
    gpio_set_level(PIN_EPD_DC, 1);       // DC high = data
    for (int off = 0; off < n; off += 512) {        // 512-byte chunks
        int chunk = (n - off > 512) ? 512 : (n - off);
        spi_transaction_t t = { .length = 8 * chunk, .tx_buffer = d + off };
        spi_device_polling_transmit(s_spi, &t);
    }
}

static void epd_reset(void) {
    gpio_set_level(PIN_EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_EPD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
}

static void epd_send(uint8_t cmd, const uint8_t *data, int n) {
    epd_cmd(cmd);
    epd_data(data, n);
}

// Vendor external-LUT init + waveform upload (SSD2683 P420E55). Works for any
// vendor LUT body (5s = LUT A, 10s = LUT B) because the 8-byte timing trailer at
// L[0x210] carries the per-waveform PWR / 0x82 / PLL values. Two things MUST be
// right or the panel hangs (BUSY never releases): the 0x06 BTST booster must run
// (else the HV rails never rise), and the 0x20 LUT upload must be prefixed with
// the 7-byte header (trailer bytes) so the controller has phase/frame counts —
// payload is 7 + 528 = 535 bytes.
static void epd_init_vendor_lut(const uint8_t *L) {
    const uint8_t *TR = L + 0x210;        // trailer; first 7 bytes = 0x20 header

    epd_send(0x06, (const uint8_t[]){0x0F, 0x8B, 0x9C, 0x96}, 4); // BTST booster
    epd_send(0x50, (const uint8_t[]){0x37}, 1);                   // CDI
    epd_send(0x00, (const uint8_t[]){0x07, 0xA9}, 2);            // PSR (0x80=ext LUT)
    vTaskDelay(pdMS_TO_TICKS(5));

    uint8_t pwr[6] = { 0x07, TR[0], TR[1], TR[3], TR[2], TR[4] };
    // pwr[]: 0=flags 1=VGH/VGL 2=VDH 3=VDL 4=VDHR 5=+ . Temperature nudge (if
    // enabled) adjusts one source-drive byte; no-op at the reference temp.
#if EPD_TEMPCOMP
    {
        int die  = epd_read_die_temp_c();
        int corr = (EPD_TEMPCOMP_FORCE != 0) ? EPD_TEMPCOMP_FORCE
                 : (die == INT_MIN)           ? 0
                 : epd_tempcomp_corr(die, EPD_TEMPCOMP_REF_C, EPD_TEMPCOMP_CLAMP);
        int idx = EPD_TEMPCOMP_TARGET_IDX;
        int nv  = (int)pwr[idx] + corr;
        if (nv < 0) nv = 0; else if (nv > 255) nv = 255;
        uint8_t old = pwr[idx];
        pwr[idx] = (uint8_t)nv;
        if (die == INT_MIN)
            ESP_LOGW(TAG, "tempcomp: die temp read FAILED; no correction applied");
        ESP_LOGI(TAG, "tempcomp: die=%dC ref=%d clamp=%d force=%d delta=%d corr=%d | "
                      "PWR[%d] %02X->%02X | full=%02X %02X %02X %02X %02X %02X",
                 die, EPD_TEMPCOMP_REF_C, EPD_TEMPCOMP_CLAMP, EPD_TEMPCOMP_FORCE,
                 (die == INT_MIN ? 0 : die - EPD_TEMPCOMP_REF_C), corr, idx, old, pwr[idx],
                 pwr[0], pwr[1], pwr[2], pwr[3], pwr[4], pwr[5]);
    }
#endif
    epd_send(0x01, pwr, 6);                                       // PWR (trailer)
    const uint8_t v82 = (uint8_t)(TR[5] - 0x80);
    epd_send(0x82, &v82, 1);                                      // vendor
    const uint8_t pll = TR[6];
    epd_send(0x30, &pll, 1);                                      // PLL (5s=0x06,10s=0x03)

    epd_send(0x61, (const uint8_t[]){0x01, 0x90, 0x01, 0x2C}, 4); // TRES 400x300
    vTaskDelay(pdMS_TO_TICKS(5));
    epd_send(0x62, (const uint8_t[]){0x62, 0x51}, 2);            // vendor
    epd_send(0x65, (const uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4); // GSST
    vTaskDelay(pdMS_TO_TICKS(5));
    epd_send(0xE7, (const uint8_t[]){0x96}, 1);                  // vendor
    epd_send(0xE9, (const uint8_t[]){0x01}, 1);                  // vendor

    // LUT load: cmd 0x20, the 7-byte header (trailer), THEN 528 waveform bytes
    // streamed column-major (48 rows x 11, stride 48).
    epd_cmd(0x20);
    epd_data(TR, 7);                                              // 7-byte header
    for (int row = 0; row < 48; row++) {
        uint8_t line[11];
        for (int col = 0; col < 11; col++) line[col] = L[row + 48 * col];
        epd_data(line, 11);
    }
    ESP_LOGI(TAG, "init done (vendor LUT, PLL=0x%02X)", pll);
}

// Panel built-in "native MTP" waveform — the safe, slow default. Iterate by
// size (0xFF is a valid command byte here, not a sentinel).
static void epd_init_native(void) {
    const uint8_t *seq = EPD_INIT_SPECIFIC;      // shipping panels report EPD ID 06 04
    size_t n = sizeof(EPD_INIT_SPECIFIC);
    for (size_t i = 0; i < n; ) {
        uint8_t cmd = seq[i++];
        uint8_t len = seq[i++];
        epd_cmd(cmd);
        epd_data(&seq[i], len);
        i += len;
    }
    ESP_LOGI(TAG, "init done (native MTP)");
}

esp_err_t epd_init(void) {
    gpio_config_t out = { .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_DC) | (1ULL << PIN_EPD_RST) };
    gpio_config(&out);
    gpio_config_t in = { .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_EPD_BUSY) };
    gpio_config(&in);

    if (!s_spi_ready) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_EPD_MOSI, .miso_io_num = PIN_EPD_MISO,
            .sclk_io_num = PIN_EPD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
            .max_transfer_sz = 512,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
        spi_device_interface_config_t dev = {
            .clock_speed_hz = EPD_SPI_HZ, .mode = 0,
            .spics_io_num = PIN_EPD_CS, .queue_size = 4,
        };
        ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_spi));
        s_spi_ready = true;
    }

    epd_reset();
    epd_wait_busy();

    // Dispatch on the selected refresh waveform (portal-set, NVS-persisted, with
    // the compile-time DEFAULT_WAVEFORM fallback). 5s/10s upload a vendor LUT;
    // native uses the panel's built-in MTP waveform.
    switch (config_get_waveform(DEFAULT_WAVEFORM)) {
    case EPD_WAVE_5S:
        ESP_LOGI(TAG, "waveform: 5s (vendor fast LUT)");
        epd_init_vendor_lut(EPD_LUT5S);
        break;
    case EPD_WAVE_10S:
        ESP_LOGI(TAG, "waveform: 10s (vendor balanced LUT)");
        epd_init_vendor_lut(EPD_LUT10S);
        break;
    default:
        ESP_LOGI(TAG, "waveform: native MTP");
        epd_init_native();
        break;
    }
    return ESP_OK;
}

void epd_display(const uint8_t *fb) {
    epd_cmd(0x10);                       // DTM1: framebuffer load (confirm opcode)
    epd_data(fb, EPD_FB_BYTES);
    epd_cmd(0x04); epd_wait_busy();      // Power ON
    epd_cmd(0x12); epd_wait_busy();      // Display Refresh
    ESP_LOGI(TAG, "display done");
}

void epd_sleep(void) {
    // Power OFF (0x02) carries a 0x00 parameter byte on this controller.
    uint8_t pof = 0x00;
    epd_cmd(0x02); epd_data(&pof, 1); epd_wait_busy();
    // Deep Sleep (0x07) requires the 0xA5 check-code parameter — the controller
    // ignores a bare 0x07 (a guard against an accidental sleep), so without the
    // check-code the panel never leaves standby and keeps drawing between wakes.
    uint8_t dslp = 0xA5;
    epd_cmd(0x07); epd_data(&dslp, 1);
    ESP_LOGI(TAG, "panel deep sleep");
}
