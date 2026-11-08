// web_ui.c - see web_ui.h
//
// Serves three files embedded at build time via EMBED_TXTFILES
// (main/web/index.html, style.css, app.js). The visual design follows the
// WaterMonitor web-OTA page: Apple-ish light surface, 20px cards, blue accent,
// dark mode. Functionality is this device's own: one raw-body upload that
// writes the spare slot, with the browser never able to switch the boot
// partition itself.
//
// Security: none beyond esp_ota's image validation - plain HTTP, no password,
// no signing, no TLS. Intended for a trusted LAN only.
#include "web_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "esp_http_server.h"

#include "lb_ota.h"
#include "safety_sm.h"
#include "wifi_prov.h"
#include "app_matter.h"
#include "lightbar_out.h"
#include "lightbar_mix.h"
#include "solar_policy.h"
#include "tps_bus.h"
#include "tps92390.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "rgb_status.h"
#include "evlog.h"
#ifdef CONFIG_LB_REV_NEW
#define LB_REPROBE_SUPPORTED 1
#define LB_REV_NAME "PCB4"
#else
#define LB_REPROBE_SUPPORTED 0
#define LB_REV_NAME "PCB3_3"
#endif

/* LED_SET string configuration code -> number of used outputs (Table 7-27) */
static unsigned hw_strings(uint8_t cfg)
{
    switch (cfg) { case 0: return 4; case 1: return 3; case 2: return 2; case 3: return 2; case 4: return 1; default: return 0; }
}

/* last LED_STATUS the monitor saw for chip i */
static uint16_t hwled(int i)
{
    lb_diag_t d;
    safety_sm_diag(&d);
    return d.led[i];
}

static const char *TAG = "web";

/* EMBED_TXTFILES names the symbols after the file's basename (the generated .S
 * lands in the build root, not in the web/ subdir). */
extern const char _binary_index_html_start[] asm("_binary_index_html_start");
extern const char _binary_index_html_end[] asm("_binary_index_html_end");
extern const char _binary_style_css_start[] asm("_binary_style_css_start");
extern const char _binary_style_css_end[] asm("_binary_style_css_end");
extern const char _binary_app_js_start[] asm("_binary_app_js_start");
extern const char _binary_app_js_end[] asm("_binary_app_js_end");

static httpd_handle_t s_server;

/* form helpers (defined further down, used by several handlers) */
static void url_decode(char *s);
static bool form_get(const char *body, const char *key, char *out, size_t len);

static esp_err_t send_asset(httpd_req_t *req, const char *start, const char *end, const char *type)
{
    /* EMBED_TXTFILES appends a NUL after the content, and _end points past it.
     * Sending that byte makes app.js end with 0x00, which browsers reject with a
     * syntax error - the page then renders (HTML/CSS tolerate it) but no script
     * ever runs. Trim the terminator. */
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, start, len);
}

static esp_err_t h_index(httpd_req_t *req)
{
    return send_asset(req, _binary_index_html_start, _binary_index_html_end,
                      "text/html; charset=utf-8");
}

static esp_err_t h_css(httpd_req_t *req)
{
    return send_asset(req, _binary_style_css_start, _binary_style_css_end, "text/css");
}

static esp_err_t h_js(httpd_req_t *req)
{
    return send_asset(req, _binary_app_js_start, _binary_app_js_end, "text/javascript");
}


