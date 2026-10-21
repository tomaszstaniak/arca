/* SPDX-License-Identifier: MIT */
/* Packing: files and drawers into a new ZIP, tar.gz or 7z archive with
 * libarchive's writers.
 *
 * The archive is written under a temporary name in its own drawer and renamed
 * when complete, so a failed or cancelled run never leaves a partial archive
 * under the real name, and an existing archive is never replaced without the
 * caller saying so. Inputs are read with DOS calls relative to a drawer lock,
 * one name at a time, as the extraction side does (long absolute paths are
 * not safe on AROS). Links are stored as nothing and reported: following them
 * could loop, and a link outside the inputs would pull in files nobody chose.
 * The archive being written is never read back into itself. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <archive.h>
#include <archive_entry.h>
#include "ua.h"
#include "helper.h"

#define MAXDEPTH 128

typedef struct pk {
    struct archive *a;
    ua_pack_opts *o;
    ua_pack_report *r;
    BPTR out;                 /* the temporary file */
    BPTR outdir;              /* lock on the archive's drawer */
    BPTR outlock;             /* lock on the temporary file, to skip it */
    BPTR oldlock;             /* lock on an archive this one replaces, to skip it too */
    char tmp[40];
    char scratch[40];         /* 7z: libarchive's own temporary drawer, or "" */
    UQUAD ticked;
    int cancel;
    int write_failed;
    LONG write_err;
    UBYTE *buf;
    BPTR anc[MAXDEPTH + 1];   /* the drawers on the current path, for cycles */
    int nanc;
    /* LhA: the file the helper is compressing, from its indicator */
    UQUAD lsize;              /* its size */
    int lmarks;               /* the number of marks for all of it */
    int lstarted;             /* its start was reported */
    char lname[256];          /* its name, for the progress */
} pk;

static void msg(pk *p, const char *fmt, ...)
{
    char b[UA_PATHMAX + 200];
    va_list ap;
    if (!p->o->message)
        return;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    p->o->message(p->o->ui_ctx, "%s", b);
}

static int check_cancel(pk *p)
{
    if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        p->cancel = 1;
    if (p->o->cancel_after && p->r->bytes_in >= p->o->cancel_after)
        p->cancel = 1;
    return p->cancel;
}

/* ---- the output, through DOS ---- */

static la_ssize_t w_write(struct archive *a, void *ctx, const void *buf, size_t n)
{
    pk *p = ctx;
    LONG w;
    /* Also while the writer closes the archive: 7z writes most of it then. */
    if (check_cancel(p)) {
        archive_set_error(a, EINTR, "cancelled");
        return -1;
    }
    w = Write(p->out, (APTR)buf, (LONG)n);
    if (w != (LONG)n) {
        p->write_failed = 1;
        p->write_err = IoErr();
        archive_set_error(a, EIO, "write failed");
        return -1;
    }
    p->r->bytes_out += n;
    return (la_ssize_t)n;
}

static int w_close(struct archive *a, void *ctx)
{
    (void)a;
    (void)ctx;
    return ARCHIVE_OK;
}

/* ---- names and metadata ---- */

