/* SPDX-License-Identifier: MIT */
/* 7-Zip engine: 7z, RAR (1.5 to 5) and ZIP through 7-Zip's own handlers,
 * which read every method and encryption those formats have. It reads RAR
 * first; 7z and ZIP only when libarchive cannot (encryption, a method it
 * lacks, a 7z set: registry.c). The handlers run in a separate program,
 * unarc-7z, next to this one (PROGDIR:): 7-Zip is C++ under the LGPL, and
 * its RAR code carries the unRAR licence restriction, so it stays a program
 * of its own that can be replaced or left out. When it is not there, or
 * speaks another protocol version, this engine declares no formats and
 * libarchive reads what it can as before.
 *
 * The program is started with LoadSeg and CreateNewProc, not through a
 * Shell: the archive path goes as hex, so no name needs quoting, and a Shell
 * would keep a lock on our drawer (SystemTags passes no NP_HomeDir). Its
 * output, the line protocol of experiments/7z-backend/PROTOCOL.md, comes
 * through a PIPE: handle. Reads are sent as packets, so that Ctrl-C reaches
 * the program even while it decodes for minutes without writing. The
 * password goes through a second pipe that the program reads at once; AROS
 * puts a process's arguments into its input stream, so stdin is not used.
 * Files reach the disk through the file layer like every other engine's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#ifndef UA_7Z_HOST_TEST
#include <dos/dostags.h>
#include <dos/dosextens.h>
#endif
#include "../core/ua.h"

#define PROG "unarc-7z"
#define PROTOCOL 3           /* experiments/7z-backend/PROTOCOL.md */
#define BUFSZ 65536

typedef struct child child;
static int fill(child *c);
static char *read_line(child *c);

#ifdef UA_7Z_HOST_TEST
/* tests/host/be7z_host.c: the program's output from a file instead. */
#include UA_7Z_HOST_TEST
#else
/* ---------------------------------------------------------------- the program */

struct child {
    struct Process *proc;
    struct Task *parent;
    BYTE sig;                 /* signalled by the exit code below */
    volatile int exited;
    BPTR rd;                  /* our end of the output pipe */
    struct MsgPort *port;
    struct DosPacket *dp;     /* AllocDosObject(DOS_STDPKT) gives the packet itself */
    UBYTE *buf;
    LONG len, pos;
    int eof, cancelled;
    char idxfile[80];
    char volfile[80];         /* the set's volumes in order (-v), deleted at the end */
    char pwpipe[64];          /* drained in child_end when a password went in */
};

static int have_prog = -1;    /* -1: not looked for yet */
static int prog_ok = -1;      /* -1: not asked; 0: another protocol; 1: ours */
static char prog[UA_PATHMAX], prog_why[200];

static int child_start(child *c, const char *verb, const char *archive, const char *which,
                       char **vols, const char *password, char *err, int errlen);
static int child_end(child *c);
static void find_prog_now(void);

/* Called by the main program at start: the GUI's job worker has no home
 * drawer, so PROGDIR: is found here once. */
void ua_7z_init(void)
{
    have_prog = -1;
    prog_ok = -1;
    find_prog_now();
}

static int find_prog(void)
{
    return have_prog > 0;
}

/* Once per run: "version" answers with the protocol number. A program of
 * another release (copied over, or left from an older install) would read
 * the records differently, so it is not used at all. */
static void check_version(void)
{
    child c;
    char err[160], *line;
    int proto = -1, rc = -1;
    prog_ok = 0;
    if (child_start(&c, "version", NULL, NULL, NULL, NULL, err, sizeof(err)) < 0) {
        snprintf(prog_why, sizeof(prog_why), "%s next to Unarc could not be started: %s.", PROG,
                 err);
        return;
    }
    while ((line = read_line(&c))) {
        if (line[0] == 'V' && line[1] == '\t')
            proto = atoi(line + 2);
        else if (line[0] == 'R' && line[1] == '\t')
            rc = atoi(line + 2);
    }
    if (child_end(&c)) {
        /* Ctrl-C came while we waited: it is the caller's, and the answer
         * says nothing about the program; ask again next time. */
        SetSignal(SIGBREAKF_CTRL_C, SIGBREAKF_CTRL_C);
        prog_ok = -1;
        return;
    }
    if (proto == PROTOCOL && rc == 0)
        prog_ok = 1;
    else if (proto > 0)
        snprintf(prog_why, sizeof(prog_why), "%s next to Unarc is from another release (protocol "
                 "%d, this Unarc needs %d), so it is not used; install both from one package.",
                 PROG, proto, PROTOCOL);
    else
        snprintf(prog_why, sizeof(prog_why), "%s next to Unarc did not answer as expected, so it "
                 "is not used; install it from the Unarc package.", PROG);
}
#endif /* UA_7Z_HOST_TEST */

