// web_ui.h - tiny built-in web page so a user can update the firmware without
// any app or cloud: browse to http://<device>/ and upload a .bin from the
// project's Releases page.
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the HTTP server (idempotent). Safe to call before Wi-Fi is up; it just
 * starts listening once an interface has an address. */
esp_err_t web_ui_start(void);

#ifdef __cplusplus
}
#endif
