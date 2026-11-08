// wifi_prov.c - see wifi_prov.h
#include "wifi_prov.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "app_matter.h"
#include "evlog.h"

static const char *TAG = "prov";

#define PROV_AP_PREFIX "Lightbar-"
#define PROV_JOIN_TIMEOUT_MS 20000
#define PROV_STA_GRACE_MS 45000
#define PROV_SCAN_MAX 16

static lb_prov_state_t s_state = LB_PROV_IDLE;
static bool s_ap_active;
static char s_ap_ssid[33];
static char s_ap_pass[65];

static wifi_ap_record_t s_scan[PROV_SCAN_MAX];
static int s_scan_count;
static bool s_scan_done;
static volatile uint32_t s_join_gen;

static char s_ip[16];

bool wifi_prov_scanning(void)
{
    return !s_scan_done;
}

const char *wifi_prov_state_name(void)
{
    switch (s_state) {
    case LB_PROV_AP: return "ap";
    case LB_PROV_CONNECTING: return "connecting";
    case LB_PROV_CONNECTED: return "connected";
    case LB_PROV_FAILED: return "failed";
    default: return "idle";
    }
}

lb_prov_state_t wifi_prov_state(void)
{
    return s_state;
}

bool wifi_prov_ap_active(void)
{
    return s_ap_active;
}

const char *wifi_prov_ap_ssid(void)
{
    return s_ap_ssid;
}

const char *wifi_prov_ap_pass(void)
{
    return s_ap_pass;
}

int wifi_prov_scan_count(void)
{
    return s_scan_done ? s_scan_count : 0;
}

const char *wifi_prov_scan_ssid(int index)
{
    if (index < 0 || index >= wifi_prov_scan_count()) {
        return "";
    }
    return (const char *)s_scan[index].ssid;
}

int wifi_prov_scan_rssi(int index)
{
    if (index < 0 || index >= wifi_prov_scan_count()) {
        return -127;
    }
    return s_scan[index].rssi;
}

bool wifi_prov_scan_secure(int index)
{
    if (index < 0 || index >= wifi_prov_scan_count()) {
        return false;
    }
    return s_scan[index].authmode != WIFI_AUTH_OPEN;
}

bool wifi_prov_ip(char *out, size_t len)
{
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        return false;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return false;
    }
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    snprintf(s_ip, sizeof(s_ip), "%s", out);
    return true;
}

esp_err_t wifi_prov_start_ap(void)
{
    if (s_ap_active) {
        return ESP_OK;
    }
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap == NULL) {
        ap = esp_netif_create_default_wifi_ap();
        if (ap == NULL) {
            ESP_LOGE(TAG, "cannot create AP netif");
            return ESP_FAIL;
        }
    }

    /* The AP must coexist with the station: scanning for nearby networks needs
     * the STA interface, and without it the list came back empty and the page
     * span forever on "scanning". */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    /* SSID carries the last two MAC bytes so several units can be told apart. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), PROV_AP_PREFIX "%02X%02X", mac[4], mac[5]);
    s_ap_pass[0] = '\0'; /* open network: the user has to be able to join it */

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.ap.ssid, s_ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.pmf_cfg.required = false;

    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "AP config: %s", esp_err_to_name(e));
        return e;
    }
    s_ap_active = true;
    s_state = LB_PROV_AP;
    ESP_LOGW(TAG, "provisioning AP up: SSID \"%s\" (open)", s_ap_ssid);
    evlog("wifi", "provisioning AP up: %s", s_ap_ssid);
    return ESP_OK;
}

void wifi_prov_stop_ap(void)
{
    if (!s_ap_active) {
        return;
    }
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg)); /* empty SSID disables the AP */
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    s_ap_active = false;
    ESP_LOGW(TAG, "provisioning AP stopped");
}

static esp_err_t s_scan_err;
static SemaphoreHandle_t s_scan_sem;

esp_err_t wifi_prov_scan_error(void)
{
    return s_scan_err;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE && s_scan_sem != NULL) {
        xSemaphoreGive(s_scan_sem);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && data != NULL) {
        evlog("wifi", "disconnected reason=%u", (unsigned)((wifi_event_sta_disconnected_t *)data)->reason);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        evlog("wifi", "associated");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != NULL) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        evlog("wifi", "got ip " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static void scan_collect(void)
{
    uint16_t n = PROV_SCAN_MAX;
    if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) {
        n = 0;
    }
    int out = 0;
    for (int i = 0; i < (int)n; i++) {
        if (s_scan[i].ssid[0] == '\0') {
            continue;
        }
        if (out != i) {
            s_scan[out] = s_scan[i];
        }
        out++;
    }
    s_scan_count = out;
}

