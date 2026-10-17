/* SPDX-License-Identifier: MIT */
/* Host test of src/core/volumes.c. The DOS calls it makes are mapped to POSIX
 * (tests/host/shim, with AROS's 32-bit LONG and Seek), so finding a set and
 * reading RAR headers can be checked on the build machine before a guest run.
 * Driven by tests/host/test_volumes.py; by hand:
 *   cc -fsanitize=address,undefined -I tests/host/shim -o /tmp/vh \
 *      tests/host/volumes_host.c -lz && /tmp/vh [-exnextfail N] [-oom N] <file>...
 * -exnextfail N: the directory read fails after N names.
 * -oom N: the Nth allocation made by volumes.c (from 1) fails. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long oom_at = -1, allocs;
static int oom_now(void) { return ++allocs == oom_at; }
static void *t_realloc(void *p, size_t n) { return oom_now() ? NULL : realloc(p, n); }
static void *t_calloc(size_t a, size_t b) { return oom_now() ? NULL : calloc(a, b); }
#define realloc t_realloc
#define calloc t_calloc
#include "../../src/core/bigfile.c"
#include "../../src/core/volumes.c"
#undef realloc
#undef calloc

char *ua_strdup(const char *s) { return s && !oom_now() ? strdup(s) : NULL; }

int main(int argc, char **argv)
{
    int i, k;
    for (i = 1; i < argc; i++) {
        ua_volumes v;
        if (!strcmp(argv[i], "-exnextfail") && i + 1 < argc) {
            shim_exnext_fail = atoi(argv[++i]);
            continue;
        }
        if (!strcmp(argv[i], "-oom") && i + 1 < argc) {
            oom_at = atol(argv[++i]);
            continue;
        }
        allocs = 0;
        if (ua_volumes_find(argv[i], &v)) {
            printf("%s: set of %d, first given %d, problem \"%s\"\n", argv[i], v.count,
                   v.first_given, v.problem);
            for (k = 0; k < v.count; k++)
                printf("   %d %s\n", k + 1, v.path[k]);
        } else {
            printf("%s: single archive\n", argv[i]);
        }
        ua_volumes_free(&v);
    }
    printf("allocations %ld\n", allocs);
    return 0;
}