/* AROS names are ISO-8859-1; archives get UTF-8. */
static void latin1_to_utf8(const char *s, char *out, size_t n)
{
    size_t o = 0;
    for (; *s && o + 3 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

/* A DOS date stamp (local time) to Unix time (UTC); the inverse of the
 * extraction side's to_datestamp. */
static time_t to_unix(const struct DateStamp *ds)
{
    long long s = (long long)ds->ds_Days * 86400 + ds->ds_Minute * 60 +
                  ds->ds_Tick / TICKS_PER_SECOND;
    return (time_t)(s + 252460800LL + (long long)ua_gmtoff_minutes() * 60);
}

/* Owner RWED to Unix rwx; the extraction side maps it back. Group and other
 * bits as AROS has them. h, s, p and a have no Unix counterpart. */
static mode_t prot_to_mode(ULONG p, int dir)
{
    mode_t m = 0;
    if (!(p & FIBF_READ)) m |= S_IRUSR;
    if (!(p & FIBF_WRITE)) m |= S_IWUSR;
    if (!(p & FIBF_EXECUTE) || dir) m |= S_IXUSR;
    if (p & FIBF_GRP_READ) m |= S_IRGRP;
    if (p & FIBF_GRP_WRITE) m |= S_IWGRP;
    if (p & FIBF_GRP_EXECUTE) m |= S_IXGRP;
    if (p & FIBF_OTR_READ) m |= S_IROTH;
    if (p & FIBF_OTR_WRITE) m |= S_IWOTH;
    if (p & FIBF_OTR_EXECUTE) m |= S_IXOTH;
    if (dir)
        m |= S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
    return m;
}

/* ---- walking the inputs ---- */

/* Preparing walks every input once and keeps what it finds: the list is then
 * packed as found, and the sizes give the progress its total. Each item
 * keeps only its own name and its depth; packing opens the drawers again one
 * at a time, relative to each other, as the walk did. */
typedef struct pent {
    char *name;               /* relative to the drawer above; "" for a volume */
    char *apath;              /* the name in the archive */
    int depth;                /* 0 for an input */
    int input;                /* which input it belongs to */
    int isdir;
    ULONG prot;
    struct DateStamp date;
    UQUAD size;
} pent;

typedef struct plist {
    pent *e;
    ULONG n, cap;
} plist;

static void plist_free(plist *l)
{
    ULONG i;
    for (i = 0; i < l->n; i++) {
        free(l->e[i].name);
        free(l->e[i].apath);
    }
    free(l->e);
    memset(l, 0, sizeof(*l));
}

static int plist_add(pk *p, plist *l, const char *name, const char *apath, int depth, int input,
                     const struct FileInfoBlock *fib)
{
    pent *e;
    if (l->n == l->cap) {
        ULONG cap = l->cap ? l->cap * 2 : 256;
        pent *ne = realloc(l->e, cap * sizeof(*ne));
        if (!ne)
            return -1;
        l->e = ne;
        l->cap = cap;
    }
    e = &l->e[l->n];
    e->name = strdup(name);
    e->apath = strdup(apath);
    if (!e->name || !e->apath) {
        free(e->name);
        free(e->apath);
        return -1;
    }
    e->depth = depth;
    e->input = input;
    e->isdir = fib->fib_DirEntryType > 0;
    e->prot = fib->fib_Protection;
    e->date = fib->fib_Date;
    e->size = e->isdir ? 0 : UA_FIB_SIZE(fib);
    l->n++;
    if (!e->isdir) {
        p->r->total_files++;
        p->r->total_bytes += e->size;
    }
    if (p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_PREPARING, p->r->total_files, p->r->total_bytes);
    return 0;
}

/* Test aid: the variable UnarcTestFailDir makes reading a drawer fail, so the
 * error paths can be shown on a working volume: "name" fails at its start,
 * "name/k" after k entries. -1: no failure for this drawer. */
static int test_fail_at(const char *name)
{
    char v[108], *slash;
    if (GetVar("UnarcTestFailDir", v, sizeof(v), 0) <= 0)
        return -1;
    slash = strchr(v, '/');
    if (slash)
        *slash++ = 0;
    if (strcmp(v, name) != 0)
        return -1;
    return slash ? atoi(slash) : 0;
}

static int same_as_output(pk *p, BPTR l)
{
    return (p->outlock && SameLock(l, p->outlock) == LOCK_SAME) ||
           (p->oldlock && SameLock(l, p->oldlock) == LOCK_SAME);
}

/* The writer is about to close the archive. Test aid: UnarcTestCancelAtFinish
 * sends Ctrl-C to ourselves now, as a user's Cancel during "Finishing". */
static void finishing(pk *p)
{
    char v[4];
    if (p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_FINISHING, p->r->files, p->r->bytes_in);
    if (GetVar("UnarcTestCancelAtFinish", v, sizeof(v), 0) > 0)
        Signal(FindTask(NULL), SIGBREAKF_CTRL_C);
}

static int nomem(pk *p)
{
    p->r->failed = 1;
    snprintf(p->r->why, sizeof(p->r->why), "not enough memory");
    return -1;
}

/* seen_type: the entry type ExNext reported in the parent drawer, or 0 for an
 * input the user named. It matters because Lock follows soft links: after
 * Lock and Examine a link looks like its target, and a link to a parent
 * drawer would pack the tree again and again. Links found inside drawers are
 * therefore skipped by what the parent says they are (for LhA, which would
 * follow them, they stop the job); an input the user named is taken as what
 * it points to. Returns -1 when the whole job must stop. */
static int scan_tree(pk *p, plist *pl, BPTR dir, const char *name, const char *apath, int depth,
                     int input, LONG seen_type, int lha)
{
    BPTR old, l;
    struct FileInfoBlock *fib;
    int rc = 0, k;
    if (seen_type == ST_SOFTLINK || seen_type == ST_LINKDIR || seen_type == ST_LINKFILE) {
        if (lha) {
            snprintf(p->r->why, sizeof(p->r->why), "\"%s\" is a link; LhA would follow it, "
                     "so pack with another format or without it", apath);
            return -1;
        }
        msg(p, "skipped (link, not followed): %s", apath);
        p->r->skipped++;
        return 0;
    }
    /* Our own temporary file and drawer, when the archive is made inside
     * what is being packed. Checked by name first: the temporary file is
     * open for writing, so it cannot even be locked. */
    if (SameLock(dir, p->outdir) == LOCK_SAME &&
        (strcasecmp(name, p->tmp) == 0 || (p->scratch[0] && strcasecmp(name, p->scratch) == 0)))
        return 0;
    old = CurrentDir(dir);
    l = Lock((STRPTR)name, SHARED_LOCK);
    CurrentDir(old);
    if (!l) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        msg(p, "error: cannot open \"%s\": %s; not packed", apath, why);
        p->r->errors++;
        return 0;
    }
    if (same_as_output(p, l)) {
        msg(p, "skipped (the archive being made): %s", apath);
        p->r->skipped++;
        UnLock(l);
        return 0;
    }
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(l, fib)) {
        char why[100];
        Fault(fib ? IoErr() : ERROR_NO_FREE_STORE, NULL, why, sizeof(why));
        msg(p, "error: cannot examine \"%s\": %s; not packed", apath, why);
        FreeDosObject(DOS_FIB, fib);
        UnLock(l);
        p->r->errors++;
        return 0;
    }
    for (k = 0; k < p->nanc && fib->fib_DirEntryType > 0; k++)
        if (SameLock(l, p->anc[k]) == LOCK_SAME)
            break;
    if (fib->fib_DirEntryType > 0 && k < p->nanc) {
        /* Reached again through an assign or a link the parent did not
         * report as one: a cycle. */
        msg(p, "skipped (drawer inside itself): %s", apath);
        p->r->skipped++;
    } else if (fib->fib_DirEntryType > 0) {
        if (depth >= MAXDEPTH && lha) {
            /* The helper would pack it all the same. */
            snprintf(p->r->why, sizeof(p->r->why), "\"%s\" is nested more than %d levels deep",
                     apath, MAXDEPTH);
            rc = -1;
        } else if (depth >= MAXDEPTH) {
            msg(p, "error: \"%s\" is nested more than %d levels deep; not packed", apath,
                MAXDEPTH);
            p->r->errors++;
        } else if (plist_add(p, pl, name, apath, depth, input, fib) < 0) {
            rc = nomem(p);
        } else {
            struct FileInfoBlock *c = AllocDosObject(DOS_FIB, NULL);
            p->anc[p->nanc++] = l;
            int fail_at = test_fail_at(name);
            if (fail_at == 0)
                SetIoErr(ERROR_SEEK_ERROR);
            if (!c || fail_at == 0 || !Examine(l, c)) {
                char why[100];
                Fault(c ? IoErr() : ERROR_NO_FREE_STORE, NULL, why, sizeof(why));
                msg(p, "error: cannot read the drawer \"%s\": %s; its contents are not packed",
                    apath, why);
                p->r->errors++;
            } else {
                /* On the heap: one path per level on the stack would overrun the
                 * worker's stack long before MAXDEPTH (ABIv11 has no check). */
                char *sub = malloc(UA_PATHMAX);
                if (!sub)
                    rc = nomem(p);
                int seen = 0;
                while (rc == 0 && (seen != fail_at || (SetIoErr(ERROR_SEEK_ERROR), 0)) &&
                       ExNext(l, c)) {
                    seen++;
                    if (snprintf(sub, UA_PATHMAX, "%s/%s", apath, c->fib_FileName) >= UA_PATHMAX) {
                        msg(p, "error: path too long under \"%s\"; not packed", apath);
                        p->r->errors++;
                        continue;
                    }
                    rc = scan_tree(p, pl, l, c->fib_FileName, sub, depth + 1, input,
                                   c->fib_DirEntryType, lha);
                    if (check_cancel(p))
                        rc = -1;
                }
                /* ExNext ends with ERROR_NO_MORE_ENTRIES; anything else means
                 * the rest of the drawer was not read. */
                if (rc == 0 && sub && IoErr() != ERROR_NO_MORE_ENTRIES) {
                    char why[100];
                    Fault(IoErr(), NULL, why, sizeof(why));
                    msg(p, "error: reading the drawer \"%s\" stopped: %s; the rest of it is "
                        "not packed", apath, why);
                    p->r->errors++;
                }
                free(sub);
            }
            p->nanc--;
            FreeDosObject(DOS_FIB, c);
        }
    } else if (plist_add(p, pl, name, apath, depth, input, fib) < 0) {
        rc = nomem(p);
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return rc;
}

static int add_entry_header(pk *p, const pent *e)
{
    struct archive_entry *ae = archive_entry_new();
    char u[UA_PATHMAX * 2];
    int rc;
    if (!ae)
        return -1;
    latin1_to_utf8(e->apath, u, sizeof(u));
    if (e->isdir)
        strncat(u, "/", sizeof(u) - strlen(u) - 1);
    archive_entry_set_pathname_utf8(ae, u);
    archive_entry_set_filetype(ae, e->isdir ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(ae, prot_to_mode(e->prot, e->isdir));
    archive_entry_set_mtime(ae, to_unix(&e->date), 0);
    if (!e->isdir)
        archive_entry_set_size(ae, (la_int64_t)e->size);
    rc = archive_write_header(p->a, ae);
    archive_entry_free(ae);
    if (rc < ARCHIVE_WARN) {
        msg(p, "error: %s: %s", e->apath, archive_error_string(p->a));
        return -1;
    }
    return 0;
}

/* One file, relative to dir, as preparing found it. Returns -1 when the
 * whole job must stop. */
static int add_file(pk *p, BPTR dir, const pent *e)
{
    BPTR old = CurrentDir(dir), fh = Open((STRPTR)e->name, MODE_OLDFILE);
    UQUAD done = 0, want = e->size;
    LONG n;
    CurrentDir(old);
    if (!fh) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        msg(p, "error: cannot read \"%s\": %s; not packed", e->apath, why);
        p->r->errors++;
        return 0;
    }
    if (add_entry_header(p, e) < 0) {
        Close(fh);
        if (p->cancel)
            return -1;
        p->r->failed = 1;
        snprintf(p->r->why, sizeof(p->r->why), "the archive could not be written");
        return -1;
    }
    if (p->o->progress)
        p->o->progress(p->o->ui_ctx, e->apath, p->r->bytes_in);
    while ((n = Read(fh, p->buf, 65536)) > 0) {
        LONG take = n;
        /* The header promised the size found while preparing; a file that
         * grew since is stored as far as that and reported. */
        if (done + (UQUAD)take > want)
            take = (LONG)(want - done);
        if (take > 0 && archive_write_data(p->a, p->buf, take) != take) {
            Close(fh);
            if (p->cancel)
                return -1;
            p->r->failed = 1;
            if (p->write_failed) {
                Fault(p->write_err, NULL, p->r->why, sizeof(p->r->why));
            } else {
                snprintf(p->r->why, sizeof(p->r->why), "%s", archive_error_string(p->a));
            }
            return -1;
        }
        done += take;
        p->r->bytes_in += take;
        if (p->o->tick && p->r->bytes_in - p->ticked >= 65536) {
            p->ticked = p->r->bytes_in;
            p->o->tick(p->o->ui_ctx, e->apath, p->r->bytes_in);
        }
        if (check_cancel(p)) {
            Close(fh);
            return -1;
        }
        if ((UQUAD)n > (UQUAD)take)
            break;
    }
    if (n < 0) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        msg(p, "error: reading \"%s\" failed: %s; the archive holds %llu of %llu bytes of it",
            e->apath, why, (unsigned long long)done, (unsigned long long)want);
        p->r->errors++;
    } else {
        struct FileInfoBlock *now = AllocDosObject(DOS_FIB, NULL);
        int changed = done != want;
        if (now && ExamineFH(fh, now))
            changed |= UA_FIB_SIZE(now) != want || CompareDates(&now->fib_Date, &e->date) != 0;
        FreeDosObject(DOS_FIB, now);
        if (changed) {
            msg(p, "error: \"%s\" changed while it was being packed; the archive's copy "
                "may be inconsistent", e->apath);
            p->r->changed++;
            p->r->errors++;
        }
    }
    Close(fh);
    p->r->files++;
    return 0;
}

