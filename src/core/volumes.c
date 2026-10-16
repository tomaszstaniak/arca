/* SPDX-License-Identifier: MIT */
/* Multi-volume archives: from any one part, find the whole set, put it in
 * order and say what is missing.
 *
 * Names only propose candidates: name.part1.rar ..., name.rar + name.r00 ...,
 * name.7z.001 ... (and .zip.001, .001). The order and the completeness of a
 * RAR set come from the parts themselves: RAR 5 stores a volume number in
 * every part but the first and marks every part but the last as "continued";
 * RAR 2.9/4 marks the first volume, may store volume numbers in the end
 * block, and marks parts that continue. Plain split files (.001, .002, ...)
 * carry nothing of the kind; for them the numbers must run without a gap and
 * the engine checks the content when it reads it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "ua.h"

typedef struct cand {
    char *name;               /* leaf name in the drawer */
    long namenum;             /* number taken from the name */
    long volnum;              /* number from the content, -1 if none */
    int first, cont;          /* content says: first volume / continues */
    int rar;                  /* 4 or 5 for RAR, 0 otherwise */
} cand;

/* ---- reading RAR headers ---- */

static int rd(BPTR fh, void *buf, LONG n)
{
    return Read(fh, buf, n) == n;
}

/* RAR 5 variable-length integer. */
static int vint(const UBYTE *b, int len, int *at, UQUAD *v)
{
    int shift = 0;
    *v = 0;
    while (*at < len && shift < 64) {
        UBYTE c = b[(*at)++];
        *v |= (UQUAD)(c & 0x7F) << shift;
        if (!(c & 0x80))
            return 1;
        shift += 7;
    }
    return 0;
}

static void rar5_scan(BPTR fh, cand *c)
{
    UBYTE hb[512];
    LONG pos = 8;
    int guard = 0;
    c->rar = 5;
    while (guard++ < 100000) {
        UBYTE pre[16];
        UQUAD hsize, type, hflags, extra = 0, data = 0, aflags, vol;
        int at = 0, n;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0)
            return;
        n = Read(fh, pre, sizeof(pre));
        if (n < 7)
            return;
        at = 4;
        if (!vint(pre, n, &at, &hsize) || hsize == 0 || hsize > 0x200000)
            return;
        {
            LONG start = pos + at;       /* header data begins here */
            LONG want = hsize < sizeof(hb) ? (LONG)hsize : (LONG)sizeof(hb);
            if (Seek(fh, start, OFFSET_BEGINNING) < 0 || Read(fh, hb, want) != want)
                return;
            at = 0;
            if (!vint(hb, want, &at, &type) || !vint(hb, want, &at, &hflags))
                return;
            if ((hflags & 1) && !vint(hb, want, &at, &extra))
                return;
            if ((hflags & 2) && !vint(hb, want, &at, &data))
                return;
            if (type == 1) {                    /* main archive header */
                if (!vint(hb, want, &at, &aflags))
                    return;
                if (aflags & 2) {
                    if (!vint(hb, want, &at, &vol))
                        return;
                    c->volnum = (long)vol;
                } else {
                    c->volnum = 0;
                    c->first = (aflags & 1) != 0;
                }
            } else if (type == 5) {             /* end of archive */
                UQUAD eflags = 0;
                vint(hb, want, &at, &eflags);
                c->cont = (eflags & 1) != 0;
                return;
            }
            pos = start + (LONG)hsize + (LONG)data;
        }
    }
}

static void rar4_scan(BPTR fh, cand *c)
{
    LONG pos = 7;
    int guard = 0;
    c->rar = 4;
    while (guard++ < 100000) {
        UBYTE h[40];
        UWORD flags, size;
        UBYTE type;
        UQUAD total;
        LONG n;
        if (Seek(fh, pos, OFFSET_BEGINNING) < 0)
            return;
        n = Read(fh, h, sizeof(h));
        if (n < 7)
            return;
        type = h[2];
        flags = h[3] | (h[4] << 8);
        size = h[5] | (h[6] << 8);
        if (size < 7)
            return;
        total = size;
        if (type == 0x73) {                     /* main header */
            if (flags & 0x0100)
                c->first = 1;
        } else if (type == 0x74 || type == 0x7A) {   /* file or service header */
            UQUAD pack;
            if (n < 11)
                return;
            pack = h[7] | (h[8] << 8) | ((ULONG)h[9] << 16) | ((ULONG)h[10] << 24);
            if ((flags & 0x0100) && n >= 36)      /* LHD_LARGE: high 32 bits */
                pack |= (UQUAD)(h[32] | (h[33] << 8) | ((ULONG)h[34] << 16) |
                                ((ULONG)h[35] << 24)) << 32;
            total += pack;
        } else if (type == 0x7B) {              /* end of archive */
            int at = 7;
            c->cont = (flags & 0x0001) != 0;
            if (flags & 0x0002)
                at += 4;                        /* archive data CRC */
            if ((flags & 0x0008) && n >= at + 2)
                c->volnum = h[at] | (h[at + 1] << 8);
            return;
        } else if (flags & 0x8000) {            /* other block with data */
            if (n < 11)
                return;
            total += h[7] | (h[8] << 8) | ((ULONG)h[9] << 16) | ((ULONG)h[10] << 24);
        }
        pos += (LONG)total;
    }
}

