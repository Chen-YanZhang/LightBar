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

#define PROV_AP_PREFIX "LightBar-ES-"
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

/* AP identity derives entirely from eFuse: the SSID tail is the softAP MAC
 * bytes 4/5 (that interface is base+1), the WPA2-PSK password is the FULL
 * base MAC as 12 uppercase hex chars, no colons (e.g. DC4F22880608). The
 * eFuse MAC is factory-fused, so one firmware image auto-generates a
 * per-chip password with zero production writes, and factory reset / NVS
 * wipe cannot change it. Computed even before the AP is raised, so the web
 * page can show the password any time. Deterrent, not security: beacons
 * carry the full MAC in the clear anyway. */
static void ap_identity(void)
{
    uint8_t soft[6], base[6];
    esp_read_mac(soft, ESP_MAC_WIFI_SOFTAP);
    esp_read_mac(base, ESP_MAC_BASE);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), PROV_AP_PREFIX "%02X%02X", soft[4], soft[5]);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "%02X%02X%02X%02X%02X%02X", base[0], base[1],
             base[2], base[3], base[4], base[5]);
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

    /* Claim ownership before touching the mode: switching to APSTA raises
     * WIFI_EVENT_AP_START, and the guard in the event handler reads this flag
     * to tell "ours" from "foreign" - if we set it only after the mode switch
     * the handler would dismantle our own AP. */
    s_ap_active = true;

    /* The AP must coexist with the station: scanning for nearby networks needs
     * the STA interface, and without it the list came back empty and the page
     * span forever on "scanning". */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_APSTA) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    /* SSID tail = softAP MAC bytes 4/5, WPA2 password = full base MAC (see
     * ap_identity above): one image flashed to a thousand boards yields a
     * thousand different hotspot keys with zero production-line steps. */
    ap_identity();

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.ap.ssid, s_ap_ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK; /* was OPEN: deter free-riders */
    strncpy((char *)cfg.ap.password, s_ap_pass, sizeof(cfg.ap.password) - 1);
    cfg.ap.pmf_cfg.required = false; /* keep old phones that choke on PMF required */

    esp_err_t e = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "AP config: %s", esp_err_to_name(e));
        /* Rollback: without our SSID the beacon would be the driver-default
         * "ESP_XXXXXX", so give ownership back and drop the radio to STA. */
        s_ap_active = false;
        wifi_mode_t m2;
        if (esp_wifi_get_mode(&m2) == ESP_OK && m2 != WIFI_MODE_STA) {
            esp_wifi_set_mode(WIFI_MODE_STA);
        }
        return e;
    }
    /* The AP interface exists only from here on (we are in APSTA now), so its
     * PHY tuning lives here - in STA-only mode these calls would simply fail
     * and only produce a bogus warning at every healthy boot. 11ax needs HT20
     * ("11ax/11ac mode can not work under phy bw 40M ... changed to 11N"), so
     * pin HT20 and re-enable the HE protocol. Harmless on 11n-only networks. */
    uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    esp_err_t eb = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
    esp_err_t ep = esp_wifi_set_protocol(WIFI_IF_AP, proto);
    if (eb != ESP_OK || ep != ESP_OK) {
        ESP_LOGW(TAG, "AP HT20/11ax setup: bw=%s proto=%s", esp_err_to_name(eb), esp_err_to_name(ep));
    }
    s_state = LB_PROV_AP;
    /* Serial log is the place to read the password on a factory-fresh unit
     * (the web page is not reachable before the device has an IP). */
    ESP_LOGW(TAG, "provisioning AP up: SSID \"%s\" WPA2-PSK, password \"%s\"", s_ap_ssid, s_ap_pass);
    evlog("wifi", "provisioning AP up: %s (WPA2)", s_ap_ssid);
    return ESP_OK;
}

void wifi_prov_stop_ap(void)
{
    /* Forensics of the "ESP_880609" ghost SSID (keep this reasoning here):
     *  - Mechanism (hard evidence): the literal format string
     *    "ESP_%02X%02X%02X" lives inside the prebuilt driver blob
     *    components/esp_wifi/lib/esp32c6/libnet80211.a. Whenever the AP
     *    interface is up with an EMPTY SSID config the closed driver
     *    beacons that factory default name from the last 3 softAP MAC
     *    bytes (88-06-09 on this board), open network.
     *  - Persistence (verified): with the driver default WIFI_STORAGE_FLASH the
     *    blob keeps per-interface config keys ("ap.ssid", "sta.pswd", ...)
     *    AND an "opmode" key (the operating
     *    mode) in NVS namespace "nvs.net80211" - both the empty SSID corpse AND
     *    an APSTA mode survive reboot, and the sibling project's log shows the
     *    restored "sta + softAP" mode line at ~1 s, before any app code ran.
     *    That is also why the WIFI_EVENT_AP_START guard can miss the ghost:
     *    the AP came up before wifi_prov_init() registered the handler.
     *  - Historical trigger 1: up to 0.7.21 stop_ap() "stopped" the AP by
     *    writing an empty wifi_config_t - that only RENAMED our own
     *    provisioning AP to ESP_880609 while the mode stayed APSTA and the
     *    beacon continued. The ghost was our own AP's parked corpse.
     *  - Historical trigger 2: the later mode-based stop was gated by
     *    s_ap_active, so an AP raised by foreign code sailed through: the
     *    precompiled Matter/Thread glue does put the radio into APSTA
     *    (no readable source raises it - full-tree grep says so) and
     *    scan_task raises APSTA too when the station has no IP.
     * Fix by invariant, not by chasing callers: stop_ap() always forces the
     * radio to pure STA, and the WIFI_EVENT_AP_START guard in
     * wifi_event_handler() dismantles any raise that is not ours. A clean
     * boot with saved credentials logs zero AP events (measured 0.7.23), so
     * the normal path never raises the AP anyway. */
    bool was_up = s_ap_active;
    wifi_mode_t mode = WIFI_MODE_STA;
    bool forced = false;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        forced = true;
    }
    s_ap_active = false;
    if (was_up) {
        ESP_LOGW(TAG, "provisioning AP stopped (radio back to STA)");
        evlog("wifi", "provisioning AP down");
    } else if (forced) {
        /* The AP radio was up while we believed it was down: foreign (or an
         * AP_START event that predates our handler registration). Tear it
         * down and leave the paper trail - this is the line that names the
         * ghost's carrier, even when the guard missed the event. */
        ESP_LOGW(TAG, "AP was up while we believed it down - forced back to STA");
        evlog("wifi", "foreign AP torn down (STA forced)");
    }
}

