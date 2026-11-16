#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "timefmt.h"

#define RELATIVE_CUTOFF (24 * 60 * 60)

/* The server's times are UTC ("2026-04-13 22:17:00"). Every absolute time
 * is shown in UTC and says so; only the "how long ago" form is zone-free. */

/* The follows/followers endpoints return the literal string "Unknown"
 * for relationships that predate the server tracking timestamps. */
static int is_unknown(const char *ts) {
    return !ts || !*ts || strcmp(ts, "Unknown") == 0;
}

static int parse_ts(const char *ts, time_t *out) {
    if (is_unknown(ts)) return -1;
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!strptime(ts, "%Y-%m-%d %H:%M:%S", &tm)) return -1;
    time_t t = timegm(&tm);
    if (t == (time_t)-1) return -1;
    *out = t;
    return 0;
}

void timefmt_short(const char *ts, char *out, size_t n) {
    if (is_unknown(ts)) { out[0] = '\0'; return; }

    time_t t;
    if (parse_ts(ts, &t) != 0) {
        snprintf(out, n, "%s", ts);
        return;
    }

    double diff = difftime(time(NULL), t);
    if (diff < 0) diff = 0;

    if (diff < RELATIVE_CUTOFF) {
        int secs = (int)diff;
        if (secs < 60)        snprintf(out, n, "%ds", secs);
        else if (secs < 3600) snprintf(out, n, "%dm", secs / 60);
        else                  snprintf(out, n, "%dh", secs / 3600);
    } else {
        struct tm ut;
        gmtime_r(&t, &ut);
        strftime(out, n, "UTC %Y-%m-%d", &ut);
    }
}

void timefmt_full(const char *ts, char *out, size_t n) {
    if (is_unknown(ts)) { snprintf(out, n, "unknown"); return; }

    time_t t;
    if (parse_ts(ts, &t) != 0) {
        snprintf(out, n, "%s", ts);
        return;
    }
    struct tm ut;
    gmtime_r(&t, &ut);
    strftime(out, n, "UTC %Y-%m-%d %H:%M", &ut);
}

void timefmt_full_tz(const char *ts, const char *tz, char *out, size_t n) {
    char base[48];
    timefmt_full(ts, base, sizeof(base));
    if (tz && tz[0] && !is_unknown(ts)) snprintf(out, n, "%s (GMT%s)", base, tz);
    else snprintf(out, n, "%s", base);
}
