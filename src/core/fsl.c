/* SPDX-License-Identifier: MIT */
/* The AROS file layer: the only code that writes extracted data to disk.
 *
 * Every file is written under a temporary name in its target directory and
 * renamed into place when complete, so a cancelled or failed entry never leaves
 * a half-written file under the real name. Existing files are never replaced
 * without a decision from the conflict policy, and a replacement keeps the old
 * file until the new one is in place: old -> backup name, new -> real name,
 * backup deleted; any failure on the way renames the backup back. Protection
 * bits, comments and dates are applied after the rename, because the data
 * write would reset them.
 *
 * DOS is only ever given one path component, relative to a lock on its
 * directory. Long absolute paths are not safe on AROS: a 1 600-character path
 * made CreateDir create a directory "leve" in the wrong place, and a
 * 500-character one failed with "object already exists" (ABIv11 2026.09,
 * RAM:). Full paths are built only for messages. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include "ua.h"

typedef struct fsl {
    ua_sink sink;
    ua_plan *plan;
    ua_extract_opts *o;
    ua_report *r;
    ua_conflict sticky;      /* set once the user answers "for all" */
    int have_sticky;

    BPTR root;               /* lock on plan->root */
    BPTR dir;                /* lock on the directory of the current entry */
    char dir_rel[UA_PATHMAX];/* its path relative to root ("" = root) */
    int dir_valid;

    BPTR fh;
    UQUAD ebytes;            /* bytes written for the current entry */
    UQUAD ticked;            /* r->bytes at the last tick */
    ua_entry *cur;           /* entry being written */
    char leaf[256];          /* final name in dir */
    char tmp[40];            /* temporary name in dir */
    char shown[UA_PATHMAX];  /* full path, for messages only */
    int overwrite;
    ULONG tmpseq;
    int cancel;              /* user break or CANCELAFTER */
    int write_failed;        /* stop: the disk refused data */
    LONG write_err;
    int root_created;
    int name_max;            /* longest name the volume takes safely; 0 = no limit known */
    /* Directories whose name had to change (shortened for the volume, or
     * moved aside because a shortened one took their name): archive path ->
     * path on disk, both relative to the root. Every entry under such a
     * directory goes through the same mapping, so two directories that only
     * differ after the limit never share one directory on disk. */
    struct dmap { char *from, *to; } *dmap;
    ULONG ndmap, capdmap;
} fsl;

static const char *dmap_find(fsl *f, const char *from)
{
    ULONG i;
    for (i = 0; i < f->ndmap; i++)
        if (strcmp(f->dmap[i].from, from) == 0)
            return f->dmap[i].to;
    return NULL;
}

/* Is 'to' (a path on disk) already the mapped home of another directory? */
static int dmap_taken(fsl *f, const char *to)
{
    ULONG i;
    for (i = 0; i < f->ndmap; i++)
        if (strcasecmp(f->dmap[i].to, to) == 0)
            return 1;
    return 0;
}

static int dmap_add(fsl *f, const char *from, const char *to)
{
    if (f->ndmap == f->capdmap) {
        ULONG c = f->capdmap ? f->capdmap * 2 : 16;
        struct dmap *n = realloc(f->dmap, c * sizeof(*n));
        if (!n)
            return 0;
        f->dmap = n;
        f->capdmap = c;
    }
    f->dmap[f->ndmap].from = ua_strdup(from);
    f->dmap[f->ndmap].to = ua_strdup(to);
    if (!f->dmap[f->ndmap].from || !f->dmap[f->ndmap].to) {
        free(f->dmap[f->ndmap].from);
        free(f->dmap[f->ndmap].to);
        return 0;
    }
    f->ndmap++;
    return 1;
}

static void dmap_free(fsl *f)
{
    ULONG i;
    for (i = 0; i < f->ndmap; i++) {
        free(f->dmap[i].from);
        free(f->dmap[i].to);
    }
    free(f->dmap);
    f->dmap = NULL;
    f->ndmap = f->capdmap = 0;
}

/* name cut to max characters with " k" (k > 1) kept inside the limit; with
 * keep_ext a short extension stays at the end. */
static void shortened(const char *name, int k, int max, int keep_ext, char *out, size_t n)
{
    char ext[16] = "", tail[24];
    const char *dot = keep_ext ? strrchr(name, '.') : NULL;
    size_t nl = strlen(name), room;
    if (dot && dot != name && strlen(dot) <= 8)
        snprintf(ext, sizeof(ext), "%s", dot);
    else
        dot = NULL;
    if (k > 1)
        snprintf(tail, sizeof(tail), " %d%s", k, ext);
    else
        snprintf(tail, sizeof(tail), "%s", ext);
    room = max > 0 && (size_t)max > strlen(tail) ? (size_t)max - strlen(tail) : 0;
    if (max <= 0)
        room = nl;
    if (room > nl - strlen(ext))
        room = nl - strlen(ext);
    snprintf(out, n, "%.*s%s", (int)room, name, tail);
}

