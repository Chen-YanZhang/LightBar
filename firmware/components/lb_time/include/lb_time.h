// lb_time.h - wall clock for the sun curve.
// Sources, in order of preference: Matter commissioning brings Wi-Fi, then SNTP
// fills the clock; for bench work the CLI sets it by hand. The sun curve wants
// UTC + longitude, not local wall time, so the conversion lives here.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Anything below this is "no clock yet" (NVS default / boot default 1970). */
#define LB_TIME_EPOCH_MIN 1600000000LL

typedef enum {
    LB_TIME_SRC_UNSET = 0,
    LB_TIME_SRC_MANUAL,   /* CLI time <epoch> */
    LB_TIME_SRC_SNTP,
    LB_TIME_SRC_BUILT_IN, /* clock already running at boot (e.g. restored) */
} lb_time_src_t;

/* Applies CONFIG_LB_TZ (POSIX TZ string) so localtime() is meaningful. */
esp_err_t lb_time_init(void);

/* Start SNTP once, and only if a netif is up with an IPv4 address. Safe to call
 * repeatedly from a slow poll loop; returns ESP_ERR_INVALID_STATE while offline. */
esp_err_t lb_time_start_sntp_if_online(void);

void lb_time_set_epoch(int64_t sec);
int64_t lb_time_now_epoch(void);              /* 0-ish until a source sets it */
bool lb_time_valid(void);
lb_time_src_t lb_time_source(void);
const char *lb_time_source_name(void);

/* "YYYY-MM-DD HH:MM:SS STD/DST", or "(no clock)". */
bool lb_time_local_string(char *out, size_t len);

/* UTC epoch -> (day of year, local SOLAR time in hours) for a longitude in
 * degrees (+E). Applies the equation of time and the longitude offset, and
 * wraps into [0,24). */
bool lb_time_solar_utc(int64_t epoch_utc, float lon_deg, uint16_t *day_of_year, float *solar_hour);

#ifdef __cplusplus
}
#endif
