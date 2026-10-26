/* SPDX-License-Identifier: MIT */
/* Host test of src/gui/fullpath.c: relative requester drawers, PROGDIR:,
 * names that do not exist yet, and every way of failing (the caller must
 * then not go on with the name as given).
 *   cc -I tests/host/fullpath -I tests/host/shim -o /tmp/fp tests/host/fullpath_host.c && /tmp/fp */
#include <stdio.h>
#include <string.h>
#include "../../src/gui/fullpath.c"

static int fails;

static void check(const char *in, size_t n, int want_ok, const char *want)
{
    char out[UA_PATHMAX];
    int ok = full_path(in, out, n);
    if (ok != want_ok || (ok && strcmp(out, want)) || (!ok && !fp_why[0]) || fp_shim_locks) {
        printf("FAIL %-22s -> %d \"%s\" (%s), want %d \"%s\"%s\n", in, ok, ok ? out : "", fp_why,
               want_ok, want, fp_shim_locks ? ", a lock left" : "");
        fails++;
        fp_shim_locks = 0;
    } else
        printf("ok   %-22s -> %s\n", in, ok ? out : fp_why);
}

int main(void)
{
    check("//sub/small.7z", 4096, 1, "RAM:oa/sub/small.7z");
    check("//sub", 4096, 1, "RAM:oa/sub");
    check("", 4096, 1, "RAM:oa/app/Arca");
    check("//new/deeper", 4096, 1, "RAM:oa/new/deeper");
    check("new", 4096, 1, "RAM:oa/app/Arca/new");
    check("Licences/x", 4096, 1, "RAM:oa/app/Arca/Licences/x");
    check(":rootrel", 4096, 1, "RAM:rootrel");
    check("RAM:abs", 4096, 1, "RAM:abs");
    check("Work:a/b", 4096, 1, "Work:a/b");
    check("PROGDIR:", 4096, 1, "RAM:oa/app/Arca");
    check("PROGDIR:Licences", 4096, 1, "RAM:oa/app/Arca/Licences");
    check("PROGDIR:out/new", 4096, 1, "RAM:oa/app/Arca/out/new");
    check("progdir:x", 4096, 1, "RAM:oa/app/Arca/x");
    /* An absolute name that does not fit is refused, not cut. */
    check("RAM:abs", 5, 0, "");
    /* The drawer fits, the rest does not: refused, not the drawer alone. */
    check("//sub/small.7z", 12, 0, "");
    check("//sub/x123456", 14, 0, "");
    fp_shim_nfl_fail = 1;
    check("//sub/small.7z", 4096, 0, "");
    check("PROGDIR:", 4096, 0, "");
    check("RAM:abs", 4096, 1, "RAM:abs");
    fp_shim_nfl_fail = 0;
    fp_test_fail = 1;
    check("//sub", 4096, 0, "");
    check("RAM:abs", 4096, 1, "RAM:abs");
    fp_test_fail = 0;
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
