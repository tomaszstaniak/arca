/* SPDX-License-Identifier: MIT */
/* Opening an archive: the format from its first bytes, then the engines in
 * preference order. The command line and the GUI worker both come through
 * here, so they choose engines and treat damage the same way. */
#include <stdlib.h>
#include <string.h>
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
int ua_read_list(const char *archive, ua_format fmt, ua_op op, int recover,
                 const char *engine, ua_list *l, ua_note note, void *ctx)
{
    const ua_engine *eng[4];
    int neng, i, got = -1, damaged = 0;
    char err[256];

    memset(l, 0, sizeof(*l));
    if (engine) {
        eng[0] = ua_engine_by_name(engine);
        if (!eng[0]) {
            note(ctx, "unknown engine \"%s\" (libarchive, xad)", engine);
            return UA_OPEN_ERROR;
        }
        neng = 1;
    } else {
        neng = ua_engines_for(fmt, op, recover, eng, 4);
    }
    if (!neng)
        return UA_OPEN_NOENGINE;

    for (i = 0; i < neng; i++) {
        ua_list t;
        int r;
        memset(&t, 0, sizeof(t));
        r = eng[i]->list(archive, &t, err, sizeof(err));
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
    if (got >= 0)
        l->format = fmt;
    if (damaged && got < 0)
        return UA_OPEN_DAMAGED;
    if (got < 0)
        return UA_OPEN_ERROR;
    return got == 1 ? UA_OPEN_PARTIAL : UA_OPEN_OK;
}
