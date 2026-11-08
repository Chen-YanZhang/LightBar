// safety_sm.c - M2 implementation: flight recorder (NVS), fault monitor FSM,
// serial CLI (st/hist/clr/m1/help) over the USB-Serial-JTAG console.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tps92390.h"
#include "tps_bus.h"
#include "sht41.h"
#include "board_pins.h"
#include "lightbar_mix.h"
#include "lb_solar.h"
#include "lb_time.h"
#include "solar_policy.h"
#include "lightbar_out.h"
#include "app_matter.h"
#include "wifi_prov.h"
#include "lb_ota.h"
#include "rgb_status.h"
#include "safety_sm.h"
#include "evlog.h"
#include "driver/gpio.h"

static const char *TAG = "sm";

void tps_selftest_run(void);
static void persist_light_target_poll(void);

typedef struct {
    uint32_t magic;   /* 0x485331 "HS1" for written entries */
    uint32_t reason;
    uint16_t sup[2], bst[2], led[2];
    uint16_t flags; /* b0 warm ack, b1 cold ack, b2 warm CRCERR@boot, b3 cold CRCERR@boot */
} rec_t;
#define REC_MAGIC 0x485331u

#define HIST_N 24
static rec_t s_hist[HIST_N];
static size_t s_hist_n;
static uint32_t s_bcount;
static bool s_boot_crcerr;          /* CRCERR seen at true power-on (pre-selftest) */
static uint16_t s_last[2][3];       /* monitor's latest raw SUP/BOOST/LED per chip (0xFFFF = no data) */
static uint16_t s_fsm[2];           /* monitor's latest 0x14 FSM code per chip */
static uint16_t s_autodetect[2];    /* 1Eh read at boot (MODE / strings / boost & PWM freq from strap resistors) */
static bool s_ack[2];               /* monitor's latest presence result per chip */
static uint32_t s_crit_events;
static uint32_t s_boost_warn_events;
static uint32_t s_por_events;       /* silent chip resets detected via USER_CONFIG1 */
static uint32_t s_led_clear_events; /* LED string fault edges seen */

typedef enum { SM_BOOT, SM_NOCHIPS, SM_DISARMED, SM_BLOCKED_CRCERR, SM_ARM_OK } sm_t;
static const char *const SM_NAME[] = { "BOOT", "NOCHIPS", "DISARMED", "BLOCKED_CRCERR", "ARM_OK" };
static sm_t s_state = SM_BOOT;
static uint16_t s_prev_crit_chip[2];
static lb_target_t s_saved;          /* light target restored from NVS at boot */
static bool s_saved_valid;
static lb_target_t s_last_seen;      /* for change detection -> debounced NVS save */
static TickType_t s_dirty_tick;      /* 0 = clean */

/* VINOCP is the input current-sense comparator (RISENSE > 220 mV, DS 7.3.9.1.4).
 * PCB3_3: VSENSE_P sits on the shared VBUS node and the sense network is scaled
 * for 5 V, so it trips on every load step and permanently at a 20 V PD contract
 * (measured 20.2 V x 0.38 A = 7.7 W, no real over-current). The chip handles it
 * itself (Fault Recovery + 100 ms restart); the firmware only logs it on the old
 * board. On the new PCB VSENSE_P/N are across the real sense resistor and VINOCP
 * is a genuine system hazard (inductor/FET short) -> critical again. */
#if CONFIG_LB_REV_NEW
#define SUPPLY_CRIT (TPS_SS_BSTSYNC | TPS_SS_CP | TPS_SS_CPCAP | TPS_SS_VINOCP | \
                     TPS_SS_VDDUVLO | TPS_SS_VINOVP | TPS_SS_VINUVLO)
#define SUPPLY_LOGONLY 0u
#else
#define SUPPLY_CRIT (TPS_SS_BSTSYNC | TPS_SS_CP | TPS_SS_CPCAP | \
                     TPS_SS_VDDUVLO | TPS_SS_VINOVP | TPS_SS_VINUVLO)
#define SUPPLY_LOGONLY (TPS_SS_VINOCP)
#endif
/* BstOVP_L (bit1) is the low-threshold over-voltage *warning* - the control loop
 * overshoots it for a moment whenever brightness jumps to full, and treating it
 * as fatal made the lamp blink once and go dark. Only BstOVP_H (bit3) is a hard
 * fault worth cutting the outputs for. */
#define BOOST_CRIT  (TPS_BS_TSD | TPS_BS_ISET | TPS_BS_LEDSET | TPS_BS_MODESEL | \
                     TPS_BS_FSET | TPS_BS_BSTOCP | TPS_BS_BSTOVPH)
#define BOOST_WARN  (TPS_BS_BSTOVPL)

static const char *reason_name(uint32_t r)
{
    switch ((esp_reset_reason_t)r) {
    case ESP_RST_UNKNOWN:    return "UNKNOWN";
    case ESP_RST_POWERON:    return "POWERON";
    case ESP_RST_EXT:        return "EXT_PIN";
    case ESP_RST_SW:         return "SOFTWARE";
    case ESP_RST_PANIC:      return "PANIC";
    case ESP_RST_INT_WDT:    return "INT_WDT";
    case ESP_RST_TASK_WDT:   return "TASK_WDT";
    case ESP_RST_WDT:        return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP:  return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:   return "BROWNOUT";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    default:                 return "OTHER";
    }
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("lb", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u32(h, "bcnt", s_bcount);
    nvs_set_u32(h, "lcap", lightbar_out_level_cap());
    nvs_set_blob(h, "hist", s_hist, sizeof(s_hist));
    lb_tuning_t t;
    lightbar_out_get_tuning(&t);
    nvs_set_blob(h, "tune", &t, sizeof(t));
    nvs_set_u8(h, "ledb", rgb_status_get_brightness());
    nvs_set_u8(h, "cur", lightbar_out_current_code());
    lb_target_t lt;
    lightbar_out_get(&lt);
    nvs_set_u8(h, "lt_on", lt.on ? 1 : 0);
    nvs_set_u8(h, "lt_lv", lt.level);
    nvs_set_u16(h, "lt_mi", lt.mireds);
    nvs_commit(h);
    nvs_close(h);
}

void safety_sm_save_settings(void)
{
    nvs_save();
}

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("lb", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "bcnt", &s_bcount);
        uint32_t cap = 254;
        if (nvs_get_u32(h, "lcap", &cap) == ESP_OK && cap >= 8 && cap <= 254) {
            lightbar_out_set_level_cap((uint8_t)cap);
        }
        uint8_t on8 = 0, lv = 0; uint16_t mi = 0;
        if (nvs_get_u8(h, "lt_on", &on8) == ESP_OK && nvs_get_u8(h, "lt_lv", &lv) == ESP_OK &&
            nvs_get_u16(h, "lt_mi", &mi) == ESP_OK) {
            s_saved.on = on8 != 0; s_saved.level = lv; s_saved.mireds = mi; s_saved_valid = true;
        }
        uint8_t cur = 0;
        if (nvs_get_u8(h, "cur", &cur) == ESP_OK && cur >= 0x10) {
            (void)lightbar_out_set_current(cur); /* clamped to LB_LED_CURRENT_MAX inside */
        }
        uint8_t ledb = 0;
        if (nvs_get_u8(h, "ledb", &ledb) == ESP_OK) {
            rgb_status_set_brightness(ledb);
        }
        lb_tuning_t t;
        size_t tsz = sizeof(t);
        if (nvs_get_blob(h, "tune", &t, &tsz) == ESP_OK && tsz == sizeof(t)) {
            (void)lightbar_out_set_tuning(&t); /* chips not configured yet: value only */
        }
        /* The blob may be larger than our buffer if rec_t/HIST_N changed in an
         * older build: nvs_get_blob then fails with INVALID_LENGTH and reports
         * the stored size in sz. Never index past the array in that case. */
        size_t sz = sizeof(s_hist);
        esp_err_t e = nvs_get_blob(h, "hist", s_hist, &sz);
        if (e == ESP_OK) {
            s_hist_n = sz / sizeof(rec_t);
            if (s_hist_n > HIST_N) {
                s_hist_n = HIST_N;
            }
        } else {
            if (e != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGW(TAG, "flight recorder blob unreadable (%s, %u bytes) - starting empty",
                         esp_err_to_name(e), (unsigned)sz);
            }
            memset(s_hist, 0, sizeof(s_hist));
            s_hist_n = 0;
        }
        nvs_close(h);
    }
}