/* Test aid: the variable UnarcTestGoneDir names a drawer that packing
 * cannot open again, as if it were deleted after preparing. */
static int test_gone(const char *name)
{
    char v[108];
    return GetVar("UnarcTestGoneDir", v, sizeof(v), 0) > 0 && strcmp(v, name) == 0;
}

/* Packs the list. base[i]: the drawer input i is named in. The drawers on
 * the current path stay locked, one per depth. A drawer that cannot be
 * opened again (gone since preparing) takes everything below it with it,
 * reported once: the entries after it are skipped while they are deeper. */
static int pack_list(pk *p, plist *pl, BPTR *base)
{
    BPTR stack[MAXDEPTH + 1];
    int nstack = 0, rc = 0, skip = -1;
    ULONG i;
    if (p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_PACKING, p->r->total_files, p->r->total_bytes);
    for (i = 0; i < pl->n && rc == 0; i++) {
        pent *e = &pl->e[i];
        BPTR parent;
        if (skip >= 0 && e->depth > skip)
            continue;
        skip = -1;
        while (nstack > e->depth)
            UnLock(stack[--nstack]);
        parent = e->depth ? stack[e->depth - 1] : base[e->input];
        if (e->isdir) {
            BPTR old, l;
            old = CurrentDir(parent);
            l = test_gone(e->name) ? (SetIoErr(ERROR_OBJECT_NOT_FOUND), BNULL)
                                   : Lock((STRPTR)e->name, SHARED_LOCK);
            CurrentDir(old);
            if (l) {
                stack[nstack++] = l;
                if (add_entry_header(p, e) < 0) {
                    p->r->failed = !p->cancel;
                    rc = -1;
                    break;
                }
                p->r->dirs++;
            } else {
                char why[100];
                Fault(IoErr(), NULL, why, sizeof(why));
                msg(p, "error: cannot open the drawer \"%s\": %s; its contents are not packed",
                    e->apath, why);
                p->r->errors++;
                skip = e->depth;
            }
        } else {
            rc = add_file(p, parent, e);
        }
        if (rc == 0 && check_cancel(p))
            rc = -1;
    }
    while (nstack > 0)
        UnLock(stack[--nstack]);
    return rc;
}

