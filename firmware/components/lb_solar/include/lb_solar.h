// lb_solar.h - sunlight-adaptive CCT: sun elevation -> target mireds.
// Pure math, no hardware, so it can be tested on the bench (and in a unit test)
// without the sun. The board can only span its two channels:
//   warm 2700 K (LB_CT_WARM_MIREDS 370) .. cold 5000 K (LB_CT_COLD_MIREDS 200)
// so the curve saturates at both ends.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Solar elevation in degrees above the horizon for a given latitude, day of
 * year (1..366) and LOCAL SOLAR time in hours (0..24, 12 = solar noon).
 * Uses the standard declination + hour-angle model, accurate to ~0.5 deg.
 * NOTE: solar time, not clock time - the caller owns the longitude and
 * equation-of-time correction (Matter's Time Format cluster will feed it). */
float lb_solar_elevation_deg(float lat_deg, uint16_t day_of_year, float solar_hour);

/* Map elevation to a target CCT for this luminaire, clamped to the achievable
 * range: below the horizon = warmest, high sun = coldest, linear in between
 * over LB_SOLAR_FULL_SUN_DEG. */
#define LB_SOLAR_HORIZON_DEG   0.0f
#define LB_SOLAR_FULL_SUN_DEG 20.0f
uint16_t lb_solar_mireds(float elevation_deg);

/* Convenience: elevation + mapping in one call. */
uint16_t lb_solar_target_mireds(float lat_deg, uint16_t day_of_year, float solar_hour);

/* Invariant self-test (night=warm, high sun=cold, monotonic, morning/afternoon
 * symmetric, seasonal ordering, latitudes finite). */
bool lb_solar_selftest(char *reason, size_t len);

#ifdef __cplusplus
}
#endif
