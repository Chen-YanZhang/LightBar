#include "lightbar_mix.h"

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Perceptual (Matter CIE-L*-style) level -> optical drive, gamma-2 quadratic
 * approximation of gamma 2.2, rescaled to 0..65535. */
static uint32_t perceptual_to_optical(uint8_t level)
{
    uint32_t l = level > 254 ? 254 : level;
    return (l * l * 65535u + (254u * 254u) / 2u) / (254u * 254u);
}

lb_brt_t lb_mix_derated(uint8_t level, uint16_t mireds, uint16_t warm_max, uint16_t cold_max)
{
    const uint32_t span = LB_CT_WARM_MIREDS - LB_CT_COLD_MIREDS;
    lb_brt_t r;
    uint32_t m = clamp_u32(mireds, LB_CT_COLD_MIREDS, LB_CT_WARM_MIREDS);
    uint32_t coldw = (LB_CT_WARM_MIREDS - m) * 1024u / span; /* 0 cold .. 1024 full cold */
    uint32_t warmw = 1024u - coldw;
    uint32_t opt = perceptual_to_optical(level);

    r.warm = (uint16_t)(opt * warmw / 1024u);
    r.cold = (uint16_t)(opt * coldw / 1024u);
    if (r.warm > warm_max) r.warm = warm_max;
    if (r.cold > cold_max) r.cold = cold_max;
    r.mireds_used = (uint16_t)m;
    return r;
}

lb_brt_t lb_mix(uint8_t level, uint16_t mireds)
{
    return lb_mix_derated(level, mireds, 65535u, 65535u);
}

uint16_t lb_mixed_mireds(uint16_t warm, uint16_t cold)
{
    const uint32_t span = LB_CT_WARM_MIREDS - LB_CT_COLD_MIREDS;
    uint32_t sum = (uint32_t)warm + cold;
    if (sum == 0u) {
        return LB_CT_WARM_MIREDS; /* off state: report warm end */
    }
    uint32_t t = (uint32_t)cold * 1024u / sum;
    return (uint16_t)(LB_CT_WARM_MIREDS - (t * span) / 1024u);
}

bool lb_mix_selftest(char *reason, size_t len)
{
#define FAIL(...) do { if (len) { char *b = reason; int n = snprintf(b, (int)len, __VA_ARGS__); (void)n; } return false; } while (0)

    /* 1. zero level -> both channels silent */
    lb_brt_t z = lb_mix(0, 300);
    if (z.warm || z.cold) FAIL("level0 nonzero w=%u c=%u", z.warm, z.cold);

    /* 2. extremes */
    lb_brt_t w = lb_mix(254, LB_CT_WARM_MIREDS);
    if (w.cold != 0u || w.warm < 65500u) FAIL("warm end w=%u c=%u", w.warm, w.cold);
    lb_brt_t c = lb_mix(254, LB_CT_COLD_MIREDS);
    if (c.warm != 0u || c.cold < 65500u) FAIL("cold end w=%u c=%u", c.warm, c.cold);

    /* 3. monotonic across CCT + constant total (equal-energy crossfade) */
    int prev_w = -1, prev_c = -1;
    for (uint32_t m = LB_CT_COLD_MIREDS; m <= LB_CT_WARM_MIREDS; m += 5u) {
        lb_brt_t r = lb_mix(254, (uint16_t)m);
        if ((int)r.warm < prev_w) FAIL("warm not monotonic at mireds %u", (unsigned)m);
        if (prev_c >= 0 && (int)r.cold > prev_c) FAIL("cold not monotonic at mireds %u", (unsigned)m);
        uint32_t sum = (uint32_t)r.warm + r.cold;
        if (sum + 6u < 65535u) FAIL("energy drop at mireds %u sum=%u", (unsigned)m, (unsigned)sum);
        prev_w = (int)r.warm; prev_c = (int)r.cold;
    }

    /* 4. level monotonic at fixed CCT */
    uint32_t prev = 0;
    for (int l = 0; l <= 254; l += 10) {
        lb_brt_t r = lb_mix((uint8_t)l, 280);
        uint32_t sum = (uint32_t)r.warm + r.cold;
        if (sum < prev) FAIL("level not monotonic at level %d", l);
        prev = sum;
    }

    /* 5. CCT clamping outside range */
    if (lb_mix(254, 0).mireds_used != LB_CT_COLD_MIREDS) FAIL("clamp low");
    if (lb_mix(254, 9999).mireds_used != LB_CT_WARM_MIREDS) FAIL("clamp high");

    /* 6. derating caps */
    lb_brt_t d = lb_mix_derated(254, LB_CT_WARM_MIREDS, 2000, 500);
    if (d.warm > 2000u || d.cold > 500u) FAIL("derate caps w=%u c=%u", d.warm, d.cold);

    /* 7. mixed CCT roundtrip at extremes (post-derate reconstruction) */
    if (lb_mixed_mireds(60000, 0) + 5u < LB_CT_WARM_MIREDS || lb_mixed_mireds(60000, 0) > LB_CT_WARM_MIREDS) {
        FAIL("mixed warm end %u", (unsigned)lb_mixed_mireds(60000, 0));
    }
    if (lb_mixed_mireds(0, 60000) > LB_CT_COLD_MIREDS + 5u) {
        FAIL("mixed cold end %u", (unsigned)lb_mixed_mireds(0, 60000));
    }

    /* 8. off state reports warm end for reporting sanity */
    if (lb_mixed_mireds(0, 0) != LB_CT_WARM_MIREDS) FAIL("mixed off state");

    return true;
#undef FAIL
}
