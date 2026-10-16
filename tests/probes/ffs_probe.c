/* SPDX-License-Identifier: MIT */
/* The call sequence the file layer uses for one new file, on its own, with every
 * return code and the volume's view after each step:
 *   CreateDir <dir>, Lock it again, Open <tmp> MODE_NEWFILE, Write, Close,
 *   Rename <tmp> -> <name>, Examine both names, SetProtection <name>.
 * Run on a fresh FFS disk, then after filling the disk and deleting the filler,
 * which is the state the stage-2b run3 disk was in when the first file of a new
 * directory stayed behind as an empty temporary file.
 * Usage: ffs_probe <directory on the volume> */
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>

static void state(BPTR dir, const char *name)
{
    BPTR old = CurrentDir(dir), l = Lock((STRPTR)name, SHARED_LOCK);
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    CurrentDir(old);
    if (!l) {
        printf("    %-32s absent (IoErr %ld)\n", name, (long)IoErr());
    } else if (fib && Examine(l, fib)) {
        printf("    %-32s %s %ld bytes, stored as \"%s\"\n", name,
               fib->fib_DirEntryType > 0 ? "dir" : "file", (long)fib->fib_Size,
               fib->fib_FileName);
    } else {
        printf("    %-32s locked, Examine failed (IoErr %ld)\n", name, (long)IoErr());
    }
    FreeDosObject(DOS_FIB, fib);
    if (l)
        UnLock(l);
}

static int one(BPTR base, const char *dname, const char *leaf, LONG size, int newdir)
{
    static UBYTE data[65536];
    char tmp[40];
    BPTR dir, old, fh, made;
    LONG w, c, r;
    int good;

    memset(data, 'x', sizeof(data));
    snprintf(tmp, sizeof(tmp), ".uatmp%lx.1", (unsigned long)((IPTR)FindTask(NULL) & 0xFFFFFF));
    printf("case: dir \"%s\"%s, name \"%s\" (%u chars), %ld bytes\n", dname,
           newdir ? " (new)" : "", leaf, (unsigned)strlen(leaf), (long)size);
    old = CurrentDir(base);
    if (newdir) {
        made = CreateDir((STRPTR)dname);
        printf("  CreateDir -> %s (IoErr %ld)\n", made ? "ok" : "FAIL", (long)IoErr());
        if (made)
            UnLock(made);
    }
    dir = Lock((STRPTR)dname, SHARED_LOCK);
    CurrentDir(old);
    if (!dir) {
        printf("  Lock dir FAIL (IoErr %ld)\n", (long)IoErr());
        return 0;
    }
    old = CurrentDir(dir);
    fh = Open((STRPTR)tmp, MODE_NEWFILE);
    printf("  Open -> %s (IoErr %ld)\n", fh ? "ok" : "FAIL", (long)IoErr());
    if (!fh) {
        CurrentDir(old);
        UnLock(dir);
        return 0;
    }
    w = Write(fh, data, size);
    printf("  Write -> %ld (IoErr %ld)\n", (long)w, (long)IoErr());
    c = Close(fh);
    printf("  Close -> %ld (IoErr %ld)\n", (long)c, (long)IoErr());
    CurrentDir(old);
    state(dir, tmp);
    old = CurrentDir(dir);
    r = Rename((STRPTR)tmp, (STRPTR)leaf);
    printf("  Rename -> %ld (IoErr %ld)\n", (long)r, (long)IoErr());
    CurrentDir(old);
    state(dir, tmp);
    state(dir, leaf);
    old = CurrentDir(dir);
    r = SetProtection((STRPTR)leaf, 0);
    printf("  SetProtection -> %ld (IoErr %ld)\n", (long)r, (long)IoErr());
    CurrentDir(old);
    {
        BPTR x;
        old = CurrentDir(dir);
        x = Lock((STRPTR)leaf, SHARED_LOCK);
        CurrentDir(old);
        good = 0;
        if (x) {
            struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
            if (fib && Examine(x, fib) && fib->fib_DirEntryType < 0 && fib->fib_Size == size)
                good = 1;
            FreeDosObject(DOS_FIB, fib);
            UnLock(x);
        }
    }
    printf("  RESULT: %s\n", good ? "ok" : "WRONG");
    UnLock(dir);
    return good;
}

static void fill(BPTR base)
{
    static UBYTE data[4096];
    BPTR old = CurrentDir(base), fh = Open("filler", MODE_NEWFILE);
    LONG total = 0, w;
    if (!fh) {
        printf("fill: Open FAIL (IoErr %ld)\n", (long)IoErr());
        CurrentDir(old);
        return;
    }
    while ((w = Write(fh, data, sizeof(data))) == (LONG)sizeof(data))
        total += w;
    printf("fill: %ld bytes, then Write -> %ld (IoErr %ld)\n", (long)total, (long)w, (long)IoErr());
    printf("fill: Close -> %ld\n", (long)Close(fh));
    printf("fill: DeleteFile -> %ld (IoErr %ld)\n", (long)DeleteFile("filler"), (long)IoErr());
    CurrentDir(old);
}

int main(int argc, char **argv)
{
    static const char n30[] = "L030-xxxxxxxxxxxxxxxxxxxxxxxxx";
    BPTR base;
    int bad = 0;

    if (argc < 2) {
        printf("usage: ffs_probe <directory>\n");
        return 20;
    }
    base = Lock(argv[1], SHARED_LOCK);
    if (!base) {
        PrintFault(IoErr(), argv[1]);
        return 20;
    }
    printf("-- fresh volume\n");
    bad += !one(base, "a1", n30, 22, 1);
    bad += !one(base, "a1", "second.txt", 22, 0);
    bad += !one(base, "a2", "short", 22, 1);
    printf("-- after filling the volume and deleting the filler\n");
    fill(base);
    bad += !one(base, "b1", n30, 22, 1);
    bad += !one(base, "b1", "second.txt", 22, 0);
    printf("-- again, a larger file\n");
    fill(base);
    bad += !one(base, "c1", n30, 30000, 1);
    printf("-- disk nearly full, file does not fit\n");
    bad += !one(base, "d1", "big.bin", 65536, 1);
    UnLock(base);
    printf("wrong cases: %d\n", bad);
    return bad ? 5 : 0;
}
