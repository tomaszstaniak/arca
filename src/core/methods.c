/* SPDX-License-Identifier: MIT */
/* What an archive asks of a decoder, read from its own index before anything
 * is written: is the entry list encrypted, is some entry's data encrypted,
 * and is there a method that libarchive (as built here) cannot decode. The
 * libarchive engine reads 7z and ZIP first, as the faster engine, and uses
 * this to pass such an archive on to 7-Zip before it starts, instead of
 * failing on an entry part way through an extraction.
 *
 * 7z: the signature header points to the next header at the end; that is
 * either the header itself or a packed one (LZMA, decoded with liblzma), whose
 * folders list each coder by id. ZIP: the central directory lists each entry's
 * method and flags; WinZip AES keeps the real method in an extra field.
 *
 * The supported sets follow libarchive 3.9's readers with the options of
 * scripts/build-deps-abiv11.sh (zlib, bzip2, liblzma; no zstd, no crypto). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <lzma.h>
#include <zlib.h>
#include "ua.h"

#define MAXHDR (32UL << 20)        /* larger indexes are not judged here */

typedef struct cur {
    const UBYTE *p, *end;
} cur;

static int u8(cur *c, UBYTE *v)
{
    if (c->p >= c->end)
        return 0;
    *v = *c->p++;
    return 1;
}

/* 7z's variable-length number: the high bits of the first byte say how many
 * bytes follow. */
static int num(cur *c, UQUAD *v)
{
    UBYTE first, mask = 0x80;
    int i;
    if (!u8(c, &first))
        return 0;
    *v = 0;
    for (i = 0; i < 8; i++) {
        UBYTE b;
        if (!(first & mask)) {
            *v |= (UQUAD)(first & (mask - 1)) << (8 * i);
            return 1;
        }
        if (!u8(c, &b))
            return 0;
        *v |= (UQUAD)b << (8 * i);
        mask >>= 1;
    }
    return 1;
}

static int skip(cur *c, UQUAD n)
{
    if (n > (UQUAD)(c->end - c->p))
        return 0;
    c->p += n;
    return 1;
}

static ULONG le32(const UBYTE *b)
{
    return b[0] | (b[1] << 8) | ((ULONG)b[2] << 16) | ((ULONG)b[3] << 24);
}

static UQUAD le64(const UBYTE *b)
{
    return le32(b) | (UQUAD)le32(b + 4) << 32;
}

static UWORD le16(const UBYTE *b)
{
    return b[0] | (b[1] << 8);
}

/* ---------------------------------------------------------------- 7z */

#define C7_COPY     0x00
#define C7_DELTA    0x03
#define C7_ARM64    0x0A
#define C7_RISCV    0x0B
#define C7_LZMA2    0x21
#define C7_LZMA     0x030101
#define C7_BCJ      0x03030103
#define C7_BCJ2     0x0303011B
#define C7_PPC      0x03030205
#define C7_IA64     0x03030401
#define C7_ARM      0x03030501
#define C7_ARMT     0x03030701
#define C7_SPARC    0x03030805
#define C7_PPMD     0x030401
#define C7_DEFLATE  0x040108
#define C7_BZIP2    0x040202
#define C7_AES      0x06F10701

typedef struct folder {
    int ncoders;
    UQUAD nout;                /* out streams: one size each follows the folders */
    UQUAD id[8];
    const UBYTE *props0;       /* the first coder's properties */
    UQUAD nprops0;
} folder;

static const char *c7_name(UQUAD id)
{
    switch (id) {
    case 0x040109: return "Deflate64";
    case 0x04F71101: return "Zstandard";
    case 0x04F71102: return "Brotli";
    case 0x04F71104: return "LZ4";
    case 0x04F71105: return "LZ5";
    case 0x04F71106: return "Lizard";
    case 0x020302: return "Swap2";
    case 0x020304: return "Swap4";
    default: return NULL;
    }
}