static void scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "scan task start (state=%s)", wifi_prov_state_name());

    /* Scanning is refused outright while the station is mid-connect ("STA is
     * connecting, scan are not allowed!"). Park the Matter station and drop the
     * link for the duration of the scan. Do NOT wipe the station config here:
     * esp_wifi_set_config() persists to NVS, and wifi_prov_init() decides
     * "credentials present?" from exactly that record - a scan started while the
     * router was merely rebooting used to erase a perfectly good provision. */
    char ipbuf[16];
    if (!wifi_prov_ip(ipbuf, sizeof(ipbuf))) {
        (void)lb_matter_wifi_station_pause();
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(400));
        wifi_mode_t mode;
        if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
            esp_wifi_set_mode(WIFI_MODE_APSTA);
        }
    }

    wifi_scan_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.show_hidden = false;

    if (s_scan_sem == NULL) {
        s_scan_sem = xSemaphoreCreateBinary();
    }
    if (s_scan_sem != NULL) {
        xSemaphoreTake(s_scan_sem, 0); /* drop any stale completion */
    }

    /* Non-blocking start plus our own deadline: the blocking variant hung
     * forever when the station was mid-reconnect, leaving the page spinning.
     * An in-flight connect attempt blocks scanning for a second or two, so retry
     * until the driver is idle instead of failing on the first refusal. */
    esp_err_t e = ESP_ERR_WIFI_STATE;
    int attempt = 0;
    for (; attempt < 12; attempt++) {
        e = esp_wifi_scan_start(&cfg, false);
        if (e == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(350));
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "scan start: %s (after %d attempts)", esp_err_to_name(e), attempt);
        s_scan_err = e;
        s_scan_count = 0;
        s_scan_done = true;
        vTaskDelete(NULL);
        return;
    }
    if (attempt > 0) {
        ESP_LOGW(TAG, "scan started after %d retries", attempt);
    }

    bool done = false;
    if (s_scan_sem != NULL) {
        done = (xSemaphoreTake(s_scan_sem, pdMS_TO_TICKS(7000)) == pdTRUE);
    } else {
        /* no semaphore: poll the record count instead */
        for (int i = 0; i < 35 && !done; i++) {
            vTaskDelay(pdMS_TO_TICKS(200));
            uint16_t num = 0;
            if (esp_wifi_scan_get_ap_num(&num) == ESP_OK) {
                done = true;
            }
        }
    }

    if (done) {
        s_scan_err = ESP_OK;
        scan_collect();
        ESP_LOGI(TAG, "scan found %d networks", s_scan_count);
    } else {
        s_scan_err = ESP_ERR_TIMEOUT;
        s_scan_count = 0;
        ESP_LOGE(TAG, "scan timed out");
    }
    s_scan_done = true;
    vTaskDelete(NULL);
}