static esp_err_t h_status(httpd_req_t *req)
{
    bool v = false;
    float t = safety_sm_board_temp(&v);
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *d = esp_app_get_description();
    lb_target_t light = { 0 };
    lightbar_out_get(&light);
    char ip[16] = {0};
    (void)wifi_prov_ip(ip, sizeof(ip));
    int ack = (tps_bus_chip(0)->ack ? 1 : 0) + (tps_bus_chip(1)->ack ? 1 : 0);

    /* date + time, so two builds flashed on the same day are still tellable apart */
    char build[40];
    snprintf(build, sizeof(build), "%s %s", d ? d->date : "?", d ? d->time : "?");

    /* strap-resistor facts the chips report about themselves (1Eh), read at boot */
    static const unsigned BOOST_KHZ[8] = { 100, 200, 303, 400, 500, 1818, 2000, 2222 };
    static const unsigned PWM_HZ[8] = { 152, 305, 610, 1221, 2441, 4883, 9766, 19531 };
    tps_autodetect_t hw[2];
    int hw_inv[2];
    for (int i = 0; i < 2; i++) {
        tps92390_decode_autodetect(safety_sm_autodetect(i), &hw[i]);
        hw_inv[i] = (tps_bus_chip(i)->ack && (hwled(i) & TPS_LS_INVSTRING)) ? 1 : 0;
    }

    lb_diag_drive_t dd;
    lightbar_out_get_diag(&dd);

    uint8_t sk_w = 0, sk_c = 0;
    uint8_t sf_w = safety_sm_string_faults(0, &sk_w);
    uint8_t sf_c = safety_sm_string_faults(1, &sk_c);

    char buf[1200];
    int n = snprintf(buf, sizeof(buf),
                     "{\"version\":\"%s\",\"build\":\"%s\",\"state\":\"%s\","
                     "\"temp\":%.2f,\"temp_valid\":%d,\"heap\":%u,\"uptime\":%.0f,"
                     "\"ota_busy\":%d,\"pending\":%d,\"overtemp\":%d,\"gate\":%d,"
                     "\"slot\":\"%s\",\"slot_addr\":\"0x%06" PRIx32 "\","
                     "\"next\":\"%s\",\"next_addr\":\"0x%06" PRIx32 "\","
                     "\"light\":{\"on\":%d,\"level\":%u,\"mireds\":%u},"
                     "\"auto\":{\"on\":%d,\"mireds\":%u,\"elev\":%.1f},"
                     "\"fsm\":[%u,%u],\"driver\":\"%s\",\"level_cap\":%u,"
                     "\"current_code\":%u,\"chips_ack\":%d,\"ip\":\"%s\","
                     "\"ap_active\":%d,\"commissioned\":%d,\"led_brightness\":%u,"
                     "\"strings\":{\"n\":%d,\"warm\":{\"fault\":%u,\"kind\":%u},\"cold\":{\"fault\":%u,\"kind\":%u}},"
                     "\"reprobe\":%d,"
                     "\"rev\":\"%s\",\"hw\":{\"warm\":{\"mode\":%u,\"strings\":%u,\"boost_khz\":%u,\"pwm_hz\":%u,\"invstring\":%d},"
                     "\"cold\":{\"mode\":%u,\"strings\":%u,\"boost_khz\":%u,\"pwm_hz\":%u,\"invstring\":%d}},"
                     "\"diag\":{\"on\":%d,\"warm\":%u,\"cold\":%u},"
                     "\"chips\":[{\"fsm\":%u,\"stuck\":%d,\"want\":%d},{\"fsm\":%u,\"stuck\":%d,\"want\":%d}]}",
                     d ? d->version : "?", build, safety_sm_state_name(), t, (int)v,
                     (unsigned)esp_get_free_heap_size(), (double)(esp_timer_get_time() / 1000000),
                     (int)lb_ota_busy(), (int)lb_ota_pending_verify(), (int)safety_sm_overtemp(),
                     (int)safety_sm_gate_open(), run ? run->label : "?", run ? run->address : 0,
                     next ? next->label : "?", next ? next->address : 0,
                     (int)light.on, (unsigned)light.level, (unsigned)light.mireds,
                     (int)solar_policy_auto(), (unsigned)solar_policy_last_mireds(),
                     (double)solar_policy_last_elevation(),
                     (unsigned)(safety_sm_fsm(0) & 0x1F), (unsigned)(safety_sm_fsm(1) & 0x1F),
                     (((safety_sm_fsm(0) & 0x1F) == 0x13) || ((safety_sm_fsm(1) & 0x1F) == 0x13)) ? "latched"
                         : ((safety_sm_chip_stuck(0) || safety_sm_chip_stuck(1)) ? "stuck"
                         : (((safety_sm_fsm(0) & 0x1F) == 0x12 || (safety_sm_fsm(1) & 0x1F) == 0x12) ? "recovering" : "ok")),
                     (unsigned)lightbar_out_level_cap(),
                     (unsigned)(lightbar_out_current_code() << 4), ack, ip,
                     (int)wifi_prov_ap_active(), (int)lb_matter_commissioned(),
                     (unsigned)rgb_status_get_brightness(),
                     CONFIG_LB_STRINGS_PER_CHIP, (unsigned)sf_w, (unsigned)sk_w, (unsigned)sf_c, (unsigned)sk_c,
                     (int)LB_REPROBE_SUPPORTED, LB_REV_NAME,
                     hw[0].mode_sel, hw_strings(hw[0].string_cfg), BOOST_KHZ[hw[0].boost_freq_sel], PWM_HZ[hw[0].pwm_freq_sel], hw_inv[0],
                     hw[1].mode_sel, hw_strings(hw[1].string_cfg), BOOST_KHZ[hw[1].boost_freq_sel], PWM_HZ[hw[1].pwm_freq_sel], hw_inv[1],
                     (int)dd.on, (unsigned)dd.warm_pct, (unsigned)dd.cold_pct,
                     (unsigned)(safety_sm_fsm(0) & 0x1F), (int)safety_sm_chip_stuck(0), (int)lightbar_out_channel_requested(0),
                     (unsigned)(safety_sm_fsm(1) & 0x1F), (int)safety_sm_chip_stuck(1), (int)lightbar_out_channel_requested(1));
    if (n < 0) {
        n = 0;
    } else if (n >= (int)sizeof(buf)) {
        /* snprintf returns the would-be length; never send past the buffer */
        ESP_LOGW(TAG, "status JSON truncated (%d > %u)", n, (unsigned)sizeof(buf) - 1);
        n = (int)sizeof(buf) - 1;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

/* Everything the "advanced" tab shows, and everything an engineer needs to see
 * over a tunnel. Read-only, so the same GET works for a human and for a script. */
static esp_err_t h_diag(httpd_req_t *req)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *d = esp_app_get_description();
    lb_target_t light = { 0 };
    lightbar_out_get(&light);
    lb_diag_t dg;
    safety_sm_diag(&dg);
    bool v = false;
    float t = safety_sm_board_temp(&v);
    char ip[16] = {0};
    (void)wifi_prov_ip(ip, sizeof(ip));
    int rssi = 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }
    char build[40];
    snprintf(build, sizeof(build), "%s %s", d ? d->date : "?", d ? d->time : "?");

    /* read the diagnostic registers under the bus lock */
    static const uint8_t regs[] = { 0x00, 0x02, 0x0E, 0x10, 0x12, 0x14, 0x16, 0x18, 0x1A, 0x1C, 0x1E };
    uint16_t val[2][sizeof(regs) / sizeof(regs[0])];
    tps_bus_lock();
    for (int c = 0; c < 2; c++) {
        for (unsigned k = 0; k < sizeof(regs) / sizeof(regs[0]); k++) {
            val[c][k] = 0xFFFF;
            (void)tps92390_read(tps_bus_chip(c)->d100, regs[k], &val[c][k]);
        }
    }
    tps_bus_unlock();

    size_t cap = 3000;
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no memory");
    }
    int w = snprintf(buf, cap,
                     "{\"version\":\"%s\",\"build\":\"%s\",\"running\":\"%s\",\"running_addr\":\"0x%06" PRIx32
                     "\",\"next\":\"%s\",\"next_addr\":\"0x%06" PRIx32 "\","
                     "\"uptime\":%.0f,\"heap\":%u,\"heap_min\":%u,\"heap_block\":%u,"
                     "\"temp\":%.2f,\"temp_valid\":%d,\"state\":\"%s\",\"overtemp\":%d,"
                     "\"crit_events\":%u,\"boots\":%u,\"boot_crcerr\":%d,"
                     "\"light\":{\"on\":%d,\"level\":%u,\"mireds\":%u},\"current_code\":%u,"
                     "\"ip\":\"%s\",\"rssi\":%d,\"commissioned\":%d,\"regs\":{",
                     d ? d->version : "?", build, run ? run->label : "?",                     run ? run->address : 0, next ? next->label : "?", next ? next->address : 0,
                     (double)(esp_timer_get_time() / 1000000), (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), t, (int)v,
                     safety_sm_state_name(), (int)safety_sm_overtemp(), (unsigned)dg.crit_events,
                     (unsigned)dg.boots, (int)dg.boot_crcerr, (int)light.on, (unsigned)light.level,
                     (unsigned)light.mireds, (unsigned)(lightbar_out_current_code() << 4), ip, rssi,
                     (int)lb_matter_commissioned());

    /* Bounded appends: snprintf returns the would-be length, so a truncated
     * write must clamp w to the buffer end instead of running cap - w negative. */