/* out[0..2] = SUPPLY/BOOST/LED (0xFFFF when unreadable), returns FSM or 0xFFFF. */
static uint16_t read_chip_status(int i, uint16_t out[3], bool *ack)
{
    tps_ctx_t *c = tps_bus_chip(i);
    tps_faults_t f;
    esp_err_t e = tps92390_read_faults(c->d100, &f);
    out[0] = f.sup; out[1] = f.bst; out[2] = f.led;
    if (ack) {
        *ack = (f.sup != 0xFFFF); /* SUPPLY_STATUS can never legitimately read 0xFFFF */
    }
    (void)e;
    return f.fsm;
}

void safety_sm_recorder_boot(void)
{
    nvs_load();
    s_bcount++;
    rec_t r = { .magic = REC_MAGIC, .reason = (uint32_t)esp_reset_reason() };
    tps_bus_lock();
    for (int i = 0; i < tps_bus_chip_count(); i++) {
        uint16_t st[3];
        bool ack = false;
        uint16_t fsm = read_chip_status(i, st, &ack);
        tps_ctx_t *c = tps_bus_chip(i);
        c->ack = ack;
        s_ack[i] = ack;
        s_fsm[i] = fsm;
        r.sup[i] = st[0]; r.bst[i] = st[1]; r.led[i] = st[2];
        if (ack) r.flags |= (1u << i);
        if (ack && (st[0] & TPS_SS_CRCERR)) {
            r.flags |= (4u << i);
            s_boot_crcerr = true;
        }
        memcpy(s_last[i], st, sizeof(st));
    }

    /* SAFE STATE: the drivers are VBUS-powered and keep every register across an
     * MCU reset, so right after recording what we found, force the LED current
     * sink off. Everything else (USER_CONFIG1, LED_CURR_CONFIG, the stored
     * target) is rewritten by lightbar_out once the gate opens. Also clear the
     * I2C_ERROR latch (DS 7.3.9.3.5) that a reset mid-transaction leaves behind,
     * so the bit means something when it shows up later. */
    for (int i = 0; i < tps_bus_chip_count(); i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        if (!c->ack) {
            continue;
        }
        uint16_t rb = 0;
        esp_err_t e = tps92390_set_brightness(c->d100, 0, &rb);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s safe-state DISPLAY_BRT=0 failed: %s", c->name, esp_err_to_name(e));
        }
        if (r.led[i] != 0xFFFF && (r.led[i] & TPS_LS_I2C_ERROR)) {
            (void)tps92390_clear_faults(c->d100, TPS_REG_LED_STATUS, TPS_LS_I2C_ERROR);
        }
        tps_identity_t id;
        if (tps92390_check_identity(c->d100, &id) == ESP_OK) {
            s_autodetect[i] = id.autodetect_raw;
            ESP_LOGI(TAG, "%s identity: fingerprint %s (06=%04X 08=%04X 0A=%04X 0C=%04X) 1E=%04X -> "
                          "addr 0x%02X %s, strings_cfg=%u, boost_fsel=%u, pwm_fsel=%u",
                     c->name, id.fingerprint_ok ? "OK" : "MISMATCH", id.uc2, id.sup_int_en, id.bst_int_en,
                     id.led_int_en, id.autodetect_raw, id.ad.i2c_addr,
                     id.ad.current_dimming ? "current-dimming" : "PWM/hybrid", id.ad.string_cfg,
                     id.ad.boost_freq_sel, id.ad.pwm_freq_sel);
            if (!id.fingerprint_ok) {
                ESP_LOGW(TAG, "%s: INT_EN/UC2 differ from reset values - either a previous build wrote them "
                              "or the 16-bit access is misaligned", c->name);
            }
        }
    }
    lightbar_out_note_chip_reset(-1);
    tps_bus_unlock();

    if (s_hist_n < HIST_N) {
        s_hist[s_hist_n++] = r;
    } else {
        memmove(&s_hist[0], &s_hist[1], sizeof(rec_t) * (HIST_N - 1));
        s_hist[HIST_N - 1] = r;
    }
    nvs_save();

    ESP_LOGW(TAG, "RECORDER boot#%"PRIu32" reason=%s flags=0x%04X",
             s_bcount, reason_name(r.reason), r.flags);
    for (int i = 0; i < 2; i++) {
        ESP_LOGW(TAG, "RECORDER %s SUP=0x%04X BOOST=0x%04X LED=0x%04X FSM=0x%04X",
                 tps_bus_chip(i)->name, r.sup[i], r.bst[i], r.led[i], s_fsm[i]);
    }
}

/* --- Board over-temperature protection -----------------------------------
 * The SHT40 sits next to the two drivers on purpose: it measures BOARD
 * temperature, and the product requirement is to shut both driver outputs down
 * before the boost section cooks itself. This is NOT an ambient sensor, so it
 * must never be moved away from the heat sources.
 *
 * Behaviour:
 *   temp >= LB_OT_TRIP_C   -> latch, force DISPLAY_BRT=0 on both chips and keep
 *                             the safety gate closed (state DISARMED)
 *   temp <= LB_OT_CLEAR_C  -> clear after OT_CLEAR_SAMPLES consecutive samples
 *                             (hysteresis, so a lamp parked at the limit does
 *                             not switch on/off continuously)
 * A failing sensor is logged but never opens the gate by itself: the drivers'
 * own TSD (BOOST_STATUS bit15) remains the last line of defence. */
static float s_board_temp;
static bool s_board_temp_valid;
static bool s_ot_latched;
static uint8_t s_ot_clear_cnt;
static uint8_t s_temp_fail_cnt;
static int s_ot_trip_override; /* 0 = use Kconfig; >0 = runtime test threshold */

#define OT_CLEAR_SAMPLES 3
#define OT_FAIL_WARN     5

static int ot_trip_c(void)
{
    return (s_ot_trip_override > 0) ? s_ot_trip_override : CONFIG_LB_OT_TRIP_C;
}

static int ot_clear_c(void)
{
    int trip = ot_trip_c();
    int clear = CONFIG_LB_OT_CLEAR_C;
    if (clear >= trip) {
        clear = trip - 5; /* keep hysteresis even with an overridden trip point */
    }
    return clear;
}

static void thermal_poll(void)
{
    if (!tps_bus_sht41_present()) {
        return;
    }
    float t = 0.0f, h = 0.0f;
    tps_bus_lock();
    esp_err_t e = sht41_measure(tps_bus_sht41(), &t, &h);
    tps_bus_unlock();
    if (e != ESP_OK) {
        if (s_temp_fail_cnt < 255) {
            s_temp_fail_cnt++;
        }
        if (s_temp_fail_cnt == OT_FAIL_WARN) {
            ESP_LOGW(TAG, "board temp sensor failing (%s) - overtemp protection degraded",
                     esp_err_to_name(e));
        }
        return;
    }
    s_temp_fail_cnt = 0;
    s_board_temp = t;
    s_board_temp_valid = true;

    if (!s_ot_latched) {
        if (t >= (float)ot_trip_c()) {
            s_ot_latched = true;
            s_ot_clear_cnt = 0;
            s_crit_events++;
            ESP_LOGE(TAG, "OVERTEMP: board %.1f C >= %d C -> both driver outputs OFF",
                     t, ot_trip_c());
        }
    } else if (t <= (float)ot_clear_c()) {
        if (++s_ot_clear_cnt >= OT_CLEAR_SAMPLES) {
            s_ot_latched = false;
            s_ot_clear_cnt = 0;
            ESP_LOGW(TAG, "OVERTEMP cleared: board %.1f C <= %d C for %d samples", t, ot_clear_c(),
                     OT_CLEAR_SAMPLES);
        }
    } else {
        s_ot_clear_cnt = 0;
    }
}

/* Silent chip reset detection. A VBUS brownout (seen on 5 V, M4e 7.3) or a true
 * POR puts the driver back to USER_CONFIG1=0x08A3 (BRT_MODE=0), LED_CURR=0xFFF,
 * DISPLAY_BRT=0 - the lamp goes dark and, with the status registers all clean,
 * the state machine would never notice. Compare UC1 with what lightbar_out
 * programmed; a mismatch on a configured chip means "reconfigure + re-apply". */
static bool chip_reset_detected(int i)
{
    uint16_t uc1 = 0;
    if (tps92390_read(tps_bus_chip(i)->d100, TPS_REG_USER_CONFIG1, &uc1) != ESP_OK) {
        return false;
    }
    return uc1 != lightbar_out_expected_uc1();
}

/* ---- LED string fault tracking -------------------------------------------
 * Datasheet 7.3.9.3 / Table 7-9: an OPEN or SHORT string is disabled and
 * removed from the adaptive loop; LEDx_FAULT / OPEN_LED / SHORT_LED / GND_LED
 * "maintain their value until device power-down" and the string "is re-enabled
 * next power cycle". Only the LED_STATUS summary bit is clearable. So:
 *   - we decode LEDx_FAULT[3:0] + kind every second and publish it (web/API/log);
 *   - a reconnected string does NOT come back by itself: it needs EN low->high
 *     (new PCB, GPIO10/11) or a VBUS power cycle (PCB3_3, EN hardwired);
 *   - we clear only the summary bit after logging an edge, so the next fault on
 *     another string produces a fresh edge (and INT on the new PCB).
 * Detection happens only while the LED drivers run (brightness > 0). */
