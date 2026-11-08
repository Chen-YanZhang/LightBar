// ws2812.c - see ws2812.h
#include "ws2812.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

/* 10 MHz tick => 100 ns per duration unit. These are the bit cells from the
 * IDF RMT WS2812 example, which also satisfy WS2812C timings:
 *   bit0: 0.3 us high, 0.9 us low
 *   bit1: 0.9 us high, 0.4 us low
 * Bit period ~1.2-1.3 us. After the last symbol the line idles low, and our
 * refresh period (>= 10 ms) is far beyond the >50 us reset latch time, so no
 * explicit reset symbol is needed. */
#define WS2812_RESOLUTION_HZ 10000000u
#define WS2812_MAX_PIXELS    256
#define XFER_TIMEOUT_MS      200

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_bytes_enc;
static uint8_t *s_base; /* user colours, RGB, unscaled */
static uint8_t *s_out;  /* brightness-scaled, GRB (wire order) */
static uint16_t s_count;
static uint8_t s_brightness = 255;

esp_err_t ws2812_init(gpio_num_t gpio, uint16_t count)
{
    if (s_chan != NULL || count == 0 || count > WS2812_MAX_PIXELS) {
        return ESP_ERR_INVALID_ARG;
    }

    rmt_tx_channel_config_t cc = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_PLL_F80M, /* C6: 80 MHz PLL, /8 -> 10 MHz */
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
        .flags.init_level = 0,
    };
    esp_err_t e = rmt_new_tx_channel(&cc, &s_chan);
    if (e != ESP_OK) {
        return e;
    }

    rmt_bytes_encoder_config_t be = {
        .bit0 = { .duration0 = 3, .level0 = 1, .duration1 = 9, .level1 = 0 },
        .bit1 = { .duration0 = 9, .level0 = 1, .duration1 = 4, .level1 = 0 },
        .flags.msb_first = 1,
    };
    e = rmt_new_bytes_encoder(&be, &s_bytes_enc);
    if (e != ESP_OK) {
        rmt_del_channel(s_chan);
        s_chan = NULL;
        return e;
    }

    e = rmt_enable(s_chan);
    if (e != ESP_OK) {
        return e;
    }

    s_base = heap_caps_calloc(count, 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_out = heap_caps_calloc(count, 3, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_base == NULL || s_out == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_count = count;
    return ESP_OK;
}

uint16_t ws2812_count(void) { return s_count; }

void ws2812_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b)
{
    if (s_base == NULL || idx < 0 || idx >= (int)s_count) {
        return;
    }
    s_base[idx * 3 + 0] = r;
    s_base[idx * 3 + 1] = g;
    s_base[idx * 3 + 2] = b;
}

void ws2812_fill_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint16_t i = 0; i < s_count; i++) {
        ws2812_set_rgb((int)i, r, g, b);
    }
}

void ws2812_clear(void)
{
    if (s_base != NULL) {
        memset(s_base, 0, (size_t)s_count * 3);
    }
}

void ws2812_set_brightness(uint8_t level)
{
    s_brightness = level;
}

uint8_t ws2812_get_brightness(void)
{
    return s_brightness;
}

esp_err_t ws2812_show(void)
{
    if (s_chan == NULL || s_out == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint16_t i = 0; i < s_count; i++) {
        uint32_t r = (uint32_t)s_base[i * 3 + 0] * s_brightness / 255u;
        uint32_t g = (uint32_t)s_base[i * 3 + 1] * s_brightness / 255u;
        uint32_t b = (uint32_t)s_base[i * 3 + 2] * s_brightness / 255u;
        s_out[i * 3 + 0] = (uint8_t)g; /* WS2812C: G first */
        s_out[i * 3 + 1] = (uint8_t)r;
        s_out[i * 3 + 2] = (uint8_t)b;
    }

    rmt_transmit_config_t tc = {
        .loop_count = 0,
        .flags.eot_level = 0,
    };
    esp_err_t e = rmt_transmit(s_chan, s_bytes_enc, s_out, (size_t)s_count * 3, &tc);
    if (e == ESP_OK) {
        e = rmt_tx_wait_all_done(s_chan, XFER_TIMEOUT_MS);
    }
    return e;
}
