// lightbar_out.c - see lightbar_out.h
#include "lightbar_out.h"
#include "lightbar_mix.h"
#include "tps92390.h"
#include "tps_bus.h"
#include "safety_sm.h"
#include "app_matter.h"
#include "esp_log.h"

static const char *TAG = "lb-out";

/* Documented reset of USER_CONFIG1 (Table 7-16): spread-spectrum pseudo-random
 * off, SPREAD_MOD_FREQ 0, SPREAD_RANGE 2 (5.3 %), SLOPE_SELECT 5 (200 ms),
 * dither off, ADV_SLOPE_EN on, reserved bit0 = 1. We only switch BRT_MODE to 2
 * (DISPLAY_BRT register instead of the grounded PWM pin). */
static lb_tuning_t s_tune = { .slope = 5, .dither = 0, .ss_range = 2, .ss_mod = 0, .ss_pseudo = 0 };

/* Composed from the tuning fields; with the defaults above this is exactly
 * 0x08A3 | BRT_MODE(2) = 0x0AA3. */
static uint16_t uc1_value(void)
{
    uint16_t v = TPS_UC1_RESERVED0 | TPS_UC1_ADV_SLOPE_EN | TPS_UC1_BRT_MODE(2u);
    v |= TPS_UC1_SLOPE_SELECT((unsigned)s_tune.slope);
    v |= TPS_UC1_DITHER_SELECT((unsigned)s_tune.dither);
    v |= TPS_UC1_SPREAD_RANGE((unsigned)s_tune.ss_range);
    v |= TPS_UC1_SPREAD_MOD_FREQ((unsigned)s_tune.ss_mod);
    if (s_tune.ss_pseudo) {
        v |= TPS_UC1_SPREAD_PSEUDO_EN;
    }
    return v;
}
#define LB_UC1_VALUE uc1_value()

static lb_target_t s_target = {
    .on = false,
    .level = 200,
    .mireds = LB_CT_WARM_MIREDS,
};
static uint8_t s_current_code = LB_LED_CURRENT_LEVEL;
static uint8_t s_level_cap = 254;
static bool s_gate_warned;
static bool s_inhibit;
static lb_diag_drive_t s_diag; /* diagnostic per-channel drive, volatile (never persisted) */
/* Per chip: USER_CONFIG1 + LED_CURR_CONFIG known to be programmed. Cleared by
 * lightbar_out_note_chip_reset() when the monitor sees a reset fingerprint. */
static bool s_configured[2];
static uint16_t s_brt_goal[2]; /* last DISPLAY_BRT written per chip - lets the monitor tell "stuck" from "off" */

bool lightbar_out_channel_requested(int idx)
{
    return (idx >= 0 && idx < 2) && s_brt_goal[idx] > 0;
}

uint16_t lightbar_out_expected_uc1(void)
{
    return LB_UC1_VALUE;
}

void lightbar_out_set_diag(const lb_diag_drive_t *d)
{
    bool was = s_diag.on;
    if (d == NULL) {
        s_diag.on = false;
    } else {
        s_diag.on = d->on;
        s_diag.warm_pct = d->warm_pct > 100 ? 100 : d->warm_pct;
        s_diag.cold_pct = d->cold_pct > 100 ? 100 : d->cold_pct;
    }
    if (was != s_diag.on) {
        ESP_LOGW(TAG, "diagnostic drive %s", s_diag.on ? "ON - channels driven independently" : "off - CCT target restored");
    }
    (void)lightbar_out_apply();
    lb_matter_report_diag();
}

void lightbar_out_get_diag(lb_diag_drive_t *out)
{
    if (out) {
        *out = s_diag;
    }
}

void lightbar_out_get_tuning(lb_tuning_t *out)
{
    if (out) {
        *out = s_tune;
    }
}

static int8_t clampf(int v, int lo, int hi, int8_t keep)
{
    if (v < 0) return keep;
    return (int8_t)(v > hi ? hi : (v < lo ? lo : v));
}