#define JAPPEND(...)                                                          \
    do {                                                                      \
        if (w < (int)cap - 1) {                                               \
            int _r = snprintf(buf + w, cap - (size_t)w, __VA_ARGS__);         \
            if (_r < 0) {                                                     \
                _r = 0;                                                       \
            }                                                                 \
            w += _r;                                                          \
            if (w > (int)cap - 1) {                                           \
                w = (int)cap - 1;                                             \
            }                                                                 \
        }                                                                     \
    } while (0)
    if (w < 0) w = 0;
    if (w > (int)cap - 1) w = (int)cap - 1;
    for (int c = 0; c < 2 && w < (int)cap - 700; c++) {
        JAPPEND("%s\"%s\":{", c ? "," : "", c ? "cold" : "warm");
        for (unsigned k = 0; k < sizeof(regs) / sizeof(regs[0]); k++) {
            JAPPEND("%s\"%02X\":\"0x%04X\"", k ? "," : "", regs[k], val[c][k]);
        }
        JAPPEND("}");
    }
    lb_tuning_t tn;
    lightbar_out_get_tuning(&tn);
    JAPPEND("},\"tune\":{\"slope\":%d,\"dither\":%d,\"ss_range\":%d,\"ss_mod\":%d,\"ss_pseudo\":%d,\"uc1\":\"0x%04X\"}",
            tn.slope, tn.dither, tn.ss_range, tn.ss_mod, tn.ss_pseudo, (unsigned)lightbar_out_expected_uc1());
    JAPPEND(",\"hist\":[");
    for (int k = 0; k < dg.hist_n && w < (int)cap - 220; k++) {
        JAPPEND("%s{\"reason\":\"%s\",\"flags\":%u,\"warm\":\"0x%04X/0x%04X/0x%04X\","
                "\"cold\":\"0x%04X/0x%04X/0x%04X\"}",
                k ? "," : "", dg.hist[k].reason, (unsigned)dg.hist[k].flags,
                dg.hist[k].sup[0], dg.hist[k].bst[0], dg.hist[k].led[0],
                dg.hist[k].sup[1], dg.hist[k].bst[1], dg.hist[k].led[1]);
    }
    if (w > (int)cap - 4) {
        w = (int)cap - 4;
    }
    JAPPEND("]}");
