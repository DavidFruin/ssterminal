#ifndef TUI_TIMEFMT_H
#define TUI_TIMEFMT_H

#include <stddef.h>

/* Server timestamps are UTC, "2026-04-13 22:17:00".
 * Under 24h old renders relative ("2h", "15m"); older renders as a UTC date. */

void timefmt_short(const char *ts, char *out, size_t n);  /* list columns */
void timefmt_full(const char *ts, char *out, size_t n);   /* "UTC 2026-04-13 22:17" */
/* timefmt_full plus the creator's offset from GMT when known:
 * "UTC 2026-04-13 22:17 (GMT-06:00)". */
void timefmt_full_tz(const char *ts, const char *tz, char *out, size_t n);

#endif