static uint8_t s_str_fault[2];     /* LEDx_FAULT mask, bits 0..3 = string 1..4 */
static uint8_t s_str_kind[2];      /* b0 open, b1 short, b2 gnd */
static bool s_str_latched_any;     /* any disabled string on either chip */
static uint8_t s_notnormal_cnt[2]; /* consecutive 1 s samples with light requested but FSM != NORMAL */
static bool s_chip_stuck[2];       /* chip cannot reach NORMAL (VINOCP fault-recovery loop) */

static void led_fault_track(int i, uint16_t led, uint16_t fsm)
{
    if (led == 0xFFFF) {
        return;
    }
    uint8_t mask = (uint8_t)(led & 0x0F) & (uint8_t)((1u << CONFIG_LB_STRINGS_PER_CHIP) - 1u);
    uint8_t kind = (uint8_t)((!!(led & TPS_LS_OPEN_LED)) | ((!!(led & TPS_LS_SHORT_LED)) << 1) |
                             ((!!(led & TPS_LS_GND_LED)) << 2));
    if (mask != s_str_fault[i] || kind != s_str_kind[i]) {
        uint8_t newly = (uint8_t)(mask & ~s_str_fault[i]);
        for (int s = 0; s < CONFIG_LB_STRINGS_PER_CHIP; s++) {
            if (newly & (1u << s)) {
                s_led_clear_events++;
                evlog("led", "%s string %d %s - disabled by chip until power cycle", tps_bus_chip(i)->name,
                      s + 1, (kind & 4) ? "SHORT-TO-GND" : (kind & 2) ? "SHORT" : "OPEN");
            }
        }
        if (mask == 0 && s_str_fault[i] != 0) {
            evlog("led", "%s strings all OK again (power cycle)", tps_bus_chip(i)->name);
        }
        s_str_fault[i] = mask;
        s_str_kind[i] = kind;
        if ((fsm & TPS_FSM_MASK) == TPS_FSM_ALL_LED_FAULT) {
            evlog("led", "%s ALL_LED_FAULT latched: every string disabled", tps_bus_chip(i)->name);
        }
        /* re-arm the summary bit so a further fault shows up as a new edge */
        if (mask != 0) {
            (void)tps92390_clear_faults(tps_bus_chip(i)->d100, TPS_REG_LED_STATUS, TPS_LS_LED);
        }
    }
    s_str_latched_any = (s_str_fault[0] | s_str_fault[1]) != 0;
}

/* Chip-level stuck detection: light requested, chip answers, but FSM stays out
 * of NORMAL for >= 3 s. On PCB3_3 this is the VINOCP -> FAULT_RECOVERY restart
 * loop (restart does not use the sloper and trips again); it survives any
 * brightness command and only a VBUS power cycle (or EN on the new PCB) clears
 * it. Without this the page shows "ok" while one channel is dark. */
static void chip_stuck_track(int i, bool ack, uint16_t fsm, uint16_t sup)
{
    bool want = lightbar_out_channel_requested(i) && (s_state == SM_ARM_OK);
    bool normal = ((fsm & TPS_FSM_MASK) == TPS_FSM_NORMAL);
    if (ack && want && !normal) {
        if (s_notnormal_cnt[i] < 255) s_notnormal_cnt[i]++;
    } else {
        s_notnormal_cnt[i] = 0;
    }
    bool stuck = s_notnormal_cnt[i] >= 3;
    if (stuck != s_chip_stuck[i]) {
        s_chip_stuck[i] = stuck;
        if (stuck) {
            s_crit_events++;
            evlog("sm", "%s STUCK: FSM=0x%02X SUP=%04X with light requested for %us - restart loop, "
                        "needs VBUS power cycle (EN hardwired)", tps_bus_chip(i)->name,
                  (unsigned)(fsm & TPS_FSM_MASK), sup, (unsigned)s_notnormal_cnt[i]);
        } else {
            evlog("sm", "%s back to NORMAL", tps_bus_chip(i)->name);
        }
    }
}

bool safety_sm_chip_stuck(int idx)
{
    return (idx >= 0 && idx < 2) ? s_chip_stuck[idx] : false;
}

uint8_t safety_sm_string_faults(int idx, uint8_t *kind)
{
    if (idx < 0 || idx > 1) {
        return 0;
    }
    if (kind) {
        *kind = s_str_kind[idx];
    }
    return s_str_fault[idx];
}

/* Re-probe the strings: the only way to re-enable a disabled string is a chip
 * power cycle. New PCB: EN low >= 400 ms (7.4.2 discharge) -> EN high -> wait
 * STANDBY -> reconfigure -> re-apply. PCB3_3: not possible from software. */
esp_err_t safety_sm_reprobe_strings(void)
{
#if CONFIG_LB_REV_NEW
    evlog("led", "re-probe: EN low/high on both chips");
    lightbar_out_inhibit(true);
    gpio_set_level(LB_EN_WARM_GPIO, 0);
    gpio_set_level(LB_EN_COLD_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(500));
    gpio_set_level(LB_EN_WARM_GPIO, 1);
    gpio_set_level(LB_EN_COLD_GPIO, 1);
    tps_bus_lock();
    for (int i = 0; i < 2; i++) {
        uint16_t fsm = 0;
        (void)tps92390_wait_ready(tps_bus_chip(i)->d100, 300, &fsm);
    }
    tps_bus_unlock();
    s_str_fault[0] = s_str_fault[1] = 0;
    s_str_kind[0] = s_str_kind[1] = 0;
    lightbar_out_note_chip_reset(-1);
    lightbar_out_inhibit(false);
    return lightbar_out_apply();
#else
    evlog("led", "re-probe requested but EN is hardwired on PCB3_3 - needs a VBUS power cycle");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void monitor_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    uint16_t prev_crit = 0;
    uint32_t hb_div = 0;
    bool was_configured_hint[2] = { false, false };

    while (true) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));

        /* Board temperature first so an over-temperature trip is part of THIS
         * cycle's state decision (<= 1 s), not the next one. */
        thermal_poll();

        uint16_t sup[2], bst[2], led[2], fsm[2];
        bool ack[2] = { false, false };
        bool por[2] = { false, false };

        tps_bus_lock();
        for (int i = 0; i < 2; i++) {
            uint16_t st[3];
            fsm[i] = read_chip_status(i, st, &ack[i]);
            sup[i] = st[0]; bst[i] = st[1]; led[i] = st[2];
            memcpy(s_last[i], st, sizeof(st));
            s_fsm[i] = fsm[i];
            s_ack[i] = ack[i];
            tps_bus_chip(i)->ack = ack[i]; /* keep lightbar_out's view current */
            if (ack[i] && s_state == SM_ARM_OK && chip_reset_detected(i)) {
                /* Only meaningful once lightbar_out has programmed the chip at
                 * least once; the first apply() after boot configures it anyway. */
                if (was_configured_hint[i]) {
                    por[i] = true;
                }
            } else if (ack[i] && s_state == SM_ARM_OK) {
                was_configured_hint[i] = true;
            }
        }
        tps_bus_unlock();

        uint16_t crit = 0;
        for (int i = 0; i < 2; i++) {
            if (!ack[i]) continue;
            if (bst[i] & BOOST_WARN) {
                /* BstOVP_L is the low OVP threshold the loop may overshoot on a
                 * brightness step; the chip just pauses switching (Table 7-9). */
                static uint32_t warn_tick;
                if (xTaskGetTickCount() - warn_tick > pdMS_TO_TICKS(3000)) {
                    warn_tick = xTaskGetTickCount();
                    ESP_LOGW(TAG, "chip %d BstOVP_L warning on 0x%04X", i, bst[i]);
                }
                s_boost_warn_events++;
            }
            uint16_t f = (uint16_t)((sup[i] & SUPPLY_CRIT) | (bst[i] & BOOST_CRIT));
            if (f) {
                if (!(s_prev_crit_chip[i] & f)) s_crit_events++; /* edge-counted */
                crit |= f;
            }
            s_prev_crit_chip[i] = f;

            if (sup[i] & SUPPLY_LOGONLY) {
                static uint32_t vinocp_tick;
                if (xTaskGetTickCount() - vinocp_tick > pdMS_TO_TICKS(10000)) {
                    vinocp_tick = xTaskGetTickCount();
                    ESP_LOGW(TAG, "chip %d VINOCP asserted (SUP=0x%04X) - chip handles it; log only", i, sup[i]);
                    evlog("sm", "chip %d VINOCP fsm=%02X", i, (unsigned)(fsm[i] & TPS_FSM_MASK));
                }
            }

            /* Supply/boost faults latch in the chip. Clear exactly the pairs that
             * are set (STATUS|CLEAR written together, 7.5.2.2); a persisting
             * condition simply re-asserts next second and keeps the gate shut. */
            tps_bus_lock();
            (void)tps92390_clear_faults(tps_bus_chip(i)->d100, TPS_REG_SUPPLY_STATUS, sup[i]);
            (void)tps92390_clear_faults(tps_bus_chip(i)->d100, TPS_REG_BOOST_STATUS, bst[i]);
            led_fault_track(i, led[i], fsm[i]);
            tps_bus_unlock();
            chip_stuck_track(i, ack[i], fsm[i], sup[i]);
        }

        sm_t next;
        if (!ack[0] && !ack[1])      next = SM_NOCHIPS;
        else if (crit)               next = SM_DISARMED;   /* criticals -> gate closed */
        else if (s_ot_latched)       next = SM_DISARMED;   /* board too hot -> outputs off */
        else if (s_boot_crcerr)      next = SM_BLOCKED_CRCERR;
        else                         next = SM_ARM_OK;

        if (next != s_state) {
            ESP_LOGW(TAG, "STATE %s -> %s (SUP=0x%04X/0x%04X BOOST=0x%04X/0x%04X crit=0x%04X)",
                     SM_NAME[s_state], SM_NAME[next], sup[0], sup[1], bst[0], bst[1], crit);
            bool was_arm = (s_state == SM_ARM_OK);
            evlog("sm", "%s -> %s sup=%04X/%04X bst=%04X/%04X", SM_NAME[s_state], SM_NAME[next], sup[0], sup[1], bst[0], bst[1]);
            s_state = next;
            if (next == SM_ARM_OK) {
                lightbar_out_apply(); /* re-push stored light target once gate reopens */
            } else if (was_arm) {
                lightbar_out_emergency_off();
            }
        } else if (s_state == SM_ARM_OK && (por[0] || por[1])) {
            s_por_events++;
            for (int i = 0; i < 2; i++) {
                if (por[i]) {
                    ESP_LOGW(TAG, "%s lost its configuration (USER_CONFIG1 != 0x%04X) - brownout/POR? "
                                  "reconfiguring (#%u)", tps_bus_chip(i)->name,
                             (unsigned)lightbar_out_expected_uc1(), (unsigned)s_por_events);
                    lightbar_out_note_chip_reset(i);
                    evlog("sm", "chip %d lost config (POR/brownout) - reconfigured", i);
                }
            }
            lightbar_out_apply();
        }
        if (crit && crit != prev_crit) {
            ESP_LOGE(TAG, "CRITICAL faults 0x%04X (warm SUP=0x%04X BST=0x%04X | cold SUP=0x%04X BST=0x%04X)",
                     crit, sup[0], bst[0], sup[1], bst[1]);
        }
        prev_crit = crit;

        persist_light_target_poll();

        if (++hb_div >= 5) {
            hb_div = 0;
            ESP_LOGI(TAG, "heartbeat: state=%s crit_events=%u por=%u ledclr=%u boots=%"PRIu32,
                     SM_NAME[s_state], (unsigned)s_crit_events, (unsigned)s_por_events,
                     (unsigned)s_led_clear_events, s_bcount);
        }
        rgb_status_poll(); /* 1 Hz status LED (no-op when disabled) */
    }
}

