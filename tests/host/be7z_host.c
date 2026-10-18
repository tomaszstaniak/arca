/* SPDX-License-Identifier: MIT */
/* Host test of the record handling in src/engines/be_7z.c: what the engine
 * makes of the unarc-7z protocol (PROTOCOL.md), with the program replaced by
 * a given output (be7z_fake.h). Covers the final R code after good entries,
 * Ctrl-C before the first header and in the middle of an entry's data, a
 * cancel from the file layer, changed archives and dates.
 *   cc -fsanitize=address,undefined -I tests/host/shim -I tests/host -o /tmp/zh tests/host/be7z_host.c && /tmp/zh */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../src/core/ua.h"

/* What the engine uses from the core, kept minimal. */
static int brk;
char *ua_strdup(const char *s) { return s ? strdup(s) : NULL; }
ua_entry *ua_list_add(ua_list *l)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, l->cap * sizeof(*l->v));
    }
    memset(&l->v[l->n], 0, sizeof(l->v[0]));
    return &l->v[l->n++];
}
int ua_list_break(ua_list *l) { (void)l; return brk; }
int ua_entry_matches(const ua_entry *e, const char *name, ua_etype type, int size_valid, UQUAD size)
{
    return e->name_src && !strcmp(e->name_src, name) && e->type == type &&
           !(e->size_valid && size_valid && e->size != size);
}
int ua_utf8_valid(const char *s)
{
    const UBYTE *p = (const UBYTE *)s;
    while (*p) {
        int n = *p < 0x80 ? 0 : (*p & 0xE0) == 0xC0 ? 1 : (*p & 0xF0) == 0xE0 ? 2 :
                (*p & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0)
            return 0;
        for (p++; n--; p++)
            if ((*p & 0xC0) != 0x80)
                return 0;
    }
    return 1;
}
static LONG gmtoff = -120;                 /* UTC+2 */
void ua_unix_to_datestamp(long long t, int utc, struct DateStamp *ds)
{
    long long s = t - 252460800LL - (utc ? gmtoff * 60LL : 0);
    ds->ds_Days = (LONG)(s / 86400);
    ds->ds_Minute = (LONG)((s % 86400) / 60);
    ds->ds_Tick = (LONG)((s % 60) * 50);
}

#define UA_7Z_HOST_TEST "be7z_fake.h"
#include "../../src/engines/be_7z.c"

/* ---- a file layer that records what it is told */
typedef struct rec {
    ua_sink sink;
    int cancel_at;             /* begin of this entry index (list position) cancels; -1 never */
    int cancel_data_after;     /* cancel once this many data bytes came; -1 never */
    char log[2048];
    unsigned long bytes;
} rec;

static void logf_(rec *r, const char *fmt, ...)
{
    va_list ap;
    size_t n = strlen(r->log);
    va_start(ap, fmt);
    vsnprintf(r->log + n, sizeof(r->log) - n, fmt, ap);
    va_end(ap);
}

static ua_wdecision r_begin(ua_sink *s, ua_entry *e)
{
    rec *r = (rec *)s;
    logf_(r, "B%lu ", (unsigned long)e->index);
    if ((int)e->index == r->cancel_at)
        return UA_W_CANCEL;
    return e->target ? UA_W_WRITE : UA_W_SKIP;
}
static int r_data(ua_sink *s, const void *b, ULONG n)
{
    rec *r = (rec *)s;
    (void)b;
    r->bytes += n;
    return r->cancel_data_after >= 0 && r->bytes >= (unsigned long)r->cancel_data_after;
}
static void r_end(ua_sink *s, ua_entry *e, int ok, const char *why)
{
    logf_((rec *)s, "E%lu:%s ", (unsigned long)e->index, ok ? "ok" : why ? why : "?");
}

