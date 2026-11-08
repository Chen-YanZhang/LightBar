// lightbar_mix.h - pure math: Matter-style level + colour temperature ->
// warm/cold channel brightness. No ESP / no I2C dependencies (host-testable).
//
// Model: the luminaire is two TPS92390 channels, warm (~2700K) and cold
// (~5000K). Colour temperature is interpolated between the two native points
// and the total light output is split by the mix weights, so brightness is
// kept roughly constant across the CCT range (equal-energy crossfade).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native CCT points of our luminaire, in mireds (1e6/K). */
#define LB_CT_WARM_MIREDS 370u /* 2700 K */
#define LB_CT_COLD_MIREDS 200u /* 5000 K */

typedef struct {
    uint16_t warm; /* DISPLAY_BRT value for the warm channel, 0..65535 */
    uint16_t cold; /* DISPLAY_BRT value for the cold channel, 0..65535 */
    uint16_t mireds_used; /* clamped CCT actually applied */
} lb_brt_t;

/*
 * level   : 0..254 (Matter LevelControl CurrentLevel semantics)
 * mireds  : requested colour temperature; clamped to [COLD, WARM]
 *
 * Perceptual note: Matter level is perceptual (CIE L*), TPS DISPLAY_BRT maps
 * to optical output roughly linearly, so we apply an sRGB-ish transfer
 * (quadratic approximation of gamma 2.2) before splitting.
 */
lb_brt_t lb_mix(uint8_t level, uint16_t mireds);

/* Same split but with explicit per-channel ceilings (0..65535) for derating. */
lb_brt_t lb_mix_derated(uint8_t level, uint16_t mireds, uint16_t warm_max, uint16_t cold_max);

/* Mixed CCT (mireds) that the given split actually produces; for reporting. */
uint16_t lb_mixed_mireds(uint16_t warm, uint16_t cold);

/* Unit self-test: returns false if any invariant broke, fills a short reason. */
bool lb_mix_selftest(char *reason, size_t len);

#ifdef __cplusplus
}
#endif
