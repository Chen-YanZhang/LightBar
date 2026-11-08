// lightbar_out.h - the output stage: stores the desired light target
// (on / level 0..254 / CCT mireds) and applies it to the two TPS92390
// channels through the safety gate. Matter endpoints feed this; nothing
// else may touch DISPLAY_BRT / LED_CURR_CONFIG / USER_CONFIG1.
//
// Dimming path (datasheet-checked, see tps92390.h): the parts run in Current
// Dimming Mode, so DISPLAY_BRT is already analog (it scales the 12-bit current
// DAC through the 200 ms sloper). Brightness therefore ALWAYS goes through
// DISPLAY_BRT with BRT_MODE=2; LED_CURR_CONFIG only holds the per-string
// current ceiling and is written once per chip configuration.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_pins.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 8-bit "level" view of the 12-bit LED_CURR_CONFIG code (code = level << 4). */
#define LB_LED_CURRENT_LEVEL ((uint8_t)(LB_CODE_DEFAULT >> 4))  /* 0xD0 -> 0xD00 = 130.1 mA */
#define LB_LED_CURRENT_MAX   ((uint8_t)(LB_CODE_MAX >> 4))      /* 0xF0 -> 0xF00 = 150.1 mA (string rating) */

typedef struct {
    bool on;
    uint8_t level;  /* 0..254, Matter LevelControl semantics */
    uint16_t mireds;
} lb_target_t;

void lightbar_out_set(bool on, uint8_t level, uint16_t mireds);
void lightbar_out_get(lb_target_t *out);

/* Push stored target to hardware if the safety gate is open.
 * Returns ESP_ERR_INVALID_STATE when suppressed by the gate/inhibit, otherwise
 * the first hardware error (ESP_OK when every write landed). */
esp_err_t lightbar_out_apply(void);

/* Force DISPLAY_BRT=0 on both channels regardless of the target.
 * Called by the safety monitor whenever the gate closes. */
void lightbar_out_emergency_off(void);

/* LED current ceiling currently programmed, as 8-bit level (code >> 4). */
uint8_t lightbar_out_current_code(void);
/* Change the ceiling. Clamped to LB_LED_CURRENT_MAX; returns the level
 * actually stored. Re-applies immediately if the gate is open. */
uint8_t lightbar_out_set_current(uint8_t level);

/* Brightness ceiling (level units). 254 = no limit. Persisted by safety_sm. */
void lightbar_out_set_level_cap(uint8_t cap);
uint8_t lightbar_out_level_cap(void);

/* Hard inhibit: while set, apply() is a no-op so nothing can light the LEDs.
 * Used by OTA. Turning it on also forces DISPLAY_BRT=0 right away. */
void lightbar_out_inhibit(bool on);

/* The chip lost its configuration (POR / brownout / selftest touched it): the
 * next apply() rewrites USER_CONFIG1 + LED_CURR_CONFIG before DISPLAY_BRT.
 * idx 0 = warm, 1 = cold, -1 = both. */
void lightbar_out_note_chip_reset(int idx);

/* USER_CONFIG1 value the output stage expects on a configured chip; the
 * monitor compares against it to detect a silent chip reset. */
uint16_t lightbar_out_expected_uc1(void);

/* Acoustic / transition tuning of USER_CONFIG1 (Table 7-16). All fields are
 * the raw register codes; -1 leaves a field unchanged.
 *   slope      0=off 1=1ms 2=2ms 3=50ms 4=100ms 5=200ms(rst) 6=300ms 7=500ms
 *   dither     0=off .. 4=4-bit
 *   ss_range   boost spread-spectrum range 0=3.3% 1=4.3% 2=5.3%(rst) 3=7.2%
 *   ss_mod     modulation frequency 0=200Hz(rst) 1=500Hz 2=800Hz 3=1.2kHz
 *   ss_pseudo  1 = pseudo-random modulation instead of a fixed tone
 * Spread spectrum itself is enabled by hardware on PCB3_3 (BST_SYNC=VDD33),
 * so these only shape it. Written live to both chips (no brightness glitch). */
typedef struct {
    int8_t slope, dither, ss_range, ss_mod, ss_pseudo;
} lb_tuning_t;
esp_err_t lightbar_out_set_tuning(const lb_tuning_t *t);
void lightbar_out_get_tuning(lb_tuning_t *out);

/* Diagnostic drive: bypass the CCT mixer and set each channel's DISPLAY_BRT
 * directly in percent, so both chips can run at 100 % together (thermal and
 * efficiency tests). Still behind the safety gate and inhibit. Volatile: a
 * reboot or on=false returns to the normal CCT target, which is kept intact. */
typedef struct {
    bool on;
    uint8_t warm_pct, cold_pct; /* 0..100 */
} lb_diag_drive_t;
void lightbar_out_set_diag(const lb_diag_drive_t *d); /* NULL = off */
void lightbar_out_get_diag(lb_diag_drive_t *out);

/* True when the output stage currently wants channel idx (0 warm, 1 cold) to
 * emit light (last DISPLAY_BRT written > 0) - used by the monitor to tell "chip is
 * stuck in fault recovery" from "chip is simply off". */
bool lightbar_out_channel_requested(int idx);

#ifdef __cplusplus
}
#endif
