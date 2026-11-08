// app_matter.h - Matter node bring-up (C++ impl in app_matter.cpp).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

void lightbar_matter_start(void);

/* Onboarding codes for the provisioning page: the 11-digit manual setup code and
 * the base38 QR payload. Both need the Matter stack to be initialised. */
esp_err_t lb_matter_setup_code(char *manual, size_t manual_len, char *qr, size_t qr_len);

/* True once the node has at least one fabric. */
bool lb_matter_commissioned(void);

/* Join a Wi-Fi network through CHIP's own commissioning driver instead of poking
 * esp_wifi directly. Doing it directly leaves the Matter stack holding no
 * configured network, which floods the log with "Failed to get configured
 * network" and loses the credentials on the next reconnect. */
esp_err_t lb_matter_wifi_connect(const char *ssid, const char *password);

/* Erase the Wi-Fi provisioning stored inside the Matter stack. */
esp_err_t lb_matter_wifi_forget(void);

/* Full Matter factory reset (fabrics, credentials, Wi-Fi provision) followed by
 * a restart. Runs on the CHIP task; returns once the job is queued. */
esp_err_t lb_matter_factory_reset(void);

/* Stop the Matter stack from retrying a network that cannot be reached. Without
 * this a failed join spins every 100 ms forever, which starves the HTTP server
 * and makes every Wi-Fi scan fail with "STA is connecting". */
esp_err_t lb_matter_wifi_station_pause(void);

/* One source of truth: after anything changes lightbar_out outside Matter (web
 * console, sun curve), call this so OnOff / CurrentLevel / ColorTemperatureMireds
 * are updated in the Matter data model and Home Assistant sees the change. */
esp_err_t lb_matter_sync_target(void);

/* Called by lightbar_out when diagnostic drive changes: updates Mode Select
 * CurrentMode and publishes the physical light state (or the real target). */
void lb_matter_report_diag(void);

#ifdef __cplusplus
}
#endif
