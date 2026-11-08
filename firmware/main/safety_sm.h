// safety_sm.h - M2: NVS flight recorder + fault-monitor state machine + CLI.
//
// Honest scope on PCB3_3: TPS EN is hardwired to VDD33, so the chip is
// always hardware-armed whenever VBUS is present. Software cannot disconnect
// outputs on this revision. "ARM_OK" therefore means "software gate open:
// we may apply brightness/current writes", and VBUS removal remains the
// physical kill switch until the new PCB wires EN to the MCU.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

void safety_sm_recorder_boot(void); /* records raw power-on state, then forces DISPLAY_BRT=0 */
void safety_sm_start(void);         /* monitor + CLI tasks */

/* Persist the settings the state machine owns in NVS (boot counter, flight
 * recorder, brightness cap). Call after changing the cap at runtime. */
void safety_sm_save_settings(void);

/* Re-apply the light target saved in NVS (on/level/mireds). Call once after
 * the Matter node exists; it also syncs the Matter attributes. */
void safety_sm_restore_light_target(void);

/* Per-chip LED string fault mask (bit s = string s+1 disabled) and kind
 * (b0 open, b1 short, b2 short-to-GND). idx 0 = warm, 1 = cold. */
uint8_t safety_sm_string_faults(int idx, uint8_t *kind);

/* Chip idx is wanted on but has not reached FSM NORMAL for >= 3 s (fault-recovery
 * restart loop). Cleared automatically when it reaches NORMAL again. */
bool safety_sm_chip_stuck(int idx);

/* Power-cycle the drivers via EN to re-enable reconnected strings (new PCB only;
 * returns ESP_ERR_NOT_SUPPORTED on PCB3_3 where EN is hardwired). */
esp_err_t safety_sm_reprobe_strings(void);

/* Factory reset: light off, brightness cap + tuning erased, then Matter
 * fabrics / Wi-Fi provision erased and the chip restarts (via esp_matter).
 * The flight recorder and boot counter are kept. Returns once scheduled. */
esp_err_t safety_sm_factory_reset(void);

/* Software output gate: true only in ARM_OK (see header notes). */
bool safety_sm_gate_open(void);

/* Snapshot for telemetry / status strip (stable bit positions). */
enum {
    LB_ST_ARMED = 1u << 0,    /* state machine in ARM_OK (gate open) */
    LB_ST_CRCERR = 1u << 1,   /* boot CRCERR latch */
    LB_ST_CRIT_NOW = 1u << 2, /* critical fault bits live in the last monitor sample */
    LB_ST_NO_CHIPS = 1u << 3, /* neither TPS92390 ACKs */
    LB_ST_OVERTEMP = 1u << 4, /* board over-temperature latch: outputs forced off */
    LB_ST_STRING_FAULT = 1u << 5, /* a string disabled by the driver, or a chip stuck in fault recovery */
};
uint32_t safety_sm_status_bits(void);

/* Board temperature from the SHT40 next to the drivers (degC).
 * valid=false until the first successful measurement. */
float safety_sm_board_temp(bool *valid);

/* True while the over-temperature latch is active (both outputs forced off). */
bool safety_sm_overtemp(void);

/* Human-readable state machine name ("ARM_OK", "DISARMED", ...). */
const char *safety_sm_state_name(void);

/* Structured snapshot for the web console / remote debugging (GET /api/diag). */
#define LB_DIAG_HIST 8
typedef struct {
    uint32_t boots;
    uint32_t crit_events;
    bool boot_crcerr;
    uint16_t sup[2], bst[2], led[2];   /* last sampled status registers */
    int hist_n;                        /* newest last, up to LB_DIAG_HIST */
    struct {
        const char *reason;
        uint16_t flags;
        uint16_t sup[2], bst[2], led[2];
    } hist[LB_DIAG_HIST];
} lb_diag_t;

void safety_sm_diag(lb_diag_t *out);

/* Cached 0x14 FSM code of chip idx (0 = warm/0x3A, 1 = cold/0x3B).
 * 0x10 = normal, 0x12 = fault recovery, 0x13 = ALL_LED_FAULT latched.
 * The latched state forces every output off and only a real VBUS power cycle
 * clears it (EN is tied to VDD33 on this board), so the UI must say so. */
uint16_t safety_sm_fsm(int idx);

/* Raw 1Eh AUTO_DETECT read at boot (strap resistors -> mode/address, string
 * count, boost and PWM frequency). 0 if the chip did not answer. */
uint16_t safety_sm_autodetect(int idx);
