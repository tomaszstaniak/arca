/* SPDX-License-Identifier: MIT */
/* Archive times to AROS file dates. AROS keeps dates in local time; the
 * locale's GMT offset is the time zone. Formats that store UTC (7z, RAR 5,
 * ZIP's extra fields, tar) are shifted by it; formats that store local time
 * (RAR 1.5-4, ZIP's DOS field when no extra field says otherwise, as the
 * engines report it) are taken as they are. Every engine and the packer use
 * this one offset, so the same archive gives the same dates whichever engine
 * reads it. */
#include <proto/exec.h>
#include <proto/locale.h>
#include <libraries/locale.h>
#include "ua.h"

LONG ua_gmtoff_minutes(void)
{
    static LONG off = 0x7fffffff;
    if (off == 0x7fffffff) {
        struct Library *LocaleBase = OpenLibrary("locale.library", 0);
        off = 0;
        if (LocaleBase) {
            struct Locale *loc = OpenLocale(NULL);
            if (loc) {
                off = loc->loc_GMTOffset;      /* minutes west of GMT */
                CloseLocale(loc);
            }
            CloseLibrary(LocaleBase);
        }
    }
    return off;
}

/* t: seconds since 1970. utc: t is UTC (shift to local); else t is already
 * the local wall-clock time, counted as if it were UTC. */
void ua_unix_to_datestamp(long long t, int utc, struct DateStamp *ds)
{
    long long s = t - 252460800LL;                 /* 1978-01-01 */
    if (utc)
        s -= (long long)ua_gmtoff_minutes() * 60;
    if (s < 0)
        s = 0;
    ds->ds_Days = (LONG)(s / 86400);
    ds->ds_Minute = (LONG)((s % 86400) / 60);
    ds->ds_Tick = (LONG)((s % 60) * TICKS_PER_SECOND);
}
