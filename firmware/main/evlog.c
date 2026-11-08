// evlog.c - see evlog.h
#include "evlog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static evlog_entry_t s_ring[EVLOG_N];
static int s_head;        /* next write index */
static int s_count;
static SemaphoreHandle_t s_mtx;

void evlog_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
    }
}

void evlog(const char *tag, const char *fmt, ...)
{
    evlog_entry_t e;
    e.t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    snprintf(e.tag, sizeof(e.tag), "%s", tag ? tag : "?");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);

    /* Also mirror to the console so the serial log and /api/log agree. */
    ESP_LOGI("ev", "[%s] %s", e.tag, e.msg);

    if (s_mtx == NULL) {
        evlog_init();
    }
    if (s_mtx && xSemaphoreTake(s_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_ring[s_head] = e;
        s_head = (s_head + 1) % EVLOG_N;
        if (s_count < EVLOG_N) {
            s_count++;
        }
        xSemaphoreGive(s_mtx);
    }
}

int evlog_dump(evlog_entry_t *out, int max)
{
    if (out == NULL || max <= 0 || s_mtx == NULL) {
        return 0;
    }
    int n = 0;
    if (xSemaphoreTake(s_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        int take = (s_count < max) ? s_count : max;
        int start = (s_head - take + EVLOG_N) % EVLOG_N;
        for (int i = 0; i < take; i++) {
            out[n++] = s_ring[(start + i) % EVLOG_N];
        }
        xSemaphoreGive(s_mtx);
    }
    return n;
}
