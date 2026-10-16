/* SPDX-License-Identifier: MIT */
/* Extraction plan: target names, collisions and the folder rule.
 *
 * Folder rule (The Unarchiver's, made exact):
 *  - nothing extractable: nothing is created;
 *  - one top-level item that is a directory: it is created in the destination
 *    under its own name; if that name exists there, " 2", " 3", ... is added.
 *    Nothing is ever merged into an existing directory;
 *  - one top-level item that is a file: it goes straight into the destination;
 *    an existing file of that name is a conflict for the conflict policy;
 *  - several top-level items: a new folder named after the archive (archive
 *    suffixes removed) is created, with the same " 2" rule, and holds them all;
 *  - NOWRAP: always the destination itself, every existing name is a conflict.
 * The plan only chooses names. Directories are created by the file layer, which
 * picks the next free suffix if a name was taken in the meantime. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/dos.h>
#include "ua.h"

static unsigned char fold(unsigned char c)
{
    /* Case folding of the AROS filesystems in international mode. */
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
        return c + 0x20;
    return c;
}

static int ieq(const char *a, const char *b)
{
    while (*a && fold((unsigned char)*a) == fold((unsigned char)*b))
        a++, b++;
    return fold((unsigned char)*a) == fold((unsigned char)*b);
}

/* Small open-addressing set of target paths, compared without case. */
typedef struct { char **k; char *isdir; ULONG cap, n; } nameset;

static ULONG hash(const char *s)
{
    ULONG h = 5381;
    while (*s)
        h = h * 33 + fold((unsigned char)*s++);
    return h;
}

static int set_init(nameset *s, ULONG want)
{
    s->cap = 64;
    while (s->cap < want * 2)
        s->cap *= 2;
    s->n = 0;
    s->k = calloc(s->cap, sizeof(char *));
    s->isdir = calloc(s->cap, 1);
    return s->k && s->isdir;
}

static void set_free(nameset *s)
{
    ULONG i;
    for (i = 0; i < s->cap; i++)
        free(s->k[i]);
    free(s->k);
    free(s->isdir);
}

/* Returns slot of an existing key, or -1. */
static long set_find(nameset *s, const char *key)
{
    ULONG i = hash(key) & (s->cap - 1);
    while (s->k[i]) {
        if (ieq(s->k[i], key))
            return (long)i;
        i = (i + 1) & (s->cap - 1);
    }
    return -1;
}

static int set_add(nameset *s, const char *key, int isdir)
{
    ULONG i;
    if ((s->n + 1) * 2 > s->cap) {
        nameset t;
        ULONG j;
        if (!set_init(&t, s->cap))
            return 0;
        for (j = 0; j < s->cap; j++)
            if (s->k[j]) {
                ULONG h = hash(s->k[j]) & (t.cap - 1);
                while (t.k[h])
                    h = (h + 1) & (t.cap - 1);
                t.k[h] = s->k[j];
                t.isdir[h] = s->isdir[j];
                t.n++;
            }
        free(s->k);
        free(s->isdir);
        *s = t;
    }
    i = hash(key) & (s->cap - 1);
    while (s->k[i])
        i = (i + 1) & (s->cap - 1);
    s->k[i] = ua_strdup(key);
    s->isdir[i] = (char)isdir;
    s->n++;
    return s->k[i] != NULL;
}

