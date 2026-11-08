// sht41.c - see sht41.h. Targets the SHT4x family per datasheet D1 v7.3.
#include "sht41.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define XFER_TIMEOUT_MS 1000
#define MEAS_WAIT_MS    10 /* tMEAS,h max 8.3 ms (datasheet Table 6) */
#define RESET_WAIT_MS   2  /* tSR max 1 ms */
#define SER_WAIT_MS     2  /* 0x89: measured - no wait => NACK, 2 ms => OK */

static uint8_t crc8(const uint8_t *data, size_t len)
{
    /* SHT4x CRC: poly 0x31, init 0xFF, no reflection, no final xor. */
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t send_cmd(i2c_master_dev_handle_t dev, uint8_t cmd)
{
    return i2c_master_transmit(dev, &cmd, 1, XFER_TIMEOUT_MS);
}

esp_err_t sht41_reset(i2c_master_dev_handle_t dev)
{
    esp_err_t e = send_cmd(dev, SHT41_CMD_RESET);
    if (e == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RESET_WAIT_MS));
    }
    return e;
}

esp_err_t sht41_measure(i2c_master_dev_handle_t dev, float *temp_c, float *rel_hum)
{
    uint8_t buf[6];

    /* Command and readout are separate transactions: the sensor does NOT clock
     * stretch, it holds the bus busy internally, so we wait tMEAS ourselves.
     * If we read too early the sensor answers NACK to the read header. */
    esp_err_t e = send_cmd(dev, SHT41_CMD_MEAS_HP);
    if (e != ESP_OK) {
        return e;
    }
    vTaskDelay(pdMS_TO_TICKS(MEAS_WAIT_MS));

    e = i2c_master_receive(dev, buf, sizeof(buf), XFER_TIMEOUT_MS);
    if (e != ESP_OK) {
        return e;
    }
    if (crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t st = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t srh = (uint16_t)((buf[3] << 8) | buf[4]);

    /* Datasheet 4.6: t = -45 + 175 * S/65535, RH = -6 + 125 * S/65535.
     * (Dividing by 65536 as a "close enough" shortcut costs up to 0.003 C, but
     * the old -50 offset was a real bug: every reading was 5 C too low.) */
    if (temp_c != NULL) {
        *temp_c = -45.0f + 175.0f * (float)st / 65535.0f;
    }
    if (rel_hum != NULL) {
        float h = -6.0f + 125.0f * (float)srh / 65535.0f;
        if (h < 0.0f) {
            h = 0.0f;
        }
        if (h > 100.0f) {
            h = 100.0f;
        }
        *rel_hum = h;
    }
    return ESP_OK;
}

esp_err_t sht41_serial_ex(i2c_master_dev_handle_t dev, uint32_t *serial, uint8_t *raw, size_t raw_len,
                          size_t *rx_len)
{
    if (raw == NULL || raw_len < 6) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rx_len != NULL) {
        *rx_len = 0;
    }

    /* 0x89 answers with SIX bytes: two 16-bit words, each followed by its own
     * CRC byte. Two things are load-bearing and were both wrong before:
     *  1. the frame is 6 bytes, not 3/4 (a truncated frame makes the sensor
     *     NACK, because a half-read frame means "not ready");
     *  2. the command needs ~2 ms before the read - measured on this part:
     *     no-wait -> NACK, +2 ms -> 6 bytes with both CRCs valid. A repeated
     *     start (transmit_receive) also NACKs, so command and readout must stay
     *     two separate transactions. */
    esp_err_t e = send_cmd(dev, SHT41_CMD_READ_SER_NUM);
    if (e != ESP_OK) {
        return e;
    }
    vTaskDelay(pdMS_TO_TICKS(SER_WAIT_MS));
    e = i2c_master_receive(dev, raw, 6, XFER_TIMEOUT_MS);
    if (e != ESP_OK) {
        return e;
    }
    if (rx_len != NULL) {
        *rx_len = 6;
    }
    if (crc8(raw, 2) != raw[2]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (crc8(raw + 3, 2) != raw[5]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (serial != NULL) {
        *serial = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[3] << 8) |
                  (uint32_t)raw[4];
    }
    return ESP_OK;
}

esp_err_t sht41_serial(i2c_master_dev_handle_t dev, uint32_t *serial)
{
    uint8_t raw[6];
    return sht41_serial_ex(dev, serial, raw, sizeof(raw), NULL);
}

bool sht41_selftest(i2c_master_dev_handle_t dev, sht41_info_t *info, char *reason, size_t len)
{
    const size_t n = 3;
    float tmin = 1e9f, tmax = -1e9f;
    if (info == NULL || reason == NULL || len == 0) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    reason[0] = '\0';

    esp_err_t e = sht41_reset(dev);
    if (e != ESP_OK) {
        snprintf(reason, len, "reset failed: %s", esp_err_to_name(e));
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        float t = 0, h = 0;
        e = sht41_measure(dev, &t, &h);
        if (e != ESP_OK) {
            snprintf(reason, len, "measurement %u failed: %s", (unsigned)i, esp_err_to_name(e));
            return false;
        }
        if (t < tmin) tmin = t;
        if (t > tmax) tmax = t;
        if (i + 1 == n) {
            info->temp_c = t;
            info->rel_hum = h;
        }
    }

    uint8_t raw[6] = { 0 };
    size_t rx = 0;
    esp_err_t ser_e = sht41_serial_ex(dev, &info->serial, raw, sizeof(raw), &rx);
    info->serial_ok = (ser_e == ESP_OK);
    if (!info->serial_ok) {
        /* Soft finding, not a gate: the measurements above already passed with
         * valid CRCs, so the sensor works; only its identity is unknown. */
        info->serial = 0;
        snprintf(reason, len, "serial 0x89: %s rx=%u raw=%02X %02X %02X %02X %02X %02X",
                 esp_err_to_name(ser_e), (unsigned)rx, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
        (void)sht41_reset(dev); /* leave the sensor idle, not mid-frame */
    }

    if (!(info->temp_c > -20.0f && info->temp_c < 85.0f)) {
        snprintf(reason, len, "temperature out of bench range (%.2f C)", info->temp_c);
        return false;
    }
    if (tmax - tmin > 2.0f) {
        snprintf(reason, len, "sample spread too large (%.2f C)", tmax - tmin);
        return false;
    }
    return true;
}
