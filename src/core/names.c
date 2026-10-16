/* SPDX-License-Identifier: MIT */
/* One path component from an archive name to an AROS name.
 *
 * The target charset is ISO-8859-1. That is what the AROS filesystems tested so
 * far store and what CON: shows; it is a working assumption until the encoding
 * policy is grounded on measured volumes and locale settings. Every change made
 * here is reported through the flags, never applied silently. */
#include <stdlib.h>
#include <string.h>
#include "ua.h"

/* Decode one UTF-8 sequence. Returns bytes used, 0 on an invalid sequence. */
static int utf8_dec(const unsigned char *s, ULONG *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *cp = ((ULONG)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        return *cp >= 0x80 ? 2 : 0;
    }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((ULONG)(s[0] & 0x0F) << 12) | ((ULONG)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        return *cp >= 0x800 ? 3 : 0;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
        (s[3] & 0xC0) == 0x80) {
        *cp = ((ULONG)(s[0] & 0x07) << 18) | ((ULONG)(s[1] & 0x3F) << 12) |
              ((ULONG)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        return *cp >= 0x10000 ? 4 : 0;
    }
    return 0;
}

/* Upper half of code page 437, the de-facto charset of ZIP names written
 * without the UTF-8 flag. The SDK's iconv is built without it. */
static const UWORD cp437_hi[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
    0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
    0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
    0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0
};

/* Letter + combining mark pairs whose composed form is in ISO-8859-1. macOS
 * writes names decomposed (NFD), so "Caf\u00e9" arrives as "Cafe" + U+0301. */
static const struct { char base; UWORD mark; UBYTE composed; } compose_tab[] = {
    { 'A', 0x0300, 0xC0 },
    { 'A', 0x0301, 0xC1 },
    { 'A', 0x0302, 0xC2 },
    { 'A', 0x0303, 0xC3 },
    { 'A', 0x0308, 0xC4 },
    { 'A', 0x030A, 0xC5 },
    { 'C', 0x0327, 0xC7 },
    { 'E', 0x0300, 0xC8 },
    { 'E', 0x0301, 0xC9 },
    { 'E', 0x0302, 0xCA },
    { 'E', 0x0308, 0xCB },
    { 'I', 0x0300, 0xCC },
    { 'I', 0x0301, 0xCD },
    { 'I', 0x0302, 0xCE },
    { 'I', 0x0308, 0xCF },
    { 'N', 0x0303, 0xD1 },
    { 'O', 0x0300, 0xD2 },
    { 'O', 0x0301, 0xD3 },
    { 'O', 0x0302, 0xD4 },
    { 'O', 0x0303, 0xD5 },
    { 'O', 0x0308, 0xD6 },
    { 'U', 0x0300, 0xD9 },
    { 'U', 0x0301, 0xDA },
    { 'U', 0x0302, 0xDB },
    { 'U', 0x0308, 0xDC },
    { 'Y', 0x0301, 0xDD },
    { 'a', 0x0300, 0xE0 },
    { 'a', 0x0301, 0xE1 },
    { 'a', 0x0302, 0xE2 },
    { 'a', 0x0303, 0xE3 },
    { 'a', 0x0308, 0xE4 },
    { 'a', 0x030A, 0xE5 },
    { 'c', 0x0327, 0xE7 },
    { 'e', 0x0300, 0xE8 },
    { 'e', 0x0301, 0xE9 },
    { 'e', 0x0302, 0xEA },
    { 'e', 0x0308, 0xEB },
    { 'i', 0x0300, 0xEC },
    { 'i', 0x0301, 0xED },
    { 'i', 0x0302, 0xEE },
    { 'i', 0x0308, 0xEF },
    { 'n', 0x0303, 0xF1 },
    { 'o', 0x0300, 0xF2 },
    { 'o', 0x0301, 0xF3 },
    { 'o', 0x0302, 0xF4 },
    { 'o', 0x0303, 0xF5 },
    { 'o', 0x0308, 0xF6 },
    { 'u', 0x0300, 0xF9 },
    { 'u', 0x0301, 0xFA },
    { 'u', 0x0302, 0xFB },
    { 'u', 0x0308, 0xFC },
    { 'y', 0x0301, 0xFD },
    { 'y', 0x0308, 0xFF }
};

static ULONG compose(ULONG base, ULONG mark)
{
    unsigned i;
    if (base >= 0x80 || mark < 0x300 || mark > 0x36F)
        return 0;
    for (i = 0; i < sizeof(compose_tab) / sizeof(compose_tab[0]); i++)
        if (compose_tab[i].base == (char)base && compose_tab[i].mark == mark)
            return compose_tab[i].composed;
    return 0;
}

int ua_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        ULONG cp;
        int n = utf8_dec(p, &cp);
        if (!n)
            return 0;
        p += n;
    }
    return 1;
}