static void sink_init(rec *r)
{
    memset(r, 0, sizeof(*r));
    r->sink.begin = r_begin;
    r->sink.data = r_data;
    r->sink.end = r_end;
    r->cancel_at = -1;
    r->cancel_data_after = -1;
}

static int fails;
static void check(const char *what, int ok, const char *detail)
{
    fails += !ok;
    printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, ok ? "" : ": ", ok ? "" : detail);
}

static void set_out(const char *s, size_t n) { fake_out = s; fake_len = n; }

/* a three-entry list: x.txt (5 bytes), d/ (dir), y.txt (3 bytes) */
static const char LIST[] =
    "A\t7z\t1\t-\n"
    "E\t0\t0\t5\t-\t1000000000\t20\t0\t782e747874\tu\n"
    "E\t1\t1\t0\t-\t1000000000\t10\t0\t64\tu\n"
    "E\t2\t0\t3\t-\t1000000000\t20\t0\t792e747874\tu\n"
    "R\t0\tok\n";

static void listed(ua_list *l)
{
    char err[200] = "";
    ULONG i;
    memset(l, 0, sizeof(*l));
    set_out(LIST, sizeof(LIST) - 1);
    z_list("a.7z", l, err, sizeof(err));
    for (i = 0; i < l->n; i++)
        l->v[i].target = l->v[i].name_src;
}

static int run_extract(const char *out, size_t n, rec *r, char *err, int errlen)
{
    ua_list l;
    int rc;
    listed(&l);
    set_out(out, n);
    err[0] = 0;
    rc = z_extract("a.7z", &l, &r->sink, err, errlen);
    return rc;
}

#define S(x) x, sizeof(x) - 1