void safety_sm_restore_light_target(void)
{
    if (!s_saved_valid) {
        lightbar_out_get(&s_last_seen);
        evlog("light", "no saved target in NVS (first boot)");
        return;
    }
    if (s_saved.mireds < LB_CT_COLD_MIREDS || s_saved.mireds > LB_CT_WARM_MIREDS) {
        s_saved.mireds = LB_CT_WARM_MIREDS;
    }
    if (s_saved.level > 254) {
        s_saved.level = 254;
    }
    lightbar_out_set(s_saved.on, s_saved.level, s_saved.mireds);
    (void)lb_matter_sync_target();
    lightbar_out_get(&s_last_seen);
    evlog("light", "restored from NVS: on=%d level=%u mireds=%u (gate %s)", s_saved.on, s_saved.level,
          s_saved.mireds, safety_sm_gate_open() ? "open" : "closed, applies when open");
}

/* Called at 1 Hz from the monitor: persist the light target 5 s after the last
 * change, so a slider drag costs one NVS write, not fifty. */
static void persist_light_target_poll(void)
{
    lb_target_t t;
    lightbar_out_get(&t);
    TickType_t now = xTaskGetTickCount();
    if (t.on != s_last_seen.on || t.level != s_last_seen.level || t.mireds != s_last_seen.mireds) {
        s_last_seen = t;
        s_dirty_tick = now ? now : 1;
    } else if (s_dirty_tick != 0 && (now - s_dirty_tick) >= pdMS_TO_TICKS(5000)) {
        s_dirty_tick = 0;
        nvs_save();
        evlog("light", "saved to NVS: on=%d level=%u mireds=%u", t.on, t.level, t.mireds);
    }
}

/* ---------------- CLI ---------------- */

static int cmd_st(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("state=%s  boot_crcerr=%d  crit_events=%u  boots=%u\n",
           SM_NAME[s_state], s_boot_crcerr, (unsigned)s_crit_events, (unsigned)s_bcount);
    printf("board_temp=%s%.1f C  overtemp=%d (trip %d C / clear %d C%s)\n",
           s_board_temp_valid ? "" : "(no reading) ", s_board_temp, (int)s_ot_latched, ot_trip_c(),
           ot_clear_c(), s_ot_trip_override ? ", OVERRIDDEN" : "");
    for (int i = 0; i < 2; i++) {
        printf("%-9s SUP=0x%04X BOOST=0x%04X LED=0x%04X\n",
               tps_bus_chip(i)->name, s_last[i][0], s_last[i][1], s_last[i][2]);
    }
    return 0;
}

static int cmd_hist(int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned valid = 0;
    for (size_t k = 0; k < s_hist_n; k++) {
        if (s_hist[k].magic == REC_MAGIC) valid++;
    }
    printf("history (%u valid of %u slots, latest last):\n", valid, (unsigned)s_hist_n);
    for (size_t k = 0; k < s_hist_n; k++) {
        const rec_t *r = &s_hist[k];
        if (r->magic != REC_MAGIC) continue;
        printf("  reason=%-8s flags=0x%04X | warm SUP=0x%04X BST=0x%04X LED=0x%04X | cold SUP=0x%04X BST=0x%04X LED=0x%04X\n",
               reason_name(r->reason), r->flags,
               r->sup[0], r->bst[0], r->led[0], r->sup[1], r->bst[1], r->led[1]);
    }
    return 0;
}

static int cmd_clr(int argc, char **argv)
{
    (void)argc; (void)argv;
    tps_bus_lock();
    for (int i = 0; i < 2; i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        uint16_t st;
        const uint8_t regs[2] = { TPS_REG_SUPPLY_STATUS, TPS_REG_BOOST_STATUS };
        for (int k = 0; k < 2; k++) {
            if (tps92390_read(c->d100, regs[k], &st) == ESP_OK && st) {
                tps92390_write(c->d100, regs[k], (uint16_t)(st | (st >> 1)));
                uint16_t rb = 0;
                tps92390_read(c->d100, regs[k], &rb);
                printf("%s reg%02X: 0x%04X -> clear -> 0x%04X\n", c->name, regs[k], st, rb);
            }
        }
    }
    tps_bus_unlock();
    return 0;
}

static int cmd_m1(int argc, char **argv)
{
    (void)argc; (void)argv;
    tps_selftest_run();
    return 0;
}

bool safety_sm_gate_open(void)
{
    return s_state == SM_ARM_OK;
}

float safety_sm_board_temp(bool *valid)
{
    if (valid != NULL) {
        *valid = s_board_temp_valid;
    }
    return s_board_temp;
}

bool safety_sm_overtemp(void)
{
    return s_ot_latched;
}

const char *safety_sm_state_name(void)
{
    return SM_NAME[s_state];
}

uint16_t safety_sm_autodetect(int idx)
{
    return (idx >= 0 && idx < 2) ? s_autodetect[idx] : 0;
}

uint16_t safety_sm_fsm(int idx)
{
    return (idx >= 0 && idx < 2) ? s_fsm[idx] : 0xFFFF;
}

