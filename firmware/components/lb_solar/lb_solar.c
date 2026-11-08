// lb_solar.c - see lb_solar.h
#include "lb_solar.h"
#include "lightbar_mix.h"
#include <math.h>
#include <stdio.h>

#define DEG2RAD 0.017453292519943295f
#define RAD2DEG 57.29577951308232f

float lb_solar_elevation_deg(float lat_deg, uint16_t day_of_year, float solar_hour)
{
    /* Solar declination, simple cosine approximation (max error ~0.5 deg):
     * decl = -23.44 * cos(360/365 * (N + 10))  [degrees] */
    float n = (float)day_of_year;
    float decl = -23.44f * cosf((360.0f / 365.0f) * (n + 10.0f) * DEG2RAD);

    /* Hour angle: 15 deg per hour from local solar noon. */
    float H = 15.0f * (solar_hour - 12.0f);

    float lr = lat_deg * DEG2RAD;
    float dr = decl * DEG2RAD;
    float hr = H * DEG2RAD;

    float s = sinf(lr) * sinf(dr) + cosf(lr) * cosf(dr) * cosf(hr);
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    return asinf(s) * RAD2DEG;
}

uint16_t lb_solar_mireds(float elevation_deg)
{
    if (!isfinite(elevation_deg)) {
        return LB_CT_WARM_MIREDS;
    }
    if (elevation_deg <= LB_SOLAR_HORIZON_DEG) {
        return LB_CT_WARM_MIREDS; /* night/twilight: warmest we can do */
    }
    if (elevation_deg >= LB_SOLAR_FULL_SUN_DEG) {
        return LB_CT_COLD_MIREDS; /* high sun: coolest we can do */
    }
    float span = LB_SOLAR_FULL_SUN_DEG - LB_SOLAR_HORIZON_DEG;
    float t = (elevation_deg - LB_SOLAR_HORIZON_DEG) / span;
    float m = (float)LB_CT_WARM_MIREDS + t * (float)((int)LB_CT_COLD_MIREDS - (int)LB_CT_WARM_MIREDS);
    if (m < (float)LB_CT_COLD_MIREDS) m = LB_CT_COLD_MIREDS;
    if (m > (float)LB_CT_WARM_MIREDS) m = LB_CT_WARM_MIREDS;
    return (uint16_t)(m + 0.5f);
}

uint16_t lb_solar_target_mireds(float lat_deg, uint16_t day_of_year, float solar_hour)
{
    return lb_solar_mireds(lb_solar_elevation_deg(lat_deg, day_of_year, solar_hour));
}

bool lb_solar_selftest(char *reason, size_t len)
{
    if (reason == NULL || len == 0) {
        return false;
    }
#define FAIL(...)                                                                            \
    do {                                                                                     \
        snprintf(reason, len, __VA_ARGS__);                                                   \
        return false;                                                                         \
    } while (0)

    /* 1. ends saturate */
    if (lb_solar_mireds(-30.0f) != LB_CT_WARM_MIREDS) FAIL("night not warm (%u)", (unsigned)lb_solar_mireds(-30.0f));
    if (lb_solar_mireds(45.0f) != LB_CT_COLD_MIREDS) FAIL("high sun not cold (%u)", (unsigned)lb_solar_mireds(45.0f));

    /* 2. monotonic: CCT never gets warmer as the sun climbs */
    uint16_t prev = lb_solar_mireds(-40.0f);
    for (int e = -40; e <= 60; e++) {
        uint16_t m = lb_solar_mireds((float)e);
        if (m > prev) FAIL("CCT warmed up at elevation %d (%u -> %u)", e, (unsigned)prev, (unsigned)m);
        if (m < LB_CT_COLD_MIREDS || m > LB_CT_WARM_MIREDS) FAIL("mireds out of range at elev %d", e);
        prev = m;
    }

    /* 3. finite for every latitude/day/hour corner we can be handed */
    const float lats[] = { -66.0f, -33.0f, 0.0f, 45.0f, 60.0f };
    const uint16_t days[] = { 1, 80, 172, 266, 355, 366 };
    const float hours[] = { 0.0f, 5.5f, 9.0f, 12.0f, 15.0f, 18.5f, 23.9f };
    for (unsigned li = 0; li < sizeof(lats) / sizeof(lats[0]); li++) {
        for (unsigned di = 0; di < sizeof(days) / sizeof(days[0]); di++) {
            for (unsigned hi = 0; hi < sizeof(hours) / sizeof(hours[0]); hi++) {
                float el = lb_solar_elevation_deg(lats[li], days[di], hours[hi]);
                if (!isfinite(el) || el < -90.0f || el > 90.0f) FAIL("bad elevation lat %d", (int)lats[li]);
                uint16_t m = lb_solar_target_mireds(lats[li], days[di], hours[hi]);
                if (m < LB_CT_COLD_MIREDS || m > LB_CT_WARM_MIREDS) FAIL("bad target lat %d", (int)lats[li]);
            }
        }
    }

    /* 4. morning/afternoon symmetry (same sun height, same CCT) */
    float e9 = lb_solar_elevation_deg(45.0f, 172, 9.0f);
    float e15 = lb_solar_elevation_deg(45.0f, 172, 15.0f);
    if (fabsf(e9 - e15) > 0.6f) FAIL("asymmetry %.2f vs %.2f", e9, e15);
    if (lb_solar_mireds(e9) != lb_solar_mireds(e15)) FAIL("CCT differs AM/PM at same elevation");

    /* 5. seasonal ordering at mid-north latitude */
    float noon_summer = lb_solar_elevation_deg(45.0f, 172, 12.0f);
    float noon_winter = lb_solar_elevation_deg(45.0f, 355, 12.0f);
    if (!(noon_summer > noon_winter + 10.0f)) FAIL("no seasonal swing (%.1f vs %.1f)", noon_summer, noon_winter);

    /* 6. equator noon is high all year, and near the solstice ~66-67 deg */
    float eq = lb_solar_elevation_deg(0.0f, 172, 12.0f);
    if (fabsf(eq - 66.6f) > 1.5f) FAIL("equator noon %.1f (want ~66.6)", eq);

    /* 7. southern hemisphere summer (December) is high at latitude -33 */
    float south = lb_solar_elevation_deg(-33.0f, 355, 12.0f);
    if (south < 70.0f) FAIL("southern summer noon %.1f (want >70)", south);

    /* 8. sun below horizon at midnight for mid-latitudes */
    float mid = lb_solar_elevation_deg(45.0f, 172, 0.0f);
    if (mid > -10.0f) FAIL("midnight elevation %.1f (want < -10)", mid);
    if (lb_solar_target_mireds(45.0f, 172, 0.0f) != LB_CT_WARM_MIREDS) FAIL("midnight not warm");

#undef FAIL
    return true;
}