char *ua_map_component(const char *src, int kind, ULONG *flags)
{
    const unsigned char *s = (const unsigned char *)src;
    size_t n = strlen(src);
    char *out = malloc(n + 1), *o = out;

    if (!out)
        return NULL;
    while (*s) {
        ULONG cp;
        int used;

        if (kind == UA_NAME_UTF8) {
            used = utf8_dec(s, &cp);
            if (!used) {
                /* Not UTF-8 after all: keep the byte as ISO-8859-1. */
                cp = *s;
                used = 1;
                *flags |= UA_MAP_GUESSED;
            } else {
                ULONG mark, c;
                int mused = s[used] ? utf8_dec(s + used, &mark) : 0;
                if (mused && (c = compose(cp, mark)) != 0) {
                    cp = c;
                    used += mused;
                }
                if (cp >= 0x80)
                    *flags |= UA_MAP_CHARSET;
            }
        } else if (kind == UA_NAME_CP437) {
            cp = *s >= 0x80 ? cp437_hi[*s - 0x80] : *s;
            used = 1;
            if (*s >= 0x80)
                *flags |= UA_MAP_CHARSET;
        } else {
            cp = *s;
            used = 1;
            if (kind == UA_NAME_GUESSED && cp >= 0x80)
                *flags |= UA_MAP_GUESSED;
        }
        s += used;

        if (cp > 0xFF) {
            *flags |= UA_MAP_LOSSY;
            *o++ = '_';
        } else if (cp < 0x20 || cp == 0x7F || cp == ':' || cp == '/') {
            /* ':' would name a volume or device; control bytes break the Shell. */
            *flags |= UA_MAP_ILLEGAL;
            *o++ = '_';
        } else {
            *o++ = (char)cp;
        }
    }
    *o = 0;
    return out;
}

/* The archive's own name, for showing on an ISO-8859-1 console: decoded the
 * same way as for the disk, '/' kept, anything not showable as '?'. */
char *ua_display_name(const char *src, int kind)
{
    const unsigned char *s = (const unsigned char *)src;
    char *out = malloc(strlen(src) + 1), *o = out;

    if (!out)
        return NULL;
    while (*s) {
        ULONG cp;
        int used = 1;
        if (kind == UA_NAME_UTF8 && (used = utf8_dec(s, &cp)) == 0) {
            cp = *s;
            used = 1;
        } else if (kind == UA_NAME_UTF8) {
            ULONG mark, c;
            int mused = s[used] ? utf8_dec(s + used, &mark) : 0;
            if (mused && (c = compose(cp, mark)) != 0) {
                cp = c;
                used += mused;
            }
        } else if (kind == UA_NAME_CP437) {
            cp = *s >= 0x80 ? cp437_hi[*s - 0x80] : *s;
        } else if (kind != UA_NAME_UTF8) {
            cp = *s;
        }
        s += used;
        *o++ = (cp > 0xFF || cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) ? '?' : (char)cp;
    }
    *o = 0;
    return out;
}