#undef JAPPEND
    buf[cap - 1] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

static esp_err_t h_light(httpd_req_t *req)
{
    char body[128] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad body");
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "short body");
        }
        got += n;
    }
    body[got] = '\0';

    lb_target_t cur = { 0 };
    lightbar_out_get(&cur);

    char v[12] = {0};
    /* "on" absent = leave the switch alone (a slider move must not turn the lamp
     * off just because the page did not send on=1). */
    int on = cur.on ? 1 : 0;
    if (form_get(body, "on", v, sizeof(v))) {
        on = (atoi(v) != 0) ? 1 : 0;
    }
    int level = cur.level;
    if (form_get(body, "level", v, sizeof(v))) {
        level = atoi(v);
        if (level < 1) level = 1;
        if (level > 254) level = 254;
    }
    /* A missing mireds field means "leave the colour alone". That is what the
     * page sends while sun-following is active, so dimming never cancels it. */
    int mireds = cur.mireds;
    bool ct_given = form_get(body, "mireds", v, sizeof(v));
    if (ct_given) {
        mireds = atoi(v);
        if (mireds < LB_CT_COLD_MIREDS) mireds = LB_CT_COLD_MIREDS;
        if (mireds > LB_CT_WARM_MIREDS) mireds = LB_CT_WARM_MIREDS;
        /* manual colour wins: leave sun-following */
        solar_policy_note_manual_ct();
    }
    evlog("web", "light on=%d lvl=%d mi=%d%s", on, level, mireds, ct_given ? "" : " (ct kept)");
    lightbar_out_set(on != 0, (uint8_t)level, (uint16_t)mireds);
    {   /* an explicit light command ends diagnostic drive, same rule as for Matter */
        lb_diag_drive_t dd;
        lightbar_out_get_diag(&dd);
        if (dd.on) {
            evlog("web", "light command while in diagnostic mode -> back to Normal");
            lightbar_out_set_diag(NULL);
        }
    }
    /* Reflect the change in the Matter data model so Home Assistant shows it. */
    (void)lb_matter_sync_target();

    bool open = safety_sm_gate_open();
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
                     "{\"applied\":%d,\"reason\":\"%s\",\"on\":%d,\"level\":%d,\"mireds\":%d,"
                     "\"auto\":%d}",
                     (int)open, open ? "" : (safety_sm_overtemp() ? "overtemperature" : "safety gate closed"),
                     on, level, mireds, (int)solar_policy_auto());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Sun-following toggle. Turning it on recomputes immediately and pushes the
 * colour through lightbar_out, so the lamp changes at once rather than waiting
 * for the next one-minute policy tick. */
