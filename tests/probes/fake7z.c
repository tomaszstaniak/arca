/* SPDX-License-Identifier: MIT */
/* A stand-in for unarc-7z of another release, for the test of the version
 * check: it answers "version" with protocol 1 and everything else with an
 * error, as an older program would. */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "version"))
        printf("V\t1\t7-Zip 25.01\nR\t0\tok\n");
    else
        printf("R\t9\tunknown command\n");
    fflush(stdout);
    return 0;
}