/* Build the mapped relative path of an entry; NULL with UA_MAP_UNSAFE set if refused. */
static char *map_path(const char *src, int kind, ULONG *flags)
{
    size_t n = strlen(src);
    char *work = malloc(n + 1), *out = malloc(n * 4 + 2), *p;
    size_t olen = 0;

    if (!work || !out) {
        free(work);
        free(out);
        return NULL;
    }
    strcpy(work, src);
    /* Windows archivers sometimes store '\'. Treat it as a separator: no AROS
     * user wants a file literally named "dir\file". */
    for (p = work; *p; p++)
        if (*p == '\\')
            *p = '/';
    p = work;
    if (*p == '/') {
        *flags |= UA_MAP_ABSOLUTE;
        while (*p == '/')
            p++;
    }
    /* A ':' is never honoured as a volume or device ("Work:x", "C:\\x"):
     * names.c replaces it like any other character AROS cannot store. */
    out[0] = 0;
    while (*p) {
        char *slash = strchr(p, '/'), *m;
        if (slash)
            *slash = 0;
        if (strcmp(p, "..") == 0) {
            *flags |= UA_MAP_UNSAFE;
            free(work);
            free(out);
            return NULL;
        }
        if (*p && strcmp(p, ".") != 0) {
            m = ua_map_component(p, kind, flags);
            if (!m) {
                free(work);
                free(out);
                return NULL;
            }
            if (olen)
                out[olen++] = '/';
            strcpy(out + olen, m);
            olen += strlen(m);
            free(m);
        }
        if (!slash)
            break;
        p = slash + 1;
    }
    free(work);
    return out;
}

/* "name.ext" -> "name 2.ext" */
static char *numbered(const char *path, int k)
{
    const char *base = strrchr(path, '/');
    const char *dot;
    char *r = malloc(strlen(path) + 16);
    if (!r)
        return NULL;
    base = base ? base + 1 : path;
    dot = strrchr(base, '.');
    if (!dot || dot == base)
        dot = base + strlen(base);
    sprintf(r, "%.*s %d%s", (int)(dot - path), path, k, dot);
    return r;
}

static int exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    if (l) {
        UnLock(l);
        return 1;
    }
    return 0;
}

/* Returns 0 if the result would not fit. */
static int join(char *out, size_t sz, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    int w;
    if (n == 0)
        w = snprintf(out, sz, "%s", name);
    else if (dir[n - 1] == ':' || dir[n - 1] == '/')
        w = snprintf(out, sz, "%s%s", dir, name);
    else
        w = snprintf(out, sz, "%s/%s", dir, name);
    return w >= 0 && (size_t)w < sz;
}

/* A free name in dir: base, "base 2", "base 3", ... */
static int free_name(const char *dir, const char *base, char *out, size_t sz)
{
    char path[UA_PATHMAX];
    int k;
    for (k = 1; k < 1000; k++) {
        if (k == 1)
            snprintf(out, sz, "%s", base);
        else
            snprintf(out, sz, "%s %d", base, k);
        if (!join(path, sizeof(path), dir, out))
            return 0;
        if (!exists(path))
            return k;
    }
    return 0;
}

static void wrapper_name(const char *archive, char *out, size_t sz)
{
    static const char *const sfx[] = {
        ".zip", ".lha", ".lzh", ".lzx", ".7z", ".rar", ".dms", ".tgz", ".tbz", ".tbz2",
        ".txz", ".gz", ".bz2", ".xz", ".tar", ".iso", NULL
    };
    const char *b = FilePart((STRPTR)archive);
    int again = 1, i;
    snprintf(out, sz, "%s", b);
    while (again) {
        size_t n = strlen(out);
        again = 0;
        for (i = 0; sfx[i]; i++) {
            size_t m = strlen(sfx[i]);
            if (n > m && ieq(out + n - m, sfx[i])) {
                out[n - m] = 0;
                again = 1;
                break;
            }
        }
    }
    if (!out[0])
        snprintf(out, sz, "Archive");
}

