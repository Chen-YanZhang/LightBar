// Lightbar app_main: NVS flight recorder -> safe state -> (optional) selftest ->
// fault-monitor state machine + CLI -> Matter / web / provisioning.
// Outputs stay DISARMED by policy until the safety gate opens (safety_sm.h).
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "board_pins.h"
#include "tps_bus.h"
#include "sht41.h"
#include "safety_sm.h"
#include "lightbar_mix.h"
#include "lb_solar.h"
#include "lb_time.h"
#include "rgb_status.h"
#include "solar_policy.h"
#include "lb_ota.h"
#include "web_ui.h"
#include "wifi_prov.h"
#include "app_matter.h"
#include "evlog.h"

void tps_selftest_run(void);

static const char *TAG = "lightbar";

void app_main(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;
    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Lightbar %s boot", lb_ota_version());
    ESP_LOGI(TAG, "  target=esp32c6 model=%d rev=%d cores=%d",
             chip.model, chip.revision, chip.cores);
    ESP_LOGI(TAG, "  flash=%" PRIu32 "MB reset_reason=%d",
             flash_size / (1024 * 1024), (int)esp_reset_reason());

    evlog_init();
    evlog("boot", "reset_reason=%d version=%s", (int)esp_reset_reason(), lb_ota_version());

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);

    /* OTA bookkeeping: reports whether this boot is a freshly-flashed image that
     * still has to prove itself before the bootloader will keep it. */
    lb_ota_init();
    bool boot_ok = true;

    /* Wall clock (TZ for display; SNTP starts itself once Wi-Fi is up) */
    lb_time_init();

    ESP_ERROR_CHECK(tps_bus_init());

    /* Record raw power-on chip state BEFORE any of our writes, then put both
     * drivers into the safe state (DISPLAY_BRT=0). The drivers keep their
     * registers across an MCU reset, so this is what prevents a warm reboot
     * from re-lighting the lamp with stale settings. */
    safety_sm_recorder_boot();

#if CONFIG_LB_BOOT_SELFTEST
    tps_selftest_run();
#else
    ESP_LOGI(TAG, "TPS92390 selftest skipped at boot (CLI 'm1' runs it)");
#endif

    /* Pure-math CCT mixing self-test (no hardware involved) */
    char mix_reason[80];
    if (lb_mix_selftest(mix_reason, sizeof(mix_reason))) {
        ESP_LOGI(TAG, "lb_mix selftest: PASS");
    } else {
        ESP_LOGE(TAG, "lb_mix selftest: FAIL (%s)", mix_reason);
        boot_ok = false;
    }

    /* Sun-adaptive CCT curve (pure math, no hardware) */
    char sun_reason[96];
    if (lb_solar_selftest(sun_reason, sizeof(sun_reason))) {
        ESP_LOGI(TAG, "lb_solar selftest: PASS");
    } else {
        ESP_LOGE(TAG, "lb_solar selftest: FAIL (%s)", sun_reason);
        boot_ok = false;
    }

    /* Status strip first: it is the visual channel for the state machine */
    rgb_status_init();

    /* SHT40 board-temperature sensor. A failure here is logged, NOT a rollback
     * criterion: one bad I2C read at boot must not throw away a good image. */
    if (tps_bus_sht41_present()) {
        sht41_info_t si;
        char sens_reason[96];
        memset(&si, 0, sizeof(si));
        sens_reason[0] = '\0';
        tps_bus_lock();
        bool sens_ok = sht41_selftest(tps_bus_sht41(), &si, sens_reason, sizeof(sens_reason));
        tps_bus_unlock();
        if (sens_ok) {
            ESP_LOGI(TAG, "SHT4x selftest: PASS  %.2f C  %.1f %%RH  serial=0x%08" PRIX32 " serial_ok=%d",
                     si.temp_c, si.rel_hum, si.serial, si.serial_ok);
            if (sens_reason[0] != '\0') {
                ESP_LOGW(TAG, "SHT4x note: %s", sens_reason);
            }
        } else {
            ESP_LOGE(TAG, "SHT4x selftest: FAIL (%s) - over-temperature protection degraded", sens_reason);
        }
    } else {
        ESP_LOGW(TAG, "SHT4x did not ACK at 0x%02X (not fitted? bus issue?)", LB_ADDR_SHT41);
    }

    /* At least one driver must answer, otherwise the image cannot drive a lamp. */
    if (!tps_bus_chip(0)->ack && !tps_bus_chip(1)->ack) {
        boot_ok = false;
    }

    /* Monitor + CLI tasks take over; monitor task feeds the task watchdog */
    safety_sm_start();

    /* Sun-adaptive CCT: owns mireds only, writes go through the same gate */
    solar_policy_start();

    /* Matter node + ColorTemperatureLight endpoint. Controller commands land
     * in lightbar_out, which is gated by the safety state machine. */
    lightbar_matter_start();

    /* Restore the last light target saved in NVS. Done AFTER the Matter node is
     * created because its boot-time attribute init posts its own values; the
     * restore then pushes ours back into the Matter data model so both agree. */
    safety_sm_restore_light_target();

    /* Built-in web page: status, light control, firmware upload. */
    web_ui_start();

    /* No stored Wi-Fi credentials -> raise the provisioning AP so the device can
     * be put on the network from a browser. */
    wifi_prov_init();

    ESP_LOGI(TAG, "heap after Matter start: free=%u min-free-so-far=%u (internal free=%u)",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* OTA rollback: a freshly flashed image only becomes permanent once it got
     * this far with the self-tests above green. On failure lb_ota rolls back
     * immediately instead of leaving a broken image pending for weeks. */
    if (lb_ota_pending_verify()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        lb_ota_mark_valid(boot_ok);
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
