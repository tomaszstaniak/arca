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
#include <zlib.h>
#include "ua.h"

typedef struct cand {
    char *name;               /* leaf name in the drawer */
    long namenum;             /* number taken from the name */
    long volnum;              /* number from the content, -1 if none */
    int first, cont;          /* content says: first volume / continues */
    int rar;                  /* 4 or 5 for RAR, 0 otherwise */
    int oom;                  /* reading its headers ran out of memory */
} cand;

/* ---- reading RAR headers ---- */

/* Headers past 2 GB are reached through ua_rdr (bigfile.c); each one is
 * checked against its CRC, so a move that went wrong is not taken for a
 * header. */
/* The whole header at the current position, n bytes, into a buffer that
 * grows as needed. */
static UBYTE *r_take(ua_rdr *r, UBYTE **buf, ULONG *cap, ULONG n, int *oom)
{
    if (n > *cap) {
        UBYTE *nb = realloc(*buf, n);
        if (!nb) {
            *oom = 1;
            return NULL;
        }
        *buf = nb;
        *cap = n;
    }
    return ua_rdr_read(r, *buf, (LONG)n) == (LONG)n ? *buf : NULL;
}

/* RAR 5 variable-length integer. */
static int vint(const UBYTE *b, ULONG len, ULONG *at, UQUAD *v)
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

