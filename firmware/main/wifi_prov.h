// wifi_prov.h - first-run Wi-Fi provisioning without a phone app.
//
// Flow: a device with no stored credentials raises a SoftAP, the user joins it
// and opens the built-in page (http://192.168.4.1/setup), picks their network and
// types the password. The page then shows the Matter pairing code so the device
// can be added to Home Assistant straight afterwards.
//
// Matter itself still commissions over BLE (or over IP once it is on the LAN) -
// this module only makes sure the device can reach the network without needing a
// controller to hand it credentials first.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LB_PROV_IDLE = 0,   /* nothing to do: already has credentials or not started */
    LB_PROV_AP,         /* SoftAP up, waiting for the user */
    LB_PROV_CONNECTING, /* STA joining the chosen network */
    LB_PROV_CONNECTED,  /* got an IP */
    LB_PROV_FAILED,     /* join failed or timed out */
} lb_prov_state_t;

/* Called after nvs_flash_init(): if there are no stored Wi-Fi credentials the
 * provisioning AP is raised automatically. If there ARE credentials, a watchdog
 * waits for an IP and raises the AP anyway when the device cannot join - without
 * that, one bad password stored in NVS locks the user out of the web UI for good. */
esp_err_t wifi_prov_init(void);

/* Erase the stored credentials and go back to the provisioning AP. Recovery path
 * when the device was provisioned with a wrong password. */
esp_err_t wifi_prov_forget(void);

esp_err_t wifi_prov_start_ap(void);
void wifi_prov_stop_ap(void);
bool wifi_prov_ap_active(void);
const char *wifi_prov_ap_ssid(void);
const char *wifi_prov_ap_pass(void);   /* "" = open network */

/* Blocking scan (1-3 s). Returns the number of networks found. */
int wifi_prov_scan(void);
bool wifi_prov_scanning(void);
/* Result of the last scan; ESP_ERR_INVALID_STATE means "a join is in progress". */
esp_err_t wifi_prov_scan_error(void);
int wifi_prov_scan_count(void);
const char *wifi_prov_scan_ssid(int index);
int wifi_prov_scan_rssi(int index);
bool wifi_prov_scan_secure(int index);

/* Kick off a connection attempt; returns immediately, poll the state. */
esp_err_t wifi_prov_connect(const char *ssid, const char *password);

lb_prov_state_t wifi_prov_state(void);
const char *wifi_prov_state_name(void);
/* Fills out with the current station IP when connected. */
bool wifi_prov_ip(char *out, size_t len);

#ifdef __cplusplus
}
#endif