static esp_err_t s_scan_err;
static SemaphoreHandle_t s_scan_sem;

esp_err_t wifi_prov_scan_error(void)
{
    return s_scan_err;
}

/* Delayed stop for the GOT_IP path while OUR portal is still up: a phone
 * sitting on the page needs a moment to read the "joined, ip=..." result
 * before the radio drops. A foreign/ghost AP has no page to feed, so it is
 * killed immediately - no grace. */
static volatile bool s_ap_stop_pending;

static void ap_stop_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(15000));
    s_ap_stop_pending = false;
    wifi_prov_stop_ap();
    vTaskDelete(NULL);
}

static void schedule_ap_stop(void)
{
    if (s_ap_stop_pending) {
        return; /* a graceful stop is already armed */
    }
    s_ap_stop_pending = true;
    if (xTaskCreate(ap_stop_task, "ap_stop", 3072, NULL, 4, NULL) != pdPASS) {
        s_ap_stop_pending = false;
        wifi_prov_stop_ap(); /* out of memory: drop the grace and close now */
    }
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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        /* Invariant: the AP radio may only beacon while our start_ap() owns
         * it. Anyone else raising it (Matter/Thread stack, scan_task fallback)
         * beacons the closed driver's factory default "ESP_XXXXXX" - an open
         * ghost network with an empty SSID config. Kill it and log who tried. */
        if (!s_ap_active) {
            evlog("wifi", "foreign AP start - forcing STA");
            wifi_prov_stop_ap();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP && data != NULL) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        evlog("wifi", "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        /* The AP must go once the station has an IP - but in two grades:
         * ours (portal in use, a phone may be reading the join result on the
         * page) gets the 15 s grace of schedule_ap_stop(); foreign/ghost has
         * no page to feed and is torn down right here, unconditionally. */
        if (s_ap_active) {
            schedule_ap_stop();
        } else {
            wifi_prov_stop_ap();
        }
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
        /* APSTA keeps OUR AP beaconing for the page that asked for this scan.
         * When start_ap() did not run (s_ap_active false) this very call
         * raises the driver-default "ESP_XXXXXX" ghost (empty AP config) -
         * the AP_START guard tears it back to STA, and the scan itself works
         * fine in pure STA mode. See the forensics block in stop_ap(). */
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
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_event_handler, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
        handler_added = true;
    }

    /* The ESP32-C6 is an 802.11ax part, but the driver boots with a 40 MHz PHY and
     * therefore silently downgrades the station to 11N:
     *   "11ax/11ac mode can not work under phy bw 40M, the sta 2G phymode
     *    changed to 11N"
     * 11ax needs 20 MHz, so pin the STATION to HT20 and re-enable the HE
     * protocol. Harmless on 11n-only networks. (The AP interface is tuned in
     * wifi_prov_start_ap(): it does not exist in STA-only mode and calling
     * into it here only logged errors on every healthy boot.) */
    esp_err_t bw1 = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    uint8_t proto = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_11AX;
    esp_err_t pr = esp_wifi_set_protocol(WIFI_IF_STA, proto);
    if (bw1 != ESP_OK || pr != ESP_OK) {
        ESP_LOGW(TAG, "STA HT20/11ax setup: bw=%s proto=%s", esp_err_to_name(bw1),
                 esp_err_to_name(pr));
    } else {
        ESP_LOGI(TAG, "STA PHY set to HT20 + 11ax");
    }

    /* Compute SSID + password up front: they are pure functions of the
     * factory-fused MAC, so /api/prov can report the password even while the
     * AP is down and the user can read it off the page before a reset. */
    ap_identity();

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK && cfg.sta.ssid[0] != '\0') {
        s_state = LB_PROV_IDLE;
        ESP_LOGI(TAG, "stored credentials for \"%s\" - waiting for an IP", (char *)cfg.sta.ssid);
        /* Pre-empt the ghost instead of waiting for GOT_IP: the driver keeps
         * the mode (and the AP config, incl. any leftover EMPTY SSID) in its
         * own NVS namespace and may bring the interface up as "sta + softAP"
         * before our handler was even registered - an AP_START we can never
         * see. With credentials in hand the AP is foreign by construction, so
         * force pure STA right now (no-op when already STA, and it evlogs
         * "foreign AP torn down" when it was not). Adopted from the sibling
         * project's fix review: the earliest possible kill, independent of
         * events. The station connect path (Matter sets STA) is unaffected. */
        wifi_prov_stop_ap();
        xTaskCreate(supervisor_task, "prov_sup", 3072, NULL, 3, NULL);
        return ESP_OK;
    }
    return wifi_prov_start_ap();
}
