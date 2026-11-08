/* tps92390.h - TPS92390 6-channel boost LED driver (TI SNVSBY7) register driver.
 *
 * I2C protocol (DS 7.5.1): a write is 4 bytes on the wire
 *   [addr+W][reg8][data byte 0][data byte 1]
 * and a read is a 5-byte combined transaction
 *   [addr+W][reg8][Sr][addr+R][data byte 0][data byte 1].
 * One 16-bit register per transaction, even register addresses only (Table
 * 7-12; odd offsets are reserved and must not be touched).
 *
 * BYTE ORDER: the part puts the LOW byte first in both directions. The
 * datasheet does not spell this out; it was established on hardware (M4c) and
 * is self-consistent with every documented reset value (06h=0100h, 08h=2AAAh,
 * 0Ah=A028h, 0Ch=00AAh) and with the field-by-field decode of 1Eh
 * (AUTO_DETECT: PWM 19531 Hz / 3 strings / 1818 kHz / constant-current mode,
 * all matching the fitted strap resistors). Both accessors swap, so callers
 * only ever see datasheet values.
 *
 * 7-bit address 0x3A / 0x3B is set by the MODE resistor (Table 7-7):
 *   42.2k -> current dimming, 0x3A (warm)     5.76k -> current dimming, 0x3B (cold)
 * A missing MODE resistor makes the part default to phase-shift PWM @0x3A and
 * raise MODESEL_STATUS.
 *
 * DIMMING MODE ON OUR BOARDS: "Current Dimming Mode" = hybrid mode with the
 * PWM/current threshold at 0 % (DS 7.3.8.5), i.e. pure analog dimming. The
 * DISPLAY_BRT path (sloper -> dither -> current multiplier, Fig 7-9) ends in
 * the 12-bit current DAC; there is no 19.5 kHz LED PWM and PWM_OUTPUT_STATUS
 * (18h) stays FFFFh. So brightness MUST go through DISPLAY_BRT (BRT_MODE=2) to
 * keep the 200 ms sloper, and LED_CURR_CONFIG is the per-string current
 * ceiling written once (verified on hardware: 1Ah = LED_CURRENT * BRT/65535).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- register map (Table 7-12) ---------------- */
#define TPS_REG_BRT_CONTROL              0x00 /* [15:0] DISPLAY_BRT              rst 0x0000 */
#define TPS_REG_LED_CURR_CONFIG          0x02 /* [11:0] LED_CURRENT (all outs)   rst 0x0FFF */
#define TPS_REG_USER_CONFIG1             0x04 /*                                 rst 0x08A3 */
#define TPS_REG_USER_CONFIG2             0x06 /*                                 rst 0x0100 */
#define TPS_REG_SUPPLY_INT_EN            0x08 /*                                 rst 0x2AAA */
#define TPS_REG_BOOST_INT_EN             0x0A /*                                 rst 0xA028 */
#define TPS_REG_LED_INT_EN               0x0C /*                                 rst 0x00AA */
#define TPS_REG_SUPPLY_STATUS            0x0E
#define TPS_REG_BOOST_STATUS             0x10
#define TPS_REG_LED_STATUS               0x12
#define TPS_REG_FSM_DIAGNOSTICS          0x14
#define TPS_REG_PWM_IN_DIAGNOSTICS       0x16
#define TPS_REG_PWM_OUT_DIAGNOSTICS      0x18
#define TPS_REG_LED_CURR_DIAGNOSTICS     0x1A
#define TPS_REG_ADAPT_BOOST_DIAGNOSTICS  0x1C
#define TPS_REG_AUTO_DETECT_DIAGNOSTICS  0x1E

/* POR values of the configuration block (7.6.1.1 .. 7.6.1.7). The four
 * INT_EN/UC2 values double as the "access alignment fingerprint": they are
 * never written by the application, so reading anything else means the 16-bit
 * access is broken, not that the chip is faulty. */
#define TPS_RST_BRT_CONTROL      0x0000
#define TPS_RST_LED_CURR_CONFIG  0x0FFF
#define TPS_RST_USER_CONFIG1     0x08A3
#define TPS_RST_USER_CONFIG2     0x0100
#define TPS_RST_SUPPLY_INT_EN    0x2AAA
#define TPS_RST_BOOST_INT_EN     0xA028
#define TPS_RST_LED_INT_EN       0x00AA

