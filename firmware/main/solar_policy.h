// solar_policy.h - sun-adaptive CCT policy task.
// Owns the mireds target ONLY: it never touches on/level, and every write goes
// through lightbar_out (i.e. the safety gate). A controller setting colour
// temperature by hand wins immediately: note_manual_ct() switches auto off.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void solar_policy_start(void);
void solar_policy_set_auto(bool on);
bool solar_policy_auto(void);
uint16_t solar_policy_last_mireds(void);   /* 0 = never applied */
/* Last computed sun elevation in degrees; 0.0 until the first computation
 * (kept finite on purpose: the value is printed into JSON). */
float solar_policy_last_elevation(void);

/* Called from the Matter attribute callback when someone writes
 * ColorControl.ColorTemperatureMireds. Turns auto off and says so. */
void solar_policy_note_manual_ct(void);

#ifdef __cplusplus
}
#endif
