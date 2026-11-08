#include "tps_bus.h"
#include "tps92390.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

static const char *TAG = "tps-bus";

static i2c_master_bus_handle_t s_bus;
static tps_ctx_t s_ctx[2] = {
    { .addr = LB_ADDR_TPS_WARM, .name = "warm(3A)" },
    { .addr = LB_ADDR_TPS_COLD, .name = "cold(3B)" },
};
static SemaphoreHandle_t s_mtx;
static i2c_master_dev_handle_t s_sht41;
static bool s_sht41_present;

/* I2C bus recovery before the driver claims the pins. The TPS92390 latches
 * I2C_ERROR_STATUS when a command is left without STOP for 500 ms (DS
 * 7.3.9.3.5) - exactly what an MCU reset in the middle of a transaction does
 * (USB re-enumeration, OTA reboot, panic). Clock SCL 9 times with SDA released
 * so any slave stuck mid-byte finishes, then issue a STOP. */
static void bus_recover(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LB_I2C_SDA_GPIO) | (1ULL << LB_I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE, /* external pull-ups fitted */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level(LB_I2C_SDA_GPIO, 1);
    gpio_set_level(LB_I2C_SCL_GPIO, 1);
    esp_rom_delay_us(10);
    bool sda_stuck = (gpio_get_level(LB_I2C_SDA_GPIO) == 0);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(LB_I2C_SCL_GPIO, 0);
        esp_rom_delay_us(5);
        gpio_set_level(LB_I2C_SCL_GPIO, 1);
        esp_rom_delay_us(5);
    }
    /* STOP: SDA low -> SCL high -> SDA high */
    gpio_set_level(LB_I2C_SDA_GPIO, 0);
    esp_rom_delay_us(5);
    gpio_set_level(LB_I2C_SCL_GPIO, 1);
    esp_rom_delay_us(5);
    gpio_set_level(LB_I2C_SDA_GPIO, 1);
    esp_rom_delay_us(10);
    if (sda_stuck) {
        ESP_LOGW(TAG, "SDA was held low at boot - bus recovery clocks issued (now %s)",
                 gpio_get_level(LB_I2C_SDA_GPIO) ? "released" : "STILL LOW");
    }
    gpio_reset_pin(LB_I2C_SDA_GPIO);
    gpio_reset_pin(LB_I2C_SCL_GPIO);
}

esp_err_t tps_bus_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateRecursiveMutex();
    }
    if (s_bus != NULL) {
        return ESP_OK; /* already initialised */
    }
    bus_recover();

    i2c_master_bus_config_t bcfg = {
        .i2c_port = LB_I2C_PORT,
        .sda_io_num = LB_I2C_SDA_GPIO,
        .scl_io_num = LB_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false, /* external pull-ups on the board */
    };
    esp_err_t e = i2c_new_master_bus(&bcfg, &s_bus);
    if (e != ESP_OK) {
        return e;
    }
    for (int i = 0; i < 2; i++) {
        e = tps92390_add_device(s_bus, s_ctx[i].addr, LB_I2C_FREQ_HZ, &s_ctx[i].d100);
        if (e != ESP_OK) {
            return e;
        }
        e = tps92390_add_device(s_bus, s_ctx[i].addr, 20000, &s_ctx[i].d20);
        if (e != ESP_OK) {
            return e;
        }
        /* DS 7.4.4: registers are reachable from STANDBY onwards. On PCB3_3 EN
         * is tied high so the part is long past that; on the new PCB this is
         * the hand-over point after EN goes high. */
        uint16_t fsm = 0xFFFF;
        esp_err_t r = tps92390_wait_ready(s_ctx[i].d100, 200, &fsm);
        s_ctx[i].ack = (r == ESP_OK || r == TPS_ERR_NOT_READY);
        if (r == TPS_ERR_NOT_READY) {
            ESP_LOGW(TAG, "%s answers but FSM=0x%02X < STANDBY after 200 ms", s_ctx[i].name,
                     (unsigned)(fsm & TPS_FSM_MASK));
        } else if (r != ESP_OK) {
            ESP_LOGW(TAG, "%s no ACK (%s)", s_ctx[i].name, esp_err_to_name(r));
        }
    }

    /* SHT40 lives on the same bus (0x44) */
    i2c_device_config_t scfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LB_ADDR_SHT41,
        .scl_speed_hz = LB_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &scfg, &s_sht41) == ESP_OK) {
        s_sht41_present = (i2c_master_probe(s_bus, LB_ADDR_SHT41, 200) == ESP_OK);
    }
    return ESP_OK;
}

tps_ctx_t *tps_bus_chip(int idx)
{
    return (idx >= 0 && idx < 2) ? &s_ctx[idx] : NULL;
}

int tps_bus_chip_count(void) { return 2; }

i2c_master_dev_handle_t tps_bus_sht41(void) { return s_sht41; }
bool tps_bus_sht41_present(void) { return s_sht41_present; }

void tps_bus_lock(void)   { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
void tps_bus_unlock(void) { xSemaphoreGiveRecursive(s_mtx); }
