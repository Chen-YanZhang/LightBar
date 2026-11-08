#include "tps92390.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TPS_XFER_TIMEOUT_MS 100

esp_err_t tps92390_add_device(i2c_master_bus_handle_t bus, uint8_t addr7, uint32_t hz,
                              i2c_master_dev_handle_t *out)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr7,
        .scl_speed_hz = hz,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

/* Low byte first on the wire - see the header for the evidence. */
esp_err_t tps92390_write(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    return i2c_master_transmit(dev, buf, sizeof(buf), TPS_XFER_TIMEOUT_MS);
}

esp_err_t tps92390_read(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *out)
{
    uint8_t rx[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, rx, sizeof(rx), TPS_XFER_TIMEOUT_MS);
    if (err == ESP_OK && out) {
        *out = (uint16_t)((rx[1] << 8) | rx[0]);
    }
    return err;
}

esp_err_t tps92390_write_verify(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t val,
                                uint16_t *readback)
{
    esp_err_t err = tps92390_write(dev, reg, val);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t rb = 0;
    err = tps92390_read(dev, reg, &rb);
    if (err != ESP_OK) {
        return err;
    }
    if (readback) {
        *readback = rb;
    }
    return (rb == val) ? ESP_OK : TPS_ERR_VERIFY_MISMATCH;
}

esp_err_t tps92390_update_bits(i2c_master_dev_handle_t dev, uint8_t reg,
                               uint16_t mask, uint16_t set, uint16_t *readback)
{
    uint16_t cur = 0;
    esp_err_t err = tps92390_read(dev, reg, &cur);
    if (err != ESP_OK) {
        return err;
    }
    return tps92390_write_verify(dev, reg, (uint16_t)((cur & ~mask) | (set & mask)), readback);
}

/* ---------------- chip-level helpers ---------------- */

