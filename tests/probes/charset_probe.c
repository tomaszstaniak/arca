/* SPDX-License-Identifier: MIT */
/* What the ABIv11 SDK's static libiconv says the current charset is, and which
 * conversions it can open. Built against the SDK only, without compat.c. */
#include <stdio.h>
#include <iconv.h>
#include <localcharset.h>

static void try(const char *to, const char *from)
{
    iconv_t cd = iconv_open(to, from);
    printf("iconv_open(\"%s\", \"%s\"): %s\n", to, from, cd == (iconv_t)-1 ? "fails" : "ok");
    if (cd != (iconv_t)-1)
        iconv_close(cd);
}

int main(void)
{
    const char *cs = locale_charset();
    printf("locale_charset() = \"%s\"\n", cs ? cs : "(null)");
    try("ISO-8859-1", "UTF-8");
    try("UTF-8", "CP437");
    try("UTF-8", "CP850");
    try("UTF-8", "CP1252");
    try("", "UTF-8");
    try("UTF-8", "");
    fflush(stdout);
    return 0;
}
