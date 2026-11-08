// evlog.h - in-RAM event ring log, readable at GET /api/log.
//
// Purpose: when the lamp "looks dead" and there is no serial console, this is
// what tells us whether the command ever arrived, whether Wi-Fi/Matter were up,
// and what the state machine did. 64 entries, newest last. Cleared by reboot
// (the NVS flight recorder keeps the per-boot driver snapshots).
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVLOG_N      64
#define EVLOG_MSG    56

typedef struct {
    uint32_t t_ms;            /* esp_timer ms since boot */
    char tag[8];              /* "boot" "wifi" "matter" "light" "sm" "ota" "web" "cli" */
    char msg[EVLOG_MSG];
} evlog_entry_t;

void evlog_init(void);
void evlog(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* Copy up to max entries, oldest first. Returns count. */
int evlog_dump(evlog_entry_t *out, int max);

#ifdef __cplusplus
}
#endif