static esp_err_t h_auto(httpd_req_t *req)
{
    char body[64] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) {
        int got = 0;
        while (got < total) {
            int n = httpd_req_recv(req, body + got, total - got);
            if (n <= 0) {
                break;
            }
            got += n;
        }
        body[got] = '\0';
    }
    char v[12] = {0};
    bool wanted = form_get(body, "on", v, sizeof(v)) ? (atoi(v) != 0) : !solar_policy_auto();
    solar_policy_set_auto(wanted);

    lb_target_t t = { 0 };
    lightbar_out_get(&t);
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "{\"auto\":%d,\"mireds\":%u,\"elev\":%.1f,\"target_mireds\":%u}",
                     (int)solar_policy_auto(), (unsigned)solar_policy_last_mireds(),
                     (double)solar_policy_last_elevation(), (unsigned)t.mireds);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_update(httpd_req_t *req)
{
    size_t total = req->content_len;
    if (total == 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "empty body: POST the raw app image bytes");
    }

    evlog("ota", "web upload start %u bytes", (unsigned)total);
    esp_err_t e = lb_ota_stream_begin(total);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "refused: overtemperature, busy, or no spare slot");
    }

    char *buf = malloc(2048);
    if (buf == NULL) {
        lb_ota_stream_abort();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory");
    }

    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf, 2048);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* A stalled browser must not keep the OTA handle (and the lamp
             * inhibit) forever: give up after ~10 receive timeouts. */
            if (++timeouts >= 10) {
                free(buf);
                lb_ota_stream_abort();
                ESP_LOGE(TAG, "upload stalled at %u/%u bytes - aborted", (unsigned)got, (unsigned)total);
                httpd_resp_set_status(req, "408 Request Timeout");
                return httpd_resp_sendstr(req, "upload stalled");
            }
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            free(buf);
            lb_ota_stream_abort();
            ESP_LOGE(TAG, "recv failed at %u/%u", (unsigned)got, (unsigned)total);
            return ESP_FAIL; /* socket gone; abort already released the slot */
        }
        e = lb_ota_stream_write((const uint8_t *)buf, (size_t)n);
        if (e != ESP_OK) {
            free(buf);
            lb_ota_stream_abort();
            ESP_LOGE(TAG, "write failed at %u: %s", (unsigned)got, esp_err_to_name(e));
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    free(buf);
    ESP_LOGW(TAG, "upload complete: %u bytes, switching slot", (unsigned)got);

    /* Respond first so the browser sees success, then reboot (does not return). */
    httpd_resp_sendstr(req, "OK");
    lb_ota_stream_end_and_reboot();
    return ESP_OK;
}

/* ---------------- provisioning endpoints ---------------- */

/* Minimal percent-decoder for form bodies. */
static void url_decode(char *s)
{
    char *o = s;
    for (char *i = s; *i; i++) {
        if (*i == '+') {
            *o++ = ' ';
        } else if (*i == '%' && i[1] && i[2]) {
            char hex[3] = { i[1], i[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            *o++ = *i;
        }
    }
    *o = '\0';
}

/* Non-destructive form parser. The previous version split '&'/'=' in place and
 * left the buffer truncated after the first hit, so only the FIRST parameter of
 * any request was ever read: "on=1&level=200&mireds=200" applied only "on", and
 * "ssid=X&pass=Y" sent an empty password. Both symptoms came from this. */
static bool form_get(const char *body, const char *key, char *out, size_t len)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        if (eq != NULL) {
            size_t k = (size_t)(eq - p);
            if (k == klen && strncmp(p, key, klen) == 0) {
                size_t vlen = seg - k - 1;
                if (vlen >= len) {
                    vlen = len - 1;
                }
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
                url_decode(out);
                return true;
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    return false;
}

static esp_err_t h_scan(httpd_req_t *req)
{
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(query, "start", v, sizeof(v)) == ESP_OK) {
            wifi_prov_scan();
        }
    }
    int n = wifi_prov_scan_count();
    bool scanning = wifi_prov_scanning();
    /* The list is small but SSIDs can contain quotes/backslashes; escape them. */
    size_t cap = 512 + (size_t)n * 96;
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no memory");
    }
    int w = snprintf(buf, cap, "{\"scanning\":%d,\"error\":\"%s\",\"nets\":[", (int)scanning,
                     esp_err_to_name(wifi_prov_scan_error()));
    for (int i = 0; i < n && w < (int)cap - 96; i++) {
        const char *ssid = wifi_prov_scan_ssid(i);
        if (w > 0 && buf[w - 1] == '}') {
            buf[w++] = ',';
        }
        w += snprintf(buf + w, cap - w, "{\"ssid\":\"");
        for (const char *c = ssid; *c && w < (int)cap - 24; c++) {
            if (*c == '"' || *c == '\\') {
                buf[w++] = '\\';
            }
            if ((unsigned char)*c < 0x20) {
                continue;
            }
            buf[w++] = *c;
        }
        w += snprintf(buf + w, cap - w, "\",\"rssi\":%d,\"secure\":%d}",
                      wifi_prov_scan_rssi(i), (int)wifi_prov_scan_secure(i));
    }
    snprintf(buf + w, cap - w, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

static esp_err_t h_prov_get(httpd_req_t *req)
{
    char ip[16] = {0};
    bool has_ip = wifi_prov_ip(ip, sizeof(ip));
    char manual[16] = {0}, qr[128] = {0};
    bool have_code = (lb_matter_setup_code(manual, sizeof(manual), qr, sizeof(qr)) == ESP_OK);
    char buf[420];
    int n = snprintf(buf, sizeof(buf),
                     "{\"state\":\"%s\",\"ap_active\":%d,\"ap_ssid\":\"%s\",\"ip\":\"%s\","
                     "\"has_ip\":%d,\"manual_code\":\"%s\",\"qr\":\"%s\",\"commissioned\":%d}",
                     wifi_prov_state_name(), (int)wifi_prov_ap_active(), wifi_prov_ap_ssid(), ip,
                     (int)has_ip, have_code ? manual : "", have_code ? qr : "",
                     (int)lb_matter_commissioned());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

static esp_err_t h_prov_post(httpd_req_t *req)
{
    char body[320] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad body");
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, body + got, total - got);
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "short body");
        }
        got += n;
    }
    body[got] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid");
    }
    (void)form_get(body, "pass", pass, sizeof(pass));

    esp_err_t e = wifi_prov_connect(ssid, pass);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "cannot start join");
    }
    ESP_LOGW(TAG, "provisioning request for \"%s\"", ssid);
    return httpd_resp_sendstr(req, "OK");
}

