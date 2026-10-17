/* SPDX-License-Identifier: MIT */
/* Format recognition and engine choice.
 *
 * The choice is by format AND operation: two engines read LhA, but only XAD
 * keeps Amiga comments and protection bits, so it goes first for extraction.
 * A later engine is tried only when an earlier one fails before anything was
 * written; the caller enforces that (see main.c / the job layer). */
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include "ua.h"

extern const ua_engine ua_engine_libarchive;
extern const ua_engine ua_engine_libarchive_stream;
extern const ua_engine ua_engine_xad;
extern const ua_engine ua_engine_7z;

static const ua_engine *const all_engines[] = {
    &ua_engine_libarchive, &ua_engine_libarchive_stream, &ua_engine_xad, &ua_engine_7z, NULL
};

/* Preference per format, most suitable first. Engines missing a capability
 * are filtered out by ua_engines_for(). */
static const struct pref {
    ua_format fmt;
    const ua_engine *order[3];
} prefs[] = {
    { UA_FMT_LHA,   { &ua_engine_xad, &ua_engine_libarchive } },  /* Amiga metadata */
    { UA_FMT_LZX,   { &ua_engine_xad } },
    { UA_FMT_DMS,   { &ua_engine_xad } },
    /* libarchive first where it reads the archive: it is 2-3 times faster
     * than going through unarc-7z with many small entries. It answers
     * UA_LIST_UNSUPPORTED, before anything is written, for what it cannot
     * read (encryption, some methods: methods.c), and 7-Zip takes over. */
    { UA_FMT_ZIP,   { &ua_engine_libarchive, &ua_engine_7z, &ua_engine_xad } },
    { UA_FMT_7Z,    { &ua_engine_libarchive, &ua_engine_7z } },
    /* RAR: 7-Zip reads every method, encryption and RAR 4 volume set;
     * libarchive's RAR 4 reader does not. */
    { UA_FMT_RAR,   { &ua_engine_7z, &ua_engine_libarchive } },
    { UA_FMT_TAR,   { &ua_engine_libarchive, &ua_engine_xad } },
    { UA_FMT_GZIP,  { &ua_engine_libarchive, &ua_engine_xad } },
    { UA_FMT_BZIP2, { &ua_engine_libarchive, &ua_engine_xad } },
    { UA_FMT_XZ,    { &ua_engine_libarchive } },
    { UA_FMT_ISO9660, { &ua_engine_libarchive } },
    /* Unknown signature: let each engine try its own recognition. */
    { UA_FMT_UNKNOWN, { &ua_engine_libarchive, &ua_engine_xad } },
};

/* Multi-volume sets go to 7-Zip first: it reads each volume as one. */
static const struct pref multi_prefs[] = {
    { UA_FMT_7Z,    { &ua_engine_7z, &ua_engine_libarchive } },
};

/* Recovery: readers that walk local headers instead of trusting an index. */
static const struct pref recover_prefs[] = {
    { UA_FMT_ZIP, { &ua_engine_libarchive_stream, &ua_engine_xad } },
};

static const char *const names[UA_FMT_COUNT] = {
    "unknown", "zip", "lha", "lzx", "dms", "7z", "rar", "tar", "gzip", "bzip2",
    "xz", "iso9660"
};

const char *ua_format_name(ua_format f)
{
    return (f >= 0 && f < UA_FMT_COUNT) ? names[f] : "?";
}

ua_format ua_sniff(const UBYTE *h, LONG n)
{
    if (n >= 4 && h[0] == 'P' && h[1] == 'K' &&
        ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6) || (h[2] == 7 && h[3] == 8)))
        return UA_FMT_ZIP;
    if (n >= 7 && h[2] == '-' && h[3] == 'l' && (h[4] == 'h' || h[4] == 'z') && h[6] == '-')
        return UA_FMT_LHA;
    if (n >= 3 && h[0] == 'L' && h[1] == 'Z' && h[2] == 'X')
        return UA_FMT_LZX;
    if (n >= 4 && memcmp(h, "DMS!", 4) == 0)
        return UA_FMT_DMS;
    if (n >= 6 && memcmp(h, "7z\xBC\xAF\x27\x1C", 6) == 0)
        return UA_FMT_7Z;
    if (n >= 6 && memcmp(h, "Rar!\x1A\x07", 6) == 0)
        return UA_FMT_RAR;
    if (n >= 2 && h[0] == 0x1F && h[1] == 0x8B)
        return UA_FMT_GZIP;
    if (n >= 3 && memcmp(h, "BZh", 3) == 0)
        return UA_FMT_BZIP2;
    if (n >= 6 && memcmp(h, "\xFD" "7zXZ\0", 6) == 0)
        return UA_FMT_XZ;
    if (n >= 262 && memcmp(h + 257, "ustar", 5) == 0)
        return UA_FMT_TAR;
    if (n >= 0x8006 && memcmp(h + 0x8001, "CD001", 5) == 0)
        return UA_FMT_ISO9660;
    return UA_FMT_UNKNOWN;
}

