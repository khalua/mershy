// Wall-clock time via SNTP (the board has no battery-backed RTC), in the
// LOCAL_TIMEZONE from app_config.h.
#ifndef TIME_SYNC_H
#define TIME_SYNC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Starts background SNTP sync. Call after the network stack is up; it keeps
// retrying until WiFi connects and re-syncs periodically.
void time_sync_start(void);

// Formats the local time like "Thursday, September 25 2026, 3:42 PM".
// Returns false (and leaves `out` empty) until the first sync succeeds.
bool time_sync_now(char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif  // TIME_SYNC_H