void safety_sm_diag(lb_diag_t *out){
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->boots = s_bcount;
    out->crit_events = s_crit_events;
    out->boot_crcerr = s_boot_crcerr;
    for (int i = 0; i < 2; i++) {
        out->sup[i] = s_last[i][0];
        out->bst[i] = s_last[i][1];
        out->led[i] = s_last[i][2];
    }
    /* newest last, capped for the JSON response */
    size_t valid = 0;
    for (size_t k = 0; k < s_hist_n; k++) {
        if (s_hist[k].magic == REC_MAGIC) {
            valid++;
        }
    }
    size_t skip = (valid > LB_DIAG_HIST) ? (valid - LB_DIAG_HIST) : 0;
    size_t seen = 0;
    int w = 0;
    for (size_t k = 0; k < s_hist_n && w < LB_DIAG_HIST; k++) {
        if (s_hist[k].magic != REC_MAGIC) {
            continue;
        }
        if (seen++ < skip) {
            continue;
        }
        out->hist[w].reason = reason_name(s_hist[k].reason);
        out->hist[w].flags = s_hist[k].flags;
        for (int i = 0; i < 2; i++) {
            out->hist[w].sup[i] = s_hist[k].sup[i];
            out->hist[w].bst[i] = s_hist[k].bst[i];
            out->hist[w].led[i] = s_hist[k].led[i];
        }
        w++;
    }
    out->hist_n = w;
}

uint32_t safety_sm_status_bits(void)
{
    uint32_t b = 0;
    if (s_state == SM_ARM_OK) {
        b |= LB_ST_ARMED;
    }
    if (s_boot_crcerr) {
        b |= LB_ST_CRCERR;
    }
    if (s_state == SM_NOCHIPS) {
        b |= LB_ST_NO_CHIPS;
    }
    if (s_ot_latched) {
        b |= LB_ST_OVERTEMP;
    }
    if (s_str_latched_any || s_chip_stuck[0] || s_chip_stuck[1]) {
        b |= LB_ST_STRING_FAULT;
    }
    /* 0xFFFF in s_last means "chip did not answer", not "every fault set". */
    uint16_t crit = 0;
    for (int i = 0; i < 2; i++) {
        if (!s_ack[i]) continue;
        crit |= (uint16_t)((s_last[i][0] & SUPPLY_CRIT) | (s_last[i][1] & BOOST_CRIT));
    }
    if (crit != 0) {
        b |= LB_ST_CRIT_NOW;
    }
    return b;
}

static int cmd_set(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: set <on:0|1> <level:0-254> [mireds:%u-%u]\n",
               (unsigned)LB_CT_COLD_MIREDS, (unsigned)LB_CT_WARM_MIREDS);
        return 0;
    }
    bool on = atoi(argv[1]) != 0;
    int level = atoi(argv[2]);
    int mireds = argc > 3 ? atoi(argv[3]) : (int)LB_CT_WARM_MIREDS;
    if (level < 0) level = 0;
    if (level > 254) level = 254;
    /* A colour typed here is a manual choice just like a slider drag or a Matter
     * write, so drop out of sun-following to keep one source of truth. */
    if (argc > 3) {
        solar_policy_note_manual_ct();
    }
    evlog("cli", "set on=%d level=%d mireds=%d", on, level, mireds);
    lightbar_out_set(on, (uint8_t)level, (uint16_t)mireds); /* applies internally */
    {
        lb_diag_drive_t dd;
        lightbar_out_get_diag(&dd);
        if (dd.on) {
            lightbar_out_set_diag(NULL); /* explicit light command ends diagnostic drive */
        }
    }
    (void)lb_matter_sync_target();
    printf("set on=%d level=%d mireds=%d -> %s\n", on, level, mireds,
           safety_sm_gate_open() ? "applied" : "SUPPRESSED by safety gate");
    return 0;
}

static int cmd_out(int argc, char **argv)
{
    (void)argc; (void)argv;
    lb_target_t t;
    lightbar_out_get(&t);
    printf("target: on=%d level=%u mireds=%u  current_code=0x%02X  gate=%s\n",
           t.on, t.level, t.mireds, lightbar_out_current_code(), safety_sm_gate_open() ? "OPEN" : "CLOSED");
    return 0;
}

static int cmd_sens(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!tps_bus_sht41_present()) {
        printf("sht41: not ACKing at 0x%02X\n", LB_ADDR_SHT41);
        return 0;
    }
    float t = 0, h = 0;
    uint32_t sn = 0;
    uint8_t raw[6] = { 0 };
    size_t rx = 0;
    tps_bus_lock();
    esp_err_t e = sht41_measure(tps_bus_sht41(), &t, &h);
    esp_err_t e2 = sht41_serial_ex(tps_bus_sht41(), &sn, raw, sizeof(raw), &rx);
    tps_bus_unlock();
    if (e != ESP_OK) {
        printf("sht41: measure failed: %s\n", esp_err_to_name(e));
        return 0;
    }
    printf("sht4x: %.2f C  %.2f %%RH  serial=0x%08" PRIX32 " (%s rx=%u raw=%02X %02X %02X %02X %02X %02X)\n",
           t, h, sn, esp_err_to_name(e2), (unsigned)rx, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    return 0;
}

static int cmd_cur(int argc, char **argv)
{
    if (argc < 2) {
        printf("current ceiling level=0x%02X (code 0x%03X = %.1f mA, max 0x%02X, gate=%s)\n",
               lightbar_out_current_code(), (unsigned)TPS_CURR_CODE(lightbar_out_current_code()),
               160.08 * TPS_CURR_CODE(lightbar_out_current_code()) / 4095.0, LB_LED_CURRENT_MAX,
               safety_sm_gate_open() ? "OPEN" : "CLOSED");
        return 0;
    }
    uint8_t got = lightbar_out_set_current((uint8_t)strtoul(argv[1], NULL, 0));
    safety_sm_save_settings();
    evlog("cli", "current ceiling -> 0x%03X", (unsigned)TPS_CURR_CODE(got));
    printf("current ceiling -> 0x%02X (code 0x%03X = %.1f mA, max 0x%02X, gate=%s)\n", got,
           (unsigned)TPS_CURR_CODE(got), 160.08 * TPS_CURR_CODE(got) / 4095.0, LB_LED_CURRENT_MAX,
           safety_sm_gate_open() ? "OPEN" : "CLOSED");
    return 0;
}

static int cmd_rgb(int argc, char **argv)
{
    if (argc < 4) {
        printf("rgb strip %s (usage: rgb <0-255> <0-255> <0-255>)\n",
               rgb_status_enabled() ? "ENABLED" : "disabled in Kconfig");
        return 0;
    }
    bool ok = rgb_status_manual(atoi(argv[1]), atoi(argv[2]), atoi(argv[3]));
    printf("rgb -> %s\n", ok ? "sent" : "failed/disabled");
    return 0;
}

static int cmd_sun(int argc, char **argv)
{
    uint16_t day = (argc > 1) ? (uint16_t)atoi(argv[1]) : 172;
    float hour = (argc > 2) ? strtof(argv[2], NULL) : 12.0f;
    float lat = (float)CONFIG_LB_SUN_LAT / 10.0f;
    float el = lb_solar_elevation_deg(lat, day, hour);
    uint16_t m = lb_solar_mireds(el);
#ifdef CONFIG_LB_SUN_AUTO
    int sun_auto = 1;
#else
    int sun_auto = 0;
#endif
    printf("sun: lat=%.1f day=%u solar_hour=%.2f -> elev=%.1f deg  target=%u mireds (~%d K)  auto=%d\n",
           lat, day, hour, el, (unsigned)m, 1000000 / (int)m, sun_auto);
    return 0;
}

/* Full diagnostic dump: 00h BRT, 02h LED_CURR ceiling, the three fault
 * registers, FSM, PWM in/out, 1Ah LED current DAC (= 02h * 00h / 65535 in
 * current-dimming mode), 1Ch adaptive boost code, 1Eh auto-detect. */
static int cmd_diag(int argc, char **argv)
{
    static const uint8_t regs[] = { TPS_REG_BRT_CONTROL,     TPS_REG_LED_CURR_CONFIG,
                                    TPS_REG_SUPPLY_STATUS,   TPS_REG_BOOST_STATUS,
                                    TPS_REG_LED_STATUS,      TPS_REG_FSM_DIAGNOSTICS,
                                    TPS_REG_PWM_IN_DIAGNOSTICS, TPS_REG_PWM_OUT_DIAGNOSTICS,
                                    TPS_REG_LED_CURR_DIAGNOSTICS, TPS_REG_ADAPT_BOOST_DIAGNOSTICS,
                                    TPS_REG_AUTO_DETECT_DIAGNOSTICS };
    (void)argc; (void)argv;
    tps_bus_lock();
    for (int c = 0; c < tps_bus_chip_count(); c++) {
        tps_ctx_t *ch = tps_bus_chip(c);
        printf("%s diag:", ch->name);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            uint16_t v = 0;
            esp_err_t e = tps92390_read(ch->d100, regs[i], &v);
            if (e != ESP_OK) {
                printf(" %02Xh=ERR(%s)", regs[i], esp_err_to_name(e));
            } else {
                printf(" %02Xh=%04X", regs[i], v);
            }
        }
        printf("\n");
    }
    tps_bus_unlock();
    printf("legend: 00=BRT 02=LED_CURR_CONFIG 0e=SUP 10=BOOST 12=LED 14=FSM 16=PWM_IN 18=PWM_OUT "
           "1a=LED_CURR_DAC 1c=ADAPT_BOOST 1e=AUTO_DETECT\n");
    return 0;
}