static void judge_folder(const folder *f, ua_needs *n)
{
    int i, bcj2 = 0, enc = 0;
    for (i = 0; i < f->ncoders && i < 8; i++) {
        UQUAD id = f->id[i];
        switch (id) {
        case 0x06F10101: case 0x06F10303: case C7_AES:
            enc = 1;
            break;
        case C7_BCJ2:
            bcj2++;
            break;
        case C7_COPY: case C7_LZMA: case C7_LZMA2: case C7_DEFLATE: case C7_BZIP2: case C7_PPMD:
        case C7_DELTA: case C7_BCJ: case C7_PPC: case C7_IA64: case C7_ARM: case C7_ARMT:
        case C7_ARM64: case C7_RISCV: case C7_SPARC:
            break;
        default:
            if (!n->unsupported[0]) {
                const char *nm = c7_name(id);
                if (nm)
                    snprintf(n->unsupported, sizeof(n->unsupported), "the %s method", nm);
                else
                    snprintf(n->unsupported, sizeof(n->unsupported), "7z method %llX",
                             (unsigned long long)id);
            }
        }
    }
    if (enc)
        n->data_encrypted = 1;
    else if (((f->ncoders > 2 && !bcj2) || bcj2 > 1) && !n->unsupported[0])
        snprintf(n->unsupported, sizeof(n->unsupported), "a chain of %d coders", f->ncoders);
}

static int read_folder(cur *c, folder *f)
{
    UQUAD nc, i, tin = 0, tout = 0, k;
    memset(f, 0, sizeof(*f));
    if (!num(c, &nc) || nc == 0 || nc > 64)
        return 0;
    f->ncoders = (int)nc;
    for (i = 0; i < nc; i++) {
        UBYTE fl;
        UQUAD id = 0;
        int sz, b;
        if (!u8(c, &fl) || (fl & 0x80))          /* alternative methods: never used */
            return 0;
        sz = fl & 15;
        if (sz > c->end - c->p)
            return 0;
        for (b = 0; b < sz; b++)
            id = (id << 8) | *c->p++;
        if (i < 8)
            f->id[i] = sz <= 8 ? id : ~(UQUAD)0;
        if (fl & 0x10) {
            UQUAD ni, no;
            if (!num(c, &ni) || !num(c, &no) || ni > 64 || no > 64)
                return 0;
            tin += ni;
            tout += no;
        } else {
            tin++;
            tout++;
        }
        if (fl & 0x20) {
            UQUAD ps;
            if (!num(c, &ps) || ps > (UQUAD)(c->end - c->p))
                return 0;
            if (i == 0) {
                f->props0 = c->p;
                f->nprops0 = ps;
            }
            c->p += ps;
        }
    }
    if (tout == 0 || tin < tout - 1)
        return 0;
    f->nout = tout;
    for (k = 0; k < tout - 1; k++) {               /* bind pairs */
        UQUAD a, b2;
        if (!num(c, &a) || !num(c, &b2))
            return 0;
    }
    if (tin - (tout - 1) > 1)                       /* packed streams */
        for (k = 0; k < tin - (tout - 1); k++) {
            UQUAD a;
            if (!num(c, &a))
                return 0;
        }
    return 1;
}

static int digests(cur *c, UQUAD count)
{
    UBYTE all;
    UQUAD i, defined = count;
    if (!u8(c, &all))
        return 0;
    if (!all) {
        defined = 0;
        for (i = 0; i < count; i += 8) {
            UBYTE bits;
            int k;
            if (!u8(c, &bits))
                return 0;
            for (k = 0; k < 8 && i + k < count; k++)
                defined += (bits >> (7 - k)) & 1;
        }
    }
    return skip(c, defined * 4);
}

/* StreamsInfo up to and including its UnpackInfo; every folder is judged.
 * For the packed header, the first pack stream and folder are returned. */