/* Preparing: walks every input into pl. base[i] gets the drawer input i is
 * named in (for a volume or an assign, its own lock, with the name ""), to be
 * unlocked by the caller; inputs that cannot be opened are reported and get
 * none. */
static int scan_inputs(pk *p, plist *pl, char **inputs, int ninputs, BPTR *base, int lha)
{
    int i, rc = 0;
    if (p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_PREPARING, 0, 0);
    for (i = 0; i < ninputs && rc == 0; i++) {
        BPTR l = Lock((STRPTR)inputs[i], SHARED_LOCK), parent;
        const char *nm = (const char *)FilePart((STRPTR)inputs[i]);
        if (!l) {
            char why[100];
            Fault(IoErr(), NULL, why, sizeof(why));
            msg(p, "error: cannot open \"%s\": %s; not packed", inputs[i], why);
            p->r->errors++;
            continue;
        }
        parent = ParentDir(l);
        if (!*nm || !parent) {
            /* A volume or an assign: its contents, under its name. */
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            if (fib && Examine(l, fib)) {
                char top[110];
                snprintf(top, sizeof(top), "%s", fib->fib_FileName);
                base[i] = l;
                l = BNULL;
                rc = scan_tree(p, pl, base[i], "", top, 0, i, 0, lha);
            } else {
                msg(p, "error: cannot examine \"%s\"; not packed", inputs[i]);
                p->r->errors++;
            }
            FreeDosObject(DOS_FIB, fib);
            if (parent)
                UnLock(parent);
        } else {
            base[i] = parent;
            rc = scan_tree(p, pl, parent, nm, nm, 0, i, 0, lha);
        }
        if (l)
            UnLock(l);
        if (rc == 0 && check_cancel(p))
            rc = -1;
    }
    return rc;
}

/* libarchive's 7z writer keeps the compressed data in a temporary file of its
 * own (__archive_mktemp: TMPDIR or "/tmp", then unlink while open). On AROS the
 * unlink of an open file fails and the file would stay, and "/tmp" is not a
 * temporary drawer. So it gets a drawer of its own next to the archive, through
 * a variable local to this process, and the drawer is emptied and removed
 * afterwards whatever happened. */
static int scratch_open(pk *p)
{
    char path[UA_PATHMAX];
    BPTR old = CurrentDir(p->outdir), made;
    snprintf(p->scratch, sizeof(p->scratch), ".uatmp%lx.d",
             (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF));
    made = CreateDir((STRPTR)p->scratch);
    CurrentDir(old);
    if (!made) {
        p->scratch[0] = 0;
        return -1;
    }
    UnLock(made);
    if (!NameFromLock(p->outdir, path, sizeof(path)) ||
        !AddPart(path, (STRPTR)p->scratch, sizeof(path)))
        return -1;
    strncat(path, "/", sizeof(path) - strlen(path) - 1);
    return SetVar("TMPDIR", path, -1, GVF_LOCAL_ONLY) ? 0 : -1;
}

static void scratch_close(pk *p)
{
    BPTR old, d;
    struct FileInfoBlock *fib;
    if (!p->scratch[0])
        return;
    DeleteVar("TMPDIR", GVF_LOCAL_ONLY);
    old = CurrentDir(p->outdir);
    d = Lock((STRPTR)p->scratch, SHARED_LOCK);
    fib = AllocDosObject(DOS_FIB, NULL);
    if (d && fib && Examine(d, fib)) {
        BPTR o2 = CurrentDir(d);
        /* Delete one at a time from the start: ExNext after a delete in the
         * same drawer is not reliable. */
        while (Examine(d, fib) && ExNext(d, fib))
            if (!DeleteFile((STRPTR)fib->fib_FileName))
                break;
        CurrentDir(o2);
    }
    FreeDosObject(DOS_FIB, fib);
    if (d)
        UnLock(d);
    DeleteFile((STRPTR)p->scratch);
    CurrentDir(old);
}

/* ---- the job ---- */

const char *ua_pack_format_name(ua_packfmt f)
{
    return f == UA_PACK_ZIP ? "ZIP" : f == UA_PACK_TGZ ? "tar.gz" : f == UA_PACK_7Z ? "7z" :
           f == UA_PACK_LHA ? "LhA" : "?";
}

int ua_pack_format_from_name(const char *archive, ua_packfmt *f)
{
    size_t n = strlen(archive);
#define ENDS(s) (n >= strlen(s) && strcasecmp(archive + n - strlen(s), s) == 0)
    if (ENDS(".zip"))
        *f = UA_PACK_ZIP;
    else if (ENDS(".tar.gz") || ENDS(".tgz"))
        *f = UA_PACK_TGZ;
    else if (ENDS(".7z"))
        *f = UA_PACK_7Z;
    else if (ENDS(".lha") || ENDS(".lzh"))
        *f = UA_PACK_LHA;
    else
        return 0;
    return 1;
#undef ENDS
}