esp_err_t tps92390_wait_ready(i2c_master_dev_handle_t dev, uint32_t timeout_ms, uint16_t *fsm_out)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t last = ESP_FAIL;
    uint16_t fsm = 0xFFFF;
    do {
        last = tps92390_read(dev, TPS_REG_FSM_DIAGNOSTICS, &fsm);
        if (last == ESP_OK && (fsm & TPS_FSM_MASK) >= TPS_FSM_STANDBY) {
            if (fsm_out) {
                *fsm_out = fsm;
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((int32_t)(deadline - xTaskGetTickCount()) > 0);
    if (fsm_out) {
        *fsm_out = fsm;
    }
    return (last == ESP_OK) ? TPS_ERR_NOT_READY : last;
}

void tps92390_decode_autodetect(uint16_t raw, tps_autodetect_t *out)
{
    if (out == NULL) {
        return;
    }
    out->pwm_freq_sel = (uint8_t)((raw >> 12) & 7u);
    out->string_cfg = (uint8_t)((raw >> 8) & 7u);
    out->boost_freq_sel = (uint8_t)((raw >> 3) & 7u);
    out->mode_sel = (uint8_t)(raw & 7u);
    out->i2c_addr = (out->mode_sel >= 4u) ? 0x3A : 0x3B;
    out->current_dimming = ((out->mode_sel & 3u) == 2u);
}

esp_err_t tps92390_check_identity(i2c_master_dev_handle_t dev, tps_identity_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t e;
    if ((e = tps92390_read(dev, TPS_REG_USER_CONFIG2, &out->uc2)) != ESP_OK) return e;
    if ((e = tps92390_read(dev, TPS_REG_SUPPLY_INT_EN, &out->sup_int_en)) != ESP_OK) return e;
    if ((e = tps92390_read(dev, TPS_REG_BOOST_INT_EN, &out->bst_int_en)) != ESP_OK) return e;
    if ((e = tps92390_read(dev, TPS_REG_LED_INT_EN, &out->led_int_en)) != ESP_OK) return e;
    if ((e = tps92390_read(dev, TPS_REG_AUTO_DETECT_DIAGNOSTICS, &out->autodetect_raw)) != ESP_OK) return e;
    out->fingerprint_ok = (out->uc2 == TPS_RST_USER_CONFIG2) &&
                          (out->sup_int_en == TPS_RST_SUPPLY_INT_EN) &&
                          (out->bst_int_en == TPS_RST_BOOST_INT_EN) &&
                          (out->led_int_en == TPS_RST_LED_INT_EN);
    tps92390_decode_autodetect(out->autodetect_raw, &out->ad);
    return ESP_OK;
}

esp_err_t tps92390_set_brightness(i2c_master_dev_handle_t dev, uint16_t brt, uint16_t *readback)
{
    return tps92390_write_verify(dev, TPS_REG_BRT_CONTROL, brt, readback);
}

esp_err_t tps92390_set_current_code(i2c_master_dev_handle_t dev, uint16_t code, uint16_t *readback)
{
    if (code > 0x0FFF) {
        code = 0x0FFF;
    }
    return tps92390_write_verify(dev, TPS_REG_LED_CURR_CONFIG, code, readback);
}

uint8_t tps92390_int_en_mask(uint16_t raw)
{
    uint8_t m = 0;
    for (int p = 0; p < 8; p++) {
        if (TPS_INT_RD_ENABLED(raw, p)) {
            m |= (uint8_t)(1u << p);
        }
    }
    return m;
}

esp_err_t tps92390_set_int_en(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t enable_mask,
                              uint16_t *readback)
{
    if (reg != TPS_REG_SUPPLY_INT_EN && reg != TPS_REG_BOOST_INT_EN && reg != TPS_REG_LED_INT_EN) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Unused pairs (reserved per register) are written with the disable code,
     * which is the only harmless defined write value. */
    uint16_t wr = 0, expect = 0;
    for (int p = 0; p < 8; p++) {
        bool en = (enable_mask >> p) & 1u;
        wr |= (uint16_t)((en ? 3u : 1u) << (p * 2));
        expect |= (uint16_t)((en ? 2u : 0u) << (p * 2));
    }
    esp_err_t e = tps92390_write(dev, reg, wr);
    if (e != ESP_OK) {
        return e;
    }
    uint16_t rb = 0;
    e = tps92390_read(dev, reg, &rb);
    if (e != ESP_OK) {
        return e;
    }
    if (readback) {
        *readback = rb;
    }
    /* Reserved pairs may read back 0 regardless; compare only the requested ones. */
    uint16_t cmp_mask = 0;
    for (int p = 0; p < 8; p++) {
        cmp_mask |= (uint16_t)(3u << (p * 2));
    }
    return ((rb & cmp_mask) == (expect & cmp_mask)) ? ESP_OK : TPS_ERR_VERIFY_MISMATCH;
}

esp_err_t tps92390_clear_faults(i2c_master_dev_handle_t dev, uint8_t status_reg, uint16_t status_val)
{
    uint16_t status_mask;
    switch (status_reg) {
    case TPS_REG_SUPPLY_STATUS: status_mask = TPS_SS_STATUS_MASK; break;
    case TPS_REG_BOOST_STATUS:  status_mask = TPS_BS_STATUS_MASK; break;
    case TPS_REG_LED_STATUS:    status_mask = TPS_LS_STATUS_MASK; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    uint16_t st = (uint16_t)(status_val & status_mask);
    if (st == 0) {
        return ESP_OK;
    }
    /* STATUS bit and the CLEAR bit right below it, both written 1 (7.5.2.2). */
    return tps92390_write(dev, status_reg, (uint16_t)(st | (st >> 1)));
}

esp_err_t tps92390_read_faults(i2c_master_dev_handle_t dev, tps_faults_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->sup = out->bst = out->led = out->fsm = 0xFFFF;
    esp_err_t first = ESP_OK, e;
    e = tps92390_read(dev, TPS_REG_SUPPLY_STATUS, &out->sup); if (e != ESP_OK && first == ESP_OK) first = e;
    e = tps92390_read(dev, TPS_REG_BOOST_STATUS, &out->bst);  if (e != ESP_OK && first == ESP_OK) first = e;
    e = tps92390_read(dev, TPS_REG_LED_STATUS, &out->led);    if (e != ESP_OK && first == ESP_OK) first = e;
    e = tps92390_read(dev, TPS_REG_FSM_DIAGNOSTICS, &out->fsm); if (e != ESP_OK && first == ESP_OK) first = e;
    return first;
}
