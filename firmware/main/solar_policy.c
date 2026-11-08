// solar_policy.c - see solar_policy.h
#include "solar_policy.h"
#include "lb_solar.h"
#include "lb_time.h"
#include "lightbar_out.h"
#include "app_matter.h"
#include "esp_log.h"
#include "evlog.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

static const char *TAG = "solar";
#define SOLAR_POLL_S 60

static TaskHandle_t s_task = NULL;
static volatile bool s_auto = false;
static volatile uint16_t s_last_mireds = 0;
static volatile float s_last_elev = 0.0f;

static void policy_step(bool force)
{
    if (!s_auto) {
        return;
    }
    if (!lb_time_valid()) {
        ESP_LOGD(TAG, "auto on but no clock yet (source=%s)", lb_time_source_name());
        return;
    }
    float lat = (float)CONFIG_LB_SUN_LAT / 10.0f;
    float lon = (float)CONFIG_LB_SUN_LON / 10.0f;
    uint16_t doy = 0;
    float sh = 0.0f;
    if (!lb_time_solar_utc(lb_time_now_epoch(), lon, &doy, &sh)) {
        return;
    }
    float elev = lb_solar_elevation_deg(lat, doy, sh);
    uint16_t m = lb_solar_mireds(elev);
    s_last_elev = elev;
    if (!force && m == s_last_mireds) {
        return;
    }
    lb_target_t t;
    lightbar_out_get(&t);
    /* Only the colour changes; on/level stay exactly as the controller left them. */
    evlog("sun", "elev=%.1f -> %u mireds", elev, (unsigned)m);
    lightbar_out_set(t.on, t.level, m);
    /* The sun curve is a third writer besides the web page and Matter; publish it
     * so a controller sees the colour move with the daylight. */
    (void)lb_matter_sync_target();
    ESP_LOGI(TAG, "doy=%u solar=%.2f h elev=%.1f deg -> %u mireds (~%d K)%s", doy, sh, elev, (unsigned)m,
             1000000 / (int)m, (m == s_last_mireds) ? "" : " (applied)");
    s_last_mireds = m;
}

static void solar_task(void *arg)
{
    (void)arg;
    /* Deliberately NOT subscribed to the task watchdog: this loop sleeps 60 s
     * between steps (watchdog timeout is 5 s), and it has no deadline that
     * matters - the light keeps whatever the last successful step set. */
    TickType_t next = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SOLAR_POLL_S * 1000));
        lb_time_start_sntp_if_online();
        policy_step(false);
    }
}

void solar_policy_start(void)
{
#ifdef CONFIG_LB_SUN_AUTO
    s_auto = true;
#else
    s_auto = false;
#endif
    if (s_task != NULL) {
        return;
    }
    BaseType_t ok = xTaskCreate(solar_task, "solar", 3072, NULL, 3, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TAG, "task create failed");
        return;
    }
    ESP_LOGI(TAG, "policy task started, auto=%d lat=%.1f lon=%.1f", s_auto, (float)CONFIG_LB_SUN_LAT / 10.0f,
             (float)CONFIG_LB_SUN_LON / 10.0f);
}

void solar_policy_set_auto(bool on)
{
    s_auto = on;
    ESP_LOGI(TAG, "auto CCT %s", on ? "ON" : "off");
    if (on) {
        policy_step(true);
    }
}

bool solar_policy_auto(void)
{
    return s_auto;
}

uint16_t solar_policy_last_mireds(void)
{
    return s_last_mireds;
}

float solar_policy_last_elevation(void)
{
    return s_last_elev;
}

void solar_policy_note_manual_ct(void)
{
    if (s_auto) {
        s_auto = false;
        ESP_LOGI(TAG, "controller set colour temperature by hand -> auto CCT off");
    }
}