int main(void)
{
    char err[200], d[600];
    ua_list l;
    rec r;
    int rc;

    /* ---- listing */
    memset(&l, 0, sizeof(l));
    set_out(S(LIST));
    rc = z_list("a.7z", &l, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d n %lu", rc, (unsigned long)l.n);
    check("list: three entries", rc == UA_LIST_OK && l.n == 3 && !strcmp(l.v[2].name_src, "y.txt") &&
          l.v[1].type == UA_ET_DIR, d);
    /* 1000000000 = 2001-09-09 01:46:40 UTC; at UTC+2 that is 03:46 local. */
    snprintf(d, sizeof(d), "minute %ld", (long)l.v[0].date.ds_Minute);
    check("list: 7z time (UTC) shifted to local time", l.v[0].date.ds_Minute == 3 * 60 + 46, d);
    {
        static const char rar4[] = "A\tRar\t1\t-\nE\t0\t0\t1\t-\t1000000000\t20\t0\t61\tl\nR\t0\tok\n";
        ua_list l4;
        memset(&l4, 0, sizeof(l4));
        set_out(S(rar4));
        z_list("a.rar", &l4, err, sizeof(err));
        snprintf(d, sizeof(d), "minute %ld", (long)l4.v[0].date.ds_Minute);
        check("list: RAR 4 time (stored local time) taken as it is",
              l4.n == 1 && l4.v[0].date.ds_Minute == 1 * 60 + 46, d);
    }
    {
        static const char zipn[] = "A\tzip\t1\t-\nE\t0\t0\t1\t-\t1000000000\t-\t0\t4772816e2e747874\tl\n"
                                   "E\t1\t0\t1\t-\t1000000000\t-\t0\t47726e2e747874\tu\nR\t0\tok\n";
        ua_list lz;
        memset(&lz, 0, sizeof(lz));
        set_out(S(zipn));
        z_list("a.zip", &lz, err, sizeof(err));
        check("list: a ZIP name that is not UTF-8 is CP437, a UTF-8 one UTF-8",
              lz.n == 2 && lz.v[0].name_kind == UA_NAME_CP437 && lz.v[1].name_kind == UA_NAME_UTF8, "");
        snprintf(d, sizeof(d), "minutes %ld %ld", (long)lz.v[0].date.ds_Minute, (long)lz.v[1].date.ds_Minute);
        check("list: a ZIP DOS time (l) as stored, a UTC one (u) shifted",
              lz.v[0].date.ds_Minute == 1 * 60 + 46 && lz.v[1].date.ds_Minute == 3 * 60 + 46, d);
    }
    {
        static const char brk0[] = "A\t7z\t1\t-\n~BREAK\n";
        ua_list lb;
        memset(&lb, 0, sizeof(lb));
        set_out(S(brk0));
        rc = z_list("a.7z", &lb, err, sizeof(err));
        snprintf(d, sizeof(d), "rc %d (%s)", rc, err);
        check("list: Ctrl-C before the first entry, program ends without R: cancelled",
              rc == UA_LIST_CANCELLED, d);
    }

    /* ---- extraction */
    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n"
                       "H\t1\t1\t0\t64\nD\t1\t0\nF\t1\t0\tok\n"
                       "H\t2\t0\t3\t792e747874\nD\t2\t3\nabcF\t2\t0\tok\nR\t0\tok\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d log %s", rc, r.log);
    check("extract: all entries, R 0", rc == 0 && strstr(r.log, "E2:ok") && r.bytes == 8, d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n"
                       "H\t1\t1\t0\t64\nD\t1\t0\nF\t1\t0\tok\n"
                       "H\t2\t0\t3\t792e747874\nD\t2\t3\nabcF\t2\t0\tok\nR\t3\tdata error\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: every entry answered 0, then R 3 for the job: an error", rc == -1 &&
          strstr(err, "data error"), d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n"
                       "H\t1\t1\t0\t64\nD\t1\t0\nF\t1\t0\tok\n"
                       "H\t2\t0\t3\t792e747874\nF\t2\t2\twrong password\nR\t2\twrong password\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: R 2 after the entry that failed: that entry's error, job done",
          rc == 0 && strstr(r.log, "E2:wrong password"), d);

    sink_init(&r);
    fake_pokes = 0;
    rc = run_extract(S("~BREAK\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: Ctrl-C before the first header, no R: cancelled, nothing begun",
          rc == 1 && !strstr(r.log, "E"), d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhe~BREAK\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: Ctrl-C in an entry's data (solid decoding), output cut: cancelled",
          rc == 1 && strstr(r.log, "E0:cancelled"), d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n~BREAK\nR\t8\tcancelled\n"),
                     &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: Ctrl-C between entries, R 8: cancelled", rc == 1, d);

    sink_init(&r);
    r.cancel_at = 2;
    fake_pokes = 0;
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n"
                       "H\t1\t1\t0\t64\nD\t1\t0\nF\t1\t0\tok\n"
                       "H\t2\t0\t3\t792e747874\nD\t2\t3\nabcF\t2\t0\tok\nR\t0\tok\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d pokes %d log %s", rc, fake_pokes, r.log);
    check("extract: the file layer cancels at an entry: cancelled, the program told once",
          rc == 1 && fake_pokes == 1 && r.bytes == 5, d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\n"
                       "H\t2\t0\t3\t7a2e747874\nD\t2\t3\nabcF\t2\t0\tok\nR\t0\tok\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: a header that is not the listed entry: stopped before its data",
          rc == -1 && strstr(err, "changed") && r.bytes == 5, d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhelloF\t0\t0\tok\nR\t0\tok\n"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: R 0 but a wanted entry never answered: an error", rc == -1, d);

    sink_init(&r);
    rc = run_extract(S("H\t0\t0\t5\t782e747874\nD\t0\t5\nhel"), &r, err, sizeof(err));
    snprintf(d, sizeof(d), "rc %d err '%s' log %s", rc, err, r.log);
    check("extract: output ends in an entry without Ctrl-C: an error", rc == -1 &&
          strstr(r.log, "E0:the data ended early"), d);

    printf("%d failure(s)\n", fails);
    return fails != 0;
}
