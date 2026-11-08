// lb_ota.h - OTA core: write the inactive slot, switch boot partition, and let
// the bootloader roll back if the new image does not survive its own boot.
//
// Scope for this (open-source) project: basic safety only. No secure boot, no
// image signing. What we DO guarantee:
//   * the image is validated by esp_ota (app descriptor + checksum) before it
//     can ever become the boot partition;
//   * a new image that panics before marking itself valid rolls back on the next
//     reset (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is on);
//   * OTA is refused while the board is over-temperature or the drivers are
//     reporting critical faults, and the light outputs are forced off for the
//     whole flash write.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LB_OTA_URL_MAX 300

/* Call once, early in app_main (before the self-tests). Reports whether this
 * boot is a freshly-flashed image awaiting its verification mark. */
esp_err_t lb_ota_init(void);

/* True when the running image is in ESP_OTA_IMG_PENDING_VERIFY. */
bool lb_ota_pending_verify(void);

/* Mark the running image good (cancels rollback). Call once the boot self-tests
 * and the Matter start have succeeded. No-op when no verification is pending. */
void lb_ota_mark_valid(bool boot_ok);

const char *lb_ota_version(void);   /* app version string */
bool lb_ota_busy(void);

/* Download url into the inactive slot and reboot. Blocking (seconds); call it
 * from a worker task, not from the console or the monitor task.
 * On success this function does NOT return - the device restarts.
 * err must be at least 96 bytes. */
esp_err_t lb_ota_from_url(const char *url, char *err, size_t err_len);

/* ---- streaming writer, for the web-upload path ------------------------- */
esp_err_t lb_ota_stream_begin(size_t image_size /* 0 = unknown */);
esp_err_t lb_ota_stream_write(const uint8_t *data, size_t len);
esp_err_t lb_ota_stream_end_and_reboot(void);
void lb_ota_stream_abort(void);

#ifdef __cplusplus
}
#endif