static int setup_format(pk *p, ua_packfmt f, int level, char *err, int errlen)
{
    char opt[64];
    int rc = ARCHIVE_OK;
    switch (f) {
    case UA_PACK_ZIP:
        rc = archive_write_set_format_zip(p->a);
        if (rc == ARCHIVE_OK && level >= 0) {
            snprintf(opt, sizeof(opt), "zip:compression-level=%d", level);
            rc = archive_write_set_options(p->a, opt);
        }
        if (rc == ARCHIVE_OK)
            rc = archive_write_set_options(p->a, "zip:hdrcharset=UTF-8");
        break;
    case UA_PACK_TGZ:
        rc = archive_write_set_format_pax_restricted(p->a);
        if (rc == ARCHIVE_OK)
            rc = archive_write_add_filter_gzip(p->a);
        if (rc == ARCHIVE_OK && level >= 0) {
            snprintf(opt, sizeof(opt), "gzip:compression-level=%d", level ? level : 1);
            rc = archive_write_set_options(p->a, opt);
        }
        break;
    case UA_PACK_7Z:
        rc = archive_write_set_format_7zip(p->a);
        if (rc == ARCHIVE_OK && level >= 0) {
            snprintf(opt, sizeof(opt), "7zip:compression-level=%d", level);
            rc = archive_write_set_options(p->a, opt);
        }
        break;    case UA_PACK_LHA:         /* written by unarc-lha, not here */
        break;
    }
    if (rc != ARCHIVE_OK) {
        snprintf(err, errlen, "%s", archive_error_string(p->a));
        return -1;
    }
    return 0;
}

/* The old archive could not be deleted (most often something had it open
 * for a moment). It is not deleted by force: it gets a name a user can find
 * next to the new one, "<name>.old" (or .old2 ...), and the caller is told
 * where it is. The current directory is the archive's drawer. */
static void keep_old(pk *p, const char *drawer, const char *leaf, const char *bak)
{
    char nm[300];
    const char *now = bak;
    int i;
    for (i = 1; i < 100; i++) {
        BPTR x;
        if (i == 1)
            snprintf(nm, sizeof(nm), "%s.old", leaf);
        else
            snprintf(nm, sizeof(nm), "%s.old%d", leaf, i);
        if ((x = Lock((STRPTR)nm, SHARED_LOCK))) {
            UnLock(x);
            continue;
        }
        if (Rename((STRPTR)bak, (STRPTR)nm))
            now = nm;
        break;
    }
    snprintf(p->r->kept, sizeof(p->r->kept), "%s", drawer);
    AddPart((STRPTR)p->r->kept, (STRPTR)now, sizeof(p->r->kept));
    msg(p, "warning: the old archive could not be deleted; it is kept as \"%s\"", p->r->kept);
}

/* The temporary archive in the archive's drawer (the current directory)
 * gets the archive's name. An archive it replaces goes to a backup name
 * first and is deleted only after that; when the new one cannot be renamed,
 * the old one gets its name back. have_tmp: there is a temporary to deal
 * with; on failure it is deleted. */
static int finish(pk *p, const char *drawer, const char *leaf, int exists, int rc, int have_tmp,
                  char *err, int errlen)
{
    if (have_tmp && rc == 0) {
        /* Complete: the old archive (if the caller said to replace it) goes
         * to a backup name until the new one has its name. */
        char bak[40];
        int ok;
        snprintf(bak, sizeof(bak), ".uabak%lx.pk", (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF));
        if (exists && !Rename((STRPTR)leaf, (STRPTR)bak)) {
            Fault(IoErr(), NULL, err, errlen);
            DeleteFile((STRPTR)p->tmp);
            rc = -1;
        } else if (!(ok = Rename((STRPTR)p->tmp, (STRPTR)leaf))) {
            char why[100];
            Fault(IoErr(), NULL, why, sizeof(why));
            if (exists && !Rename((STRPTR)bak, (STRPTR)leaf))
                snprintf(err, errlen, "%s; THE OLD ARCHIVE IS NOW NAMED \"%s\"", why, bak);
            else
                snprintf(err, errlen, "%s", why);
            DeleteFile((STRPTR)p->tmp);
            p->r->failed = 1;
            snprintf(p->r->why, sizeof(p->r->why), "%s", err);
            rc = -1;
        } else if (exists) {
            /* Test aid: UnarcTestKeepOld holds the old archive open, as
             * Wanderer reading the drawer did, so that its delete fails. */
            char tv[4];
            BPTR hold = GetVar("UnarcTestKeepOld", tv, sizeof(tv), 0) > 0
                            ? Lock((STRPTR)bak, SHARED_LOCK) : BNULL;
            SetProtection((STRPTR)bak, 0);
            if (!DeleteFile((STRPTR)bak))
                keep_old(p, drawer, leaf, bak);
            if (hold)
                UnLock(hold);
            p->r->replaced = 1;
        }
    } else if (have_tmp) {
        DeleteFile((STRPTR)p->tmp);
    }
    return rc;
}

/* ---- LhA, through the helper unarc-lha ---- */

/* LHa for UNIX (patched, see patches/lha) is a program of its own under its
 * own licence, so it runs as a child process (helper.c). It is given its
 * names in a list file and runs in the drawer of the items, so that they are
 * stored by their own names; items in several drawers make one run per
 * drawer, each adding to the same new archive. It writes under a temporary
 * name of ours in the archive's drawer; finish() then gives the archive its
 * name, as for the other formats. */
#define LHA_PROG "unarc-lha"

/* Found once, by the program's main process: the GUI's job worker has no
 * home drawer, so PROGDIR: means nothing there. */
static int lha_have = -1;
static char lha_prog[UA_PATHMAX];

void ua_lha_init(void)
{
    lha_have = ua_helper_find(LHA_PROG, lha_prog, sizeof(lha_prog));
}

/* 1 when the drawer d is the drawer top or below it. */
static int lha_inside(BPTR d, BPTR top)
{
    BPTR cur = DupLock(d), up;
    int in = 0;
    while (cur && !in) {
        if (SameLock(cur, top) == LOCK_SAME)
            in = 1;
        up = ParentDir(cur);
        UnLock(cur);
        cur = up;
    }
    if (cur)
        UnLock(cur);
    return in;
}

/* The helper's running indicator (LHa's quiet mode 0, not on a terminal):
 * for each file a line "name\t- Frozen :  ...." with one dot for each part
 * of the file, then "name\t- Frozen :  " followed by an "o" as each part is
 * done, then the line of the finished file. Returns the text after " :  "
 * when line is one of the first two, else NULL. */
static const char *lha_marks(const char *line)
{
    const char *t = strstr(line, "\t- "), *m = t ? strstr(t, " :  ") : NULL;
    return m ? m + 4 : NULL;
}

static UQUAD lha_size(BPTR dir, const char *name)
{
    BPTR old = CurrentDir(dir), l = Lock((STRPTR)name, SHARED_LOCK);
    UQUAD size = 0;
    CurrentDir(old);
    if (l) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib && Examine(l, fib))
            size = UA_FIB_SIZE(fib);
        FreeDosObject(DOS_FIB, fib);
        UnLock(l);
    }
    return size;
}

