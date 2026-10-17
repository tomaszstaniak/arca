/* SPDX-License-Identifier: MIT */
/* Reading a file at positions past 2 GB. ABIv11's Seek takes and returns a
 * LONG (posixc's lseek goes through the same call), so a position past 2 GB
 * cannot be given; the reader keeps its own 64-bit position and gets there by
 * relative moves of at most 1 GB. Callers check what they find (a CRC, a
 * signature): a move that went wrong is not believed.
 *
 * SFS (ABIv11 2026.09, AROS One 1.3) refuses every seek in a file over 2 GB:
 * its ACTION_SEEK compares the new position with the size as a LONG, which
 * is negative there (rom/filesys/SFS/FS/filesystemmain.c). Reading on works.
 * So when a move fails, the reader reads forward to the position instead,
 * and to go back it opens the file again (when it has the path). Slow, but
 * only on such files. */
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <zlib.h>
#include "ua.h"

static int skip_forward(ua_rdr *r, UQUAD to)
{
    static UBYTE junk[65536];
    while (r->pos < to) {
        UQUAD gap = to - r->pos;
        LONG got = Read(r->fh, junk, gap > sizeof(junk) ? (LONG)sizeof(junk) : (LONG)gap);
        if (got <= 0)
            return 0;
        r->pos += got;
    }
    return 1;
}

int ua_rdr_goto(ua_rdr *r, UQUAD to)
{
    while (r->pos != to) {
        UQUAD gap = to > r->pos ? to - r->pos : r->pos - to;
        LONG step = gap > 0x40000000 ? 0x40000000 : (LONG)gap;
        if (to < r->pos)
            step = -step;
        if (!r->noseek) {
            SetIoErr(0);
            if (Seek(r->fh, step, OFFSET_CURRENT) != -1 || !IoErr()) {
                r->pos += step;
                continue;
            }
            r->noseek = 1;
        }
        if (to > r->pos)
            return skip_forward(r, to);
        /* Back: from the start again. */
        if (!r->path)
            return 0;
        Close(r->fh);
        r->fh = Open((STRPTR)r->path, MODE_OLDFILE);
        r->pos = 0;
        if (!r->fh)
            return 0;
    }
    return 1;
}

LONG ua_rdr_read(ua_rdr *r, void *buf, LONG n)
{
    LONG got = Read(r->fh, buf, n);
    if (got > 0)
        r->pos += got;
    return got;
}

/* The whole of n bytes at a position, or 0. */
int ua_rdr_at(ua_rdr *r, UQUAD at, void *buf, LONG n)
{
    return ua_rdr_goto(r, at) && ua_rdr_read(r, buf, n) == n;
}

/* What a file is now: size, date, and a CRC of its first and last 64 KB.
 * Taken when an archive is listed and again before it is extracted, so that
 * an archive replaced or rewritten in between is refused before anything is
 * written. A change in the middle that keeps the size, the date and both
 * ends is not seen here; the engines' own checks (entry headers, data CRCs)
 * remain for that. */
int ua_stamp_file(const char *path, ua_stamp *st)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    static UBYTE buf[65536];
    ua_rdr r;
    LONG n;
    int ok = 0;
    memset(st, 0, sizeof(*st));
    memset(&r, 0, sizeof(r));
    if (!fib)
        return 0;
    r.fh = Open((STRPTR)path, MODE_OLDFILE);
    r.path = path;
    if (r.fh && ExamineFH(r.fh, fib)) {
        st->size = UA_FIB_SIZE(fib);
        st->date = fib->fib_Date;
        n = st->size < sizeof(buf) ? (LONG)st->size : (LONG)sizeof(buf);
        if (ua_rdr_at(&r, 0, buf, n)) {
            st->crc_head = crc32(0L, buf, (uInt)n);
            if (ua_rdr_at(&r, st->size - n, buf, n)) {
                st->crc_tail = crc32(0L, buf, (uInt)n);
                ok = 1;
            }
        }
    }
    if (r.fh)
        Close(r.fh);
    FreeDosObject(DOS_FIB, fib);
    return ok;
}