int ua_7z_available(char *why, int whylen)
{
    find_prog_now();
    if (!find_prog()) {
        if (why)
            snprintf(why, whylen, "%s is not next to Unarc; with it, Unarc also reads encrypted "
                     "7z and ZIP archives and every RAR.", PROG);
        return 0;
    }
    if (prog_ok < 0)
        check_version();
    if (prog_ok < 0) {            /* interrupted: not now */
        if (why)
            snprintf(why, whylen, "%s was not asked for its version (stopped).", PROG);
        return 0;
    }
    if (!prog_ok) {
        if (why)
            snprintf(why, whylen, "%s", prog_why);
        return 0;
    }
    return 1;
}
#ifndef UA_7Z_HOST_TEST

static void find_prog_now(void)
{
    if (have_prog < 0) {
        BPTR d = GetProgramDir(), l;
        have_prog = 0;
        if (d && NameFromLock(d, prog, sizeof(prog)) && AddPart(prog, PROG, sizeof(prog)) &&
            (l = Lock(prog, SHARED_LOCK))) {
            UnLock(l);
            have_prog = 1;
        }
    }
}


/* Runs in the program's process when it ends, before DOS frees it. Only
 * this function's own return is left to run in our code after the signal;
 * child_end waits a tick before the caller may go on. */
static void child_exit(IPTR rc, IPTR data)
{
    child *c = (child *)data;
    (void)rc;
    c->exited = 1;
    Signal(c->parent, 1UL << c->sig);
}

static void hex_of(const char *s, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (; *s; s++) {
        *out++ = h[(UBYTE)*s >> 4];
        *out++ = h[(UBYTE)*s & 15];
    }
    *out = 0;
}

/* Ctrl-C to the program, if it still runs. child_exit sets exited before
 * DOS frees the process, so under Forbid the process is valid when unset. */
static void poke(child *c)
{
    Forbid();
    if (c->proc && !c->exited)
        Signal(&c->proc->pr_Task, SIGBREAKF_CTRL_C);
    Permit();
}

/* Everything child_start allocates, except the pipes and the process. */
static void child_free(child *c)
{
    if (c->idxfile[0])
        DeleteFile((STRPTR)c->idxfile);
    if (c->volfile[0])
        DeleteFile((STRPTR)c->volfile);
    if (c->sig >= 0)
        FreeSignal(c->sig);
    if (c->dp)
        FreeDosObject(DOS_STDPKT, c->dp);
    DeleteMsgPort(c->port);
    free(c->buf);
    memset(c, 0, sizeof(*c));
    c->sig = -1;
}

/* A password the program did not read stays in its pipe (PIPE: keeps a
 * buffer that holds data until a reader takes it). Read it out and forget
 * it. If the program did read it, this opens an empty pipe and ends at once. */
static void drain_password(const char *pipe)
{
    char junk[64];
    BPTR r = Open((STRPTR)pipe, MODE_OLDFILE);
    if (r) {
        while (Read(r, junk, sizeof(junk)) > 0)
            ;
        memset(junk, 0, sizeof(junk));
        Close(r);
    }
}

/* The volumes of a set, in volumes.c's order, for -v: one hex path a line. */
static int write_vols(char **vols, char *file, size_t n)
{
    BPTR fh;
    int i, ok = 1;
    snprintf(file, n, "T:unarc7z-%08lx.vol", (unsigned long)(IPTR)FindTask(NULL));
    if (!(fh = Open((STRPTR)file, MODE_NEWFILE)))
        return 0;
    for (i = 0; vols[i] && ok; i++) {
        char *h = malloc(2 * strlen(vols[i]) + 2);
        if (!h) {
            ok = 0;
            break;
        }
        hex_of(vols[i], h);
        strcat(h, "\n");
        ok = FPuts(fh, h) == 0;
        free(h);
    }
    Close(fh);
    if (!ok)
        DeleteFile((STRPTR)file);
    return ok;
}

/* verb: "listx" or "extractx"; which: NULL, "all" or "@file"; vols: the
 * set's volumes in order, or NULL. */
