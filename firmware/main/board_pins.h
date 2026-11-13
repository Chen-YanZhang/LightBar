// LightBar board pin map — NEW PCB revision
// Old PCB3_3 note: EN/PWM were hardwired, INT/PG not wired — those pins only
// exist on the new revision. Keep this file as the single source of truth.
#pragma once

#include "driver/gpio.h"

// --- I2C bus (unchanged from PCB3_3) ---
#define LB_I2C_SDA_GPIO     GPIO_NUM_22
#define LB_I2C_SCL_GPIO     GPIO_NUM_21
#define LB_I2C_PORT         I2C_NUM_0
#define LB_I2C_FREQ_HZ      100000

// I2C addresses (fixed by strap resistors; see datasheet tables)
#define LB_ADDR_TPS_WARM    0x3A   // U12 warm 2700K, MODE=42.2k
#define LB_ADDR_TPS_COLD    0x3B   // U4  cold 5000K, MODE=5.76k
#define LB_ADDR_SHT41       0x44

// --- Safety control (NEW on new PCB) ---
#define LB_EN_WARM_GPIO     GPIO_NUM_10   // TPS92390 U12 EN, 100k pulldown
#define LB_EN_COLD_GPIO     GPIO_NUM_11   // TPS92390 U4  EN, 100k pulldown
#define LB_INT_WARM_GPIO    GPIO_NUM_6    // U12 INT, 10k pullup
#define LB_INT_COLD_GPIO    GPIO_NUM_7    // U4  INT, 10k pullup
#define LB_PD_PG_GPIO       GPIO_NUM_20   // CH224K power-good

// --- Power monitor: VBUS divider 68k/10k -> ADC1 (C6 ADC1 = GPIO0..4 ONLY) ---
#define LB_VBUS_ADC_GPIO    GPIO_NUM_0
#define LB_VBUS_ADC_UNIT    ADC_UNIT_1
#define LB_VBUS_ADC_CHAN    ADC_CHANNEL_0
#define LB_VBUS_DIV_NUM     78            // (68k+10k)/10k
#define LB_VBUS_DIV_DEN     10

// --- User interface ---
#define LB_BTN_GPIO         GPIO_NUM_9    // BOOT button + arm/disarm gesture
#define LB_WS2812_GPIO      GPIO_NUM_15   // via 51R, 5V-fed LED (new PCB)

// --- Current limits (hardware ISET=19.502k -> ~160.08 mA FS) ---
// I_LED = 2580 * 1.21V / R_ISET * code/4095   (DS eq.15)
//   code 0xD00 (3328) = 130.1 mA   <- conservative default
//   code 0xF00 (3840) = 150.1 mA   <- design point (150 mA/string)
//   code 0xFFF (4095) = 160.1 mA   <- absolute full scale
// SINGLE source of truth: lightbar_out.h derives its 8-bit "level" constants
// (LB_LED_CURRENT_LEVEL/MAX = code >> 4) from these two values.
#define LB_CODE_DEFAULT     3840          // 150 mA: rated string current (full power; was 3328 = 130 mA bench value)
#define LB_CODE_MAX         3840          // ~150 mA: the rated string current
