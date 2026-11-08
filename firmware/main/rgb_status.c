// rgb_status.c - see rgb_status.h
// Colour language (deliberate, keep it stable so it can be memorised; the web
// page "说明" tab documents exactly this list):
//   magenta  solid  neither TPS92390 ACKs (bus dead / chips unpowered)
//   purple   blink  board over-temperature, outputs forced off
//   red      blink  live critical fault in the last monitor sample
//   amber    solid  blocked by boot CRCERR
//   white    solid  ARM_OK, output gate open (normal running)
//   white    blink  running, but >=1 LED string disabled by the driver (open/short)
//   dim cyan solid  BOOT / disarmed but chips alive
//   blue     blink  Matter commissioning window open (overrides everything)
#include "rgb_status.h"
#include "sdkconfig.h"
#include "esp_log.h"

#if CONFIG_LB_WS2812_ENABLE
#include "ws2812.h"
#include "board_pins.h"
#include "safety_sm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "rgb";
static bool s_pairing;
static uint8_t s_phase;
static bool s_ready;
static uint8_t s_bri = CONFIG_LB_WS2812_MAX_BRIGHTNESS;
static SemaphoreHandle_t s_lk; /* monitor task polls, CLI paints: serialise */

void rgb_status_init(void)
{
    s_lk = xSemaphoreCreateMutex();
    esp_err_t e = ws2812_init((gpio_num_t)CONFIG_LB_WS2812_GPIO, CONFIG_LB_WS2812_COUNT);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "ws2812 init failed: %s", esp_err_to_name(e));
        return;
    }
    s_ready = true;
    ws2812_set_brightness(s_bri);
    ws2812_fill_rgb(0, 0, 0);
    ws2812_show();
    ESP_LOGI(TAG, "status LED on GPIO%d x%d, brightness %d/255", CONFIG_LB_WS2812_GPIO,
             CONFIG_LB_WS2812_COUNT, s_bri);
}

bool rgb_status_enabled(void) { return s_ready; }

void rgb_status_note_pairing(bool open) { s_pairing = open; }

void rgb_status_set_brightness(uint8_t level)
{
    s_bri = level;
    if (s_ready) {
        if (s_lk) xSemaphoreTake(s_lk, portMAX_DELAY);
        ws2812_set_brightness(s_bri);
        (void)ws2812_show(); /* repaint the current colour at the new level */
        if (s_lk) xSemaphoreGive(s_lk);
    }
}

uint8_t rgb_status_get_brightness(void) { return s_bri; }

void rgb_status_poll(void)
{
    if (!s_ready) {
        return;
    }
    uint32_t st = safety_sm_status_bits();
    s_phase ^= 1u;

    uint8_t r = 0, g = 0, b = 0;
    bool blink = false;

    if (st & LB_ST_NO_CHIPS) {
        r = 255; b = 255; /* magenta */
    } else if (st & LB_ST_OVERTEMP) {
        r = 255; b = 110; blink = true; /* purple blink */
    } else if (st & LB_ST_CRIT_NOW) {
        r = 255; blink = true; /* red blink: live fault */
    } else if (st & LB_ST_CRCERR) {
        r = 255; g = 140; /* amber */
    } else if (st & LB_ST_ARMED) {
        r = 255; g = 235; b = 200; /* warm-ish white: gate open, running */
        blink = (st & LB_ST_STRING_FAULT) != 0; /* white blink: running but a string is disabled */
    } else {
        g = 70; b = 70; /* dim cyan: boot/disarmed */
    }

    if (s_pairing) {
        b = 255; r = 0; g = 0;
        blink = true;
    }

    if (blink && (s_phase & 1u) == 0u) {
        r = g = b = 0;
    }

    if (s_lk) xSemaphoreTake(s_lk, portMAX_DELAY);
    ws2812_fill_rgb(r, g, b);
    esp_err_t e = ws2812_show();
    if (s_lk) xSemaphoreGive(s_lk);
    if (e != ESP_OK) {
        static uint8_t said;
        if (said < 3) {
            said++;
            ESP_LOGW(TAG, "ws2812_show failed: %s", esp_err_to_name(e));
        }
    }
}

bool rgb_status_manual(int r, int g, int b)
{
    if (!s_ready) {
        return false;
    }
    if (s_lk) xSemaphoreTake(s_lk, portMAX_DELAY);
    ws2812_fill_rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
    esp_err_t e = ws2812_show();
    if (s_lk) xSemaphoreGive(s_lk);
    return e == ESP_OK;
}

#else /* !CONFIG_LB_WS2812_ENABLE */

static uint8_t s_bri_stub;
void rgb_status_init(void) {}
void rgb_status_poll(void) {}
void rgb_status_note_pairing(bool open) { (void)open; }
bool rgb_status_enabled(void) { return false; }
bool rgb_status_manual(int r, int g, int b)
{
    (void)r; (void)g; (void)b;
    return false;
}
void rgb_status_set_brightness(uint8_t level) { s_bri_stub = level; }
uint8_t rgb_status_get_brightness(void) { return s_bri_stub; }

#endif