static int child_start(child *c, const char *verb, const char *archive, const char *which,
                       char **vols, const char *password, char *err, int errlen)
{
    static ULONG serial;
    char out[64], pw[64], *args;
    size_t alen;
    BPTR seg, wr, in, er, pwr = BNULL;

    memset(c, 0, sizeof(*c));
    c->sig = -1;
    if (!find_prog()) {
        snprintf(err, errlen, "%s is not next to the program", PROG);
        return -1;
    }
    serial++;
    snprintf(out, sizeof(out), "PIPE:unarc7z-%08lx-%lu", (unsigned long)(IPTR)FindTask(NULL),
             (unsigned long)serial);
    snprintf(pw, sizeof(pw), "PIPE:unarc7zp-%08lx-%lu", (unsigned long)(IPTR)FindTask(NULL),
             (unsigned long)serial);
    if (vols && !write_vols(vols, c->volfile, sizeof(c->volfile))) {
        snprintf(err, errlen, "cannot write the volume list to T:");
        c->volfile[0] = 0;
        return -1;
    }
    alen = 2 * (archive ? strlen(archive) : 0) + strlen(pw) + strlen(c->volfile) +
           (which ? strlen(which) : 0) + 40;
    args = malloc(alen);
    c->buf = malloc(BUFSZ);
    c->port = CreateMsgPort();
    c->dp = AllocDosObject(DOS_STDPKT, NULL);
    c->sig = AllocSignal(-1);
    c->parent = FindTask(NULL);
    if (!args || !c->buf || !c->port || !c->dp || c->sig < 0) {
        snprintf(err, errlen, "out of memory");
        goto fail;
    }
    {
        char *p = args;
        if (password)
            p += sprintf(p, "-p %s ", pw);
        if (c->volfile[0])
            p += sprintf(p, "-v %s ", c->volfile);
        p += sprintf(p, "%s", verb);
        if (archive) {
            *p++ = ' ';
            hex_of(archive, p);
            p += strlen(p);
        }
        if (which)
            p += sprintf(p, " %s", which);
        strcpy(p, "\n");
    }
    if (!(seg = LoadSeg((STRPTR)prog))) {
        snprintf(err, errlen, "cannot load %s", PROG);
        goto fail;
    }
    wr = Open((STRPTR)out, MODE_NEWFILE);
    c->rd = wr ? Open((STRPTR)out, MODE_OLDFILE) : BNULL;
    in = Open("NIL:", MODE_OLDFILE);
    er = Open("NIL:", MODE_NEWFILE);
    /* The password pipe's writer is opened before the program starts: a read
     * from a pipe that has no writer yet ends at once with nothing. */
    if (password)
        pwr = Open((STRPTR)pw, MODE_NEWFILE);
    if (!wr || !c->rd || !in || !er || (password && !pwr)) {
        snprintf(err, errlen, "cannot open a pipe (is PIPE: mounted?)");
        UnLoadSeg(seg);
        if (wr) Close(wr);
        if (in) Close(in);
        if (er) Close(er);
        if (pwr) Close(pwr);
        goto fail;
    }
    SetSignal(0, 1UL << c->sig);
    c->proc = CreateNewProcTags(NP_Seglist, (IPTR)seg, NP_FreeSeglist, TRUE,
                                NP_Name, (IPTR)PROG, NP_CommandName, (IPTR)PROG,
                                NP_Cli, TRUE, NP_Arguments, (IPTR)args,
                                NP_Input, (IPTR)in, NP_Output, (IPTR)wr, NP_Error, (IPTR)er,
                                NP_CloseInput, TRUE, NP_CloseOutput, TRUE, NP_CloseError, TRUE,
                                NP_StackSize, 262144,
                                /* Our drawer must not stay locked by a helper. */
                                NP_HomeDir, (IPTR)BNULL,
                                NP_ExitCode, (IPTR)child_exit, NP_ExitData, (IPTR)c,
                                TAG_END);
    if (!c->proc) {
        snprintf(err, errlen, "cannot start %s", PROG);
        UnLoadSeg(seg);
        Close(wr);
        Close(in);
        Close(er);
        if (pwr) {
            Close(pwr);
            drain_password(pw);
        }
        goto fail;
    }
    free(args);
    args = NULL;
    if (pwr) {
        /* One short line: PIPE: buffers it and answers at once, reader or
         * not, so this never waits. */
        size_t n = strlen(password);
        char *line = AllocVec(n + 2, MEMF_ANY);
        if (line) {
            memcpy(line, password, n);
            line[n] = '\n';
            Write(pwr, line, n + 1);
            memset(line, 0, n + 1);
            FreeVec(line);
        }
        Close(pwr);
        snprintf(c->pwpipe, sizeof(c->pwpipe), "%s", pw);
    }
    return 0;
fail:
    free(args);
    if (c->rd)
        Close(c->rd);
    child_free(c);
    return -1;
}

