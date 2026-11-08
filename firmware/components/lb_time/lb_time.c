// lb_time.c - see lb_time.h
#include "lb_time.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#ifndef CONFIG_LB_TZ
#define CONFIG_LB_TZ "UTC"
#endif

static const char *TAG = "lb_time";
static volatile bool s_sntp_started = false;
static volatile lb_time_src_t s_src = LB_TIME_SRC_UNSET;

static void sntp_sync_cb(struct timeval *tv)
{
    s_src = LB_TIME_SRC_SNTP;
    ESP_LOGI(TAG, "SNTP synced: epoch=%lld", (long long)(tv ? tv->tv_sec : 0));
}

esp_err_t lb_time_init(void)
{
    int rc = setenv("TZ", CONFIG_LB_TZ, 1);
    tzset();
    if (rc != 0) {
        ESP_LOGW(TAG, "setenv(TZ) failed for \"%s\"", CONFIG_LB_TZ);
    }
    ESP_LOGI(TAG, "TZ=\"%s\"", CONFIG_LB_TZ);

    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec > LB_TIME_EPOCH_MIN) {
        s_src = LB_TIME_SRC_BUILT_IN;
    }
    return ESP_OK;
}

esp_err_t lb_time_start_sntp_if_online(void)
{
    if (s_sntp_started) {
        return ESP_OK;
    }
    /* The STATION interface specifically. While the provisioning SoftAP is up it
     * owns 192.168.4.1 and may be the "default" netif, which made this start
     * SNTP with no route to the internet and then never retry (the lwIP SNTP
     * poll interval is an hour). */
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (nif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0 || ip.gw.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One server only: CONFIG_LWIP_SNTP_MAX_SERVERS defaults to 1 and the
     * config array is sized by it, so a longer list would overflow it. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_sync_cb;
    cfg.wait_for_sync = false;
    esp_err_t e = esp_netif_sntp_init(&cfg);
    if (e == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started (netif has IPv4)");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(e));
    }
    return e;
}

void lb_time_set_epoch(int64_t sec)
{
    struct timeval tv = { .tv_sec = (time_t)sec, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday failed");
        return;
    }
    s_src = LB_TIME_SRC_MANUAL;
}

int64_t lb_time_now_epoch(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

bool lb_time_valid(void)
{
    return lb_time_now_epoch() > LB_TIME_EPOCH_MIN;
}

lb_time_src_t lb_time_source(void)
{
    return s_src;
}

const char *lb_time_source_name(void)
{
    switch (s_src) {
    case LB_TIME_SRC_MANUAL:   return "manual";
    case LB_TIME_SRC_SNTP:     return "sntp";
    case LB_TIME_SRC_BUILT_IN: return "boot";
    default:                   return "unset";
    }
}

bool lb_time_local_string(char *out, size_t len)
{
    if (out == NULL || len == 0) {
        return false;
    }
    if (!lb_time_valid()) {
        snprintf(out, len, "(no clock)");
        return false;
    }
    time_t t = (time_t)lb_time_now_epoch();
    struct tm l;
    if (localtime_r(&t, &l) == NULL) {
        snprintf(out, len, "(clock error)");
        return false;
    }
    snprintf(out, len, "%04d-%02d-%02d %02d:%02d:%02d %s", l.tm_year + 1900, l.tm_mon + 1, l.tm_mday,
             l.tm_hour, l.tm_min, l.tm_sec, l.tm_isdst > 0 ? "DST" : "STD");
    return true;
}

bool lb_time_solar_utc(int64_t epoch_utc, float lon_deg, uint16_t *day_of_year, float *solar_hour)
{
    time_t t = (time_t)epoch_utc;
    struct tm g;
    if (gmtime_r(&t, &g) == NULL) {
        return false;
    }
    uint16_t doy = (uint16_t)(g.tm_yday + 1);
    float hours = (float)g.tm_hour + (float)g.tm_min / 60.0f + (float)g.tm_sec / 3600.0f;

    /* Equation of time (minutes), standard Fourier approximation:
     * eot = 9.87 sin(2B) - 7.53 cos(B) - 1.5 sin(B),  B = 360/365 (N-81) */
    float b = (360.0f / 365.0f) * ((float)doy - 81.0f) * 0.017453292519943295f;
    float eot = 9.87f * sinf(2.0f * b) - 7.53f * cosf(b) - 1.5f * sinf(b);

    float sh = hours + lon_deg / 15.0f + eot / 60.0f;
    while (sh < 0.0f) {
        sh += 24.0f;
    }
    while (sh >= 24.0f) {
        sh -= 24.0f;
    }
    if (!isfinite(sh) || sh >= 24.0f) {
        return false;
    }
    if (day_of_year != NULL) {
        *day_of_year = doy;
    }
    if (solar_hour != NULL) {
        *solar_hour = sh;
    }
    return true;
}
