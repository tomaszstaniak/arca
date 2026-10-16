/* SPDX-License-Identifier: MIT */
/* Core model shared by the engines, the file layer and the front ends.
 * Plain C over exec/dos; no GUI code may be needed to include this. */
#ifndef UA_H
#define UA_H

#include <exec/types.h>
#include <dos/dos.h>

/* Longest full path the program builds. Longer ones are reported as errors,
 * never cut short (a cut path is another file). */
#define UA_PATHMAX 4096

/* Formats the registry recognises by signature before any engine is asked. */
typedef enum {
    UA_FMT_UNKNOWN = 0,
    UA_FMT_ZIP, UA_FMT_LHA, UA_FMT_LZX, UA_FMT_DMS, UA_FMT_7Z, UA_FMT_RAR,
    UA_FMT_TAR, UA_FMT_GZIP, UA_FMT_BZIP2, UA_FMT_XZ, UA_FMT_ISO9660,
    UA_FMT_COUNT
} ua_format;

typedef enum { UA_OP_LIST, UA_OP_EXTRACT } ua_op;

/* Results of ua_engine.list(). */
#define UA_LIST_OK          0
#define UA_LIST_PARTIAL     1   /* recognised, damaged: what is in the list was read */
#define UA_LIST_FAILED     -1   /* recognised, nothing usable */
#define UA_LIST_UNKNOWN     2   /* this engine does not know the format */

/* Capability bits an engine declares per format. The GUI reads these; it never
 * asks an engine by name. */
#define UA_CAP_LIST        (1UL << 0)
#define UA_CAP_EXTRACT     (1UL << 1)
#define UA_CAP_CREATE      (1UL << 2)
#define UA_CAP_AMIGA_PROT  (1UL << 3)  /* Amiga protection bits survive */
#define UA_CAP_COMMENTS    (1UL << 4)  /* file comments survive */
#define UA_CAP_UTF8_NAMES  (1UL << 5)  /* names can be read as UTF-8 */
#define UA_CAP_PASSWORD    (1UL << 6)
#define UA_CAP_DISKIMAGE   (1UL << 7)
#define UA_CAP_RECOVERY    (1UL << 8)  /* reads what it can from damaged archives */

typedef enum { UA_ET_FILE, UA_ET_DIR, UA_ET_LINK, UA_ET_OTHER } ua_etype;

/* Name provenance, so a conversion is never silent. */
#define UA_NAME_UTF8      1   /* name_src is UTF-8 */
#define UA_NAME_HOST      2   /* name_src is already ISO-8859-1 (XAD output) */
#define UA_NAME_GUESSED   3   /* encoding unknown; bytes taken as ISO-8859-1 */
#define UA_NAME_CP437     4   /* ZIP name without the UTF-8 flag */

typedef struct ua_entry {
    ULONG index;            /* engine's own order; extraction follows it */
    char *name_src;         /* path as the engine gave it, '/'-separated */
    int name_kind;          /* UA_NAME_* */
    ua_etype type;
    UQUAD size;
    int size_valid;         /* size is known (some streams only learn it at the end) */
    int encrypted;
    int prot_valid;
    ULONG prot;             /* AROS FIBF_* layout: RWED active low */
    char *comment;          /* NULL if none */
    int date_valid;
    struct DateStamp date;
    /* Filled by the plan: */
    char *target;           /* relative to the extraction root, host charset */
    ULONG map_flags;        /* UA_MAP_* */
    int skip;               /* planned not to be written (unsafe, encrypted, ...) */
    int excluded;           /* not chosen by the user; not written, not counted */
} ua_entry;