/* One read of the pipe. Ctrl-C while waiting is passed on to the program,
 * which then ends its output with R 8. */
static LONG child_read(child *c, void *buf, LONG len)
{
    struct FileHandle *fh = BADDR(c->rd);
    c->dp->dp_Type = ACTION_READ;
    c->dp->dp_Arg1 = fh->fh_Arg1;
    c->dp->dp_Arg2 = (SIPTR)buf;
    c->dp->dp_Arg3 = len;
    SendPkt(c->dp, fh->fh_Type, c->port);
    for (;;) {
        ULONG got = Wait(1UL << c->port->mp_SigBit | SIGBREAKF_CTRL_C);
        if ((got & SIGBREAKF_CTRL_C) && !c->cancelled) {
            c->cancelled = 1;
            poke(c);
        }
        if (GetMsg(c->port))
            return (LONG)c->dp->dp_Res1;
    }
}

/* Reads to the end, waits for the program to end and frees everything.
 * Returns whether the run was cancelled (by us or by Ctrl-C passed on),
 * taken before the structure is cleared. */
static int child_end(child *c)
{
    int cancelled;
    if (c->rd) {
        /* Read to the end, whatever is left. Closing the reader early would
         * leave the program waiting for ever on a full pipe: PIPE: holds a
         * writer until someone reads. */
        c->pos = c->len = 0;
        while (fill(c))
            c->pos = c->len = 0;
        Close(c->rd);
    }
    if (c->proc) {
        while (!c->exited)
            Wait(1UL << c->sig);
        Delay(1);      /* let child_exit return out of our code */
    }
    if (c->pwpipe[0])
        drain_password(c->pwpipe);
    cancelled = c->cancelled;
    child_free(c);
    return cancelled;
}

#endif /* UA_7Z_HOST_TEST */

/* ---------------------------------------------------------------- output */

static int fill(child *c)
{
    LONG n;
    if (c->eof)
        return 0;
    if (c->pos < c->len) {
        memmove(c->buf, c->buf + c->pos, c->len - c->pos);
        c->len -= c->pos;
    } else {
        c->len = 0;
    }
    c->pos = 0;
    if (c->len >= BUFSZ)
        return 1;            /* full: the caller has to take some out first */
    n = child_read(c, c->buf + c->len, BUFSZ - c->len);
    if (n <= 0) {
        c->eof = 1;
        return 0;
    }
    c->len += n;
    return 1;
}

/* A protocol line without its LF, in place; NULL at the end of output. */
static char *read_line(child *c)
{
    for (;;) {
        UBYTE *nl = c->pos < c->len ? memchr(c->buf + c->pos, '\n', c->len - c->pos) : NULL;
        if (nl) {
            char *line = (char *)c->buf + c->pos;
            *nl = 0;
            c->pos = nl - c->buf + 1;
            return line;
        }
        if (c->len - c->pos >= BUFSZ - 1 || !fill(c))
            return NULL;     /* no line that long in the protocol */
    }
}

/* n raw bytes to sink (when given), or skipped. Returns 0, 1 if the sink
 * cancelled, -1 if the output ended early. */
static int read_data(child *c, UQUAD n, ua_sink *s)
{
    while (n) {
        ULONG take;
        if (c->pos >= c->len && !fill(c))
            return -1;
        take = c->len - c->pos;
        if (take > n)
            take = (ULONG)n;
        if (s && s->data(s, c->buf + c->pos, take))
            return 1;
        c->pos += take;
        n -= take;
    }
    return 0;
}

/* ---------------------------------------------------------------- records */

static int split(char *line, char **f, int max)
{
    int n = 0;
    f[n++] = line;
    while (n < max && (line = strchr(line, '\t'))) {
        *line++ = 0;
        f[n++] = line;
    }
    return n;
}