static int read_streams(cur *c, ua_needs *n, UQUAD *packpos, UQUAD *packsize, folder *f0,
                        UQUAD *unpack0)
{
    UBYTE t;
    UQUAD i, npack = 0, nfold = 0;
    if (!u8(c, &t))
        return 0;
    if (t == 0x06) {                                /* PackInfo */
        UQUAD pos;
        if (!num(c, &pos) || !num(c, &npack))
            return 0;
        if (packpos)
            *packpos = pos;
        for (;;) {
            if (!u8(c, &t))
                return 0;
            if (t == 0)
                break;
            if (t == 0x09) {
                for (i = 0; i < npack; i++) {
                    UQUAD s;
                    if (!num(c, &s))
                        return 0;
                    if (i == 0 && packsize)
                        *packsize = s;
                }
            } else if (t == 0x0A) {
                if (!digests(c, npack))
                    return 0;
            } else {
                return 0;
            }
        }
        if (!u8(c, &t))
            return 0;
    }
    if (t == 0x07) {                                /* UnpackInfo */
        UBYTE ext;
        ULONG *outs;
        if (!u8(c, &t) || t != 0x0B || !num(c, &nfold) || nfold > (1UL << 22) ||
            !u8(c, &ext) || ext)
            return 0;
        outs = malloc((nfold ? nfold : 1) * sizeof(*outs));
        if (!outs)
            return 0;
        for (i = 0; i < nfold; i++) {
            folder f;
            if (!read_folder(c, &f)) {
                free(outs);
                return 0;
            }
            judge_folder(&f, n);
            if (i == 0 && f0)
                *f0 = f;
            outs[i] = (ULONG)f.nout;
        }
        if (!u8(c, &t) || t != 0x0C) {
            free(outs);
            return 0;
        }
        for (i = 0; i < nfold; i++) {
            ULONG k;
            for (k = 0; k < outs[i]; k++) {
                UQUAD s;
                if (!num(c, &s)) {
                    free(outs);
                    return 0;
                }
                /* The packed header is one coder: its only size is the result. */
                if (i == 0 && unpack0)
                    *unpack0 = s;
            }
        }
        free(outs);
    }
    return 1;
}

/* The decoded header: judge the folders of its streams. */
static int judge_header(cur *c, ua_needs *n)
{
    UBYTE t;
    if (!u8(c, &t) || t != 0x01)
        return 0;
    for (;;) {
        if (!u8(c, &t))
            return 0;
        if (t == 0x02) {                            /* archive properties */
            for (;;) {
                UBYTE pt;
                UQUAD sz;
                if (!u8(c, &pt))
                    return 0;
                if (!pt)
                    break;
                if (!num(c, &sz) || !skip(c, sz))
                    return 0;
            }
        } else if (t == 0x04) {                     /* main streams */
            return read_streams(c, n, NULL, NULL, NULL, NULL);   /* files follow */
        } else if (t == 0x03) {
            return 0;      /* additional streams: 7-Zip writes none; not judged */
        } else {
            return 1;                               /* files only, or the end */
        }
    }
}

static UBYTE *lzma_unpack(const folder *f, const UBYTE *in, UQUAD inlen, UQUAD outlen)
{
    lzma_filter filters[2];
    lzma_stream s = LZMA_STREAM_INIT;
    UBYTE *out;
    lzma_ret r;
    if (outlen == 0 || outlen > MAXHDR || f->ncoders != 1)
        return NULL;
    filters[0].id = f->id[0] == C7_LZMA ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2;
    filters[0].options = NULL;
    filters[1].id = LZMA_VLI_UNKNOWN;
    if ((f->id[0] != C7_LZMA && f->id[0] != C7_LZMA2) ||
        lzma_properties_decode(&filters[0], NULL, f->props0, (size_t)f->nprops0) != LZMA_OK)
        return NULL;
    out = malloc((size_t)outlen);
    if (out && lzma_raw_decoder(&s, filters) == LZMA_OK) {
        s.next_in = in;
        s.avail_in = (size_t)inlen;
        s.next_out = out;
        s.avail_out = (size_t)outlen;
        r = lzma_code(&s, LZMA_FINISH);
        lzma_end(&s);
        if ((r == LZMA_OK || r == LZMA_STREAM_END) && s.avail_out == 0) {
            free(filters[0].options);
            return out;
        }
    }
    free(filters[0].options);
    free(out);
    return NULL;
}