#define UA_MAP_CHARSET    (1UL << 0)  /* converted from UTF-8 */
#define UA_MAP_LOSSY      (1UL << 1)  /* characters not representable, replaced */
#define UA_MAP_ILLEGAL    (1UL << 2)  /* characters not allowed in AROS names, replaced */
#define UA_MAP_ABSOLUTE   (1UL << 3)  /* leading '/' removed */
#define UA_MAP_UNSAFE     (1UL << 4)  /* '..' component: entry refused */
#define UA_MAP_COLLISION  (1UL << 5)  /* same target as an earlier entry: renamed */
#define UA_MAP_GUESSED    (1UL << 6)  /* encoding guessed */

typedef struct ua_list {
    ua_entry *v;
    ULONG n, cap;
    ua_format format;
    const struct ua_engine *engine;
    int disk_image;          /* archive holds a disk image, not files */
} ua_list;

/* What the file layer tells an engine before each entry's data. */
typedef enum { UA_W_WRITE, UA_W_SKIP, UA_W_CANCEL } ua_wdecision;

typedef struct ua_sink {
    ua_wdecision (*begin)(struct ua_sink *s, ua_entry *e);
    /* returns 0 to go on, nonzero to cancel */
    int (*data)(struct ua_sink *s, const void *buf, ULONG len);
    int (*seek)(struct ua_sink *s, UQUAD skip);   /* sparse gap, write zeros */
    void (*end)(struct ua_sink *s, ua_entry *e, int ok, const char *err);
    void *priv;
} ua_sink;

/* Engine adapter. A new engine is one of these plus a line in the registry
 * table; nothing in the front ends changes. It still means rebuilding the
 * program: engines are linked in, not loaded at run time. */
typedef struct ua_engine {
    const char *name;
    ULONG (*caps)(ua_format f);                  /* 0: format not handled */
    int (*list)(const char *path, ua_list *out, char *err, int errlen);  /* UA_LIST_* */
    /* Walk the archive again in entry order and feed planned entries to the sink.
     * Returns 0 when every entry was handled, 1 when cancelled, -1 on an error
     * that stopped the walk. */
    int (*extract)(const char *path, ua_list *l, ua_sink *s, char *err, int errlen);
} ua_engine;

/* registry.c */
ua_format ua_sniff(const UBYTE *head, LONG len);
const char *ua_format_name(ua_format f);
/* Engines in preference order for this format and operation. Recovery mode
 * (reading what can be read from a damaged archive) has its own order and is
 * only used when asked for. Returns count. */
int ua_engines_for(ua_format f, ua_op op, int recover, const ua_engine **out, int max);
const ua_engine *ua_engine_by_name(const char *name);

/* session.c: opening an archive, shared by every front end */
#define UA_OPEN_OK        0   /* complete listing */
#define UA_OPEN_PARTIAL   1   /* damaged; the list holds what was read (LIST or recovery) */
#define UA_OPEN_DAMAGED   2   /* damaged; refused, nothing to extract (normal mode) */
#define UA_OPEN_NOENGINE  3   /* no engine handles this format and operation */
#define UA_OPEN_ERROR     4   /* every engine failed, or an unknown engine was named */
typedef void (*ua_note)(void *ctx, const char *fmt, ...);
/* 0, or -1 with the DOS error when the file cannot be opened. */
int ua_sniff_file(const char *archive, ua_format *fmt, LONG *ioerr);
/* Engines in order, as the rules for normal and recovery mode say; engine
 * names a single engine (test aid) or is NULL. Engine messages go to note. */
int ua_read_list(const char *archive, ua_format fmt, ua_op op, int recover,
                 const char *engine, ua_list *l, ua_note note, void *ctx);

