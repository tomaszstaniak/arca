/* SPDX-License-Identifier: MIT */
/* Host test of src/core/volumes.c. The DOS calls it makes are mapped to POSIX
 * (tests/host/shim), so finding a set and reading RAR headers can be checked
 * on the build machine before a guest run.
 *   cc -I tests/host/shim -o /tmp/vh tests/host/volumes_host.c && /tmp/vh <file>... */
#include <stdio.h>
#include <string.h>
#include "../../src/core/volumes.c"

char *ua_strdup(const char *s) { return s ? strdup(s) : NULL; }

int main(int argc, char **argv)
{
    int i, k;
    for (i = 1; i < argc; i++) {
        ua_volumes v;
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
    return 0;
}