esp_err_t lightbar_out_set_tuning(const lb_tuning_t *t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_tune.slope = clampf(t->slope, 0, 7, s_tune.slope);
    s_tune.dither = clampf(t->dither, 0, 4, s_tune.dither);
    s_tune.ss_range = clampf(t->ss_range, 0, 3, s_tune.ss_range);
    s_tune.ss_mod = clampf(t->ss_mod, 0, 3, s_tune.ss_mod);
    s_tune.ss_pseudo = clampf(t->ss_pseudo, 0, 1, s_tune.ss_pseudo);

    /* Live rewrite of USER_CONFIG1 only: brightness and current are untouched,
     * so changing slope/spread while the lamp is on does not blink it. */
    esp_err_t first = ESP_OK;
    tps_bus_lock();
    for (int i = 0; i < tps_bus_chip_count(); i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        if (!c->ack || !s_configured[i]) {
            continue; /* unconfigured chips pick the value up on their next apply */
        }
        uint16_t rb = 0;
        esp_err_t e = tps92390_write_verify(c->d100, TPS_REG_USER_CONFIG1, LB_UC1_VALUE, &rb);
        if (e != ESP_OK && first == ESP_OK) {
            first = e;
        }
    }
    tps_bus_unlock();
    ESP_LOGI(TAG, "tuning: slope=%d dither=%d ss_range=%d ss_mod=%d ss_pseudo=%d -> UC1=0x%04X (%s)",
             s_tune.slope, s_tune.dither, s_tune.ss_range, s_tune.ss_mod, s_tune.ss_pseudo,
             (unsigned)LB_UC1_VALUE, esp_err_to_name(first));
    return first;
}

void lightbar_out_note_chip_reset(int idx)
{
    if (idx < 0) {
        s_configured[0] = s_configured[1] = false;
    } else if (idx < 2) {
        s_configured[idx] = false;
    }
}

void lightbar_out_set_level_cap(uint8_t cap)
{
    s_level_cap = (cap < 8) ? 8 : cap;
}

uint8_t lightbar_out_level_cap(void)
{
    return s_level_cap;
}

void lightbar_out_set(bool on, uint8_t level, uint16_t mireds)
{
    s_target.on = on;
    s_target.level = level > 254 ? 254 : level;
    s_target.mireds = mireds;
    esp_err_t e = lightbar_out_apply();
    if (e == ESP_ERR_INVALID_STATE && !s_gate_warned) {
        s_gate_warned = true;
        ESP_LOGW(TAG, "output suppressed: safety gate closed (target stored, apply on gate open)");
    }
}

void lightbar_out_get(lb_target_t *out)
{
    *out = s_target;
}

uint8_t lightbar_out_current_code(void)
{
    return s_current_code;
}

uint8_t lightbar_out_set_current(uint8_t level)
{
    if (level > LB_LED_CURRENT_MAX) {
        level = LB_LED_CURRENT_MAX;
    }
    s_current_code = level;
    lightbar_out_note_chip_reset(-1); /* ceiling changed -> rewrite LED_CURR_CONFIG */
    esp_err_t e = lightbar_out_apply();
    ESP_LOGI(TAG, "LED current ceiling level=0x%02X -> code 0x%03X (%s)", level,
             (unsigned)TPS_CURR_CODE(level), esp_err_to_name(e));
    return s_current_code;
}

void lightbar_out_inhibit(bool on)
{
    s_inhibit = on;
    if (on) {
        lightbar_out_emergency_off();
        ESP_LOGW(TAG, "outputs INHIBITED (apply() is a no-op until released)");
    } else {
        ESP_LOGW(TAG, "outputs released");
    }
}

/* One-time chip configuration, done with the string current sink at zero
 * brightness so a changed ceiling can never flash the lamp. */
static esp_err_t configure_chip(tps_ctx_t *c, int idx)
{
    uint16_t rb = 0;
    esp_err_t e = tps92390_write_verify(c->d100, TPS_REG_USER_CONFIG1, LB_UC1_VALUE, &rb);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s USER_CONFIG1 write failed: %s (rb=0x%04X)", c->name, esp_err_to_name(e), rb);
        return e;
    }
    e = tps92390_set_current_code(c->d100, TPS_CURR_CODE(s_current_code), &rb);
    if (e == TPS_ERR_VERIFY_MISMATCH) {
        /* The chip rewrites LED_CURRENT itself on ISET short (1/4) or missing
         * LEDSET (0x3FF) - report, keep going, the monitor will show the fault. */
        ESP_LOGW(TAG, "%s LED_CURR_CONFIG wrote 0x%03X but reads 0x%04X - chip is limiting current "
                      "(check BOOST_STATUS ISET/LEDSET)", c->name, (unsigned)TPS_CURR_CODE(s_current_code), rb);
    } else if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s LED_CURR_CONFIG write failed: %s", c->name, esp_err_to_name(e));
        return e;
    }
    s_configured[idx] = true;
    ESP_LOGI(TAG, "%s configured: UC1=0x%04X (BRT_MODE=2, sloper 200 ms) LED_CURR=0x%03X", c->name,
             (unsigned)LB_UC1_VALUE, (unsigned)TPS_CURR_CODE(s_current_code));
    return ESP_OK;
}