static ULONG le32(const UBYTE *b)
{
    return b[0] | (b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

/* RAR 5: a header is CRC32, size (vint), then size bytes; the CRC covers the
 * size and the bytes. */
static void rar5_scan(ua_rdr *r, cand *c)
{
    UBYTE *hb = NULL;
    ULONG cap = 0;
    UQUAD pos = 8;
    int guard = 0;
    c->rar = 5;
    while (guard++ < 100000) {
        UBYTE pre[16];
        UQUAD hsize, type, hflags, extra = 0, data = 0, aflags, vol;
        ULONG at = 4, n, szlen;
        if (!ua_rdr_goto(r, pos) || (LONG)(n = ua_rdr_read(r, pre, sizeof(pre))) < 7)
            break;
        if (!vint(pre, n, &at, &hsize) || hsize == 0 || hsize > 0x200000)
            break;
        szlen = at - 4;
        if (!ua_rdr_goto(r, pos + at) || !r_take(r, &hb, &cap, (ULONG)hsize, &c->oom))
            break;
        {
            uLong crc = crc32(0L, pre + 4, szlen);
            if (crc32(crc, hb, (uInt)hsize) != le32(pre))
                break;
        }
        at = 0;
        if (!vint(hb, hsize, &at, &type) || !vint(hb, hsize, &at, &hflags))
            break;
        if ((hflags & 1) && !vint(hb, hsize, &at, &extra))
            break;
        if ((hflags & 2) && !vint(hb, hsize, &at, &data))
            break;
        if (type == 1) {                    /* main archive header */
            if (!vint(hb, hsize, &at, &aflags))
                break;
            if (aflags & 2) {
                if (!vint(hb, hsize, &at, &vol))
                    break;
                c->volnum = (long)vol;
            } else {
                c->volnum = 0;
                c->first = (aflags & 1) != 0;
            }
        } else if (type == 5) {             /* end of archive */
            UQUAD eflags = 0;
            vint(hb, hsize, &at, &eflags);
            c->cont = (eflags & 1) != 0;
            break;
        }
        pos += 4 + szlen + hsize + data;
    }
    free(hb);
}

/* RAR 1.5-4: CRC16 (the low half of a CRC32 of the header from its type
 * byte on), type, flags, size; then the header's data area if it has one. */
static void rar4_scan(ua_rdr *r, cand *c)
{
    UBYTE *h = NULL;
    ULONG cap = 0;
    UQUAD pos = 7;
    int guard = 0;
    c->rar = 4;
    while (guard++ < 100000) {
        UBYTE pre[7];
        UWORD flags, size;
        UBYTE type;
        UQUAD total;
        if (!ua_rdr_goto(r, pos) || ua_rdr_read(r, pre, 7) != 7)
            break;
        type = pre[2];
        flags = pre[3] | (pre[4] << 8);
        size = pre[5] | (pre[6] << 8);
        if (size < 7 || !r_take(r, &h, &cap, size - 7, &c->oom))
            break;
        if ((crc32(crc32(0L, pre + 2, 5), h, size - 7) & 0xFFFF) != (ULONG)(pre[0] | (pre[1] << 8)))
            break;
        total = size;
        /* h holds the header from offset 7 on. */
        if (type == 0x73) {                     /* main header */
            if (flags & 0x0100)
                c->first = 1;
        } else if (type == 0x74 || type == 0x7A) {   /* file or service header */
            UQUAD pack;
            if (size < 11)
                break;
            pack = le32(h);
            if ((flags & 0x0100) && size >= 36)   /* LHD_LARGE: high 32 bits */
                pack |= (UQUAD)le32(h + 25) << 32;
            total += pack;
        } else if (type == 0x7B) {              /* end of archive */
            ULONG at = 0;
            c->cont = (flags & 0x0001) != 0;
            if (flags & 0x0002)
                at += 4;                        /* archive data CRC */
            if ((flags & 0x0008) && size >= 7 + at + 2)
                c->volnum = h[at] | (h[at + 1] << 8);
            break;
        } else if (flags & 0x8000) {            /* other block with data */
            if (size < 11)
                break;
            total += le32(h);
        }
        pos += total;
    }
    free(h);
}

static void scan(BPTR dir, cand *c)
{
    BPTR old = CurrentDir(dir);
    ua_rdr r;
    UBYTE sig[8];
    r.fh = Open((STRPTR)c->name, MODE_OLDFILE);
    r.pos = 0;
    CurrentDir(old);
    c->volnum = -1;
    if (!r.fh)
        return;
    if (ua_rdr_read(&r, sig, 8) == 8 && !memcmp(sig, "Rar!\x1A\x07", 6)) {
        if (sig[6] == 1 && sig[7] == 0)
            rar5_scan(&r, c);
        else if (sig[6] == 0)
            rar4_scan(&r, c);
    }
    Close(r.fh);
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
    const char *bad = NULL;

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
    snprintf(drawer, sizeof(drawer), "%.*s", (int)(leaf - path), path);
    dir = Lock((STRPTR)(drawer[0] ? drawer : ""), SHARED_LOCK);
    if (!dir)
        return 0;                 /* opening the archive will say why */
    fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(dir, fib))
        bad = "the drawer could not be read";
    while (!bad) {
        long nn;
        if (!ExNext(dir, fib)) {
            if (IoErr() != ERROR_NO_MORE_ENTRIES)
                bad = "the drawer could not be read completely";
            break;
        }
        if (fib->fib_DirEntryType > 0)
            continue;
        if (classify(fib->fib_FileName, b2, sizeof(b2), &nn) != fam || strcasecmp(b2, base))
            continue;
        if (n == cap) {
            cand *nc = realloc(c, (cap ? cap * 2 : 16) * sizeof(*c));
            if (!nc) {
                bad = "out of memory";
                break;
            }
            c = nc;
            cap = cap ? cap * 2 : 16;
        }
        memset(&c[n], 0, sizeof(c[n]));
        if (!(c[n].name = ua_strdup(fib->fib_FileName))) {
            bad = "out of memory";
            break;
        }
        c[n].namenum = nn;
        n++;
    }
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (bad) {
        /* A set that cannot be seen whole is refused, not guessed at. */
        v->multi = 1;
        snprintf(v->problem, sizeof(v->problem), "its volumes could not be found: %s", bad);
        goto out;
    }
    if (n == 0)
        goto out;                 /* the file named is not there: not a set */
    if (n < 2 && fam != FAM_SPLIT) {
        /* name.rar alone, with no .r00 or .part beside it: an ordinary archive, unless its content says it is part
         * of a set (a continued RAR volume with its partners missing). */
        scan(dir, &c[0]);
        if (c[0].oom) {
            v->multi = 1;
            snprintf(v->problem, sizeof(v->problem), "its volumes could not be read: out of memory");
        } else if (c[0].cont || (c[0].volnum > 0)) {
            v->multi = 1;
            snprintf(v->problem, sizeof(v->problem), "\"%s\" is one volume of a set and "
                     "the other volumes are not in this drawer", c[0].name);
        }
        goto out;
    }
    for (i = 0; i < n; i++) {
        scan(dir, &c[i]);
        if (c[i].oom) {
            v->multi = 1;
            snprintf(v->problem, sizeof(v->problem), "its volumes could not be read: out of memory");
            goto out;
        }
    }
    qsort(c, n, sizeof(*c), cmp_cand);
#ifdef VOL_DEBUG
    for (i = 0; i < n; i++)
        printf("  [%s name %ld vol %ld first %d cont %d rar %d]\n", c[i].name, c[i].namenum,
               c[i].volnum, c[i].first, c[i].cont, c[i].rar);
#endif
    v->multi = 1;
    v->path = calloc(n + 1, sizeof(char *));
    for (i = 0; v->path && i < n; i++) {
        char p[UA_PATHMAX];
        snprintf(p, sizeof(p), "%s%s", drawer, c[i].name);
        if (!(v->path[i] = ua_strdup(p)))
            break;
    }
    if (!v->path || i < n) {
        for (i = 0; v->path && v->path[i]; i++)
            free(v->path[i]);
        free(v->path);
        v->path = NULL;
        snprintf(v->problem, sizeof(v->problem), "its volumes could not be listed: out of memory");
        goto out;
    }
    v->count = n;
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
out:
    for (i = 0; i < n; i++)
        free(c[i].name);
    free(c);
    UnLock(dir);
    return v->multi;
}
