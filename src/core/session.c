/* SPDX-License-Identifier: MIT */
/* Opening an archive: the format from its first bytes, then the engines in
 * preference order. The command line and the GUI worker both come through
 * here, so they choose engines and treat damage the same way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ua.h"

int ua_sniff_file(const char *archive, ua_format *fmt, LONG *ioerr)
{
    UBYTE *head;
    LONG hlen = 0;
    BPTR fh = Open((STRPTR)archive, MODE_OLDFILE);

    *fmt = UA_FMT_UNKNOWN;
    if (!fh) {
        *ioerr = IoErr();
        return -1;
    }
    head = malloc(0x8010);
    if (head)
        hlen = Read(fh, head, 0x8010);
    Close(fh);
    *fmt = ua_sniff(head, hlen < 0 ? 0 : hlen);
    free(head);
    return 0;
}

/* Falling back to another engine is allowed only here, before anything is
 * written; after extraction starts, an engine's failure is reported, not
 * retried with another one. In normal mode the next engine is tried only when
 * this one does not know the format: an archive it knows but finds damaged is
 * reported, and reading around the damage is recovery's job. */
static LONG slow_ticks = -1;

int ua_list_break(ua_list *l)
{
    if (slow_ticks < 0) {
        char v[16];
        slow_ticks = GetVar("UnarcTestSlowList", v, sizeof(v), 0) > 0 ? atoi(v) : 0;
    }
    if (slow_ticks && l->n % 100 == 0)
        Delay(slow_ticks);
    if (l->reading && l->n % 64 == 0)
        l->reading(l->reading_ctx, l->n);
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

int ua_read_list(const char *archive, ua_format fmt, ua_op op, int recover,
                 const char *engine, ua_list *l, ua_note note, void *ctx)
{
    const ua_engine *eng[4];
    int neng, i, got = -1, damaged = 0, have_fallback = 0;
    ua_list fallback;
    const ua_engine *fallback_engine = NULL;
    char why[200] = "";
    char err[256];
    void (*reading)(void *, ULONG) = l->reading;
    void *reading_ctx = l->reading_ctx;
    char **vols = l->volumes;
    const char *password = l->password;

    slow_ticks = -1;
    memset(l, 0, sizeof(*l));
    memset(&fallback, 0, sizeof(fallback));
    if (engine) {
        eng[0] = ua_engine_by_name(engine);
        if (!eng[0]) {
            note(ctx, "unknown engine \"%s\" (libarchive, xad, 7-zip)", engine);
            return UA_OPEN_ERROR;
        }
        neng = 1;
    } else {
        neng = ua_engines_for(fmt, op, recover, vols != NULL, eng, 4);
    }
    if (!neng)
        return UA_OPEN_NOENGINE;

    for (i = 0; i < neng; i++) {
        ua_list t;
        int r;
        memset(&t, 0, sizeof(t));
        t.reading = reading;
        t.reading_ctx = reading_ctx;
        t.volumes = vols;              /* borrowed while listing */
        t.password = password;
        r = eng[i]->list(archive, &t, err, sizeof(err));
        if (r == UA_LIST_CANCELLED) {
            ua_list_free(&t);
            ua_list_free(l);
            if (have_fallback)
                ua_list_free(&fallback);
            note(ctx, "Reading stopped by the user");
            return UA_OPEN_CANCELLED;
        }
        if (r == UA_LIST_PASSWORD) {
            int pw = t.pw_state;
            note(ctx, "Engine %s: %s", eng[i]->name, err);
            ua_list_free(&t);
            ua_list_free(l);
            if (have_fallback)
                ua_list_free(&fallback);
            l->pw_state = pw;
            l->password = password;
            l->volumes = vols;          /* still the caller's, for a retry */
            return UA_OPEN_PASSWORD;
        }
        if (r == UA_LIST_OK && recover && got >= 0 && l->n >= t.n) {
            note(ctx, "Engine %s: %lu entries; keeping %s with %lu", eng[i]->name,
                 (unsigned long)t.n, l->engine->name, (unsigned long)l->n);
            ua_list_free(&t);
            continue;
        }
        if (r == UA_LIST_OK) {
            ua_list_free(l);
            *l = t;
            l->engine = eng[i];
            got = 0;
            break;
        }
        note(ctx, "Engine %s: %s", eng[i]->name, err);
        if (r == UA_LIST_UNKNOWN) {
            ua_list_free(&t);
            continue;
        }
        if (r == UA_LIST_UNSUPPORTED) {
            /* Not damage: an engine that reads it may come next. The first
             * such list is kept in case none does. */
            if (!why[0])
                snprintf(why, sizeof(why), "%s", err);
            if (!have_fallback) {
                fallback = t;
                fallback_engine = eng[i];
                have_fallback = 1;
            } else {
                ua_list_free(&t);
            }
            continue;
        }
        if (!recover) {
            damaged = 1;
            if (r == UA_LIST_PARTIAL && op == UA_OP_LIST) {
                ua_list_free(l);
                *l = t;
                l->engine = eng[i];
                got = 1;
            } else {
                ua_list_free(&t);
            }
            break;
        }
        /* Recovery: keep the reading that found the most entries; on a tie the
         * engine earlier in the preference order (better names) stays. */
        if (r == UA_LIST_PARTIAL && (got < 0 || t.n > l->n)) {
            ua_list_free(l);
            *l = t;
            l->engine = eng[i];
            got = 1;
        } else {
            ua_list_free(&t);
        }
    }
    if (got < 0 && !damaged && have_fallback && fallback.n) {
        /* No engine reads all of it: the first one's list, as far as it goes
         * (encrypted entries are left out by the plan, entries in a method
         * it lacks fail one by one). */
        note(ctx, "Using %s's list: %s", fallback_engine->name, why);
        ua_list_free(l);
        *l = fallback;
        l->engine = fallback_engine;
        got = 0;
        have_fallback = 0;
    }
    if (have_fallback)
        ua_list_free(&fallback);
    l->password = password;
    if (got >= 0) {
        int nv = 1, k;
        if (vols)
            for (nv = 0; vols[nv]; nv++)
                ;
        l->stamps = calloc(nv, sizeof(*l->stamps));
        for (k = 0; l->stamps && k < nv; k++)
            if (!ua_stamp_file(vols ? vols[k] : archive, &l->stamps[k]))
                break;
        l->nstamps = l->stamps && k == nv ? nv : 0;
        l->format = fmt;
        l->partial = got == 1;
        l->volumes = NULL;
        l->volumes_owned = 0;
        if (vols) {
            int n = 0, k;
            while (vols[n])
                n++;
            l->volumes = calloc(n + 1, sizeof(char *));
            for (k = 0; l->volumes && k < n; k++)
                l->volumes[k] = ua_strdup(vols[k]);
            l->volumes_owned = l->volumes != NULL;
        }
    }
    if (damaged && got < 0)
        return UA_OPEN_DAMAGED;
    if (got < 0 && why[0]) {
        snprintf(l->why, sizeof(l->why), "%s", why);
        return UA_OPEN_UNSUPPORTED;
    }
    if (got < 0)
        return UA_OPEN_ERROR;
    return got == 1 ? UA_OPEN_PARTIAL : UA_OPEN_OK;
}