static char *unhex(const char *h)
{
    size_t n = strlen(h) / 2, i;
    char *s = malloc(n + 1);
    if (!s)
        return NULL;
    for (i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(h + 2 * i, "%2x", &v) != 1) {
            free(s);
            return NULL;
        }
        s[i] = (char)v;
    }
    s[n] = 0;
    return s;
}

/* The handlers' attribute word: Windows bits below, a Unix mode above when
 * bit 15 is set. */
static int attr_to_prot(ULONG a, ULONG *prot)
{
    if (a & 0x8000) {
        ULONG m = a >> 16, p = 0;   /* RWED bits are active low */
        if (!(m & 0400)) p |= FIBF_READ;
        if (!(m & 0200)) p |= FIBF_WRITE | FIBF_DELETE;
        if (!(m & 0100)) p |= FIBF_EXECUTE;
        if (m & 0040) p |= FIBF_GRP_READ;
        if (m & 0020) p |= FIBF_GRP_WRITE | FIBF_GRP_DELETE;
        if (m & 0010) p |= FIBF_GRP_EXECUTE;
        if (m & 0004) p |= FIBF_OTR_READ;
        if (m & 0002) p |= FIBF_OTR_WRITE | FIBF_OTR_DELETE;
        if (m & 0001) p |= FIBF_OTR_EXECUTE;
        *prot = p;
        return 1;
    }
    if (a & 1) {                    /* read-only */
        *prot = FIBF_WRITE | FIBF_DELETE | FIBF_EXECUTE;
        return 1;
    }
    return 0;
}

static const char *code_text(int code, const char *msg)
{
    switch (code) {
    case 1: return "a password is needed";
    case 2: return "wrong password";
    default: return msg && *msg ? msg : "error";
    }
}

/* ---------------------------------------------------------------- engine */

static ULONG z_caps(ua_format f)
{
    if ((f == UA_FMT_7Z || f == UA_FMT_RAR || f == UA_FMT_ZIP) && ua_7z_available(NULL, 0))
        return UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_PASSWORD | UA_CAP_UTF8_NAMES;
    return 0;
}

static int z_list(const char *path, ua_list *out, char *err, int errlen)
{
    child c;
    char *line, *f[10];
    int rc = -1, got_r = 0, oom = 0, cancelled, zip = 0;
    const char *first = out->volumes ? out->volumes[0] : path;

    if (child_start(&c, "listx", first, NULL, out->volumes, out->password, err, errlen) < 0)
        return UA_LIST_UNKNOWN;
    while ((line = read_line(&c))) {
        int n = split(line, f, 10);
        if (f[0][0] == 'A' && n >= 2) {
            zip = !strcmp(f[1], "zip");
        } else if (f[0][0] == 'E' && n == 10) {
            ua_entry *e = ua_list_add(out);
            ULONG prot;
            if (!e) {
                oom = 1;
                poke(&c);
                break;
            }
            e->index = strtoul(f[1], NULL, 10);
            e->type = f[2][0] == '1' ? UA_ET_DIR : UA_ET_FILE;
            /* A Unix mode above bit 15 can say "link"; such entries are not
             * written, as with the other engines. */
            if (f[6][0] != '-') {
                ULONG a = strtoul(f[6], NULL, 16);
                if ((a & 0x8000) && ((a >> 16) & 0170000) == 0120000)
                    e->type = UA_ET_LINK;
            }
            e->size_valid = f[3][0] != '-';
            e->size = e->size_valid ? strtoull(f[3], NULL, 10) : 0;
            if (f[5][0] != '-') {
                /* "l": stored local time (RAR 1.5-4, a ZIP's DOS time), given
                 * as stored; "u": UTC, shifted to the locale's time. */
                ua_unix_to_datestamp(strtoll(f[5], NULL, 10), f[9][0] != 'l', &e->date);
                e->date_valid = 1;
            }
            if (f[6][0] != '-' && e->type == UA_ET_FILE &&
                attr_to_prot(strtoul(f[6], NULL, 16), &prot)) {
                e->prot = prot;
                e->prot_valid = 1;
            }
            e->encrypted = f[7][0] == '1';
            e->name_src = unhex(f[8]);
            if (!e->name_src) {
                oom = 1;
                poke(&c);
                break;
            }
            /* The program gives a name's bytes as they are when they are not
             * UTF-8; as with libarchive, those are CP437 in a ZIP. */
            e->name_kind = ua_utf8_valid(e->name_src) ? UA_NAME_UTF8 :
                           zip ? UA_NAME_CP437 : UA_NAME_GUESSED;
            if (ua_list_break(out) && !c.cancelled) {
                c.cancelled = 1;
                poke(&c);
            }
        } else if (f[0][0] == 'R' && n >= 2) {
            rc = atoi(f[1]);
            got_r = 1;
            snprintf(err, errlen, "%s", n >= 3 ? f[2] : "");
        }
    }
    cancelled = child_end(&c);
    if (oom) {
        snprintf(err, errlen, "out of memory");
        return UA_LIST_FAILED;
    }
    if (cancelled || rc == 8)
        return UA_LIST_CANCELLED;
    if (!got_r) {
        snprintf(err, errlen, "%s stopped without an answer", PROG);
        return UA_LIST_FAILED;
    }
    switch (rc) {
    case 0:
        return UA_LIST_OK;
    case 1: case 2:
        out->pw_state = rc == 1 ? UA_PW_NEEDED : UA_PW_WRONG;
        snprintf(err, errlen, "%s", rc == 1 ? "the entry list is encrypted: a password is needed"
                                            : "wrong password for the entry list");
        return UA_LIST_PASSWORD;
    case 5:
        return UA_LIST_UNKNOWN;     /* not a format of this build: try another engine */
    default:
        return out->n ? UA_LIST_PARTIAL : UA_LIST_FAILED;
    }
}