/* The part of the current line the helper has written so far: the marks of
 * the file being compressed, as input bytes for the progress. */
static void lha_partial(pk *p, const char *line)
{
    const char *m = lha_marks(line);
    UQUAD part;
    int o = 0;
    if (!m || !p->lmarks || !p->o->tick)
        return;
    for (; *m == 'o'; m++)
        o++;
    if (o > p->lmarks)
        o = p->lmarks;
    part = p->lsize / (UQUAD)p->lmarks * (UQUAD)o;
    if (p->r->bytes_in + part >= p->ticked + 65536) {
        p->ticked = p->r->bytes_in + part;
        p->o->tick(p->o->ui_ctx, p->lname, p->ticked);
    }
}

/* A line of the helper's output: "name\t- Frozen(42%)" when a file or
 * drawer is done, "LHa: Warning: ..." or "LHa: Error: ...", or one of the
 * indicator's. */
static void lha_line(pk *p, BPTR dir, char *line)
{
    char *tab = strchr(line, '\t');
    const char *m = lha_marks(line);
    if (m) {
        if (*m == '.') {
            /* A file starts: its size and how many marks it gets. */
            p->lmarks = (int)strspn(m, ".");
            *strstr(line, "\t- ") = 0;
            p->lsize = lha_size(dir, line);
            p->lstarted = 1;
            snprintf(p->lname, sizeof(p->lname), "%s", line);
            if (p->o->progress)
                p->o->progress(p->o->ui_ctx, line, p->r->bytes_in);
        }
        return;
    }
    if (!strncmp(line, "LHa: ", 5)) {
        msg(p, "%s", line + 5);
        if (!strncmp(line + 5, "Warning", 7) || !strncmp(line + 5, "Error", 5))
            p->r->errors++;
        if (!strncmp(line + 5, "Error", 5) || !strncmp(line + 5, "Fatal", 5))
            snprintf(p->r->why, sizeof(p->r->why), "%s", line + 5);
        return;
    }
    if (!tab || strncmp(tab, "\t- ", 3))
        return;
    *tab = 0;
    if (tab > line && tab[-1] == '/') {
        p->r->dirs++;
    } else {
        /* The size, for the byte count the front ends show. */
        p->r->bytes_in += p->lstarted ? p->lsize : lha_size(dir, line);
        p->ticked = p->r->bytes_in;
        p->r->files++;
    }
    /* Empty files and drawers have no indicator: reported when done. */
    if (p->o->progress && !p->lstarted)
        p->o->progress(p->o->ui_ctx, line, p->r->bytes_in);
    p->lmarks = 0;
    p->lstarted = 0;
}

/* One run of the helper: the items in names[0..n-1], all in drawer dir. */
static int lha_run(pk *p, const char *prog, BPTR dir, char **names, int n, const char *arcpath,
                   char *err, int errlen)
{
    char list[64], args[160];
    ua_helper h;
    BPTR fh;
    UBYTE buf[512];
    char line[UA_PATHMAX + 200];
    size_t ll = 0;
    LONG got, rc;
    int i, ok = 1;

    snprintf(list, sizeof(list), "T:unarc-lha-%08lx.lst", (unsigned long)(IPTR)FindTask(NULL));
    if (!(fh = Open((STRPTR)list, MODE_NEWFILE))) {
        snprintf(err, errlen, "cannot write the list of names to T:");
        return -1;
    }
    ok = FPuts(fh, (STRPTR)arcpath) == 0 && FPutC(fh, '\n') >= 0;
    for (i = 0; ok && i < n; i++)
        ok = FPuts(fh, (STRPTR)names[i]) == 0 && FPutC(fh, '\n') >= 0;
    Close(fh);
    if (!ok) {
        DeleteFile((STRPTR)list);
        snprintf(err, errlen, "cannot write the list of names to T:");
        return -1;
    }
    /* a: add (a new archive here), 1: header level 1 as Amiga LhA writes,
     * q0: the running indicator, for the progress; z: store only. */
    snprintf(args, sizeof(args), "a1q0%s --list=%s\n", p->o->level == 0 ? "z" : "", list);
    if (ua_helper_start_polled(&h, prog, LHA_PROG, args, dir, 1, err, errlen) < 0) {
        DeleteFile((STRPTR)list);
        return -1;
    }
    while ((got = ua_helper_read(&h, buf, sizeof(buf))) > 0) {
        LONG k;
        for (k = 0; k < got; k++) {
            if (buf[k] == '\n' || buf[k] == '\r') {
                line[ll] = 0;
                if (ll)
                    lha_line(p, dir, line);
                ll = 0;
            } else if (ll < sizeof(line) - 1) {
                line[ll++] = (char)buf[k];
            }
        }
        if (ll) {
            line[ll] = 0;
            lha_partial(p, line);
        }
        if (!h.cancelled && p->o->cancel_after && p->r->bytes_in >= p->o->cancel_after) {
            h.cancelled = 1;
            ua_helper_poke(&h);
        }
    }
    if (ll) {
        line[ll] = 0;
        lha_line(p, dir, line);
    }
    rc = ua_helper_end(&h);
    DeleteFile((STRPTR)list);
    if (h.cancelled) {
        p->cancel = 1;
        return -1;
    }
    if (rc != 0) {
        p->r->failed = 1;
        if (!p->r->why[0])
            snprintf(p->r->why, sizeof(p->r->why), "%s ended with return code %ld", LHA_PROG,
                     (long)rc);
        return -1;
    }
    return 0;
}

