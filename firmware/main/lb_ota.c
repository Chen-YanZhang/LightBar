// lb_ota.c - see lb_ota.h
#include "lb_ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_system.h"

#include "lightbar_out.h"
#include "safety_sm.h"

static const char *TAG = "lb-ota";

static bool s_busy;
static bool s_pending_verify;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_slot;
static size_t s_written;

const char *lb_ota_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}

bool lb_ota_busy(void)
{
    return s_busy;
}

bool lb_ota_pending_verify(void)
{
    return s_pending_verify;
}

esp_err_t lb_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_ota_img_states_t st;
    esp_err_t e = esp_ota_get_state_partition(running, &st);
    s_pending_verify = (e == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY);
    ESP_LOGI(TAG, "running slot %s @0x%06" PRIx32 " version=%s pending_verify=%d", running->label,
             running->address, lb_ota_version(), (int)s_pending_verify);
    return ESP_OK;
}

void lb_ota_mark_valid(bool boot_ok)
{
    if (!s_pending_verify) {
        return; /* nothing to confirm: normal boot of an already-valid image */
    }
    if (boot_ok) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "new image confirmed (rollback cancelled): %s", esp_err_to_name(e));
        s_pending_verify = false;
    } else {
        /* Roll back NOW. Leaving the image in PENDING_VERIFY would let a broken
         * build keep running until the next (possibly weeks-later) reset, and
         * then silently revert - impossible to correlate with anything. */
        ESP_LOGE(TAG, "boot self-test FAILED - rolling back to the previous image");
        vTaskDelay(pdMS_TO_TICKS(500)); /* let the log flush */
        esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "rollback request returned %s (no valid previous image?) - staying up",
                 esp_err_to_name(e));
        s_pending_verify = false; /* nothing left to do; do not retry every boot */
    }
}

/* Shared precondition check for both entry points. */
static esp_err_t ota_precheck(char *err, size_t err_len)
{
    if (s_busy) {
        snprintf(err, err_len, "OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    if (safety_sm_overtemp()) {
        bool v = false;
        float t = safety_sm_board_temp(&v);
        snprintf(err, err_len, "refused: board overtemperature (%.1f C)", t);
        return ESP_ERR_INVALID_STATE;
    }
    /* Force the light off for the whole update: the flash write stalls the CPU
     * long enough that keeping 6 LED strings running is not worth the risk. */
    lightbar_out_inhibit(true);
    lightbar_out_emergency_off();
    s_busy = true;
    return ESP_OK;
}

static void ota_release(void)
{
    s_busy = false;
    lightbar_out_inhibit(false);
}

esp_err_t lb_ota_stream_begin(size_t image_size)
{
    char err[96];
    esp_err_t e = ota_precheck(err, sizeof(err));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s", err);
        return e;
    }
    s_slot = esp_ota_get_next_update_partition(NULL);
    if (s_slot == NULL) {
        ota_release();
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "writing %s @0x%06" PRIx32 " (%" PRIu32 " bytes)", s_slot->label, s_slot->address,
             (uint32_t)s_slot->size);
    e = esp_ota_begin(s_slot, image_size ? image_size : OTA_SIZE_UNKNOWN, &s_handle);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
        ota_release();
        return e;
    }
    s_written = 0;
    return ESP_OK;
}

esp_err_t lb_ota_stream_write(const uint8_t *data, size_t len)
{
    if (!s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = esp_ota_write(s_handle, data, len);
    if (e == ESP_OK) {
        s_written += len;
    }
    return e;
}

esp_err_t lb_ota_stream_end_and_reboot(void)
{
    if (!s_busy) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = esp_ota_end(s_handle);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s (image rejected)", esp_err_to_name(e));
        esp_ota_abort(s_handle);
        ota_release();
        return e;
    }
    e = esp_ota_set_boot_partition(s_slot);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "set_boot_partition: %s", esp_err_to_name(e));
        ota_release();
        return e;
    }
    ESP_LOGW(TAG, "%u bytes written to %s - rebooting into the new image", (unsigned)s_written,
             s_slot->label);
    vTaskDelay(pdMS_TO_TICKS(400)); /* let the log flush before the reset */
    esp_restart();
    return ESP_OK; /* not reached */
}

void lb_ota_stream_abort(void)
{
    if (!s_busy) {
        return;
    }
    esp_ota_abort(s_handle);
    ESP_LOGW(TAG, "OTA aborted after %u bytes", (unsigned)s_written);
    ota_release();
}

esp_err_t lb_ota_from_url(const char *url, char *err, size_t err_len)
{
    if (url == NULL || strlen(url) < 8) {
        snprintf(err, err_len, "bad url");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e = ota_precheck(err, err_len);
    if (e != ESP_OK) {
        return e;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        ota_release();
        snprintf(err, err_len, "http client init failed");
        return ESP_FAIL;
    }
    e = esp_http_client_open(cli, 0);
    if (e != ESP_OK) {
        snprintf(err, err_len, "connect: %s", esp_err_to_name(e));
        esp_http_client_cleanup(cli);
        ota_release();
        return e;
    }
    int64_t clen = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    if (status != 200) {
        snprintf(err, err_len, "HTTP %d", status);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        ota_release();
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "downloading %s (content-length %" PRId64 ")", url, clen);

    s_slot = esp_ota_get_next_update_partition(NULL);
    if (s_slot == NULL) {
        snprintf(err, err_len, "no update partition");
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        ota_release();
        return ESP_ERR_NOT_FOUND;
    }
    e = esp_ota_begin(s_slot, (clen > 0) ? (size_t)clen : OTA_SIZE_UNKNOWN, &s_handle);
    if (e != ESP_OK) {
        snprintf(err, err_len, "ota_begin: %s", esp_err_to_name(e));
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        ota_release();
        return e;
    }

    uint8_t *buf = malloc(4096);
    if (buf == NULL) {
        snprintf(err, err_len, "no memory");
        esp_ota_abort(s_handle);
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        ota_release();
        return ESP_ERR_NO_MEM;
    }
    s_written = 0;
    e = ESP_OK;
    while (true) {
        int n = esp_http_client_read(cli, (char *)buf, 4096);
        if (n < 0) {
            e = ESP_FAIL;
            snprintf(err, err_len, "read error at %u bytes", (unsigned)s_written);
            break;
        }
        if (n == 0) {
            break;
        }
        esp_err_t we = esp_ota_write(s_handle, buf, (size_t)n);
        if (we != ESP_OK) {
            e = we;
            snprintf(err, err_len, "ota_write: %s at %u bytes", esp_err_to_name(we), (unsigned)s_written);
            break;
        }
        s_written += (size_t)n;
        if ((s_written & 0xFFFF) < 4096) {
            ESP_LOGI(TAG, "  %u bytes", (unsigned)s_written);
        }
    }
    free(buf);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    if (e != ESP_OK) {
        esp_ota_abort(s_handle);
        ota_release();
        return e;
    }
    return lb_ota_stream_end_and_reboot();
}