int wifi_prov_scan(void)
{
    if (s_state == LB_PROV_CONNECTING) {
        /* Scanning now would either fail or cancel the attempt in progress. */
        s_scan_err = ESP_ERR_INVALID_STATE;
        s_scan_done = true;
        s_scan_count = 0;
        return 0;
    }
    s_scan_done = false;
    s_scan_count = 0;
    s_scan_err = ESP_OK;
    /* A blocking scan inside the HTTP handler would stall the server; give it a
     * small stack and let the page poll until the list is ready. */
    if (xTaskCreate(scan_task, "prov_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scan_err = ESP_ERR_NO_MEM;
        s_scan_done = true;
        return 0;
    }
    return 0; /* caller polls wifi_prov_scan_count() */
}

static void join_task(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROV_JOIN_TIMEOUT_MS);
    while (xTaskGetTickCount() < deadline) {
        if (gen != s_join_gen) { /* a newer attempt superseded this one */
            vTaskDelete(NULL);
            return;
        }
        if (nif != NULL) {
            esp_netif_ip_info_t ip;
            memset(&ip, 0, sizeof(ip));
            if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
                char buf[16];
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
                snprintf(s_ip, sizeof(s_ip), "%s", buf);
                s_state = LB_PROV_CONNECTED;
                ESP_LOGW(TAG, "joined network, ip=%s", s_ip);
                /* Keep the AP for a moment so the page can show the result, then
                 * drop it; the device is reachable on the LAN from now on. */
                vTaskDelay(pdMS_TO_TICKS(15000));
                if (gen == s_join_gen) {
                    wifi_prov_stop_ap();
                }
                vTaskDelete(NULL);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (gen == s_join_gen) {
        s_state = LB_PROV_FAILED;
        /* Park the Matter station and stop the driver retry loop; otherwise the
         * log is flooded at 10 Hz and the next scan is refused. */
        (void)lb_matter_wifi_station_pause();
        esp_wifi_disconnect();
        ESP_LOGE(TAG, "join timed out after %d ms - wrong password, 5 GHz only, or out of range",
                 PROV_JOIN_TIMEOUT_MS);
    }
    vTaskDelete(NULL);
}

esp_err_t wifi_prov_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    /* Hand the credentials to CHIP rather than to esp_wifi directly: its driver
     * clears the old provision, stores the new one and starts the connection, so
     * the Matter stack stays in sync and the log stops flooding. */
    esp_err_t e = lb_matter_wifi_connect(ssid, password != NULL ? password : "");
    if (e != ESP_OK) {
        s_state = LB_PROV_FAILED;
        return e;
    }
    uint32_t gen = ++s_join_gen;
    s_state = LB_PROV_CONNECTING;
    ESP_LOGW(TAG, "joining \"%s\" (password %s)", ssid,
             (password != NULL && password[0] != '\0') ? "supplied" : "EMPTY");
    if (xTaskCreate(join_task, "prov_join", 3072, (void *)(uintptr_t)gen, 4, NULL) != pdPASS) {
        s_state = LB_PROV_FAILED;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* If credentials are stored but the station never gets an IP, the device would be
 * unreachable: no AP (because credentials exist) and no LAN address. Raise the AP
 * after a grace period so the page can always be reached. */
static void supervisor_task(void *arg)
{
    (void)arg;
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(PROV_STA_GRACE_MS);
    while (xTaskGetTickCount() < deadline) {
        esp_netif_ip_info_t ip;
        memset(&ip, 0, sizeof(ip));
        if (nif != NULL && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (s_ap_active) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "no IP after %d s - raising the provisioning AP as a fallback",
             PROV_STA_GRACE_MS / 1000);
    wifi_prov_start_ap();
    vTaskDelete(NULL);
}

esp_err_t wifi_prov_forget(void)
{
    s_join_gen++; /* stop any pending join report from overwriting the new state */
    (void)lb_matter_wifi_forget();
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_disconnect();
    ESP_LOGW(TAG, "stored credentials erased - back to provisioning mode");
    s_state = LB_PROV_AP;
    return wifi_prov_start_ap();
}

esp_err_t wifi_prov_init(void)
{
    if (s_scan_sem == NULL) {
        s_scan_sem = xSemaphoreCreateBinary();
    }
    static bool handler_added;
    if (!handler_added) {
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
        handler_added = true;
    }

    /* The ESP32-C6 is an 802.11ax part, but the driver boots with a 40 MHz PHY and
     * therefore silently downgrades the station to 11N:
     *   "11ax/11ac mode can not work under phy bw 40M, the sta 2G phymode
     *    changed to 11N"
     * 11ax needs 20 MHz, so pin both interfaces to HT20 and re-enable the HE
     * protocol. Harmless on 11n-only networks. */
    esp_err_t bw1 = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    esp_err_t bw2 = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    esp_err_t pr = esp_wifi_set_protocol(WIFI_IF_STA, proto);
    esp_err_t pr2 = esp_wifi_set_protocol(WIFI_IF_AP, proto);
    if (bw1 != ESP_OK || bw2 != ESP_OK || pr != ESP_OK || pr2 != ESP_OK) {
        ESP_LOGW(TAG, "HT20/11ax setup: bw_sta=%s bw_ap=%s proto_sta=%s proto_ap=%s",
                 esp_err_to_name(bw1), esp_err_to_name(bw2), esp_err_to_name(pr),
                 esp_err_to_name(pr2));
    } else {
        ESP_LOGI(TAG, "PHY set to HT20 + 11ax on both interfaces");
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
        s_state = LB_PROV_IDLE;
        ESP_LOGI(TAG, "stored credentials for \"%s\" - waiting for an IP", (char *)cfg.sta.ssid);
        xTaskCreate(supervisor_task, "prov_sup", 3072, NULL, 3, NULL);
        return ESP_OK;
    }
    return wifi_prov_start_ap();
}