static int cmd_time(int argc, char **argv)
{
    if (argc > 1) {
        long long v = strtoll(argv[1], NULL, 10);
        if (v <= 0) {
            printf("usage: time [epoch-seconds]\n");
            return 0;
        }
        lb_time_set_epoch((int64_t)v);
        printf("clock set to epoch %lld\n", v);
    }
    char s[48];
    lb_time_local_string(s, sizeof(s));
    int64_t ep = lb_time_now_epoch();
    uint16_t doy = 0;
    float sh = 0.0f;
    bool ok = false;
    if (lb_time_valid()) {
        ok = lb_time_solar_utc(ep, (float)CONFIG_LB_SUN_LON / 10.0f, &doy, &sh);
    }
    printf("clock: epoch=%lld src=%s local=%s\n", (long long)ep, lb_time_source_name(), s);
    if (ok) {
        printf("solar: doy=%u solar_hour=%.2f (lon %.1f)  auto=%d last=%u mireds\n", doy, sh,
               (float)CONFIG_LB_SUN_LON / 10.0f, solar_policy_auto(), (unsigned)solar_policy_last_mireds());
    } else {
        printf("solar: no usable clock (set with 'time <epoch>' or wait for SNTP after Wi-Fi)\n");
    }
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (argc > 1) {
        solar_policy_set_auto(atoi(argv[1]) != 0);
    }
    printf("auto CCT=%d  last_solar_mireds=%u  clock=%s\n", solar_policy_auto(),
           (unsigned)solar_policy_last_mireds(), lb_time_valid() ? "valid" : "unset");
    return 0;
}

/* Dump the config registers whose reset values are documented, next to the
 * diagnostic ones. The reset fingerprint (04=08A3 06=0100 08=2AAA 0A=A028
 * 0C=00AA) is what tells a real fault from a mis-aligned 16-bit access. */
static int cmd_regs(int argc, char **argv)
{
    static const uint8_t regs[] = { 0x04, 0x06, 0x08, 0x0A, 0x0C, 0x0E, 0x10, 0x12,
                                    0x14, 0x16, 0x18, 0x1A, 0x1C, 0x1E };
    (void)argc; (void)argv;
    tps_bus_lock();
    for (int c = 0; c < tps_bus_chip_count(); c++) {
        tps_ctx_t *ch = tps_bus_chip(c);
        printf("%s:", ch->name);
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
            uint16_t v = 0;
            esp_err_t e = tps92390_read(ch->d100, regs[i], &v);
            if (e != ESP_OK) {
                printf(" %02X=ERR", regs[i]);
            } else {
                printf(" %02X=%04X", regs[i], v);
            }
        }
        printf("\n");
    }
    printf("fingerprint: 04=08A3 06=0100 08=2AAA 0A=A028 0C=00AA (any other value = access problem)\n");
    tps_bus_unlock();
    return 0;
}

/* Write the documented reset value of BOOST_INT_EN and read it straight back.
 * A match proves our 16-bit access is aligned, so odd "reserved" bits in the
 * diagnostic registers are genuine chip content, not a shift artefact.
 * Also samples ADAPT_BOOST twice: a moving VBOOST means the boost is hunting. */
static int cmd_probe(int argc, char **argv)
{
    (void)argc; (void)argv;
    tps_bus_lock();
    for (int c = 0; c < tps_bus_chip_count(); c++) {
        tps_ctx_t *ch = tps_bus_chip(c);
        uint16_t before = 0, after = 0;
        tps92390_read(ch->d100, TPS_REG_BOOST_INT_EN, &before);
        /* Re-program the pairs that currently read "enabled" with the documented
         * 01/11 write codes; the read-back must reproduce the same 00/10 pattern. */
        esp_err_t pe = tps92390_set_int_en(ch->d100, TPS_REG_BOOST_INT_EN, tps92390_int_en_mask(before), &after);
        printf("%s 0A: was=%04X mask=%02X read=%04X %s\n", ch->name, before, tps92390_int_en_mask(before), after,
               (pe == ESP_OK && after == before) ? "MATCH" : "MISMATCH -> access alignment problem");
        uint16_t fsm = 0, v0 = 0, v1 = 0, v2 = 0;
        tps92390_read(ch->d100, TPS_REG_FSM_DIAGNOSTICS, &fsm);
        tps92390_read(ch->d100, TPS_REG_ADAPT_BOOST_DIAGNOSTICS, &v0);
        vTaskDelay(pdMS_TO_TICKS(150));
        tps92390_read(ch->d100, TPS_REG_ADAPT_BOOST_DIAGNOSTICS, &v1);
        vTaskDelay(pdMS_TO_TICKS(150));
        tps92390_read(ch->d100, TPS_REG_ADAPT_BOOST_DIAGNOSTICS, &v2);
        printf("%s fsm=%04X vboost=%04X/%04X/%04X %s\n", ch->name, fsm, v0, v1, v2,
               (v0 != v1 || v1 != v2) ? "(moving -> boost active)" : "(static)");
    }
    tps_bus_unlock();
    return 0;
}

/* Clear LED faults with the documented procedure (write 1 to BOTH the status
 * and its clear bit, 7.5.2.2). Bits 7..4 of LED_STATUS are read-only, so this
 * deliberately does NOT use 0xFFFF: on a read-only bit the write would be a
 * no-op at best and the datasheet says bit 5/4 must be written 0. */
static int cmd_ledclr(int argc, char **argv)
{
    const uint16_t mask = (1u << 10) | (1u << 9) |    /* LED open/short/gnd */
                          (1u << 12) | (1u << 11) |   /* INVSTRING */
                          (1u << 14) | (1u << 13);    /* I2C timeout */
    (void)argc; (void)argv;
    tps_bus_lock();
    for (int c = 0; c < tps_bus_chip_count(); c++) {
        tps_ctx_t *ch = tps_bus_chip(c);
        uint16_t st = 0xFFFF, rb = 0, fsm = 0;
        tps92390_read(ch->d100, TPS_REG_LED_STATUS, &st);
        tps92390_write(ch->d100, TPS_REG_LED_STATUS, mask);
        tps92390_read(ch->d100, TPS_REG_LED_STATUS, &rb);
        tps92390_read(ch->d100, TPS_REG_FSM_DIAGNOSTICS, &fsm);
        printf("%s LED_STATUS %04X -> write %04X -> %04X  fsm=%04X\n", ch->name, st, mask, rb, fsm);
    }
    tps_bus_unlock();
    return 0;
}

/* Local copy of the SHT4x CRC (poly 0x31, init 0xFF) so the CLI can validate
 * frames without exporting a helper from the sensor component. */