/* pack.c: new archives */
typedef enum { UA_PACK_ZIP, UA_PACK_TGZ, UA_PACK_7Z } ua_packfmt;
typedef struct ua_pack_opts {
    ua_packfmt fmt;
    int level;                /* 0..9, or -1 for the format's default */
    int replace;              /* an existing archive of that name may be replaced */
    /* Front end hooks; may be NULL. progress: a file starts; tick: about
     * every 64 KB of input. done = input bytes so far. */
    void (*progress)(void *ctx, const char *name, UQUAD done);
    void (*tick)(void *ctx, const char *name, UQUAD done);
    void (*message)(void *ctx, const char *fmt, ...);
    void *ui_ctx;
    UQUAD cancel_after;       /* test aid */
} ua_pack_opts;
typedef struct ua_pack_report {
    ULONG files, dirs, skipped, changed, errors;
    UQUAD bytes_in, bytes_out;
    int cancelled, failed, replaced;
    char why[160];
} ua_pack_report;
const char *ua_pack_format_name(ua_packfmt f);
/* 1 and the format when the name ends in .zip, .tar.gz/.tgz or .7z. */
int ua_pack_format_from_name(const char *archive, ua_packfmt *f);
/* 0 when the archive was made (it may still report errors for some inputs),
 * -1 when it was not; nothing is left under the archive's name then. */
int ua_pack(const char *archive, char **inputs, int ninputs, ua_pack_opts *o,
            ua_pack_report *r, char *err, int errlen);
/* Every capability of every engine for a format (for help texts). */
ULONG ua_format_caps(ua_format f);

/* list.c */
ua_entry *ua_list_add(ua_list *l);
void ua_list_free(ua_list *l);
char *ua_strdup(const char *s);

/* names.c */
char *ua_map_component(const char *src, int kind, ULONG *flags);
int ua_utf8_valid(const char *s);
char *ua_display_name(const char *src, int kind);

/* plan.c */
typedef enum { UA_ROOT_DEST, UA_ROOT_TOPDIR, UA_ROOT_WRAPPER } ua_rootkind;
typedef struct ua_plan {
    char dest[UA_PATHMAX];   /* where the user asked to extract */
    char root[UA_PATHMAX];   /* directory every target is relative to */
    ua_rootkind kind;
    char rootname[256];      /* created folder name, when kind != DEST */
    int root_renamed;        /* rootname differs from the archive's own name */
    ULONG planned, unsafe, encrypted, collisions, converted, lossy;
} ua_plan;
int ua_plan_make(ua_list *l, const char *archive, const char *dest, int nowrap,
                 ua_plan *p, char *err, int errlen);

/* fsl.c: the only code that writes to disk */
typedef enum { UA_CONFLICT_ASK, UA_CONFLICT_OVERWRITE, UA_CONFLICT_SKIP,
               UA_CONFLICT_RENAME } ua_conflict;
typedef struct ua_report {
    ULONG files, dirs, skipped, renamed, overwritten, errors;
    UQUAD bytes;
    int cancelled;           /* the user (or CANCELAFTER) stopped it */
    int failed;              /* a write error stopped it, e.g. disk full */
    char why[160];           /* reason for 'failed' */
} ua_report;
typedef struct ua_extract_opts {
    ua_conflict conflict;
    /* Front end hook for UA_CONFLICT_ASK; returns OVERWRITE, SKIP or RENAME,
     * or -1 to cancel. 'all' set to 1 makes the answer stick. e is the entry
     * that would replace the file at path. */
    int (*ask)(void *ctx, const char *path, const ua_entry *e, int *all);
    void *ask_ctx;
    /* Front end hooks for progress and messages; may be NULL. */
    void (*progress)(void *ctx, const ua_entry *e, const char *written, UQUAD done);
    void (*message)(void *ctx, const char *fmt, ...);
    /* Called while a file is written, about every 64 KB, with the bytes of
     * the whole job so far; may be NULL. Rate limiting is the front end's. */
    void (*tick)(void *ctx, const ua_entry *e, UQUAD done);
    void *ui_ctx;
    UQUAD cancel_after;      /* test aid: cancel once this many bytes are written */
} ua_extract_opts;
int ua_extract(const char *archive, ua_list *l, ua_plan *p, ua_extract_opts *o,
               ua_report *r, char *err, int errlen);

#endif