static int lha_pack(pk *p, char **inputs, int ninputs, char *err, int errlen)
{
    char arcpath[UA_PATHMAX];
    const char *prog = lha_prog;
    char **names = NULL, *taken = NULL;
    plist pl = { NULL, 0, 0 };
    int i, rc = 0;

    if (lha_have < 0)
        ua_lha_init();
    if (!lha_have) {
        snprintf(err, errlen, "%s is not next to the program; it makes LhA archives", LHA_PROG);
        return -1;
    }
    snprintf(p->tmp, sizeof(p->tmp), ".uatmp%lx.lha", (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF));
    if (!NameFromLock(p->outdir, (STRPTR)arcpath, sizeof(arcpath)) ||
        !AddPart((STRPTR)arcpath, (STRPTR)p->tmp, sizeof(arcpath))) {
        snprintf(err, errlen, "the archive's path is too long");
        return -1;
    }
    {
        /* A temporary archive of that name left by an earlier run. */
        BPTR old = CurrentDir(p->outdir);
        DeleteFile((STRPTR)p->tmp);
        CurrentDir(old);
    }
    names = calloc(ninputs ? ninputs : 1, sizeof(char *));
    taken = calloc(ninputs ? ninputs : 1, 1);
    if (p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_PREPARING, 0, 0);
    if (!names || !taken) {
        free(names);
        free(taken);
        snprintf(err, errlen, "not enough memory");
        return -1;
    }
    /* Each input: there, not a volume, not the archive being replaced, not
     * a drawer that holds the archive, and nothing linked inside. */
    for (i = 0; i < ninputs && rc == 0; i++) {
        BPTR l = Lock((STRPTR)inputs[i], SHARED_LOCK), parent = l ? ParentDir(l) : BNULL;
        struct FileInfoBlock *fib = l ? AllocDosObject(DOS_FIB, NULL) : NULL;
        const char *nm = (const char *)FilePart((STRPTR)inputs[i]);
        if (!l) {
            char why[100];
            Fault(IoErr(), NULL, why, sizeof(why));
            msg(p, "error: cannot open \"%s\": %s; not packed", inputs[i], why);
            p->r->errors++;
        } else if (!parent || !*nm) {
            snprintf(err, errlen, "\"%s\" is a volume; LhA packs drawers and files: choose those "
                     "in it", inputs[i]);
            rc = -1;
        } else if (p->oldlock && SameLock(l, p->oldlock) == LOCK_SAME) {
            snprintf(err, errlen, "\"%s\" is one of the files to pack; choose another name",
                     inputs[i]);
            rc = -1;
        } else if (fib && Examine(l, fib) && fib->fib_DirEntryType > 0 &&
                   lha_inside(p->outdir, l)) {
            snprintf(err, errlen, "the archive would be inside \"%s\", which is being "
                     "packed; save it somewhere else", inputs[i]);
            rc = -1;
        } else if (scan_tree(p, &pl, parent, nm, nm, 0, i, 0, 1) < 0) {
            /* LHa for UNIX follows links (lstat follows AmigaDOS soft links),
             * and a link to a drawer above it would never end: a link
             * refuses LhA packing. The other formats store links as nothing. */
            snprintf(err, errlen, "%s", p->r->why[0] ? p->r->why : "stopped");
            p->r->why[0] = 0;
            rc = -1;
        }
        if (fib)
            FreeDosObject(DOS_FIB, fib);
        if (parent)
            UnLock(parent);
        if (l)
            UnLock(l);
    }
    /* The helper walks the drawers itself; the list gives the totals. */
    plist_free(&pl);
    if (rc == 0 && check_cancel(p))
        rc = -1;
    if (rc == 0 && p->o->phase)
        p->o->phase(p->o->ui_ctx, UA_PACK_PH_PACKING, p->r->total_files, p->r->total_bytes);
    /* One run per drawer of items, in the order given. A name with a line
     * break cannot go in the list. */
    for (i = 0; i < ninputs && rc == 0; i++) {
        BPTR l = taken[i] ? BNULL : Lock((STRPTR)inputs[i], SHARED_LOCK), dir;
        int n = 0, k;
        if (!l)
            continue;
        dir = ParentDir(l);
        UnLock(l);
        for (k = i; k < ninputs; k++) {
            BPTR lk, kp;
            if (taken[k] || !(lk = Lock((STRPTR)inputs[k], SHARED_LOCK)))
                continue;
            kp = ParentDir(lk);
            UnLock(lk);
            if (kp && SameLock(kp, dir) == LOCK_SAME) {
                const char *nm = (const char *)FilePart((STRPTR)inputs[k]);
                taken[k] = 1;
                if (strchr(nm, '\n')) {
                    msg(p, "error: \"%s\" has a line break in its name; not packed", inputs[k]);
                    p->r->errors++;
                } else {
                    names[n++] = (char *)nm;
                }
            }
            if (kp)
                UnLock(kp);
        }
        if (n)
            rc = lha_run(p, prog, dir, names, n, arcpath, err, errlen);
        UnLock(dir);
    }
    free(names);
    free(taken);
    plist_free(&pl);
    if (rc == 0)
        finishing(p);
    if (rc == 0 && !p->r->files && !p->r->dirs) {
        p->r->failed = 1;
        snprintf(p->r->why, sizeof(p->r->why), "nothing could be packed");
        rc = -1;
    }
    if (rc == 0) {
        BPTR old = CurrentDir(p->outdir), x = Lock((STRPTR)p->tmp, SHARED_LOCK);
        CurrentDir(old);
        if (x) {
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            if (fib && Examine(x, fib))
                p->r->bytes_out = UA_FIB_SIZE(fib);
            FreeDosObject(DOS_FIB, fib);
            UnLock(x);
        } else {
            p->r->failed = 1;
            snprintf(p->r->why, sizeof(p->r->why), "%s made no archive", LHA_PROG);
            rc = -1;
        }
    }
    if (rc < 0 && !p->r->failed && !p->cancel && err[0])
        snprintf(p->r->why, sizeof(p->r->why), "%s", err);
    if (rc < 0 && !p->r->failed)
        p->r->cancelled = p->cancel;
    if (rc < 0 && !p->r->failed && !p->r->cancelled)
        p->r->failed = 1;
    return rc;
}

