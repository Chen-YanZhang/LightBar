// ws2812.h - RMT-driven WS2812C status/status-bar strip.
// WS2812C takes its bytes in GRB order (that is the "C" difference); the API
// below speaks RGB and reorders internally, so callers never think about it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Allocate the framebuffer and bring up one RMT TX channel on `gpio`.
 * Call once at boot; count = number of LEDs. */
esp_err_t ws2812_init(gpio_num_t gpio, uint16_t count);

uint16_t ws2812_count(void);

/* Framebuffer writes only (no bus activity). Out-of-range indices ignored. */
void ws2812_set_rgb(int idx, uint8_t r, uint8_t g, uint8_t b);
void ws2812_fill_rgb(uint8_t r, uint8_t g, uint8_t b);
void ws2812_clear(void);

/* Scale the whole framebuffer (0..255) - keeps status strip dim at night. */
void ws2812_set_brightness(uint8_t level);
uint8_t ws2812_get_brightness(void);

/* Push the framebuffer to the strip (blocking until the RMT transfer is done,
 * ~1.2 us per bit => ~36 us per LED). Call from one task only. */
esp_err_t ws2812_show(void);

#ifdef __cplusplus
}
#endif