static int pick(const struct pref *t, unsigned nt, ua_format f, ULONG need,
                const ua_engine **out, int max)
{
    unsigned i;
    int k, n = 0;
    for (i = 0; i < nt; i++) {
        if (t[i].fmt != f)
            continue;
        for (k = 0; k < 3 && t[i].order[k] && n < max; k++)
            if (t[i].order[k]->caps(f) & need)
                out[n++] = t[i].order[k];
        break;
    }
    return n;
}

int ua_engines_for(ua_format f, ua_op op, int recover, int multi, const ua_engine **out, int max)
{
    ULONG need = (op == UA_OP_LIST) ? UA_CAP_LIST : UA_CAP_EXTRACT;
    int n;
    if (recover) {
        n = pick(recover_prefs, sizeof(recover_prefs) / sizeof(recover_prefs[0]), f,
                 need | UA_CAP_RECOVERY, out, max);
        if (n)
            return n;
    }
    if (multi) {
        n = pick(multi_prefs, sizeof(multi_prefs) / sizeof(multi_prefs[0]), f, need, out, max);
        if (n)
            return n;
    }
    return pick(prefs, sizeof(prefs) / sizeof(prefs[0]), f, need, out, max);
}

const ua_engine *ua_engine_by_name(const char *name)
{
    int i;
    for (i = 0; all_engines[i]; i++)
        if (strcasecmp(all_engines[i]->name, name) == 0)   /* AmigaDOS keywords ignore case */
            return all_engines[i];
    return NULL;
}

char *ua_strdup(const char *s)
{
    char *d;
    if (!s)
        return NULL;
    d = malloc(strlen(s) + 1);
    if (d)
        strcpy(d, s);
    return d;
}

ua_entry *ua_list_add(ua_list *l)
{
    if (l->n == l->cap) {
        ULONG nc = l->cap ? l->cap * 2 : 64;
        ua_entry *nv = realloc(l->v, nc * sizeof(*nv));
        if (!nv)
            return NULL;
        l->v = nv;
        l->cap = nc;
    }
    memset(&l->v[l->n], 0, sizeof(l->v[0]));
    l->v[l->n].index = l->n;
    return &l->v[l->n++];
}

void ua_list_free(ua_list *l)
{
    ULONG i;
    if (l->volumes_owned && l->volumes) {
        for (i = 0; l->volumes[i]; i++)
            free(l->volumes[i]);
        free(l->volumes);
    }
    for (i = 0; i < l->n; i++) {
        free(l->v[i].name_src);
        free(l->v[i].comment);
        free(l->v[i].target);
    }
    free(l->v);
    free(l->stamps);
    memset(l, 0, sizeof(*l));
}

ULONG ua_list_encrypted_wanted(const ua_list *l)
{
    ULONG i, n = 0;
    for (i = 0; i < l->n; i++)
        if (l->v[i].encrypted && !l->v[i].excluded && !l->v[i].skip && l->v[i].target)
            n++;
    return n;
}

ULONG ua_format_caps(ua_format f)
{
    ULONG c = 0;
    int i;
    for (i = 0; all_engines[i]; i++)
        c |= all_engines[i]->caps(f);
    return c;
}

int ua_entry_matches(const ua_entry *e, const char *name, ua_etype type, int size_valid,
                     UQUAD size)
{
    if (!e->name_src || !name || strcmp(e->name_src, name) != 0)
        return 0;
    if (e->type != type)
        return 0;
    if (e->size_valid && size_valid && e->size != size)
        return 0;
    return 1;
}