int ua_pack(const char *archive, char **inputs, int ninputs, ua_pack_opts *o,
            ua_pack_report *r, char *err, int errlen)
{
    pk p;
    plist pl = { NULL, 0, 0 };
    BPTR *base = NULL;
    char drawer[UA_PATHMAX], leaf[256];
    const char *fp;
    int i, rc = 0, exists;
    BPTR old;

    memset(&p, 0, sizeof(p));
    memset(r, 0, sizeof(*r));
    p.o = o;
    p.r = r;
    fp = (const char *)FilePart((STRPTR)archive);
    if (!*fp || strlen(fp) >= sizeof(leaf)) {
        snprintf(err, errlen, "no archive name");
        return -1;
    }
    snprintf(leaf, sizeof(leaf), "%s", fp);
    snprintf(drawer, sizeof(drawer), "%.*s", (int)(fp - archive), archive);
    p.outdir = Lock((STRPTR)(drawer[0] ? drawer : ""), SHARED_LOCK);
    if (!p.outdir) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        snprintf(err, errlen, "cannot open \"%s\": %s", drawer, why);
        return -1;
    }
    old = CurrentDir(p.outdir);
    {
        BPTR x = Lock((STRPTR)leaf, SHARED_LOCK);
        exists = x != BNULL;
        if (x)
            UnLock(x);
    }
    CurrentDir(old);
    if (exists && !o->replace) {
        snprintf(err, errlen, "\"%s\" exists; not replaced", archive);
        UnLock(p.outdir);
        return -1;
    }
    if (o->fmt == UA_PACK_LHA) {
        if (exists) {
            old = CurrentDir(p.outdir);
            p.oldlock = Lock((STRPTR)leaf, SHARED_LOCK);
            CurrentDir(old);
        }
        rc = lha_pack(&p, inputs, ninputs, err, errlen);
        if (p.oldlock)
            UnLock(p.oldlock);
        if (rc == 0 && check_cancel(&p)) {
            rc = -1;
            r->cancelled = 1;
        }
        old = CurrentDir(p.outdir);
        rc = finish(&p, drawer, leaf, exists, rc, 1, err, errlen);
        CurrentDir(old);
        UnLock(p.outdir);
        if (rc < 0 && !err[0] && r->why[0])
            snprintf(err, errlen, "%s", r->why);
        return rc;
    }
    p.buf = malloc(65536);
    p.a = archive_write_new();
    if (!p.buf || !p.a) {
        snprintf(err, errlen, "not enough memory");
        rc = -1;
        goto out;
    }
    if (setup_format(&p, o->fmt, o->level, err, errlen) < 0) {
        rc = -1;
        goto out;
    }
    if (o->fmt == UA_PACK_7Z && scratch_open(&p) < 0) {
        snprintf(err, errlen, "cannot make a temporary drawer in \"%s\"", drawer);
        rc = -1;
        goto out;
    }
    snprintf(p.tmp, sizeof(p.tmp), ".uatmp%lx.pk", (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF));
    old = CurrentDir(p.outdir);
    p.out = Open((STRPTR)p.tmp, MODE_NEWFILE);
    if (p.out)
        p.outlock = Lock((STRPTR)p.tmp, SHARED_LOCK);
    if (p.out && exists)
        p.oldlock = Lock((STRPTR)leaf, SHARED_LOCK);
    CurrentDir(old);
    if (!p.out) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        snprintf(err, errlen, "cannot write in \"%s\": %s", drawer, why);
        rc = -1;
        goto out;
    }
    /* libarchive pads its last block; ZIP and 7z have none, tar.gz is
     * compressed after the padding. */
    archive_write_set_bytes_in_last_block(p.a, 1);
    if (archive_write_open2(p.a, &p, NULL, w_write, w_close, NULL) != ARCHIVE_OK) {
        snprintf(err, errlen, "%s", archive_error_string(p.a));
        rc = -1;
        goto out;
    }
    /* Replacing an archive with one made from that same archive would leave
     * an empty archive and delete the original. Refuse before writing. */
    for (i = 0; i < ninputs && p.oldlock; i++) {
        BPTR l = Lock((STRPTR)inputs[i], SHARED_LOCK);
        int same = l && SameLock(l, p.oldlock) == LOCK_SAME;
        if (l)
            UnLock(l);
        if (same) {
            snprintf(err, errlen, "\"%s\" is one of the files to pack; choose another name",
                     archive);
            rc = -1;
            goto out;
        }
    }
    base = calloc(ninputs ? ninputs : 1, sizeof(BPTR));
    if (!base) {
        rc = nomem(&p);
        goto out;
    }
    rc = scan_inputs(&p, &pl, inputs, ninputs, base, 0);
    if (rc == 0)
        rc = pack_list(&p, &pl, base);
    /* Nothing found to pack (names that do not exist, say): no archive,
     * rather than an empty one reported as made. An empty drawer still
     * counts, as a drawer. */
    if (rc == 0 && !r->files && !r->dirs && r->errors) {
        r->failed = 1;
        snprintf(r->why, sizeof(r->why), "nothing could be packed");
        rc = -1;
    }
    /* 7z compresses most of the data only now. */
    if (rc == 0)
        finishing(&p);
    if (rc == 0 && archive_write_close(p.a) != ARCHIVE_OK) {
        r->failed = !p.cancel;
        if (p.write_failed)
            Fault(p.write_err, NULL, r->why, sizeof(r->why));
        else
            snprintf(r->why, sizeof(r->why), "%s", archive_error_string(p.a));
        rc = -1;
    }
    if (rc < 0 && !r->failed)
        r->cancelled = p.cancel;
    if (rc < 0 && !r->failed && !r->cancelled) {
        r->failed = 1;
        snprintf(r->why, sizeof(r->why), "%s", archive_error_string(p.a));
    }

out:
    /* On failure, do not let free() finish the archive (7z would write all
     * of it out first). */
    if (p.a && rc < 0)
        archive_write_fail(p.a);
    if (p.a)
        archive_write_free(p.a);
    scratch_close(&p);
    for (i = 0; base && i < ninputs; i++)
        if (base[i])
            UnLock(base[i]);
    free(base);
    plist_free(&pl);
    if (p.outlock)
        UnLock(p.outlock);
    if (p.oldlock)
        UnLock(p.oldlock);
    if (p.out && !Close(p.out) && rc == 0) {
        r->failed = 1;
        snprintf(r->why, sizeof(r->why), "the archive could not be closed");
        rc = -1;
    }
    /* The last moment a Cancel can still keep the old archive. */
    if (rc == 0 && check_cancel(&p)) {
        rc = -1;
        r->cancelled = 1;
    }
    old = CurrentDir(p.outdir);
    rc = finish(&p, drawer, leaf, exists, rc, p.out != BNULL, err, errlen);
    CurrentDir(old);
    UnLock(p.outdir);
    free(p.buf);
    if (rc < 0 && !err[0] && r->why[0])
        snprintf(err, errlen, "%s", r->why);
    return rc;
}
