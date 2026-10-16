/* SPDX-License-Identifier: MIT */
/* Delete a directory tree using relative names only, for trees too deep for
 * C:Delete (absolute paths over about 255 characters fail on AROS).
 * Usage: reldel <directory> */
#include <stdio.h>
#include <string.h>
#include <proto/dos.h>

static int del_tree(BPTR dir)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    int errors = 0, again = 1;
    if (!fib)
        return 1;
    /* Restart the scan after each delete: ExNext after a delete is not reliable. */
    while (again) {
        again = 0;
        if (!Examine(dir, fib))
            break;
        while (ExNext(dir, fib)) {
            char name[256];
            BPTR old;
            snprintf(name, sizeof(name), "%s", fib->fib_FileName);
            old = CurrentDir(dir);
            if (fib->fib_DirEntryType > 0) {
                BPTR sub = Lock(name, SHARED_LOCK);
                if (sub) {
                    errors += del_tree(sub);
                    UnLock(sub);
                }
            }
            SetProtection(name, 0);
            if (DeleteFile(name))
                again = 1;
            else
                errors++;
            CurrentDir(old);
            if (again)
                break;
        }
    }
    FreeDosObject(DOS_FIB, fib);
    return errors;
}

int main(int argc, char **argv)
{
    BPTR d;
    int errors;
    if (argc < 2) {
        printf("usage: reldel <directory>\n");
        return 20;
    }
    d = Lock(argv[1], SHARED_LOCK);
    if (!d) {
        PrintFault(IoErr(), argv[1]);
        return 20;
    }
    errors = del_tree(d);
    UnLock(d);
    if (!errors && !DeleteFile(argv[1]))
        errors++;
    printf("%s: %s\n", argv[1], errors ? "errors left" : "deleted");
    return errors ? 10 : 0;
}