int ua_plan_make(ua_list *l, const char *archive, const char *dest, int nowrap,
                 ua_plan *p, char *err, int errlen)
{
    nameset set;
    ULONG i;
    char top[256] = "", *slash;
    int ntop = 0, top_is_dir = 0;

    memset(p, 0, sizeof(*p));
    snprintf(p->dest, sizeof(p->dest), "%s", dest);
    if (!set_init(&set, l->n + 16)) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    for (i = 0; i < l->n; i++) {
        ua_entry *e = &l->v[i];
        char *t;

        free(e->target);
        e->target = NULL;
        e->map_flags = 0;
        e->skip = 0;
        if (e->type != UA_ET_FILE && e->type != UA_ET_DIR) {
            e->skip = 1;     /* links and devices: not in this slice */
            continue;
        }
        if (e->encrypted) {
            e->skip = 1;
            p->encrypted++;
            continue;
        }
        t = map_path(e->name_src, e->name_kind, &e->map_flags);
        if (!t) {
            e->skip = 1;
            p->unsafe++;
            continue;
        }
        if (!*t) {           /* "." or "/" only */
            free(t);
            e->skip = 1;
            continue;
        }
        if (e->map_flags & (UA_MAP_CHARSET | UA_MAP_ILLEGAL | UA_MAP_ABSOLUTE))
            p->converted++;
        if (e->map_flags & UA_MAP_LOSSY)
            p->lossy++;

        {
            long slot = set_find(&set, t);
            if (slot >= 0 && !(e->type == UA_ET_DIR && set.isdir[slot])) {
                int k;
                char *alt = NULL;
                for (k = 2; k < 1000; k++) {
                    free(alt);
                    alt = numbered(t, k);
                    if (!alt || set_find(&set, alt) < 0)
                        break;
                }
                free(t);
                t = alt;
                e->map_flags |= UA_MAP_COLLISION;
                p->collisions++;
                if (!t) {
                    e->skip = 1;
                    continue;
                }
            }
            if (slot < 0 || e->map_flags & UA_MAP_COLLISION)
                set_add(&set, t, e->type == UA_ET_DIR);
        }
        /* Parents are implied directories. */
        {
            char *q = t;
            while ((q = strchr(q, '/'))) {
                *q = 0;
                if (set_find(&set, t) < 0)
                    set_add(&set, t, 1);
                *q = '/';
                q++;
            }
        }
        e->target = t;
        p->planned++;

        slash = strchr(t, '/');
        {
            size_t tl = slash ? (size_t)(slash - t) : strlen(t);
            char first[256];
            snprintf(first, sizeof(first), "%.*s", (int)tl, t);
            if (ntop == 0) {
                snprintf(top, sizeof(top), "%s", first);
                ntop = 1;
                top_is_dir = slash != NULL || e->type == UA_ET_DIR;
            } else if (ntop == 1) {
                if (!ieq(first, top))
                    ntop = 2;
                else if (slash || e->type == UA_ET_DIR)
                    top_is_dir = 1;
            }
        }
    }
    set_free(&set);

    if (nowrap || ntop == 0 || (ntop == 1 && !top_is_dir)) {
        p->kind = UA_ROOT_DEST;
        snprintf(p->root, sizeof(p->root), "%s", dest);
        return 0;
    }
    if (ntop == 1) {
        int k = free_name(dest, top, p->rootname, sizeof(p->rootname));
        size_t tl = strlen(top);
        if (!k) {
            snprintf(err, errlen, "no free folder name for \"%s\"", top);
            return -1;
        }
        p->kind = UA_ROOT_TOPDIR;
        p->root_renamed = k > 1;
        if (!join(p->root, sizeof(p->root), dest, p->rootname)) {
            snprintf(err, errlen, "destination path too long");
            return -1;
        }
        /* Targets become relative to the top directory itself. */
        for (i = 0; i < l->n; i++) {
            ua_entry *e = &l->v[i];
            if (!e->target)
                continue;
            if (e->target[tl] == '/')
                memmove(e->target, e->target + tl + 1, strlen(e->target + tl + 1) + 1);
            else
                e->target[0] = 0;   /* the top directory entry: it is the root */
        }
        return 0;
    }
    {
        char base[256];
        int k;
        wrapper_name(archive, base, sizeof(base));
        k = free_name(dest, base, p->rootname, sizeof(p->rootname));
        if (!k) {
            snprintf(err, errlen, "no free folder name for \"%s\"", base);
            return -1;
        }
        p->kind = UA_ROOT_WRAPPER;
        p->root_renamed = k > 1;
        if (!join(p->root, sizeof(p->root), dest, p->rootname)) {
            snprintf(err, errlen, "destination path too long");
            return -1;
        }
    }
    return 0;
}
