// rgb_status.h - WS2812C status LED: shows what the safety state machine
// and Matter are doing without a serial port. All calls are safe stubs when
// CONFIG_LB_WS2812_ENABLE is off.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rgb_status_init(void);           /* once, after the I2C bus is up */
void rgb_status_poll(void);           /* call at ~1 Hz (monitor task) */
void rgb_status_note_pairing(bool open); /* Matter commissioning window state */

/* Bench helper for the CLI: paints a raw colour and shows it immediately.
 * Returns false when the LED is disabled in Kconfig. */
bool rgb_status_manual(int r, int g, int b);
bool rgb_status_enabled(void);

/* Overall LED brightness 0..255 (0 = LED off, including fault colours).
 * May be called before init (the stored value is applied at init); the
 * monitor's next 1 Hz poll repaints with the new level. Persisted by safety_sm. */
void rgb_status_set_brightness(uint8_t level);
uint8_t rgb_status_get_brightness(void);

#ifdef __cplusplus
}
#endif