static ua_entry *by_index(ua_list *l, ULONG idx, ULONG *pos)
{
    ULONG k;
    if (idx < l->n && l->v[idx].index == idx) {
        *pos = idx;
        return &l->v[idx];
    }
    for (k = 0; k < l->n; k++)
        if (l->v[k].index == idx) {
            *pos = k;
            return &l->v[k];
        }
    return NULL;
}

static int wanted(const ua_entry *e)
{
    return !e->excluded && !e->skip && e->target;
}

/* Returns 0 when every entry was handled, 1 when cancelled, -1 on an error
 * that stopped the walk or that the program reported for the job as a whole.
 * The program's final R code means:
 *   0  done; every wanted entry must have had its answer;
 *   8  cancelled;
 *   another, after an F record with an error: that entry's error, already
 *      reported for it (and counted by the file layer); the job is done;
 *   another, with every F record 0: an error of the whole job, so -1. */
static int z_extract(const char *path, ua_list *l, ua_sink *s, char *err, int errlen)
{
    child c;
    char *line, *f[6], which[80];
    ULONG k, next = 0, cur = (ULONG)-1, nwant = 0, f_errors = 0;
    int result = 0, begun = 0, rc = -1, stop = 0, cancelled;
    ua_wdecision d = UA_W_SKIP;
    const char *first = l->volumes ? l->volumes[0] : path;
    BPTR fh;

    for (k = 0; k < l->n; k++)
        if (wanted(&l->v[k]))
            nwant++;
    if (nwant == 0) {
        for (k = 0; k < l->n; k++)
            if (s->begin(s, &l->v[k]) == UA_W_CANCEL)
                return 1;
        return 0;
    }
    /* Indices go through a file: a large selection would not fit a command
     * line. Not secret; deleted in child_end. */
    snprintf(which, sizeof(which), "T:unarc7z-%08lx.idx", (unsigned long)(IPTR)FindTask(NULL));
    if (nwant == l->n) {
        strcpy(which, "all");
    } else if ((fh = Open((STRPTR)which, MODE_NEWFILE))) {
        char num[16];
        for (k = 0; k < l->n; k++)
            if (wanted(&l->v[k])) {
                snprintf(num, sizeof(num), "%lu\n", (unsigned long)l->v[k].index);
                FPuts(fh, num);
            }
        Close(fh);
        memmove(which + 1, which, strlen(which) + 1);
        which[0] = '@';
    } else {
        snprintf(err, errlen, "cannot write %s", which);
        return -1;
    }
    if (child_start(&c, "extractx", first, which, l->volumes, l->password, err, errlen) < 0) {
        if (which[0] == '@')
            DeleteFile((STRPTR)which + 1);
        return -1;
    }
    if (which[0] == '@')
        snprintf(c.idxfile, sizeof(c.idxfile), "%s", which + 1);

    while (!stop && (line = read_line(&c))) {
        int n = split(line, f, 6);
        ULONG pos = 0;
        ua_entry *e;
        if (f[0][0] == 'H' && n == 5) {
            char *name = unhex(f[4]);
            e = by_index(l, strtoul(f[1], NULL, 10), &pos);
            if (!e || !name || !wanted(e) || pos < next ||
                !ua_entry_matches(e, name, f[2][0] == '1' ? UA_ET_DIR : UA_ET_FILE,
                                  f[3][0] != '-', f[3][0] != '-' ? strtoull(f[3], NULL, 10) : 0)) {
                snprintf(err, errlen, "%s (entry %lu is not \"%s\")", UA_CHANGED_MSG,
                         (unsigned long)strtoul(f[1], NULL, 10),
                         e && e->name_src ? e->name_src : "?");
                free(name);
                result = -1;
                stop = 1;
                break;
            }
            free(name);
            /* Entries before it that were not asked for: the file layer still
             * sees each one (it counts skipped ones). */
            for (; next < pos; next++)
                if (s->begin(s, &l->v[next]) == UA_W_CANCEL) {
                    result = 1;
                    stop = 1;
                    break;
                }
            cur = pos;
            begun = 0;
        } else if (f[0][0] == 'D') {
            UQUAD size;
            int r;
            if (n != 3 || cur == (ULONG)-1 || l->v[cur].index != strtoul(f[1], NULL, 10)) {
                /* Its data would be read as lines: stop instead. */
                snprintf(err, errlen, "%s sent data out of order", PROG);
                result = -1;
                stop = 1;
                break;
            }
            size = strtoull(f[2], NULL, 10);
            e = &l->v[cur];
            d = s->begin(s, e);
            begun = 1;
            next = cur + 1;
            if (d == UA_W_CANCEL) {     /* before reading: the entry may be huge */
                result = 1;
                stop = 1;
                break;
            }
            r = read_data(&c, size, d == UA_W_WRITE ? s : NULL);
            if (r == 1 || (r < 0 && c.cancelled)) {
                /* Cancelled here, or by Ctrl-C passed on: the program stops in
                 * the middle of the entry, so the data ends early. */
                if (d == UA_W_WRITE)
                    s->end(s, e, 0, "cancelled");
                result = 1;
                stop = 1;
            } else if (r < 0) {
                if (d == UA_W_WRITE)
                    s->end(s, e, 0, "the data ended early");
                snprintf(err, errlen, "%s stopped in the middle of an entry", PROG);
                result = -1;
                stop = 1;
            }
        } else if (f[0][0] == 'F' && n >= 3 && cur != (ULONG)-1) {
            int code = atoi(f[2]);
            e = &l->v[cur];
            if (code)
                f_errors++;
            if (!begun) {
                /* No data came (missing or wrong password, an error before
                 * the first byte). */
                d = s->begin(s, e);
                next = cur + 1;
                if (d == UA_W_CANCEL) {
                    result = 1;
                    stop = 1;
                    break;
                }
                if (d == UA_W_WRITE)
                    s->end(s, e, 0, code ? code_text(code, n > 3 ? f[3] : NULL)
                                         : "no data came for it");
            } else if (d == UA_W_WRITE) {
                s->end(s, e, code == 0, code ? code_text(code, n > 3 ? f[3] : NULL) : NULL);
            }
            cur = (ULONG)-1;
            begun = 0;
        } else if (f[0][0] == 'R' && n >= 2) {
            rc = atoi(f[1]);
            if (rc && rc != 8)
                snprintf(err, errlen, "%s", code_text(rc, n > 2 ? f[2] : NULL));
        }
    }
    if (stop && !c.cancelled) {
        c.cancelled = 1;
        poke(&c);
    }
    cancelled = child_end(&c);
    if (result)
        return result;
    if (cancelled || rc == 8)
        return 1;
    if (rc < 0) {
        snprintf(err, errlen, "%s stopped without an answer", PROG);
        return -1;
    }
    if (rc && !f_errors)
        return -1;                  /* err holds the program's message */
    /* Everything after the last entry it sent. A wanted entry here was never
     * answered for: the archive ended early (a missing volume, damage). */
    for (; next < l->n; next++) {
        ua_entry *e = &l->v[next];
        if (wanted(e)) {
            if (!*err)
                snprintf(err, errlen, "%s did not extract \"%s\"", PROG, e->name_src);
            return -1;
        }
        if (s->begin(s, e) == UA_W_CANCEL)
            return 1;
    }
    return 0;
}

const ua_engine ua_engine_7z = { "7-zip", z_caps, z_list, z_extract };