static uint8_t sht4x_crc8(const uint8_t *d, size_t n)
{
    uint8_t crc = 0xFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x31u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Serial-number probe: the SHT4x answers NACK to the read header whenever it is
 * "not ready to process a command" (datasheet Table 8 note), so the only way to
 * tell a timing problem from an unsupported command is to try the variants.
 * V1..V3 send the command as its own transaction and wait progressively longer
 * before reading; V4 uses a repeated start (no STOP in between). */
static int cmd_sn(int argc, char **argv)
{
    static const uint8_t delays_ms[] = { 0, 2, 10 };
    static const char *const vname[] = { "cmd,no-wait ", "cmd,+2ms    ", "cmd,+10ms   " };
    uint8_t raw[6];
    size_t rx = 0;
    uint32_t sn = 0;
    (void)argc; (void)argv;

    tps_bus_lock();
    for (unsigned v = 0; v < 3; v++) {
        sht41_reset(tps_bus_sht41());
        memset(raw, 0xEE, sizeof(raw));
        uint8_t c = SHT41_CMD_READ_SER_NUM;
        esp_err_t e = i2c_master_transmit(tps_bus_sht41(), &c, 1, 1000);
        if (e != ESP_OK) {
            printf("%s tx failed: %s\n", vname[v], esp_err_to_name(e));
            continue;
        }
        if (delays_ms[v]) {
            vTaskDelay(pdMS_TO_TICKS(delays_ms[v]));
        }
        memset(raw, 0, sizeof(raw));
        e = i2c_master_receive(tps_bus_sht41(), raw, 6, 1000);
        rx = (e == ESP_OK) ? 6 : 0;
        printf("%s rx=%u %-22s raw=%02X %02X %02X %02X %02X %02X\n", vname[v], (unsigned)rx,
               esp_err_to_name(e), raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
        if (e == ESP_OK) {
            uint8_t crc1 = sht4x_crc8(raw, 2), crc2 = sht4x_crc8(raw + 3, 2);
            printf("           crc: %02X/%02X (word1 %s) %02X/%02X (word2 %s)\n", crc1, raw[2],
                   crc1 == raw[2] ? "OK" : "BAD", crc2, raw[5], crc2 == raw[5] ? "OK" : "BAD");
        }
    }

    /* V4: repeated start in one transaction */
    sht41_reset(tps_bus_sht41());
    memset(raw, 0, sizeof(raw));
    esp_err_t e4 = sht41_serial_ex(tps_bus_sht41(), &sn, raw, sizeof(raw), &rx);
    printf("repeated-start    rx=%u %-22s raw=%02X %02X %02X %02X %02X %02X  sn=%08" PRIX32 "\n",
           (unsigned)rx, esp_err_to_name(e4), raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], sn);
    tps_bus_unlock();
    return 0;
}

/* Over-temperature control. `ot` shows the state; `ot <degC>` overrides the
 * trip point at runtime so the protection path can be verified without heating
 * the board; `ot 0` restores the Kconfig value. */
static int cmd_ot(int argc, char **argv)
{
    if (argc > 1) {
        s_ot_trip_override = atoi(argv[1]);
        if (s_ot_trip_override < 0) {
            s_ot_trip_override = 0;
        }
        printf("overtemp trip override = %d C (0 = Kconfig default %d C)\n", s_ot_trip_override,
               CONFIG_LB_OT_TRIP_C);
        /* Re-evaluate immediately so a lowered threshold acts now, not in 1 s.
         * (The state machine still applies the result on its next cycle.) */
        s_ot_clear_cnt = 0;
        thermal_poll();
    }
    printf("board=%.2f C valid=%d  latched=%d  trip=%d C  clear=%d C\n", s_board_temp,
           (int)s_board_temp_valid, (int)s_ot_latched, ot_trip_c(), ot_clear_c());
    printf("gate=%s  state=%s\n", safety_sm_gate_open() ? "OPEN" : "CLOSED", SM_NAME[s_state]);
    return 0;
}

static int cmd_ver(int argc, char **argv)
{
    (void)argc; (void)argv;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run != NULL) {
        (void)esp_ota_get_state_partition(run, &st);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    printf("app_version=%s  idf=%s\n", lb_ota_version(), esp_app_get_description()->idf_ver);
    printf("running=%s @0x%06" PRIx32 "  state=%d  pending_verify=%d\n", run ? run->label : "?",
           run ? run->address : 0, (int)st, (int)lb_ota_pending_verify());
    printf("next_slot=%s @0x%06" PRIx32 "  size=%" PRIu32 "  free_heap=%" PRIu32 "\n",
           next ? next->label : "?", next ? next->address : 0, next ? (uint32_t)next->size : 0,
           (uint32_t)esp_get_free_heap_size());
    return 0;
}

/* OTA over HTTP(S). Runs on its own 8 KB task: the console task has 4 KB of
 * stack, which the HTTP client (and certainly a TLS handshake) would overflow. */
static char s_ota_url[LB_OTA_URL_MAX];

static void ota_worker(void *arg)
{
    (void)arg;
    char err[128] = {0};
    esp_err_t e = lb_ota_from_url(s_ota_url, err, sizeof(err));
    printf("OTA failed: %s (%s)\nlb> ", esp_err_to_name(e), err);
    fflush(stdout);
    vTaskDelete(NULL);
}

static int cmd_ota(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ota <url>   (current version %s, busy=%d)\n", lb_ota_version(),
               (int)lb_ota_busy());
        printf("  e.g. ota http://192.168.1.10:8000/lightbar.bin\n");
        return 0;
    }
    if (lb_ota_busy()) {
        printf("OTA already in progress\n");
        return 0;
    }
    snprintf(s_ota_url, sizeof(s_ota_url), "%s", argv[1]);
    printf("OTA: %s (background task)\n", s_ota_url);
    if (xTaskCreate(ota_worker, "ota", 8192, NULL, 4, NULL) != pdPASS) {
        printf("OTA: cannot create worker task\n");
    }
    return 0;
}

static void print_ip(void)
{
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (nif == NULL) {
        printf("netif: none\n");
        return;
    }
    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK) {
        printf("netif: ip info unavailable\n");
        return;
    }
    printf("ip=" IPSTR "  gw=" IPSTR "\n", IP2STR(&ip.ip), IP2STR(&ip.gw));
}

/* Bench-only Wi-Fi provisioning: join a network without going through the phone
 * commissioning flow, so an OTA can be tested over the LAN. The Matter stack
 * keeps owning the station; this simply fills in the credentials it would have
 * received over BLE. */
static int cmd_wifi(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: wifi <ssid> <password>    (current:)\n");
        print_ip();
        return 0;
    }
    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy((char *)cfg.sta.ssid, argv[1], sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, argv[2], sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_err_t e = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (e != ESP_OK) {
        printf("wifi set_config: %s\n", esp_err_to_name(e));
        return 0;
    }
    e = esp_wifi_connect();
    printf("connecting to \"%s\": %s\n", argv[1], esp_err_to_name(e));
    for (int i = 0; i < 15; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_netif_t *nif = esp_netif_get_default_netif();
        esp_netif_ip_info_t ip;
        if (nif != NULL && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            print_ip();
            return 0;
        }
    }
    printf("no IP after 15 s\n");
    return 0;
}

/* Ask the device's own web server for a page over its AP interface. Proves the
 * HTTP stack and the JSON handlers work without needing another machine on the
 * network (and works while the device is stuck in provisioning mode). */
static int cmd_webtest(int argc, char **argv)
{
    bool post = (argc > 1 && !strcmp(argv[1], "post"));
    const char *path = post ? ((argc > 2) ? argv[2] : "/api/status") : ((argc > 1) ? argv[1] : "/api/status");
    const char *body = post ? ((argc > 3) ? argv[3] : "") : NULL;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    char ip[16] = "192.168.4.1";
    if (ap != NULL) {
        esp_netif_ip_info_t info;
        memset(&info, 0, sizeof(info));
        if (esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    char url[128];
    snprintf(url, sizeof(url), "http://%s%s", ip, path);
    printf("webtest: %s %s%s\n", post ? "POST" : "GET", url, post ? " (form body)" : "");

    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000, .buffer_size = 1024 };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        printf("webtest: client init failed\n");
        return 0;
    }
    if (post) {
        esp_http_client_set_method(cli, HTTP_METHOD_POST);
        esp_http_client_set_header(cli, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(cli, body, (int)strlen(body));
        printf("webtest: body=\"%s\"\n", body);
    }
    esp_err_t e = esp_http_client_open(cli, post ? (int)strlen(body) : 0);
    if (e != ESP_OK) {
        printf("webtest: connect failed: %s\n", esp_err_to_name(e));
        esp_http_client_cleanup(cli);
        return 0;
    }
    if (post) {
        esp_http_client_write(cli, body, (int)strlen(body));
    }
    int len = esp_http_client_fetch_headers(cli);
    int status = esp_http_client_get_status_code(cli);
    printf("webtest: HTTP %d, content-length=%d\n", status, len);
    char buf[513];
    int n = esp_http_client_read(cli, buf, 512);
    if (n > 0) {
        buf[n] = '\0';
        printf("webtest: body[%d]: %s\n", n, buf);
    } else {
        printf("webtest: empty body (%d)\n", n);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return 0;
}

static int cmd_prov(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "clear")) {
        esp_err_t e = wifi_prov_forget();
        printf("prov: credentials erased, AP=%s (%s)\n", wifi_prov_ap_ssid(), esp_err_to_name(e));
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "ap")) {
        esp_err_t e = wifi_prov_start_ap();
        printf("prov: AP %s (%s)\n", wifi_prov_ap_ssid(), esp_err_to_name(e));
        return 0;
    }
    char ip[16] = {0};
    bool has_ip = wifi_prov_ip(ip, sizeof(ip));
    printf("prov: state=%s ap=%d ssid=\"%s\" ip=%s\n", wifi_prov_state_name(),
           (int)wifi_prov_ap_active(), wifi_prov_ap_ssid(), has_ip ? ip : "none");
    printf("prov: 'prov ap' force the setup hotspot, 'prov clear' erase stored Wi-Fi\n");
    return 0;
}