esp_err_t lightbar_out_apply(void)
{
    if (s_inhibit) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!safety_sm_gate_open()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t lvl = s_target.on ? s_target.level : 0;
    if (lvl > s_level_cap) {
        lvl = s_level_cap;
    }
    lb_brt_t b;
    if (s_diag.on) {
        /* Diagnostic mode: the two channels are driven independently from the
         * CCT mixer. Each slider is a plain 0..100 % of DISPLAY_BRT, so both
         * chips can sit at 100 % at the same time (thermal / efficiency tests).
         * The stored CCT target is untouched and comes back when diag is off. */
        b.warm = (uint16_t)(((uint32_t)s_diag.warm_pct * 65535u) / 100u);
        b.cold = (uint16_t)(((uint32_t)s_diag.cold_pct * 65535u) / 100u);
        b.mireds_used = s_target.mireds;
        lvl = (s_diag.warm_pct || s_diag.cold_pct) ? 254 : 0;
    } else {
        b = lb_mix(lvl, s_target.mireds);
    }

    /* Smallest DISPLAY_BRT that still yields one DAC LSB at the current ceiling
     * (DAC = code * BRT / 65535), so "level 1" is a dim glow, not off. */
    const uint16_t code_max = TPS_CURR_CODE(s_current_code);
    const uint16_t brt_min = (code_max > 0) ? (uint16_t)((65535u + code_max - 1u) / code_max) : 1u;
    if (lvl > 0) {
        if (b.warm > 0 && b.warm < brt_min) b.warm = brt_min;
        if (b.cold > 0 && b.cold < brt_min) b.cold = brt_min;
    }

    esp_err_t first = ESP_OK;
    tps_bus_lock();
    int n = tps_bus_chip_count();
    for (int i = 0; i < n; i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        if (!c->ack) {
            continue;
        }
        esp_err_t e = ESP_OK;
        uint16_t brt = (i == 0) ? b.warm : b.cold; /* chip0=warm, chip1=cold */
        uint16_t rb = 0;

        if (!s_configured[i]) {
            /* Configure with the sink off first: a fresh chip (POR) has
             * LED_CURR=0xFFF and BRT_MODE=0, and we never want BRT_MODE=2 to
             * take effect while an unknown DISPLAY_BRT value is in the register. */
            e = tps92390_set_brightness(c->d100, 0, &rb);
            if (e == ESP_OK) {
                e = configure_chip(c, i);
            }
        }
        if (e == ESP_OK) {
            e = tps92390_set_brightness(c->d100, brt, &rb);
            if (e == ESP_OK) {
                s_brt_goal[i] = brt;
            }
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s apply failed: %s (rb=0x%04X)", c->name, esp_err_to_name(e), rb);
            if (first == ESP_OK) {
                first = e;
            }
        }
    }
    tps_bus_unlock();

    ESP_LOGD(TAG, "apply on=%d level=%u mireds=%u -> warm=%u cold=%u", s_target.on, lvl,
             s_target.mireds, b.warm, b.cold);
    return first;
}

void lightbar_out_emergency_off(void)
{
    tps_bus_lock();
    int n = tps_bus_chip_count();
    for (int i = 0; i < n; i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        if (!c->ack) {
            continue;
        }
        uint16_t rb;
        if (tps92390_set_brightness(c->d100, 0x0000, &rb) != ESP_OK) {
            ESP_LOGE(TAG, "%s emergency off write failed", c->name);
        }
        s_brt_goal[i] = 0;
    }
    tps_bus_unlock();
    ESP_LOGW(TAG, "emergency off: DISPLAY_BRT=0 on both channels");
}