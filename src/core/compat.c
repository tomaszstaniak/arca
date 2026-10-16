/* SPDX-License-Identifier: MIT */
/* Symbols the static dependencies expect the program to provide. */
#include <stdio.h>
#include <stdlib.h>

/* libbz2_nostdio.a leaves its assertion handler to the program. bzip2 state is
 * undefined after it fires, so do not try to carry on. */
void bz_internal_error(int errcode)
{
    fprintf(stderr, "bzip2 internal error %d\n", errcode);
    fflush(stderr);
    abort();
}

/* libarchive asks locale_charset() for the "current charset" and converts
 * every name it hands out into it. AROS has neither nl_langinfo() nor a
 * locale_charset() that knows the Shell's setting, and the empty answer made
 * libarchive drop UTF-8 names outright. Inside this program the answer is
 * UTF-8; names.c converts to the AROS charset and records every change.
 * This also replaces libiconv's own locale_charset(), which iconv_open("")
 * would otherwise use. */
const char *locale_charset(void)
{
    return "UTF-8";
}