static void print_tuning(void)
{
    static const char *const slope_ms[] = { "off", "1ms", "2ms", "50ms", "100ms", "200ms", "300ms", "500ms" };
    static const char *const ss_rng[] = { "3.3%", "4.3%", "5.3%", "7.2%" };
    static const char *const ss_mod[] = { "200Hz", "500Hz", "800Hz", "1.2kHz" };
    lb_tuning_t t;
    lightbar_out_get_tuning(&t);
    printf("tuning: slope=%d (%s) dither=%d  spread: range=%d (%s) mod=%d (%s) pseudo=%d  UC1=0x%04X\n",
           t.slope, slope_ms[t.slope & 7], t.dither, t.ss_range, ss_rng[t.ss_range & 3], t.ss_mod,
           ss_mod[t.ss_mod & 3], t.ss_pseudo, (unsigned)lightbar_out_expected_uc1());
    printf("        (spread spectrum is enabled by hardware on PCB3_3: BST_SYNC=VDD33)\n");
}

/* slope <0-7> [dither 0-4] : brightness sloper / dither (Table 7-16) */
static int cmd_slope(int argc, char **argv)
{
    if (argc > 1) {
        lb_tuning_t t = { .slope = (int8_t)atoi(argv[1]), .dither = (int8_t)(argc > 2 ? atoi(argv[2]) : -1),
                          .ss_range = -1, .ss_mod = -1, .ss_pseudo = -1 };
        esp_err_t e = lightbar_out_set_tuning(&t);
        safety_sm_save_settings();
        printf("slope -> %s\n", esp_err_to_name(e));
    }
    print_tuning();
    return 0;
}

/* ss <range 0-3> <mod 0-3> [pseudo 0|1] : boost spread-spectrum shaping */
static int cmd_ss(int argc, char **argv)
{
    if (argc > 2) {
        lb_tuning_t t = { .slope = -1, .dither = -1, .ss_range = (int8_t)atoi(argv[1]),
                          .ss_mod = (int8_t)atoi(argv[2]), .ss_pseudo = (int8_t)(argc > 3 ? atoi(argv[3]) : -1) };
        esp_err_t e = lightbar_out_set_tuning(&t);
        safety_sm_save_settings();
        printf("spread -> %s\n", esp_err_to_name(e));
    } else if (argc > 1) {
        printf("usage: ss <range 0-3> <mod 0-3> [pseudo 0|1]\n");
    }
    print_tuning();
    return 0;
}

/* factory confirm : erase Matter fabrics + Wi-Fi provision + our cap, reboot */
static int cmd_factory(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "confirm") != 0) {
        printf("usage: factory confirm   (erases Matter fabrics, Wi-Fi credentials and the brightness cap; "
               "keeps the flight recorder; reboots into the provisioning AP)\n");
        return 0;
    }
    evlog("cli", "FACTORY RESET via CLI");
    printf("factory reset in 1 s...\n");
    fflush(stdout);
    esp_err_t e = safety_sm_factory_reset();
    printf("factory reset: %s\n", esp_err_to_name(e));
    return 0;
}

/* dd <0|1> [warm%] [cold%] : diagnostic per-channel drive */
static int cmd_dd(int argc, char **argv)
{
    lb_diag_drive_t d;
    lightbar_out_get_diag(&d);
    if (argc > 1) {
        d.on = atoi(argv[1]) != 0;
        if (argc > 2) { int x = atoi(argv[2]); d.warm_pct = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x)); }
        if (argc > 3) { int x = atoi(argv[3]); d.cold_pct = (uint8_t)(x < 0 ? 0 : (x > 100 ? 100 : x)); }
        evlog("cli", "diagdrive on=%d warm=%u cold=%u", d.on, d.warm_pct, d.cold_pct);
        lightbar_out_set_diag(&d);
        lightbar_out_get_diag(&d);
    }
    printf("diag drive: %s  warm=%u%%  cold=%u%%  (gate=%s)\n", d.on ? "ON" : "off", d.warm_pct, d.cold_pct,
           safety_sm_gate_open() ? "OPEN" : "CLOSED");
    return 0;
}

static int cmd_help(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("commands: st | hist | clr | m1 | diag | regs | probe | ledclr | sn | ot [degC] | ota <url> | "
           "ver | wifi <ssid> <pass> | ip | prov [ap|clear] | webtest [post] [path] [body] | out | "
           "set <0|1> <0-254> [mireds] | cur [hex] | slope <0-7> [dither] | ss <range> <mod> [pseudo] | "
           "dd <0|1> [warm%%] [cold%%] | factory confirm | sens | rgb <r> <g> <b> | sun [day] [hour] | time [epoch] | auto <0|1> | help\n");
    return 0;
}

esp_err_t safety_sm_factory_reset(void)
{
    /* Light off first; the drivers keep running on VBUS through the reboot. */
    lightbar_out_inhibit(true);
    lightbar_out_set_level_cap(254);
    nvs_handle_t h;
    if (nvs_open("lb", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "lcap");
        nvs_erase_key(h, "tune");
        nvs_erase_key(h, "lt_on");
        nvs_erase_key(h, "lt_lv");
        nvs_erase_key(h, "lt_mi");
        nvs_erase_key(h, "cur");
        nvs_commit(h);
        nvs_close(h);
    }
    /* esp_matter::factory_reset(): erases the Matter fabrics / credentials and
     * the CHIP config namespace, restores the Wi-Fi station provision to
     * defaults and restarts the chip. Scheduled on the CHIP task. */
    return lb_matter_factory_reset();
}

static void console_task(void *arg)
{
    (void)arg;
    char line[96];
    printf("lb> ");
    fflush(stdout);
    while (true) {
        if (!fgets(line, sizeof(line), stdin)) {
            /* stdin EOF on host disconnect/reconnect: back off and retry.
             * The task must never return (FreeRTOS aborts otherwise). */
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        char *argv[4];
        char *save = NULL;
        int argc = 0;
        for (char *tok = strtok_r(line, " \t", &save); tok != NULL && argc < 4;
             tok = strtok_r(NULL, " \t", &save)) {
            argv[argc++] = tok;
        }
        if (argc == 0) { printf("lb> "); fflush(stdout); continue; }
        if (!strcmp(argv[0], "st"))        cmd_st(argc, argv);
        else if (!strcmp(argv[0], "hist")) cmd_hist(argc, argv);
        else if (!strcmp(argv[0], "clr"))  cmd_clr(argc, argv);
        else if (!strcmp(argv[0], "m1"))   cmd_m1(argc, argv);
        else if (!strcmp(argv[0], "out"))  cmd_out(argc, argv);
        else if (!strcmp(argv[0], "set"))  cmd_set(argc, argv);
        else if (!strcmp(argv[0], "cur"))  cmd_cur(argc, argv);
        else if (!strcmp(argv[0], "slope")) cmd_slope(argc, argv);
        else if (!strcmp(argv[0], "ss"))   cmd_ss(argc, argv);
        else if (!strcmp(argv[0], "factory")) cmd_factory(argc, argv);
        else if (!strcmp(argv[0], "dd"))   cmd_dd(argc, argv);
        else if (!strcmp(argv[0], "sens")) cmd_sens(argc, argv);
        else if (!strcmp(argv[0], "rgb"))  cmd_rgb(argc, argv);
        else if (!strcmp(argv[0], "sun"))  cmd_sun(argc, argv);
        else if (!strcmp(argv[0], "diag")) cmd_diag(argc, argv);
        else if (!strcmp(argv[0], "regs")) cmd_regs(argc, argv);
        else if (!strcmp(argv[0], "probe")) cmd_probe(argc, argv);
        else if (!strcmp(argv[0], "ledclr")) cmd_ledclr(argc, argv);
        else if (!strcmp(argv[0], "sn"))   cmd_sn(argc, argv);
        else if (!strcmp(argv[0], "ot"))   cmd_ot(argc, argv);
        else if (!strcmp(argv[0], "ota"))  cmd_ota(argc, argv);
        else if (!strcmp(argv[0], "ver"))  cmd_ver(argc, argv);
        else if (!strcmp(argv[0], "wifi")) cmd_wifi(argc, argv);
        else if (!strcmp(argv[0], "webtest")) cmd_webtest(argc, argv);
        else if (!strcmp(argv[0], "prov")) cmd_prov(argc, argv);
        else if (!strcmp(argv[0], "ip"))   { print_ip(); }
        else if (!strcmp(argv[0], "time")) cmd_time(argc, argv);
        else if (!strcmp(argv[0], "auto")) cmd_auto(argc, argv);
        else                               cmd_help(argc, argv);
        printf("lb> ");
        fflush(stdout);
    }
}

void safety_sm_start(void)
{
    /* EN/INT GPIO: NOT touched on PCB3_3 (EN hardwired, EN/INT MCU pins not
     * validated against the old netlist). New PCB enables gpio_safety_init(). */
    xTaskCreate(monitor_task, "lmon", 4096, NULL, 5, NULL);
    xTaskCreate(console_task, "lcli", 4096, NULL, 4, NULL);
}