static void scan(BPTR dir, cand *c)
{
    BPTR old = CurrentDir(dir), fh = Open((STRPTR)c->name, MODE_OLDFILE);
    UBYTE sig[8];
    CurrentDir(old);
    c->volnum = -1;
    if (!fh)
        return;
    if (rd(fh, sig, 8) && !memcmp(sig, "Rar!\x1A\x07", 6)) {
        if (sig[6] == 1 && sig[7] == 0)
            rar5_scan(fh, c);
        else if (sig[6] == 0)
            rar4_scan(fh, c);
    }
    Close(fh);
}

/* ---- names ---- */

typedef enum { FAM_NONE, FAM_PART, FAM_RNN, FAM_SPLIT } family;

/* Which family a name belongs to, its base (everything before the volume
 * part) and its position in the set by name, from 0: name.part1.rar is 0,
 * name.rar 0 and name.r00 1, name.001 0. */
static family classify(const char *n, char *base, size_t bn, long *num)
{
    size_t l = strlen(n);
    const char *p;
    /* name.partNN.rar */
    if (l > 9 && !strcasecmp(n + l - 4, ".rar")) {
        const char *e = n + l - 4, *d = e;
        while (d > n && isdigit((unsigned char)d[-1]))
            d--;
        if (d < e && d - n >= 5 && !strncasecmp(d - 5, ".part", 5)) {
            snprintf(base, bn, "%.*s", (int)(d - 5 - n), n);
            *num = atol(d) - 1;
            return FAM_PART;
        }
    }
    /* name.rar / name.rNN */
    if (l > 4 && !strcasecmp(n + l - 4, ".rar")) {
        snprintf(base, bn, "%.*s", (int)(l - 4), n);
        *num = 0;                                /* the first, before .r00 */
        return FAM_RNN;
    }
    p = strrchr(n, '.');
    if (p && (p[1] == 'r' || p[1] == 'R') && strlen(p) >= 4) {
        const char *d = p + 2;
        while (*d && isdigit((unsigned char)*d))
            d++;
        if (!*d && d - (p + 2) >= 2) {
            snprintf(base, bn, "%.*s", (int)(p - n), n);
            *num = atol(p + 2) + 1;
            return FAM_RNN;
        }
    }
    /* name.7z.001, name.zip.001, name.001 */
    if (p && strlen(p) >= 4) {
        const char *d = p + 1;
        while (*d && isdigit((unsigned char)*d))
            d++;
        if (!*d && d - (p + 1) >= 3) {
            snprintf(base, bn, "%.*s", (int)(p - n), n);
            *num = atol(p + 1) - 1;
            return FAM_SPLIT;
        }
    }
    return FAM_NONE;
}

/* The file name volume idx (from 0) would have in this family. */
static void expected_name(family fam, const char *base, int width, long idx, char *out, size_t n)
{
    if (fam == FAM_PART)
        snprintf(out, n, "%s.part%0*ld.rar", base, width, idx + 1);
    else if (fam == FAM_RNN)
        snprintf(out, n, idx ? "%s.r%02ld" : "%s.rar", base, idx - 1);
    else
        snprintf(out, n, "%s.%03ld", base, idx + 1);
}

static int cmp_cand(const void *a, const void *b)
{
    const cand *x = a, *y = b;
    long kx = x->volnum >= 0 ? x->volnum : x->namenum;
    long ky = y->volnum >= 0 ? y->volnum : y->namenum;
    if (x->first != y->first)
        return x->first ? -1 : 1;
    return kx < ky ? -1 : kx > ky ? 1 : 0;
}

void ua_volumes_free(ua_volumes *v)
{
    int i;
    for (i = 0; v->path && v->path[i]; i++)
        free(v->path[i]);
    free(v->path);
    memset(v, 0, sizeof(*v));
}

