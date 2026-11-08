// tps_selftest.c v3 - TPS92390 functional probing.
//
// SAFETY CONTRACT (why v3 differs from v2):
//  * The drivers are powered from VBUS and keep their registers across an MCU
//    reset. v2 wrote LED_CURR_CONFIG patterns up to 0x0FFF (160 mA, above the
//    string rating) and BRT=0x1234 while BRT_MODE could still be 2 from the
//    previous run -> the lamp strobed on every warm reboot. v3 forces
//    DISPLAY_BRT=0 on a chip BEFORE touching anything else, never writes a
//    current code above LB_CODE_MAX, and hands the chip back to lightbar_out
//    (note_chip_reset) so the production configuration is rewritten.
//  * Odd register offsets are reserved (Table 7-12) and are not read any more.
//  * INT_EN pairs are exercised with tps92390_set_int_en() (01/11 write codes,
//    00/10 read codes, DS 7.5.2.3) instead of a raw RMW.
//  * Runs at boot only with CONFIG_LB_BOOT_SELFTEST; otherwise CLI "m1".

#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "tps92390.h"
#include "tps_bus.h"
#include "board_pins.h"
#include "lightbar_out.h"

static const char *TAG = "tps-st";
static int n_pass, n_fail, n_info;

#define CHECK(cond, ...) do { if (cond) { n_pass++; ESP_LOGI(TAG, "PASS " __VA_ARGS__); } \
                              else { n_fail++; ESP_LOGE(TAG, "FAIL " __VA_ARGS__); } } while (0)
#define INFO(...)  do { n_info++; ESP_LOGW(TAG, "INFO " __VA_ARGS__); } while (0)

static void curr_bitmap(const tps_ctx_t *c, i2c_master_dev_handle_t dev, const char *label)
{
    /* Every pattern <= LB_CODE_MAX (0xF00). Brightness is 0 while this runs, so
     * the DAC output stays at 0 regardless of the code (1Ah tracks code*BRT). */
    static const uint16_t pats[] = {
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
        0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800,
        0x000F, 0x00F0, 0x0F00, 0x0CE4, 0x0A5A, 0x0000,
    };
    int bad = 0;
    for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
        uint16_t rb = 0;
        esp_err_t e = tps92390_write_verify(dev, TPS_REG_LED_CURR_CONFIG, pats[i], &rb);
        if (e != ESP_OK) {
            bad++;
            INFO("%s CURR %s: wr=0x%04X rd=0x%04X xor=0x%04X", c->name, label, pats[i], rb,
                 (uint16_t)(pats[i] ^ rb));
        }
    }
    CHECK(bad == 0, "%s CURR 12-bit map @%s: %d/%d mismatches", c->name, label, bad,
          (int)(sizeof(pats) / sizeof(pats[0])));
}

static void config_battery(const tps_ctx_t *c, i2c_master_dev_handle_t dev, const char *label)
{
    uint16_t rb = 0;
    esp_err_t e;

    /* UC1 fields with BRT_MODE forced to 0 (PWM pin is grounded on this board,
     * so nothing can light while the sloper/dither bits are toggled). */
    const uint16_t uc1_test = (uint16_t)((TPS_RST_USER_CONFIG1 & ~(TPS_UC1_SLOPE_SELECT(7) | TPS_UC1_DITHER_SELECT(4) | (3u << 2)))
                                         | TPS_UC1_SLOPE_SELECT(3) | TPS_UC1_DITHER_SELECT(2));
    e = tps92390_write_verify(dev, TPS_REG_USER_CONFIG1, uc1_test, &rb);
    CHECK(e == ESP_OK, "%s UC1 slope=3 dither=2 @%s -> 0x%04X", c->name, label, rb);
    e = tps92390_write_verify(dev, TPS_REG_USER_CONFIG1, TPS_RST_USER_CONFIG1, &rb);
    CHECK(e == ESP_OK, "%s UC1 reset value 0x08A3 @%s -> 0x%04X", c->name, label, rb);

    /* DISPLAY_BRT register is writable while BRT_MODE=0 (no optical effect). */
    e = tps92390_set_brightness(dev, 0x1234, &rb);
    CHECK(e == ESP_OK, "%s BRT 0x1234 @%s -> 0x%04X (mode 0, no light)", c->name, label, rb);
    e = tps92390_set_brightness(dev, 0x0000, &rb);
    CHECK(e == ESP_OK, "%s BRT restore 0 @%s", c->name, label);

    /* INT_EN semantics (7.5.2.3): disable pair 0 -> reads 00, enable -> reads 10. */
    uint16_t cur = 0;
    if (tps92390_read(dev, TPS_REG_SUPPLY_INT_EN, &cur) == ESP_OK) {
        uint8_t mask = tps92390_int_en_mask(cur);
        e = tps92390_set_int_en(dev, TPS_REG_SUPPLY_INT_EN, (uint8_t)(mask & ~1u), &rb);
        CHECK(e == ESP_OK && !TPS_INT_RD_ENABLED(rb, 0), "%s INT_EN pair0 disable -> 0x%04X", c->name, rb);
        e = tps92390_set_int_en(dev, TPS_REG_SUPPLY_INT_EN, (uint8_t)(mask | 1u), &rb);
        CHECK(e == ESP_OK && TPS_INT_RD_ENABLED(rb, 0), "%s INT_EN pair0 enable -> 0x%04X", c->name, rb);
        e = tps92390_set_int_en(dev, TPS_REG_SUPPLY_INT_EN, mask, &rb);
        CHECK(e == ESP_OK, "%s INT_EN restored mask 0x%02X -> 0x%04X", c->name, mask, rb);
    }
}

