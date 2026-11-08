// tps_bus.h - shared I2C bus + both TPS92390 devices, mutex-protected access.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "driver/i2c_master.h"

typedef struct {
    i2c_master_dev_handle_t d100;      /* LB_I2C_FREQ_HZ */
    i2c_master_dev_handle_t d20;       /* 20 kHz slow probe */
    uint8_t addr;
    const char *name;                  /* "warm(3A)" / "cold(3B)" */
    bool ack;                          /* last presence result */
} tps_ctx_t;

esp_err_t tps_bus_init(void);
tps_ctx_t *tps_bus_chip(int idx);      /* 0=warm, 1=cold */
int tps_bus_chip_count(void);

/* SHT41 shares this bus (LB_ADDR_SHT41). dev handle is NULL only if the bus
 * itself failed to come up; present reflects the last probe result. */
i2c_master_dev_handle_t tps_bus_sht41(void);
bool tps_bus_sht41_present(void);

void tps_bus_lock(void);
void tps_bus_unlock(void);