/* Registering more handlers than max_uri_handlers fails *silently* (the request
 * just 404s later), which is exactly how /api/prov, /api/light and /api/diag
 * disappeared once. Always check the return value. */
static void reg(httpd_handle_t srv, httpd_uri_t *u)
{
    esp_err_t e = httpd_register_uri_handler(srv, u);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", u->uri, esp_err_to_name(e));
    }
}

/* Reset or query the brightness ceiling. Needed because the cap is persisted:
 * after the supply is improved the user must be able to ask for full output
 * again without a serial console. */
static esp_err_t h_cap(httpd_req_t *req)
{
    char body[64] = {0};
    int total = req->content_len;
    if (total > 0 && total < (int)sizeof(body)) {
        int got = 0;
        while (got < total) {
            int n = httpd_req_recv(req, body + got, total - got);
            if (n <= 0) {
                break;
            }
            got += n;
        }
        body[got] = '\0';
    }
    char v[12] = {0};
    if (form_get(body, "level", v, sizeof(v))) {
        int lvl = atoi(v);
        if (lvl < 8) lvl = 8;
        if (lvl > 254) lvl = 254;
        lightbar_out_set_level_cap((uint8_t)lvl);
        safety_sm_save_settings(); /* the cap survives a reboot only if written now */
        ESP_LOGW(TAG, "brightness cap set to %d by web request (persisted)", lvl);
        /* apply immediately so the change is visible (set() applies internally) */
        lb_target_t t;
        lightbar_out_get(&t);
        lightbar_out_set(t.on, t.level, t.mireds);
    }
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"level_cap\":%u}", (unsigned)lightbar_out_level_cap());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Read a small urlencoded body into buf (NUL terminated). Returns false on a
 * missing/oversized body. */
static bool read_small_body(httpd_req_t *req, char *buf, size_t len)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)len) {
        buf[0] = '\0';
        return false;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) {
            buf[got] = '\0';
            return false;
        }
        got += n;
    }
    buf[got] = '\0';
    return true;
}

/* Acoustic tuning: slope / dither / spread-spectrum shaping of USER_CONFIG1.
 * POST slope=&dither=&ss_range=&ss_mod=&ss_pseudo= (any subset). Persisted. */
static esp_err_t h_tune(httpd_req_t *req)
{
    char body[128];
    (void)read_small_body(req, body, sizeof(body));
    lb_tuning_t t = { .slope = -1, .dither = -1, .ss_range = -1, .ss_mod = -1, .ss_pseudo = -1 };
    char v[8];
    if (form_get(body, "slope", v, sizeof(v))) t.slope = (int8_t)atoi(v);
    if (form_get(body, "dither", v, sizeof(v))) t.dither = (int8_t)atoi(v);
    if (form_get(body, "ss_range", v, sizeof(v))) t.ss_range = (int8_t)atoi(v);
    if (form_get(body, "ss_mod", v, sizeof(v))) t.ss_mod = (int8_t)atoi(v);
    if (form_get(body, "ss_pseudo", v, sizeof(v))) t.ss_pseudo = (int8_t)atoi(v);
    esp_err_t e = ESP_OK;
    if (t.slope >= 0 || t.dither >= 0 || t.ss_range >= 0 || t.ss_mod >= 0 || t.ss_pseudo >= 0) {
        e = lightbar_out_set_tuning(&t);
        safety_sm_save_settings();
        ESP_LOGW(TAG, "tuning changed by web request (%s)", esp_err_to_name(e));
    }
    lightbar_out_get_tuning(&t);
    char buf[160];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":%d,\"slope\":%d,\"dither\":%d,\"ss_range\":%d,\"ss_mod\":%d,\"ss_pseudo\":%d,"
                     "\"uc1\":\"0x%04X\"}",
                     (int)(e == ESP_OK), t.slope, t.dither, t.ss_range, t.ss_mod, t.ss_pseudo,
                     (unsigned)lightbar_out_expected_uc1());
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* Factory reset. Requires confirm=yes in the body. Answers first, then queues
 * the reset so the browser gets its response before the chip restarts. */