int ua_volumes_find(const char *path, ua_volumes *v)
{
    char drawer[UA_PATHMAX], base[256], b2[256];
    const char *leaf = (const char *)FilePart((STRPTR)path);
    family fam;
    long num, i, n = 0, cap = 0;
    int width = 1;
    char want[300];
    cand *c = NULL;
    BPTR dir;
    struct FileInfoBlock *fib;

    memset(v, 0, sizeof(*v));
    fam = classify(leaf, base, sizeof(base), &num);
    if (fam == FAM_NONE)
        return 0;
    if (fam == FAM_PART) {
        /* Digits in "partNN" as given, for naming a missing part alike. */
        const char *d = leaf + strlen(leaf) - 4;
        width = 0;
        while (d > leaf && d[-1] >= '0' && d[-1] <= '9') {
            d--;
            width++;
        }
    }
    /* name.rar alone, with no .r00 or .part beside it, is one archive. */
    snprintf(drawer, sizeof(drawer), "%.*s", (int)(leaf - path), path);
    dir = Lock((STRPTR)(drawer[0] ? drawer : ""), SHARED_LOCK);
    if (!dir)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(dir, fib)) {
        while (ExNext(dir, fib)) {
            long nn;
            if (fib->fib_DirEntryType > 0)
                continue;
            if (classify(fib->fib_FileName, b2, sizeof(b2), &nn) != fam || strcasecmp(b2, base))
                continue;
            if (n == cap) {
                cand *nc = realloc(c, (cap = cap ? cap * 2 : 16) * sizeof(*c));
                if (!nc)
                    break;
                c = nc;
            }
            memset(&c[n], 0, sizeof(c[n]));
            c[n].name = ua_strdup(fib->fib_FileName);
            c[n].namenum = nn;
            n++;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    if (n < 2 && fam != FAM_SPLIT) {
        /* One file: an ordinary archive, unless its content says it is part
         * of a set (a continued RAR volume with its partners missing). */
        if (n == 1) {
            scan(dir, &c[0]);
            if (c[0].cont || (c[0].volnum > 0)) {
                v->multi = 1;
                v->count = 0;
                snprintf(v->problem, sizeof(v->problem), "\"%s\" is one volume of a set and "
                         "the other volumes are not in this drawer", c[0].name);
            }
            free(c[0].name);
        }
        free(c);
        UnLock(dir);
        return v->multi;
    }
    for (i = 0; i < n; i++)
        scan(dir, &c[i]);
    UnLock(dir);
    qsort(c, n, sizeof(*c), cmp_cand);
#ifdef VOL_DEBUG
    for (i = 0; i < n; i++)
        printf("  [%s name %ld vol %ld first %d cont %d rar %d]\n", c[i].name, c[i].namenum,
               c[i].volnum, c[i].first, c[i].cont, c[i].rar);
#endif
    v->multi = 1;
    v->count = n;
    v->path = calloc(n + 1, sizeof(char *));
    for (i = 0; v->path && i < n; i++) {
        char p[UA_PATHMAX];
        snprintf(p, sizeof(p), "%s%s", drawer, c[i].name);
        v->path[i] = ua_strdup(p);
    }
    /* Completeness, from the content where there is any. */
    v->problem[0] = 0;
    if (c[0].rar) {
        long expect = 0;
        if (c[0].rar == 4 && !c[0].first && c[0].volnum <= 0) {
            expected_name(fam, base, width, 0, want, sizeof(want));
            snprintf(v->problem, sizeof(v->problem), "the first volume (%s) is missing", want);
        }
        for (i = 0; i < n && !v->problem[0]; i++, expect++) {
            long k = c[i].volnum >= 0 ? c[i].volnum : -1;
            if (k >= 0 && k != expect) {
                expected_name(fam, base, width, expect, want, sizeof(want));
                if (expect == 0)
                    snprintf(v->problem, sizeof(v->problem), "the first volume (%s) is missing",
                             want);
                else
                    snprintf(v->problem, sizeof(v->problem), "volume %ld (%s) is missing; the "
                             "next one here is volume %ld", expect + 1, want, k + 1);
            } else if (i < n - 1 && !c[i].cont)
                snprintf(v->problem, sizeof(v->problem), "\"%s\" ends the set, but \"%s\" "
                         "follows", c[i].name, c[i + 1].name);
        }
        if (!v->problem[0] && c[n - 1].cont) {
            expected_name(fam, base, width, n, want, sizeof(want));
            snprintf(v->problem, sizeof(v->problem), "volume %ld (%s) is missing: the set "
                     "continues after \"%s\"", n + 1, want, c[n - 1].name);
        }
    } else {
        /* Names only: the numbers must run without a gap. */
        for (i = 0; i < n && !v->problem[0]; i++)
            if (c[i].namenum != i) {
                expected_name(fam, base, width, i, want, sizeof(want));
                snprintf(v->problem, sizeof(v->problem), "part %ld (%s) is missing", i + 1, want);
            }
    }
    v->first_given = !strcasecmp(c[0].name, leaf);
    snprintf(v->setname, sizeof(v->setname), "%s", base);
    for (i = 0; i < n; i++)
        free(c[i].name);
    free(c);
    return 1;
}
