/* SPDX-License-Identifier: MIT */
/* How dos.library and a handler treat long absolute paths: build a chain of
 * directories with relative calls only, then call Lock and CreateDir with the
 * absolute path at several depths, and report where anything appeared.
 * Usage: longpath_probe <empty-directory> */
#include <stdio.h>
#include <string.h>
#include <proto/dos.h>

#define DEPTH 80

static char path[8192];

static void scan(BPTR dir, const char *prefix, int level)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    if (!fib || !Examine(dir, fib)) {
        FreeDosObject(DOS_FIB, fib);
        return;
    }
    while (ExNext(dir, fib)) {
        /* A correct chain has exactly one "level-NN" per level plus our probes. */
        int expected = strncmp(fib->fib_FileName, "level-", 6) == 0 &&
                       (fib->fib_FileName[6] - '0') * 10 + (fib->fib_FileName[7] - '0') == level;
        if (!expected)
            printf("  at depth %d: %s%s\n", level, fib->fib_FileName,
                   fib->fib_DirEntryType > 0 ? "/" : "");
        if (fib->fib_DirEntryType > 0 && expected) {
            BPTR old = CurrentDir(dir), sub = Lock(fib->fib_FileName, SHARED_LOCK);
            CurrentDir(old);
            if (sub) {
                scan(sub, prefix, level + 1);
                UnLock(sub);
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
}

int main(int argc, char **argv)
{
    static const int depths[] = { 10, 12, 13, 25, 26, 50, 51, 79, 80 };
    BPTR base, cur;
    int i, k;
    size_t len;

    if (argc < 2) {
        printf("usage: longpath_probe <empty-directory>\n");
        return 20;
    }
    base = Lock(argv[1], SHARED_LOCK);
    if (!base) {
        PrintFault(IoErr(), argv[1]);
        return 20;
    }
    cur = DupLock(base);
    for (i = 0; i < DEPTH; i++) {
        char name[32];
        BPTR old = CurrentDir(cur), made, next;
        snprintf(name, sizeof(name), "level-%02d-abcdefghij", i);
        made = CreateDir(name);
        if (made)
            UnLock(made);
        next = Lock(name, SHARED_LOCK);
        CurrentDir(old);
        UnLock(cur);
        cur = next;
        if (!cur) {
            printf("relative chain stopped at depth %d\n", i);
            break;
        }
    }
    if (cur)
        UnLock(cur);
    printf("relative chain of %d levels built\n", DEPTH);

    for (k = 0; k < (int)(sizeof(depths) / sizeof(depths[0])); k++) {
        BPTR l, d;
        snprintf(path, sizeof(path), "%s", argv[1]);
        for (i = 0; i < depths[k]; i++) {
            len = strlen(path);
            snprintf(path + len, sizeof(path) - len, "%slevel-%02d-abcdefghij",
                     (path[len - 1] == ':' || path[len - 1] == '/') ? "" : "/", i);
        }
        l = Lock(path, SHARED_LOCK);
        printf("depth %2d, %4lu characters: Lock %s", depths[k], (unsigned long)strlen(path),
               l ? "ok" : "failed");
        if (!l) {
            char why[80];
            Fault(IoErr(), NULL, why, sizeof(why));
            printf(" (%s)", why);
        }
        if (l)
            UnLock(l);
        len = strlen(path);
        snprintf(path + len, sizeof(path) - len, "/probe%d", depths[k]);
        d = CreateDir(path);
        printf(", CreateDir %s", d ? "ok" : "failed");
        if (!d) {
            char why[80];
            Fault(IoErr(), NULL, why, sizeof(why));
            printf(" (%s)", why);
        } else {
            UnLock(d);
        }
        printf("\n");
        fflush(stdout);
    }
    printf("entries that are not part of the chain:\n");
    scan(base, "", 0);
    UnLock(base);
    fflush(stdout);
    return 0;
}