static int needs_7z(ua_rdr *r, ua_needs *n)
{
    UBYTE sh[32], *hdr = NULL, *dec = NULL;
    UQUAD off, size;
    int ok = 0;
    cur c;
    if (!ua_rdr_at(r, 0, sh, 32) || memcmp(sh, "7z\xBC\xAF\x27\x1C", 6) ||
        crc32(0L, sh + 12, 20) != le32(sh + 8))
        return 0;
    off = le64(sh + 12);
    size = le64(sh + 20);
    if (size == 0)
        return 1;                                   /* an empty archive */
    if (size > MAXHDR || !(hdr = malloc((size_t)size)))
        return 0;
    if (!ua_rdr_at(r, 32 + off, hdr, (LONG)size) || crc32(0L, hdr, (uInt)size) != le32(sh + 28))
        goto out;
    c.p = hdr;
    c.end = hdr + size;
    if (hdr[0] == 0x17) {                           /* packed header */
        folder f;
        UQUAD ppos = 0, psize = 0, usize = 0;
        UBYTE *packed;
        ua_needs hn;
        memset(&hn, 0, sizeof(hn));
        c.p++;
        if (!read_streams(&c, &hn, &ppos, &psize, &f, &usize))
            goto out;
        if (hn.data_encrypted) {
            n->list_encrypted = 1;
            ok = 1;
            goto out;
        }
        if (psize == 0 || psize > MAXHDR || !(packed = malloc((size_t)psize)))
            goto out;
        if (ua_rdr_at(r, 32 + ppos, packed, (LONG)psize))
            dec = lzma_unpack(&f, packed, psize, usize);
        free(packed);
        if (!dec)
            goto out;
        c.p = dec;
        c.end = dec + usize;
    }
    ok = judge_header(&c, n);
out:
    free(hdr);
    free(dec);
    return ok;
}

/* ---------------------------------------------------------------- ZIP */

static const char *zip_name(UWORD m)
{
    switch (m) {
    case 1: return "Shrink";
    case 2: case 3: case 4: case 5: return "Reduce";
    case 6: return "Implode";
    case 9: return "Deflate64";
    case 10: return "PKWARE DCL Implode";
    case 93: return "Zstandard";
    case 96: return "JPEG";
    case 97: return "WavPack";
    default: return NULL;
    }
}