static void faults_and_diags(const tps_ctx_t *c)
{
    tps_faults_t f;
    if (tps92390_read_faults(c->d100, &f) == ESP_OK) {
        INFO("%s SUPPLY=0x%04X BOOST=0x%04X LED=0x%04X FSM=0x%02X", c->name, f.sup, f.bst, f.led,
             (unsigned)(f.fsm & TPS_FSM_MASK));
        if (f.sup & TPS_SS_CRCERR) {
            INFO("%s CRCERR set: factory trim load failed at power-up (7.3.9.1.6); device keeps running",
                 c->name);
        }
        if (f.led & TPS_LS_INVSTRING) {
            INFO("%s INVSTRING: an unused OUTx is not tied to GND (OUT4 on PCB3_3)", c->name);
        }
        if ((f.fsm & TPS_FSM_MASK) == TPS_FSM_ALL_LED_FAULT) {
            INFO("%s FSM=ALL_LED_FAULT latch: only EN low (VBUS power cycle on PCB3_3) exits", c->name);
        }
    }

    static const struct { const char *n; uint8_t r; } diags[] = {
        { "PWM_IN", TPS_REG_PWM_IN_DIAGNOSTICS }, { "PWM_OUT", TPS_REG_PWM_OUT_DIAGNOSTICS },
        { "LED_CURR_DAC", TPS_REG_LED_CURR_DIAGNOSTICS }, { "ADAPT_BOOST", TPS_REG_ADAPT_BOOST_DIAGNOSTICS },
    };
    for (size_t i = 0; i < sizeof(diags) / sizeof(diags[0]); i++) {
        uint16_t v = 0;
        if (tps92390_read(c->d100, diags[i].r, &v) == ESP_OK) {
            INFO("%s DIAG %s=0x%04X", c->name, diags[i].n, v);
            if (diags[i].r == TPS_REG_PWM_IN_DIAGNOSTICS) {
                CHECK(v == 0, "%s PWM input duty=0 (pin GND)", c->name);
            }
        } else {
            CHECK(false, "%s DIAG %s read fail", c->name, diags[i].n);
        }
    }
}

void tps_selftest_run(void)
{
    n_pass = n_fail = n_info = 0;
    CHECK(tps_bus_init() == ESP_OK, "tps_bus init sda=%d scl=%d",
          (int)LB_I2C_SDA_GPIO, (int)LB_I2C_SCL_GPIO);

    tps_bus_lock();
    for (int i = 0; i < tps_bus_chip_count(); i++) {
        tps_ctx_t *c = tps_bus_chip(i);
        uint16_t v = 0;
        /* presence */
        bool ack = (tps92390_read(c->d100, TPS_REG_BRT_CONTROL, &v) == ESP_OK);
        CHECK(ack, "%s presence ACK BRT=0x%04X", c->name, v);
        if (!ack) {
            continue;
        }

        /* SAFE STATE FIRST: sink off before any other register is touched. */
        uint16_t rb = 0;
        CHECK(tps92390_set_brightness(c->d100, 0, &rb) == ESP_OK, "%s DISPLAY_BRT=0 before tests", c->name);
        lightbar_out_note_chip_reset(i); /* we are about to disturb UC1/LED_CURR */

        tps_identity_t id;
        if (tps92390_check_identity(c->d100, &id) == ESP_OK) {
            CHECK(id.fingerprint_ok, "%s reset fingerprint 06=%04X 08=%04X 0A=%04X 0C=%04X", c->name,
                  id.uc2, id.sup_int_en, id.bst_int_en, id.led_int_en);
            INFO("%s AUTO_DETECT=0x%04X: mode_sel=%u (addr 0x%02X, %s) strings_cfg=%u boost_fsel=%u pwm_fsel=%u",
                 c->name, id.autodetect_raw, id.ad.mode_sel, id.ad.i2c_addr,
                 id.ad.current_dimming ? "current dimming" : "PWM/hybrid", id.ad.string_cfg,
                 id.ad.boost_freq_sel, id.ad.pwm_freq_sel);
            CHECK(id.ad.i2c_addr == c->addr, "%s detected address matches strap", c->name);
        } else {
            CHECK(false, "%s identity read failed", c->name);
        }

        curr_bitmap(c, c->d100, "100k");
        curr_bitmap(c, c->d20, "20k");
        /* leave the ceiling at the production default, never at 0x0FFF */
        CHECK(tps92390_set_current_code(c->d100, LB_CODE_DEFAULT, &rb) == ESP_OK,
              "%s LED_CURR restored to 0x%03X", c->name, (unsigned)LB_CODE_DEFAULT);

        config_battery(c, c->d20, "20k");
        config_battery(c, c->d100, "100k");

        faults_and_diags(c);
    }
    tps_bus_unlock();

    /* Hand the chips back: lightbar_out rewrites UC1 (BRT_MODE=2) + LED_CURR and
     * the stored brightness target the next time the gate allows it. */
    lightbar_out_note_chip_reset(-1);
    (void)lightbar_out_apply();

    ESP_LOGW(TAG, "SELFTEST v3 SUMMARY: pass=%d fail=%d info=%d", n_pass, n_fail, n_info);
}