static void factory_reset_timer_cb(void *arg)
{
    (void)arg;
    (void)safety_sm_factory_reset();
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    char body[64];
    (void)read_small_body(req, body, sizeof(body));
    char v[8] = {0};
    if (!form_get(body, "confirm", v, sizeof(v)) || strcmp(v, "yes") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "send confirm=yes");
    }
    ESP_LOGW(TAG, "FACTORY RESET requested from the web page");
    evlog("web", "FACTORY RESET requested");
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, "{\"ok\":1,\"msg\":\"resetting\"}");
    static esp_timer_handle_t s_fr_timer;
    if (s_fr_timer == NULL) {
        const esp_timer_create_args_t a = { .callback = factory_reset_timer_cb, .name = "factory" };
        esp_timer_create(&a, &s_fr_timer);
    }
    if (s_fr_timer != NULL) {
        esp_timer_start_once(s_fr_timer, 800 * 1000);
    }
    return r;
}

/* Status LED brightness 0..255, persisted. POST brightness=N; GET-less: the
 * current value is in /api/status as led_brightness. */
static esp_err_t h_led(httpd_req_t *req)
{
    char body[48];
    (void)read_small_body(req, body, sizeof(body));
    char v[8];
    if (form_get(body, "brightness", v, sizeof(v))) {
        int b = atoi(v);
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        rgb_status_set_brightness((uint8_t)b);
        safety_sm_save_settings();
        ESP_LOGI(TAG, "status LED brightness -> %d", b);
    }
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "{\"brightness\":%u}", (unsigned)rgb_status_get_brightness());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* LED current ceiling (per string). POST level=0x10..0xF0 (8-bit, code = level<<4),
 * clamped to the string rating LB_LED_CURRENT_MAX. Persisted. */
static esp_err_t h_cur(httpd_req_t *req)
{
    char body[48];
    (void)read_small_body(req, body, sizeof(body));
    char v[12];
    if (form_get(body, "level", v, sizeof(v))) {
        long lv = strtol(v, NULL, 0);
        if (lv < 0x10) lv = 0x10;
        if (lv > 0xFF) lv = 0xFF;
        uint8_t got = lightbar_out_set_current((uint8_t)lv);
        safety_sm_save_settings();
        evlog("web", "current ceiling -> 0x%03X (%.1f mA)", (unsigned)TPS_CURR_CODE(got),
              160.08 * TPS_CURR_CODE(got) / 4095.0);
    }
    uint8_t cur = lightbar_out_current_code();
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "{\"level\":%u,\"code\":%u,\"ma\":%.1f,\"max_level\":%u,\"max_ma\":%.1f}",
                     (unsigned)cur, (unsigned)TPS_CURR_CODE(cur), 160.08 * TPS_CURR_CODE(cur) / 4095.0,
                     (unsigned)LB_LED_CURRENT_MAX, 160.08 * TPS_CURR_CODE(LB_LED_CURRENT_MAX) / 4095.0);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* POST /api/strings/reprobe : EN power-cycle the drivers so reconnected strings
 * are re-enabled (new PCB). On PCB3_3 this can only tell the user to power cycle. */
static esp_err_t h_reprobe(httpd_req_t *req)
{
    evlog("web", "string re-probe requested");
    esp_err_t e = safety_sm_reprobe_strings();
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "{\"ok\":%d,\"reason\":\"%s\"}", (int)(e == ESP_OK),
                     e == ESP_OK ? "" : (e == ESP_ERR_NOT_SUPPORTED ? "EN hardwired on this board - power cycle VBUS"
                                                                     : esp_err_to_name(e)));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* POST /api/diagdrive on=0|1 warm=0-100 cold=0-100 : independent per-channel
 * drive for thermal/efficiency tests. Any field may be omitted. */
static esp_err_t h_diagdrive(httpd_req_t *req)
{
    char body[64];
    (void)read_small_body(req, body, sizeof(body));
    lb_diag_drive_t d;
    lightbar_out_get_diag(&d);
    char v[8];
    if (form_get(body, "on", v, sizeof(v))) d.on = atoi(v) != 0;
    if (form_get(body, "warm", v, sizeof(v))) { int x = atoi(v); d.warm_pct = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x)); }
    if (form_get(body, "cold", v, sizeof(v))) { int x = atoi(v); d.cold_pct = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x)); }
    evlog("web", "diagdrive on=%d warm=%u%% cold=%u%%", d.on, d.warm_pct, d.cold_pct);
    lightbar_out_set_diag(&d);
    lightbar_out_get_diag(&d);
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"on\":%d,\"warm\":%u,\"cold\":%u,\"gate\":%d}", d.on, d.warm_pct, d.cold_pct,
                     (int)safety_sm_gate_open());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