static void judge_zip_method(UWORD m, ua_needs *n)
{
    switch (m) {
    case 0: case 8: case 12: case 14: case 95: case 98:
        return;
    default:
        if (!n->unsupported[0]) {
            const char *nm = zip_name(m);
            if (nm)
                snprintf(n->unsupported, sizeof(n->unsupported), "the %s method", nm);
            else
                snprintf(n->unsupported, sizeof(n->unsupported), "ZIP method %u", m);
        }
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* An extra field with a UTC time: Info-ZIP's UT (0x5455) and Unix (0x5855),
 * NTFS (0x000a). Without one, the entry's time is the DOS field: local time. */
static int has_utc_time(const UBYTE *x, UWORD xlen)
{
    const UBYTE *xe = x + xlen;
    while (xe - x >= 4) {
        UWORD id = le16(x), sz = le16(x + 2);
        if (sz > xe - x - 4)
            break;
        if (id == 0x5455 || id == 0x5855 || id == 0x000a)
            return 1;
        x += 4 + sz;
    }
    return 0;
}

static int needs_zip(ua_rdr *r, UQUAD fsize, ua_needs *n)
{
    UBYTE *tail, *cd = NULL;
    ULONG tlen = fsize < 65557 ? (ULONG)fsize : 65557, i;
    UQUAD eocd = 0, cdend, cdoff, cdsize, entries, k;
    LONG at = -1;
    const UBYTE *e;
    int ok = 0;
    if (tlen < 22 || !(tail = malloc(tlen)))
        return 0;
    if (!ua_rdr_at(r, fsize - tlen, tail, (LONG)tlen))
        goto out;
    for (i = tlen - 22 + 1; i-- > 0;)
        if (!memcmp(tail + i, "PK\5\6", 4)) {
            at = (LONG)i;
            break;
        }
    if (at < 0)
        goto out;
    e = tail + at;
    eocd = fsize - tlen + at;
    if (le16(e + 4) != 0 || le16(e + 6) != 0) {
        n->multi_disk = 1;
        snprintf(n->unsupported, sizeof(n->unsupported), "a ZIP split over several files");
        ok = 1;
        goto out;
    }
    entries = le16(e + 10);
    cdsize = le32(e + 12);
    cdoff = le32(e + 16);
    cdend = eocd;
    if (entries == 0xFFFF || cdsize == 0xFFFFFFFF || cdoff == 0xFFFFFFFF) {
        UBYTE loc[20], e64[56];
        if (eocd < 20 || !ua_rdr_at(r, eocd - 20, loc, 20) || memcmp(loc, "PK\6\7", 4) ||
            !ua_rdr_at(r, le64(loc + 8), e64, 56) || memcmp(e64, "PK\6\6", 4))
            goto out;
        if (le32(e64 + 16) != 0 || le32(e64 + 20) != 0) {
            n->multi_disk = 1;
            snprintf(n->unsupported, sizeof(n->unsupported), "a ZIP split over several files");
            ok = 1;
            goto out;
        }
        entries = le64(e64 + 32);
        cdsize = le64(e64 + 40);
        cdoff = le64(e64 + 48);
        cdend = le64(loc + 8);              /* the directory ends at the Zip64 record */
    }
    /* A self-extracting prefix moves everything, so the stored offset is not
     * used: the directory ends where the end record (or the Zip64 one) begins. */
    (void)cdoff;
    if (cdsize > MAXHDR || cdsize > cdend || !(cd = malloc((size_t)cdsize + 1)))
        goto out;
    if (!ua_rdr_at(r, cdend - cdsize, cd, (LONG)cdsize))
        goto out;
    {
        const UBYTE *p = cd, *end = cd + cdsize;
        for (k = 0; k < entries; k++) {
            UWORD flags, method, nlen, xlen, clen;
            if (end - p < 46 || memcmp(p, "PK\1\2", 4))
                goto out;
            flags = le16(p + 8);
            method = le16(p + 10);
            nlen = le16(p + 28);
            xlen = le16(p + 30);
            clen = le16(p + 32);
            if (end - p < 46 + nlen + xlen + clen)
                goto out;
            if (flags & 0x41)                       /* encrypted, or strong encryption */
                n->data_encrypted = 1;
            if (!has_utc_time(p + 46 + nlen, xlen)) {
                char **nd = n->ndos % 256 ? n->dos_names :
                            realloc(n->dos_names, (n->ndos + 256) * sizeof(char *));
                char *nm = malloc(nlen + 1);
                if (!nd || !nm) {
                    free(nm);
                    if (nd)
                        n->dos_names = nd;
                    goto out;
                }
                n->dos_names = nd;
                memcpy(nm, p + 46, nlen);
                nm[nlen] = 0;
                n->dos_names[n->ndos++] = nm;
            }
            if (method == 99) {                     /* WinZip AES: method in extra 0x9901 */
                const UBYTE *x = p + 46 + nlen, *xe = x + xlen;
                n->data_encrypted = 1;
                while (xe - x >= 4) {
                    UWORD id = le16(x), sz = le16(x + 2);
                    if (sz > xe - x - 4)
                        break;
                    if (id == 0x9901 && sz >= 7)
                        method = le16(x + 4 + 5);
                    x += 4 + sz;
                }
                if (method != 99)
                    judge_zip_method(method, n);
            } else {
                judge_zip_method(method, n);
            }
            p += 46 + nlen + xlen + clen;
        }
    }
    if (n->ndos)
        qsort(n->dos_names, n->ndos, sizeof(char *), cmp_str);
    ok = 1;
out:
    free(tail);
    free(cd);
    return ok;
}

/* ---------------------------------------------------------------- */

int ua_needs_read(const char *path, ua_format fmt, ua_needs *n)
{
    ua_rdr r;
    struct FileInfoBlock *fib;
    UQUAD size = 0;
    int ok = 0;
    memset(n, 0, sizeof(*n));
    memset(&r, 0, sizeof(r));
    if (fmt != UA_FMT_7Z && fmt != UA_FMT_ZIP)
        return 0;
    r.fh = Open((STRPTR)path, MODE_OLDFILE);
    r.path = path;
    if (!r.fh)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && ExamineFH(r.fh, fib))
        size = UA_FIB_SIZE(fib);
    if (fib)
        FreeDosObject(DOS_FIB, fib);
    if (fmt == UA_FMT_7Z)
        ok = needs_7z(&r, n);
    else if (size)
        ok = needs_zip(&r, size, n);
    if (r.fh)
        Close(r.fh);
    if (!ok)
        ua_needs_free(n);
    return ok;
}

void ua_needs_free(ua_needs *n)
{
    ULONG i;
    for (i = 0; i < n->ndos; i++)
        free(n->dos_names[i]);
    free(n->dos_names);
    memset(n, 0, sizeof(*n));
}

int ua_needs_dos_time(const ua_needs *n, const char *name)
{
    return n->ndos && bsearch(&name, n->dos_names, n->ndos, sizeof(char *), cmp_str) != NULL;
}