/* USER_CONFIG1 fields (Table 7-16) */
#define TPS_UC1_SPREAD_PSEUDO_EN   (1u << 14)
#define TPS_UC1_SPREAD_MOD_FREQ(x) (((x) & 3u) << 12)   /* 0=200Hz 1=500Hz 2=800Hz 3=1.2kHz */
#define TPS_UC1_SPREAD_RANGE(x)    (((x) & 3u) << 10)   /* 0=3.3% 1=4.3% 2=5.3%(rst) 3=7.2% */
#define TPS_UC1_BRT_MODE(x)        (((x) & 3u) << 8)    /* 0=PWM pin (rst), 2=DISPLAY_BRT; 1/3 reserved */
#define TPS_UC1_SLOPE_SELECT(x)    (((x) & 7u) << 5)    /* 0=off 1=1ms 2=2ms 3=50ms 4=100ms 5=200ms(rst) 6=300ms 7=500ms */
#define TPS_UC1_DITHER_SELECT(x)   ((((x) > 4u ? 4u : (x)) & 7u) << 2) /* 0=off(rst) .. 4=4-bit; 5..7 undefined */
#define TPS_UC1_ADV_SLOPE_EN       (1u << 1)
#define TPS_UC1_RESERVED0          (1u << 0)             /* reserved, reset 1 - keep as read */
#define TPS_UC1_BRT_MODE_MASK      (3u << 8)

/* USER_CONFIG2 fields (Table 7-17); bits 7..4 "must write 0" */
#define TPS_UC2_EN_LED_GND_DETECT  (1u << 8)
#define TPS_UC2_LEDx_SHORT_DISABLE(n) (1u << ((n) - 1))  /* n = 1..4 */

/* SUPPLY_STATUS (Table 7-21) - STATUS bits; the CLEAR bit of each pair is one
 * position below. Same layout as SUPPLY_INT_EN pairs. */
#define TPS_SS_CRCERR   (1u << 15)
#define TPS_SS_BSTSYNC  (1u << 13)
#define TPS_SS_CP       (1u << 11)
#define TPS_SS_CPCAP    (1u << 9)
#define TPS_SS_VINOCP   (1u << 7)
#define TPS_SS_VDDUVLO  (1u << 5)
#define TPS_SS_VINOVP   (1u << 3)
#define TPS_SS_VINUVLO  (1u << 1)
#define TPS_SS_STATUS_MASK 0xAAAAu

/* BOOST_STATUS (Table 7-22) */
#define TPS_BS_TSD      (1u << 15)
#define TPS_BS_ISET     (1u << 13)
#define TPS_BS_LEDSET   (1u << 11)
#define TPS_BS_MODESEL  (1u << 9)
#define TPS_BS_FSET     (1u << 7)
#define TPS_BS_BSTOCP   (1u << 5)
#define TPS_BS_BSTOVPH  (1u << 3)
#define TPS_BS_BSTOVPL  (1u << 1)
#define TPS_BS_STATUS_MASK 0xAAAAu

/* LED_STATUS (Table 7-23). Only bits 14..9 are STATUS/CLEAR pairs; 8..0 are
 * read-only and bits 5/4 must be written 0. */
#define TPS_LS_I2C_ERROR  (1u << 14)
#define TPS_LS_INVSTRING  (1u << 12)
#define TPS_LS_LED        (1u << 10)   /* open / internal short / short-to-GND summary */
#define TPS_LS_GND_LED    (1u << 8)    /* RO */
#define TPS_LS_SHORT_LED  (1u << 7)    /* RO */
#define TPS_LS_OPEN_LED   (1u << 6)    /* RO */
#define TPS_LS_LEDn_FAULT(n) (1u << ((n) - 1)) /* RO, n = 1..4 */
#define TPS_LS_STATUS_MASK 0x5400u
#define TPS_LS_CLEARABLE_MASK 0x7E00u  /* STATUS|CLEAR pairs 14..9 */

/* INT_EN pair indices (bit pair p occupies bits [2p+1:2p]) */
#define TPS_INT_PAIR(bit_hi)   ((bit_hi) >> 1)        /* e.g. TPS_INT_PAIR(7) = VINOCP */
#define TPS_INT_RD_ENABLED(v, p) ((((v) >> ((p) * 2)) & 3u) == 2u)

/* FSM_DIAGNOSTICS[4:0] (Table 7-24) */
#define TPS_FSM_MASK           0x1Fu
#define TPS_FSM_DISABLED       0x00
#define TPS_FSM_LDO_STARTUP    0x01
#define TPS_FSM_OTP_READ       0x02
#define TPS_FSM_STANDBY        0x03   /* first state in which I2C is accessible (7.4.4) */
#define TPS_FSM_BOOST_STARTUP_MIN 0x04
#define TPS_FSM_BOOST_STARTUP_MAX 0x0F
#define TPS_FSM_NORMAL         0x10
#define TPS_FSM_SHUTDOWN       0x11
#define TPS_FSM_FAULT_RECOVERY 0x12
#define TPS_FSM_ALL_LED_FAULT  0x13   /* latch: only EN low exits (7.4.9) */

/* AUTO_DETECT_DIAGNOSTICS (Table 7-27) decoded view */
typedef struct {
    uint8_t pwm_freq_sel;   /* [14:12] 0=152Hz .. 7=19531Hz */
    uint8_t string_cfg;     /* [10:8]  0=4 strings 1=3 2=2 3=2x2 4=1 */
    uint8_t boost_freq_sel; /* [5:3]   0=100k 1=200k 2=303k 3=400k 4=500k 5=1818k 6=2000k 7=2222k */
    uint8_t mode_sel;       /* [2:0]   0..3 -> 0x3B, 4..7 -> 0x3A; 2/6 = constant current */
    uint8_t i2c_addr;       /* derived from mode_sel */
    bool    current_dimming;/* mode_sel == 2 || 6 */
} tps_autodetect_t;