/* GET /api/log : the in-RAM event ring, oldest first. */
static esp_err_t h_log(httpd_req_t *req)
{
    static evlog_entry_t ev[EVLOG_N];
    int n = evlog_dump(ev, EVLOG_N);
    size_t cap = 256 + (size_t)n * (EVLOG_MSG + 48);
    char *buf = malloc(cap);
    if (buf == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "no memory");
    }
    int w = snprintf(buf, cap, "{\"uptime_ms\":%u,\"n\":%d,\"events\":[", (unsigned)(esp_timer_get_time() / 1000), n);
    for (int i = 0; i < n && w < (int)cap - (EVLOG_MSG + 48); i++) {
        w += snprintf(buf + w, cap - (size_t)w, "%s{\"t\":%u,\"tag\":\"%s\",\"msg\":\"", i ? "," : "",
                      (unsigned)ev[i].t_ms, ev[i].tag);
        for (const char *c = ev[i].msg; *c && w < (int)cap - 8; c++) {
            if (*c == '"' || *c == '\\') {
                buf[w++] = '\\';
            }
            if ((unsigned char)*c < 0x20) {
                continue;
            }
            buf[w++] = *c;
        }
        w += snprintf(buf + w, cap - (size_t)w, "\"}");
    }
    snprintf(buf + w, cap - (size_t)w, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

esp_err_t web_ui_start(void)
{    if (s_server != NULL) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 24; /* 20 registered below + headroom; reg() logs if exceeded */
    esp_err_t e = httpd_start(&s_server, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
        return e;
    }
    httpd_uri_t u_index = { .uri = "/", .method = HTTP_GET, .handler = h_index };
    httpd_uri_t u_css = { .uri = "/style.css", .method = HTTP_GET, .handler = h_css };
    httpd_uri_t u_js = { .uri = "/app.js", .method = HTTP_GET, .handler = h_js };
    httpd_uri_t u_stat = { .uri = "/api/status", .method = HTTP_GET, .handler = h_status };
    httpd_uri_t u_upd = { .uri = "/update", .method = HTTP_POST, .handler = h_update };
    httpd_uri_t u_scan = { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan };
    httpd_uri_t u_provg = { .uri = "/api/prov", .method = HTTP_GET, .handler = h_prov_get };
    httpd_uri_t u_provp = { .uri = "/api/prov", .method = HTTP_POST, .handler = h_prov_post };
    httpd_uri_t u_light = { .uri = "/api/light", .method = HTTP_POST, .handler = h_light };
    httpd_uri_t u_auto = { .uri = "/api/auto", .method = HTTP_POST, .handler = h_auto };
    httpd_uri_t u_diag = { .uri = "/api/diag", .method = HTTP_GET, .handler = h_diag };
    httpd_uri_t u_cap = { .uri = "/api/cap", .method = HTTP_POST, .handler = h_cap };
    httpd_uri_t u_tune = { .uri = "/api/tune", .method = HTTP_POST, .handler = h_tune };
    httpd_uri_t u_fr = { .uri = "/api/factory_reset", .method = HTTP_POST, .handler = h_factory_reset };
    httpd_uri_t u_led = { .uri = "/api/led", .method = HTTP_POST, .handler = h_led };
    httpd_uri_t u_log = { .uri = "/api/log", .method = HTTP_GET, .handler = h_log };
    httpd_uri_t u_cur = { .uri = "/api/cur", .method = HTTP_POST, .handler = h_cur };
    httpd_uri_t u_rep = { .uri = "/api/strings/reprobe", .method = HTTP_POST, .handler = h_reprobe };
    httpd_uri_t u_dd = { .uri = "/api/diagdrive", .method = HTTP_POST, .handler = h_diagdrive };
    reg(s_server, &u_index);
    reg(s_server, &u_css);
    reg(s_server, &u_js);
    reg(s_server, &u_stat);
    reg(s_server, &u_upd);
    reg(s_server, &u_scan);
    reg(s_server, &u_provg);
    reg(s_server, &u_provp);
    reg(s_server, &u_light);
    reg(s_server, &u_auto);
    reg(s_server, &u_diag);
    reg(s_server, &u_cap);
    reg(s_server, &u_tune);
    reg(s_server, &u_fr);
    reg(s_server, &u_led);
    reg(s_server, &u_log);
    reg(s_server, &u_cur);
    reg(s_server, &u_rep);
    reg(s_server, &u_dd);
    ESP_LOGI(TAG, "web UI on port %d: console at / , apis /api/{status,diag,log,light,auto,cap,cur,tune,led,scan,prov,factory_reset} (no auth: LAN only)",
             cfg.server_port);
    return ESP_OK;
}
