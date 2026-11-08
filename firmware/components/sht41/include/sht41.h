// sht41.h - Sensirion SHT4x family driver (SHT40 is the part fitted on this board).
//
// The component keeps the historical name "sht41", but everything here follows
// the SHT4x datasheet (D1 v7.3): command set, 6-byte frames, two CRCs and the
// -45/+175 C conversion. Nothing in the SHT4x command table is SHT41-specific,
// so the same code drives SHT40 / SHT41 / SHT45.
//
// Verified against the datasheet on the bench:
//   * T  = -45 + 175 * ticks / 65535   (an earlier -50 offset made every reading
//                                        5 C too LOW)
//   * RH = -6 + 125 * ticks / 65535
//   * 0x89 returns TWO 16-bit words, each followed by its own CRC (6 bytes)
//   * there is NO status register command on SHT4x (0x93 belongs to SHT3x)
//   * "if the sensor is not ready to process a command it answers NACK to the
//     I2C read header" - so a NACK means "busy", not "unsupported"
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* I2C commands (SHT4x datasheet, Table 8) */
#define SHT41_CMD_MEAS_HP      0xFDu  /* high repeatability, typ 6.9 / max 8.3 ms */
#define SHT41_CMD_MEAS_MP      0xF6u  /* medium */
#define SHT41_CMD_MEAS_LP      0xE0u  /* low, typ 1.3 / max 1.6 ms */
#define SHT41_CMD_READ_SER_NUM 0x89u  /* 6 bytes: SN MSB, SN LSB, CRC, SN MSB, SN LSB, CRC */
#define SHT41_CMD_RESET        0x94u

typedef struct {
    float temp_c;
    float rel_hum;   /* %RH, clamped 0..100 by the conversion */
    uint32_t serial; /* 32-bit, two 16-bit words from command 0x89 */
    bool serial_ok;  /* 0x89 answered with valid CRCs on both words */
} sht41_info_t;

esp_err_t sht41_reset(i2c_master_dev_handle_t dev);

esp_err_t sht41_measure(i2c_master_dev_handle_t dev, float *temp_c, float *rel_hum);

/* 32-bit serial number. */
esp_err_t sht41_serial(i2c_master_dev_handle_t dev, uint32_t *serial);

/* Diagnostic form: keeps all six raw bytes so the caller can log exactly what
 * the sensor returned. */
esp_err_t sht41_serial_ex(i2c_master_dev_handle_t dev, uint32_t *serial, uint8_t *raw, size_t raw_len,
                          size_t *rx_len);

/* Reset, then measure 3x: checks CRCs, plausible range and sample spread.
 * The serial-number read is a SOFT check (info->serial_ok). */
bool sht41_selftest(i2c_master_dev_handle_t dev, sht41_info_t *info, char *reason, size_t len);

#ifdef __cplusplus
}
#endif