typedef struct {
    uint16_t uc2, sup_int_en, bst_int_en, led_int_en; /* raw fingerprint registers */
    bool fingerprint_ok;    /* all four equal their documented reset values */
    uint16_t autodetect_raw;
    tps_autodetect_t ad;
} tps_identity_t;

/* Error codes: keep the I2C layer's own codes untouched and use these two for
 * chip-level outcomes so callers can tell "bus broke" from "chip disagrees". */
#define TPS_ERR_VERIFY_MISMATCH  ESP_ERR_INVALID_RESPONSE /* readback != written (see notes on 02h) */
#define TPS_ERR_NOT_READY        ESP_ERR_INVALID_STATE    /* FSM below STANDBY: I2C not accessible yet */

/* ---------------- low level ---------------- */
esp_err_t tps92390_add_device(i2c_master_bus_handle_t bus, uint8_t addr7, uint32_t hz,
                              i2c_master_dev_handle_t *out);
esp_err_t tps92390_read(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out);
esp_err_t tps92390_write(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val);

/* Write then read back. ONLY for the plain configuration registers 00h..06h:
 * INT_EN registers read a different encoding than they are written with and
 * STATUS registers clear on write, so a verify there is meaningless. Returns
 * TPS_ERR_VERIFY_MISMATCH with the readback in *readback. For 02h a mismatch
 * can be the chip itself: ISET short -> LED_CURRENT forced to 1/4 (7.3.9.2.6),
 * LEDSET missing -> forced to 0x3FF (Table 7-9). Check BOOST_STATUS then. */
esp_err_t tps92390_write_verify(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val,
                                uint16_t *readback);
/* Read-modify-write + verify, same restriction as write_verify (00h..06h). */
esp_err_t tps92390_update_bits(i2c_master_dev_handle_t dev, uint8_t reg,
                               uint16_t mask, uint16_t set, uint16_t *readback);

/* ---------------- chip-level helpers ---------------- */

/* Poll FSM until >= STANDBY (I2C accessible, 7.4.4) or timeout. Returns
 * TPS_ERR_NOT_READY on timeout, the I2C error if the part never ACKs. */
esp_err_t tps92390_wait_ready(i2c_master_dev_handle_t dev, uint32_t timeout_ms, uint16_t *fsm_out);

/* Read the reset fingerprint (06h/08h/0Ah/0Ch) and decode 1Eh. */
esp_err_t tps92390_check_identity(i2c_master_dev_handle_t dev, tps_identity_t *out);
void tps92390_decode_autodetect(uint16_t raw, tps_autodetect_t *out);

/* Brightness through the DISPLAY_BRT path (BRT_MODE must be 2). Verified. */
esp_err_t tps92390_set_brightness(i2c_master_dev_handle_t dev, uint16_t brt, uint16_t *readback);

/* LED current ceiling, 12-bit code (clamped to 0x0FFF). Verified; see the
 * 02h note on write_verify for what a mismatch means. */
esp_err_t tps92390_set_current_code(i2c_master_dev_handle_t dev, uint16_t code, uint16_t *readback);

/* INT_EN registers (08h/0Ah/0Ch): enable_mask bit p (0..7) = pair p enabled.
 * Every pair is written explicitly with the documented write codes (01 =
 * disable, 11 = enable - 00/10 are undefined write values, 7.5.2.3) and the
 * result is checked against the documented READ codes (00/10). */
esp_err_t tps92390_set_int_en(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t enable_mask,
                              uint16_t *readback);
/* Convert a raw INT_EN read value into the 8-bit pair-enabled mask. */
uint8_t tps92390_int_en_mask(uint16_t raw);

/* Clear latched faults: writes 1 to STATUS and CLEAR of every set pair
 * (7.5.2.2). For LED_STATUS the read-only bits 8..0 are masked off. Pass the
 * status value you just read; nothing is written when no clearable bit is set. */
esp_err_t tps92390_clear_faults(i2c_master_dev_handle_t dev, uint8_t status_reg, uint16_t status_val);

/* Snapshot of the three fault registers plus FSM in one call. Fields that fail
 * to read are set to 0xFFFF; returns the first I2C error. */
typedef struct {
    uint16_t sup, bst, led, fsm;
} tps_faults_t;
esp_err_t tps92390_read_faults(i2c_master_dev_handle_t dev, tps_faults_t *out);

/* 8-bit "level" -> 12-bit LED_CURRENT code (level * 16, max 0xFF0). */
#define TPS_CURR_CODE(level) ((uint16_t)((uint16_t)(level) << 4))

#ifdef __cplusplus
}
#endif