static void msg(fsl *f, const char *fmt, ...)
{
    char buf[UA_PATHMAX + 200];
    va_list ap;
    if (!f->o->message)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    f->o->message(f->o->ui_ctx, "%s", buf);
}

static void why_now(char *why, int n)
{
    Fault(IoErr(), NULL, why, n);
}

/* For messages only; a cut message is still a correct message. */
static void show(fsl *f, char *out, size_t sz, const char *rel)
{
    size_t n = strlen(f->plan->root);
    const char *sep = (!*rel || (n && (f->plan->root[n - 1] == ':' ||
                                       f->plan->root[n - 1] == '/'))) ? "" : "/";
    snprintf(out, sz, "%s%s%s", f->plan->root, sep, rel);
}

/* 1 = file, 2 = directory, 0 = nothing; name relative to dir. */
static int what_is_rel(BPTR dir, const char *name, char *stored, size_t sn)
{
    BPTR old = CurrentDir(dir), l = Lock((STRPTR)name, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int kind = 0;
    CurrentDir(old);
    if (stored && sn)
        stored[0] = 0;
    if (!l)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(l, fib)) {
        kind = fib->fib_DirEntryType > 0 ? 2 : 1;
        if (stored && sn)
            snprintf(stored, sn, "%s", fib->fib_FileName);
    } else {
        kind = 1;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return kind;
}

static int what_is_abs(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int kind = 0;
    if (!l)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    kind = (fib && Examine(l, fib) && fib->fib_DirEntryType > 0) ? 2 : 1;
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return kind;
}

/* ASCII-only case folding is enough here: this compares a name with the same
 * name as the volume stored it. */
static int same_name(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* Some filesystems shorten long names without an error. Compare what the
 * volume stored with what was asked for, so that it is reported, not silent. */
static void check_stored(fsl *f, BPTR dir, const char *name, const char *rel)
{
    char stored[256];
    if (what_is_rel(dir, name, stored, sizeof(stored)) && stored[0] && !same_name(stored, name)) {
        char p[UA_PATHMAX];
        show(f, p, sizeof(p), rel);
        msg(f, "warning: the volume stored \"%s\" as \"%s\"", p, stored);
        f->r->renamed++;
    }
}

/* OFS and FFS volumes (DOS\0 to DOS\5) store 30 characters. Longer names must
 * not reach them: on ABIv11 2026.09 afs-handler crashes ("Illegal address
 * access") when a file with a 60-character name is created, even from the
 * Shell (Echo x >ADF:<60 chars>). Shorten here, keep the extension, keep the
 * name unique in its directory, and say so. */
static void fit_name(fsl *f, BPTR dir, char *name, size_t cap, const char *rel_shown)
{
    size_t n = strlen(name);
    char ext[16] = "", base[256], cand[256];
    const char *dot;
    int k;

    if (!f->name_max || n <= (size_t)f->name_max)
        return;
    dot = strrchr(name, '.');
    if (dot && dot != name && strlen(dot) <= 8)
        snprintf(ext, sizeof(ext), "%s", dot);
    snprintf(base, sizeof(base), "%.*s", (int)(n - strlen(ext)), name);
    for (k = 1; k < 100; k++) {
        char tail[24];
        size_t room;
        if (k == 1)
            snprintf(tail, sizeof(tail), "%s", ext);
        else
            snprintf(tail, sizeof(tail), " %d%s", k, ext);
        room = (size_t)f->name_max - strlen(tail);
        snprintf(cand, sizeof(cand), "%.*s%s", (int)room, base, tail);
        if (!dir || !what_is_rel(dir, cand, NULL, 0))
            break;
    }
    msg(f, "name shortened for this volume (%d characters at most): \"%s\" -> \"%s\"",
        f->name_max, rel_shown, cand);
    snprintf(name, cap, "%s", cand);
    f->r->renamed++;
}

/* Lock on root/rel, creating missing directories when asked. Cached: entries
 * of one directory usually come together. Returns BNULL on failure (reported). */
static BPTR dir_lock(fsl *f, const char *rel, int create)
{
    BPTR cur;
    const char *p = rel;
    char sofar[UA_PATHMAX];  /* the path on disk so far */
    char orig[UA_PATHMAX];   /* the same path as the archive names it */
    size_t sl = 0, ol = 0;

    if (f->dir_valid && strcmp(f->dir_rel, rel) == 0)
        return f->dir;
    if (f->dir_valid) {
        UnLock(f->dir);
        f->dir_valid = 0;
    }
    if (strlen(rel) >= sizeof(f->dir_rel)) {
        msg(f, "error: path too long (%lu characters): %.80s...", (unsigned long)strlen(rel), rel);
        return BNULL;
    }
    cur = DupLock(f->root);
    sofar[0] = 0;
    orig[0] = 0;
    while (cur && *p) {
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        char comp[256];
        BPTR old, next;
        int kind;

        if (n == 0 || n >= sizeof(comp)) {
            msg(f, "error: name too long (%lu characters) in %.80s...", (unsigned long)n, rel);
            UnLock(cur);
            return BNULL;
        }
        memcpy(comp, p, n);
        comp[n] = 0;
        snprintf(orig + ol, sizeof(orig) - ol, "%s%s", ol ? "/" : "", comp);
        ol = strlen(orig);
        {
            const char *m = dmap_find(f, orig);
            char want[UA_PATHMAX];
            if (m) {
                /* Mapped before: the same directory as the earlier entries. */
                const char *leafm = strrchr(m, '/');
                snprintf(comp, sizeof(comp), "%s", leafm ? leafm + 1 : m);
            } else {
                int too_long = f->name_max && n > (size_t)f->name_max;
                snprintf(want, sizeof(want), "%s%s%s", sofar, sl ? "/" : "", comp);
                if (too_long || dmap_taken(f, want)) {
                    /* A new name that neither an earlier mapped directory nor
                     * anything on the volume has. */
                    char cand[256], s2[UA_PATHMAX];
                    int k;
                    for (k = 1; k < 1000; k++) {
                        shortened(comp, k, too_long ? f->name_max :
                                  (f->name_max ? f->name_max : 255), 0, cand, sizeof(cand));
                        if (!too_long && k == 1)
                            continue;
                        snprintf(want, sizeof(want), "%s%s%s", sofar, sl ? "/" : "", cand);
                        if (!dmap_taken(f, want) && what_is_rel(cur, cand, NULL, 0) == 0)
                            break;
                    }
                    show(f, s2, sizeof(s2), orig);
                    msg(f, "folder name changed for this volume: \"%s\" -> \"%s\"", s2, cand);
                    f->r->renamed++;
                    snprintf(comp, sizeof(comp), "%s", cand);
                    if (!dmap_add(f, orig, want)) {
                        msg(f, "error: out of memory");
                        UnLock(cur);
                        return BNULL;
                    }
                }
            }
        }
        snprintf(sofar + sl, sizeof(sofar) - sl, "%s%s", sl ? "/" : "", comp);
        sl = strlen(sofar);

        kind = what_is_rel(cur, comp, NULL, 0);
        if (kind == 1) {
            char s[UA_PATHMAX];
            show(f, s, sizeof(s), sofar);
            msg(f, "error: \"%s\" exists as a file, cannot create a directory there", s);
            UnLock(cur);
            return BNULL;
        }
        if (kind == 0) {
            BPTR made = BNULL;
            if (create) {
                old = CurrentDir(cur);
                made = CreateDir((STRPTR)comp);
                CurrentDir(old);
            }
            if (!made) {
                char s[UA_PATHMAX], why[100];
                why_now(why, sizeof(why));
                show(f, s, sizeof(s), sofar);
                msg(f, "error: cannot create \"%s\": %s", s, why);
                UnLock(cur);
                return BNULL;
            }
            UnLock(made);
            f->r->dirs++;
            check_stored(f, cur, comp, sofar);
        }
        old = CurrentDir(cur);
        next = Lock((STRPTR)comp, SHARED_LOCK);
        CurrentDir(old);
        UnLock(cur);
        cur = next;
        if (!cur) {
            char s[UA_PATHMAX], why[100];
            why_now(why, sizeof(why));
            show(f, s, sizeof(s), sofar);
            msg(f, "error: cannot open \"%s\": %s", s, why);
            return BNULL;
        }
        p = slash ? slash + 1 : p + n;
    }
    if (!cur)
        return BNULL;
    f->dir = cur;
    snprintf(f->dir_rel, sizeof(f->dir_rel), "%s", rel);
    f->dir_valid = 1;
    return cur;
}

/* Split "a/b/c" into dir "a/b" and leaf "c". */
static int split(const char *rel, char *dir, size_t dn, char *leaf, size_t ln)
{
    const char *slash = strrchr(rel, '/');
    size_t d = slash ? (size_t)(slash - rel) : 0;
    const char *l = slash ? slash + 1 : rel;
    if (d >= dn || strlen(l) >= ln)
        return 0;
    memcpy(dir, rel, d);
    dir[d] = 0;
    strcpy(leaf, l);
    return 1;
}

static int check_cancel(fsl *f)
{
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        f->cancel = 1;
    if (f->o->cancel_after && f->r->bytes >= f->o->cancel_after)
        f->cancel = 1;
    return f->cancel;
}

/* "name.ext" -> "name 2.ext" */
static void numbered(const char *leaf, int k, char *out, size_t sz)
{
    const char *dot = strrchr(leaf, '.');
    if (!dot || dot == leaf)
        dot = leaf + strlen(leaf);
    snprintf(out, sz, "%.*s %d%s", (int)(dot - leaf), leaf, k, dot);
}

static void meta_warn(fsl *f, const char *what, const char *shown)
{
    char why[100];
    why_now(why, sizeof(why));
    msg(f, "warning: %s not set on \"%s\": %s", what, shown, why);
}

/* name relative to dir */
static void apply_meta(fsl *f, BPTR dir, const char *name, const char *shown, ua_entry *e)
{
    BPTR old = CurrentDir(dir);
    if (e->comment && *e->comment && !SetComment((STRPTR)name, (STRPTR)e->comment))
        meta_warn(f, "comment", shown);
    if (e->prot_valid && !SetProtection((STRPTR)name, e->prot))
        meta_warn(f, "protection", shown);
    if (e->date_valid && !SetFileDate((STRPTR)name, &e->date))
        meta_warn(f, "date", shown);
    CurrentDir(old);
}

/* After a directory could not be opened or made: is the destination itself
 * still there? If not (volume removed, drawer deleted), every further entry
 * would fail the same way, so the job stops with one clear reason. */
static int root_gone(fsl *f)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int gone = fib && !Examine(f->root, fib);
    FreeDosObject(DOS_FIB, fib);
    if (gone) {
        f->r->failed = 1;
        snprintf(f->r->why, sizeof(f->r->why), "the destination is no longer available");
    }
    return gone;
}

static ua_wdecision s_begin(ua_sink *s, ua_entry *e)
{
    fsl *f = s->priv;
    char dirrel[UA_PATHMAX];
    BPTR dir, old;

    if (check_cancel(f))
        return UA_W_CANCEL;
    /* A file that did not arrive intact means the volume cannot be trusted
     * for the rest of the job. */
    if (f->r->failed)
        return UA_W_CANCEL;
    if (e->excluded)
        return UA_W_SKIP;
    if (e->skip || !e->target) {
        f->r->skipped++;
        return UA_W_SKIP;
    }
    if (e->type == UA_ET_DIR) {
        if (*e->target && !dir_lock(f, e->target, 1)) {
            f->r->errors++;
            if (root_gone(f))
                return UA_W_CANCEL;
        }
        return UA_W_SKIP;    /* metadata after all files, see ua_extract */
    }
    if (!split(e->target, dirrel, sizeof(dirrel), f->leaf, sizeof(f->leaf))) {
        msg(f, "error: path too long (%lu characters): %.80s...",
            (unsigned long)strlen(e->target), e->target);
        f->r->errors++;
        return UA_W_SKIP;
    }
    if (!(dir = dir_lock(f, dirrel, 1))) {
        f->r->errors++;
        return root_gone(f) ? UA_W_CANCEL : UA_W_SKIP;
    }
    show(f, f->shown, sizeof(f->shown), e->target);
    if (f->name_max && strlen(f->leaf) > (size_t)f->name_max) {
        char rel[UA_PATHMAX];
        fit_name(f, dir, f->leaf, sizeof(f->leaf), f->shown);
        snprintf(rel, sizeof(rel), "%s%s%s", dirrel, *dirrel ? "/" : "", f->leaf);
        show(f, f->shown, sizeof(f->shown), rel);
    }
    f->overwrite = 0;

    switch (what_is_rel(dir, f->leaf, NULL, 0)) {
    case 2:
        msg(f, "error: \"%s\" is a directory; file not written", f->shown);
        f->r->errors++;
        return UA_W_SKIP;
    case 1: {
        int choice = f->have_sticky ? (int)f->sticky : (int)f->o->conflict;
        if (choice == UA_CONFLICT_ASK) {
            int all = 0;
            choice = f->o->ask ? f->o->ask(f->o->ask_ctx, f->shown, e, &all) : UA_CONFLICT_SKIP;
            if (choice < 0) {
                f->cancel = 1;
                return UA_W_CANCEL;
            }
            if (all) {
                f->sticky = choice;
                f->have_sticky = 1;
            }
        }
        if (choice == UA_CONFLICT_SKIP) {
            msg(f, "skipped (exists): %s", f->shown);
            f->r->skipped++;
            return UA_W_SKIP;
        }
        if (choice == UA_CONFLICT_RENAME) {
            char alt[256], rel[UA_PATHMAX];
            int k;
            for (k = 2; k < 1000; k++) {
                numbered(f->leaf, k, alt, sizeof(alt));
                if (!what_is_rel(dir, alt, NULL, 0))
                    break;
            }
            msg(f, "renamed (exists): %s -> %s", f->shown, alt);
            snprintf(f->leaf, sizeof(f->leaf), "%s", alt);
            snprintf(rel, sizeof(rel), "%s%s%s", dirrel, *dirrel ? "/" : "", alt);
            show(f, f->shown, sizeof(f->shown), rel);
            f->r->renamed++;
        } else {
            /* Find out now, not after writing the data, whether the old file
             * can be moved at all: one held open by another program cannot. */
            BPTR x;
            old = CurrentDir(dir);
            x = Lock((STRPTR)f->leaf, EXCLUSIVE_LOCK);
            CurrentDir(old);
            if (!x) {
                char why[100];
                why_now(why, sizeof(why));
                msg(f, "error: cannot replace \"%s\": %s; old file kept", f->shown, why);
                f->r->errors++;
                return UA_W_SKIP;
            }
            UnLock(x);
            f->overwrite = 1;
        }
        break;
    }
    default:
        break;
    }

    /* Temporary file in the target directory, so the final Rename stays on one
     * volume. Short enough for FFS's 30-character names. */
    snprintf(f->tmp, sizeof(f->tmp), ".uatmp%lx.%lu",
             (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF), (unsigned long)++f->tmpseq);
    old = CurrentDir(dir);
    f->fh = Open((STRPTR)f->tmp, MODE_NEWFILE);
    CurrentDir(old);
    f->ebytes = 0;
    f->cur = e;
    if (!f->fh) {
        char why[100];
        why_now(why, sizeof(why));
        msg(f, "error: cannot write \"%s\": %s", f->shown, why);
        f->r->errors++;
        return UA_W_SKIP;
    }
    return UA_W_WRITE;
}

static int s_data(ua_sink *s, const void *buf, ULONG len)
{
    fsl *f = s->priv;
    if (Write(f->fh, (APTR)buf, len) != (LONG)len) {
        /* Engines only know "stop"; remember why, so a full disk is not
         * reported as a cancel. */
        f->write_failed = 1;
        f->write_err = IoErr();
        return 1;
    }
    f->r->bytes += len;
    f->ebytes += len;
    if (f->o->tick && (f->r->bytes - f->ticked) >= 65536) {
        f->ticked = f->r->bytes;
        f->o->tick(f->o->ui_ctx, f->cur, f->r->bytes);
    }
    return check_cancel(f);
}

static int s_seek(ua_sink *s, UQUAD skip)
{
    static const char zero[4096];
    while (skip) {
        ULONG n = skip > sizeof(zero) ? sizeof(zero) : (ULONG)skip;
        int rc = s_data(s, zero, n);
        if (rc)
            return rc;
        skip -= n;
    }
    return 0;
}

/* Is name (relative to dir) a file of exactly 'want' bytes? Return codes are
 * not enough: on an FFS test disk (afs-handler, ABIv11 2026.09) the first file
 * written into a just-created directory once stayed as an empty temporary file
 * while Open, Write, Close and Rename had all reported success. The size is
 * compared in full: ABIv11's fib_Size is 64 bits (UA_FIB_SIZE). */
static int written_ok(fsl *f, const char *name, UQUAD want, char *why, size_t wn)
{
    BPTR old = CurrentDir(f->dir), l = Lock((STRPTR)name, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int ok = 0;
    CurrentDir(old);
    if (!l) {
        snprintf(why, wn, "it is not on the volume after writing it");
        return 0;
    }
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(l, fib))
        snprintf(why, wn, "it cannot be examined after writing it");
    else if (fib->fib_DirEntryType > 0)
        snprintf(why, wn, "a directory has its name after writing it");
    else if (UA_FIB_SIZE(fib) != want)
        snprintf(why, wn, "the volume holds %llu bytes of %llu written",
                 (unsigned long long)UA_FIB_SIZE(fib), (unsigned long long)want);
    else
        ok = 1;
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return ok;
}

static void s_end(ua_sink *s, ua_entry *e, int ok, const char *err)
{
    fsl *f = s->priv;
    BPTR old;
    char why[100], bad[120];

    if (!f->fh)
        return;
    if (!Close(f->fh))
        ok = 0;
    f->fh = BNULL;
    old = CurrentDir(f->dir);
    if (!ok) {
        DeleteFile((STRPTR)f->tmp);
        CurrentDir(old);
        if (f->write_failed) {
            Fault(f->write_err, NULL, why, sizeof(why));
            msg(f, "error: %s: %s", f->shown, why);
            snprintf(f->r->why, sizeof(f->r->why), "%s", why);
            f->r->errors++;
        } else if (!f->cancel) {
            msg(f, "error: %s: %s", f->shown, err ? err : "write failed");
            f->r->errors++;
        }
        return;
    }
    if (f->overwrite) {
        /* Old file to a backup name first. It is deleted only once the new file
         * is under the real name and checked; until then every failure puts it
         * back. Same directory, so these renames need no space. */
        char bak[40];
        snprintf(bak, sizeof(bak), ".uabak%lx.%lu",
                 (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF), (unsigned long)f->tmpseq);
        if (!Rename((STRPTR)f->leaf, (STRPTR)bak)) {
            why_now(why, sizeof(why));
            DeleteFile((STRPTR)f->tmp);
            CurrentDir(old);
            msg(f, "error: cannot replace \"%s\": %s; old file kept", f->shown, why);
            f->r->errors++;
            return;
        }
        if (!Rename((STRPTR)f->tmp, (STRPTR)f->leaf)) {
            int back;
            why_now(why, sizeof(why));
            back = Rename((STRPTR)bak, (STRPTR)f->leaf);
            DeleteFile((STRPTR)f->tmp);
            CurrentDir(old);
            if (back)
                msg(f, "error: cannot replace \"%s\": %s; old file kept", f->shown, why);
            else
                msg(f, "error: cannot replace \"%s\": %s; THE OLD FILE IS NOW NAMED \"%s\"",
                    f->shown, why, bak);
            f->r->errors++;
            f->r->failed = 1;
            snprintf(f->r->why, sizeof(f->r->why), "replacing a file failed");
            return;
        }
        CurrentDir(old);
        if (!written_ok(f, f->leaf, f->ebytes, bad, sizeof(bad))) {
            /* The new file is wrong or missing: drop whatever has the name and
             * the temporary name, and give the old file its name back. */
            int back;
            old = CurrentDir(f->dir);
            DeleteFile((STRPTR)f->tmp);
            SetProtection((STRPTR)f->leaf, 0);
            DeleteFile((STRPTR)f->leaf);
            back = Rename((STRPTR)bak, (STRPTR)f->leaf);
            CurrentDir(old);
            if (back)
                msg(f, "error: cannot replace \"%s\": %s; old file kept", f->shown, bad);
            else
                msg(f, "error: cannot replace \"%s\": %s; THE OLD FILE IS NOW NAMED \"%s\"",
                    f->shown, bad, bak);
            f->r->errors++;
            f->r->failed = 1;
            snprintf(f->r->why, sizeof(f->r->why), "a replaced file did not arrive intact");
            return;
        }
        /* The user chose to replace it: delete protection on the old copy is
         * no reason to keep it. */
        old = CurrentDir(f->dir);
        SetProtection((STRPTR)bak, 0);
        if (!DeleteFile((STRPTR)bak)) {
            why_now(why, sizeof(why));
            CurrentDir(old);
            msg(f, "warning: replaced \"%s\", but the old copy stays as \"%s\": %s",
                f->shown, bak, why);
        } else {
            CurrentDir(old);
        }
        f->r->overwritten++;
    } else {
        if (!Rename((STRPTR)f->tmp, (STRPTR)f->leaf)) {
            why_now(why, sizeof(why));
            DeleteFile((STRPTR)f->tmp);
            CurrentDir(old);
            msg(f, "error: cannot rename into \"%s\": %s", f->shown, why);
            f->r->errors++;
            return;
        }
        CurrentDir(old);
        if (!written_ok(f, f->leaf, f->ebytes, bad, sizeof(bad))) {
            /* Nothing of the user's is at stake here; remove the wrong file and
             * the temporary one so that no wrong data stays under a real name. */
            old = CurrentDir(f->dir);
            DeleteFile((STRPTR)f->tmp);
            if (what_is_rel(f->dir, f->leaf, NULL, 0) == 1)
                DeleteFile((STRPTR)f->leaf);
            CurrentDir(old);
            msg(f, "error: \"%s\": %s; not kept", f->shown, bad);
            f->r->errors++;
            f->r->failed = 1;
            snprintf(f->r->why, sizeof(f->r->why), "a written file did not arrive intact");
            return;
        }
    }
    check_stored(f, f->dir, f->leaf, e->target);
    apply_meta(f, f->dir, f->leaf, f->shown, e);
    f->r->files++;
    if (f->o->progress)
        f->o->progress(f->o->ui_ctx, e, f->shown, f->r->bytes);
}

static int dir_empty(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int empty = 0;
    if (l && fib && Examine(l, fib))
        empty = !ExNext(l, fib);
    FreeDosObject(DOS_FIB, fib);
    if (l)
        UnLock(l);
    return empty;
}

/* 30 for OFS/FFS (DOS\0 to DOS\5), 100 for SFS, else 107. The disk
 * type Info() reports does not tell: RAM:, FAT and CD handlers report DOS\0
 * as well (rom/filesys/ram/commands.c, fat/volume.c), and a 30-character
 * limit there cut names RAM: takes in full. The DOS type in the mount
 * environment of the volume's device does tell. */
static int ffs_name_limit(BPTR lock)
{
    struct InfoData *id = AllocVec(sizeof(*id), MEMF_ANY);
    struct MsgPort *port = NULL;
    struct DosList *dl;
    ULONG dostype = 0;

    if (id && Info(lock, id) && id->id_VolumeNode)
        port = ((struct DeviceList *)BADDR(id->id_VolumeNode))->dl_Task;
    FreeVec(id);
    if (!port)
        return 0;
    dl = LockDosList(LDF_DEVICES | LDF_READ);
    while ((dl = NextDosEntry(dl, LDF_DEVICES))) {
        struct DeviceNode *dn = (struct DeviceNode *)dl;
        struct FileSysStartupMsg *fssm;
        struct DosEnvec *de;
        if (dn->dn_Task != port)
            continue;
        /* dn_Startup can also hold a small number or a string pointer. */
        fssm = (IPTR)dn->dn_Startup > 64 ? BADDR(dn->dn_Startup) : NULL;
        de = fssm && fssm->fssm_Environ ? BADDR(fssm->fssm_Environ) : NULL;
        if (de && de->de_TableSize >= DE_DOSTYPE)
            dostype = de->de_DosType;
        break;
    }
    UnLockDosList(LDF_DEVICES | LDF_READ);
    if (dostype >= AROS_MAKE_ID('D','O','S',0) && dostype <= AROS_MAKE_ID('D','O','S',5))
        return 30;
    /* SFS stores 100 characters and cuts longer names without an error
     * (AROS One 1.3, SFS-BE): shortening here keeps two long names that
     * differ only after 100 characters from landing on one file. */
    if ((dostype & 0xFFFFFF00UL) == AROS_MAKE_ID('S','F','S',0))
        return 100;
    /* Elsewhere 107: RAM: refuses longer names ("object name invalid"), and
     * the FAT handler accepted a 107-character name it could not find
     * again. A 255-character name made Lock on RAM: answer with the
     * directory itself (ABIv11, AROS One 1.3). */
    return 107;
}

int ua_extract(const char *archive, ua_list *l, ua_plan *p, ua_extract_opts *o,
               ua_report *r, char *err, int errlen)
{
    fsl f;
    int rc;
    ULONG i;
    char tv[8];

    memset(&f, 0, sizeof(f));
    memset(r, 0, sizeof(*r));
    /* The archive must still be the one that was listed (and planned).
     * Test aid UnarcTestNoStamp skips this, so that the engines' header
     * comparison can be tested on its own. */
    for (i = 0; i < (ULONG)l->nstamps && GetVar("UnarcTestNoStamp", (STRPTR)tv, sizeof(tv), 0) < 0;
         i++) {
        ua_stamp now;
        const char *path = l->volumes ? l->volumes[i] : archive;
        if (!ua_stamp_file(path, &now) || memcmp(&now, &l->stamps[i], sizeof(now))) {
            snprintf(err, errlen, "%s (%s)", UA_REPLACED_MSG, (const char *)FilePart((STRPTR)path));
            return -1;
        }
    }
    f.plan = p;
    f.o = o;
    f.r = r;
    f.sink.begin = s_begin;
    f.sink.data = s_data;
    f.sink.seek = s_seek;
    f.sink.end = s_end;
    f.sink.priv = &f;

    if (what_is_abs(p->dest) == 0) {
        /* Like UnZip -d: a destination that does not exist yet is created,
         * including missing parents. */
        char part[UA_PATHMAX];
        size_t i, n = strlen(p->dest);
        for (i = 1; i <= n; i++) {
            if (i < n && p->dest[i] != '/')
                continue;
            if (p->dest[i - 1] == ':')
                continue;
            snprintf(part, sizeof(part), "%.*s", (int)i, p->dest);
            if (what_is_abs(part) == 0) {
                BPTR lk = CreateDir((STRPTR)part);
                if (!lk)
                    break;
                UnLock(lk);
            }
        }
    }
    if (what_is_abs(p->dest) != 2) {
        snprintf(err, errlen, "destination \"%s\" is not a directory and cannot be created",
                 p->dest);
        return -1;
    }
    if (p->kind != UA_ROOT_DEST) {
        /* The plan chose a free name; if it was taken since, take the next one. */
        int k = 2;
        BPTR lk, d = Lock((STRPTR)p->dest, SHARED_LOCK), old;
        char base[256];
        if (!d) {
            snprintf(err, errlen, "cannot open \"%s\"", p->dest);
            return -1;
        }
        snprintf(base, sizeof(base), "%s", p->rootname);
        /* The new folder is a name like any other: it must fit the volume
         * before it is created, numbered names included. */
        f.name_max = ffs_name_limit(d);
        if (f.name_max && strlen(base) > (size_t)f.name_max) {
            shortened(base, 1, f.name_max, 0, p->rootname, sizeof(p->rootname));
            p->root_renamed = 1;
            if (o->message)
                o->message(o->ui_ctx, "folder name shortened for this volume (%d characters "
                           "at most): \"%s\" -> \"%s\"", f.name_max, base, p->rootname);
        }
        old = CurrentDir(d);
        while (!(lk = CreateDir((STRPTR)p->rootname))) {
            if (IoErr() != ERROR_OBJECT_EXISTS || k > 999) {
                char why[100];
                why_now(why, sizeof(why));
                CurrentDir(old);
                UnLock(d);
                snprintf(err, errlen, "cannot create \"%s\": %s", p->rootname, why);
                return -1;
            }
            shortened(base, k++, f.name_max ? f.name_max : 255, 0, p->rootname,
                      sizeof(p->rootname));
            p->root_renamed = 1;
        }
        UnLock(lk);
        CurrentDir(old);
        UnLock(d);
        {
            size_t n = strlen(p->dest);
            snprintf(p->root, sizeof(p->root), "%s%s%s", p->dest,
                     (n && (p->dest[n - 1] == ':' || p->dest[n - 1] == '/')) ? "" : "/",
                     p->rootname);
        }
        f.root_created = 1;
    }
    f.root = Lock((STRPTR)p->root, SHARED_LOCK);
    if (!f.root) {
        char why[100];
        why_now(why, sizeof(why));
        snprintf(err, errlen, "cannot open \"%s\": %s", p->root, why);
        return -1;
    }
    f.name_max = ffs_name_limit(f.root);

    rc = l->engine->extract(archive, l, &f.sink, err, errlen);
    if (f.fh) {                         /* engine stopped inside an entry */
        BPTR old;
        Close(f.fh);
        f.fh = BNULL;
        old = CurrentDir(f.dir);
        DeleteFile((STRPTR)f.tmp);
        CurrentDir(old);
    }
    if (f.write_failed)
        r->failed = 1;
    else if (!r->failed && (rc == 1 || f.cancel))
        r->cancelled = 1;
    else if (!r->failed && rc < 0 && (r->files || r->dirs || r->skipped || r->errors)) {
        /* The engine stopped, or reported an error for the whole job, after
         * part of it was done: the result says so instead of "complete". */
        r->failed = 1;
        snprintf(r->why, sizeof(r->why), "%s", *err ? err : "the engine reported an error");
    }

    /* Directory metadata last: writing files into a directory changes its date. */
    if (!r->cancelled)
        for (i = 0; i < l->n; i++) {
            ua_entry *e = &l->v[i];
            char dirrel[UA_PATHMAX], leaf[256], shown[UA_PATHMAX];
            BPTR d;
            if (e->skip || e->excluded || !e->target || e->type != UA_ET_DIR)
                continue;
            show(&f, shown, sizeof(shown), e->target);
            if (!*e->target) {          /* the root itself */
                BPTR parent = ParentDir(f.root);
                if (parent) {
                    apply_meta(&f, parent, FilePart((STRPTR)p->root), shown, e);
                    UnLock(parent);
                }
                continue;
            }
            if (split(e->target, dirrel, sizeof(dirrel), leaf, sizeof(leaf)) &&
                (d = dir_lock(&f, dirrel, 0))) {
                /* A directory whose name changed on disk has it in the map. */
                const char *m = dmap_find(&f, e->target);
                if (m)
                    snprintf(leaf, sizeof(leaf), "%s", strrchr(m, '/') ? strrchr(m, '/') + 1 : m);
                apply_meta(&f, d, leaf, shown, e);
            }
        }

    if (f.dir_valid)
        UnLock(f.dir);
    UnLock(f.root);
    dmap_free(&f);
    if ((r->cancelled || r->failed) && f.root_created && dir_empty(p->root))
        DeleteFile((STRPTR)p->root);
    return rc < 0 ? -1 : 0;
}
