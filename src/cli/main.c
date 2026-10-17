/* SPDX-License-Identifier: MIT */
/* Command-line front end over the core. Three explicit modes: LIST shows the
 * archive and the plan without writing anything, PACK makes a new archive, and
 * the default extracts. Questions are asked only on an interactive console;
 * otherwise the switches decide and a conflict without a switch is skipped and
 * reported.
 *
 * Results (the listing, the summary line) go to standard output; progress,
 * warnings, errors and questions go to the error stream, which the AROS Shell
 * keeps on the console when output is redirected. The Shell has no way to
 * redirect the error stream, so DIAGOUT copies it to standard output for
 * scripts and logs. No terminal control sequences are ever written.
 *
 * Text is made in ISO-8859-1, the AROS console's character set, and written
 * as such to CON: and to files. A Limpet window shows UTF-8 instead, so
 * there the same text is converted on its way out.
 *
 * Return codes: 0 done; 5 done with notes (renamed, skipped, recovered);
 * 10 partly done, cancelled or stopped; 20 nothing done. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "../core/ua.h"
#include "../engines/xad_static.h"
#include <limpet/limpet.h>

#define TEMPLATE "ARCHIVE,FILES/M,TO/K,LIST/S,PACK/S,FORMAT/K,LEVEL/K/N,RECOVER/S,ENGINE/K," \
                 "OVERWRITE/S,SKIP/S,RENAME/S,NOWRAP/S,CANCELAFTER/K/N,STATS/S,QUIET/S," \
                 "DIAGOUT/S,FORMATS/S,HELP/S,TESTSWAP/K,PASSWORD/K"
enum { A_ARCHIVE, A_FILES, A_TO, A_LIST, A_PACK, A_FORMAT, A_LEVEL, A_RECOVER, A_ENGINE,
       A_OVERWRITE, A_SKIP, A_RENAME, A_NOWRAP, A_CANCELAFTER, A_STATS, A_QUIET, A_DIAGOUT,
       A_FORMATS, A_HELP, A_TESTSWAP, A_PASSWORD, A_COUNT };

static int diag_out;      /* DIAGOUT: diagnostics also to standard output */

/* STATS: guest time (under emulation it can run slow or fast against the host
 * clock) and free memory at each step of the job. */
static struct DateStamp t0;
static ULONG ms_since(void)
{
    struct DateStamp now;
    LONG d;
    DateStamp(&now);
    d = (now.ds_Days - t0.ds_Days) * 86400000L + (now.ds_Minute - t0.ds_Minute) * 60000L +
        (now.ds_Tick - t0.ds_Tick) * (1000 / TICKS_PER_SECOND);
    return d < 0 ? 0 : (ULONG)d;
}

static void stat_point(IPTR on, const char *what)
{
    if (on)
        printf("Stats: %-14s %8lu ms  free %lu bytes\n", what, (unsigned long)ms_since(),
               (unsigned long)AvailMem(MEMF_ANY));
    fflush(stdout);
}

const char version[] = "$VER: unarc-cli 0.0.3 (17.10.2026)";

/* Whether each stream goes to a Limpet window. Asked once: the query is a
 * DOS packet, which CON: and file handlers refuse at once. */
static int utf8_stdout, utf8_stderr;

static int to_limpet(BPTR h)
{
    LimpetPresentation p;
    return h && IsInteractive(h) && limpet_query_presentation(h, &p, NULL) == LIMPET_OK;
}

static void detect_consoles(void)
{
    BPTR err = ErrorOutput();
    utf8_stdout = to_limpet(Output());
    utf8_stderr = err ? to_limpet(err) : utf8_stdout;
}

/* ISO-8859-1 text to a stream, as UTF-8 when the stream is a Limpet window. */
static void put_text(FILE *f, const char *s, int utf8)
{
    char buf[512];
    size_t n = 0;
    if (!utf8) {
        fputs(s, f);
        return;
    }
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (n + 2 >= sizeof(buf)) {
            fwrite(buf, 1, n, f);
            n = 0;
        }
        if (c < 0x80) {
            buf[n++] = (char)c;
        } else {
            buf[n++] = (char)(0xC0 | (c >> 6));
            buf[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    fwrite(buf, 1, n, f);
}

static void vput(FILE *f, int utf8, const char *fmt, va_list ap)
{
    char small[UA_PATHMAX + 300], *buf = small;
    va_list aq;
    int n;
    va_copy(aq, ap);
    n = vsnprintf(small, sizeof(small), fmt, aq);
    va_end(aq);
    if (n >= (int)sizeof(small) && (buf = malloc((size_t)n + 1)) != NULL)
        vsnprintf(buf, (size_t)n + 1, fmt, ap);
    else if (!buf)
        buf = small;                         /* cut rather than lose the line */
    put_text(f, buf, utf8);
    if (buf != small)
        free(buf);
}

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vput(stdout, utf8_stdout, fmt, ap);
    va_end(ap);
    /* Unflushed stdout can vanish on handles not yet in write mode (mainline
     * before da9bbd01b5); flush every line. */
    fflush(stdout);
}

/* Diagnostics and progress: the error stream, and with DIAGOUT standard
 * output as well. */
static void diag(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vput(stderr, utf8_stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
    if (diag_out) {
        va_start(ap, fmt);
        vput(stdout, utf8_stdout, fmt, ap);
        va_end(ap);
        fflush(stdout);
    }
}

static void ui_message(void *ctx, const char *fmt, ...)
{
    char buf[UA_PATHMAX + 300];
    va_list ap;
    (void)ctx;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag("%s\n", buf);
}

static void ui_progress(void *ctx, const ua_entry *e, const char *written, UQUAD done)
{
    (void)ctx; (void)e; (void)done;
    diag("  %s\n", written);
}

static int ui_ask(void *ctx, const char *path, const ua_entry *e, int *all)
{
    char line[32];
    (void)ctx;
    (void)e;
    for (;;) {
        diag("\"%s\" exists. Overwrite, Skip, Rename, all-oVerwrite, all-sKip, all-rEname, "
             "Cancel? [o/s/r/v/k/e/c] ", path);
        if (!fgets(line, sizeof(line), stdin))
            return -1;
        switch (line[0] | 0x20) {
        case 'o': return UA_CONFLICT_OVERWRITE;
        case 's': return UA_CONFLICT_SKIP;
        case 'r': return UA_CONFLICT_RENAME;
        case 'v': *all = 1; return UA_CONFLICT_OVERWRITE;
        case 'k': *all = 1; return UA_CONFLICT_SKIP;
        case 'e': *all = 1; return UA_CONFLICT_RENAME;
        case 'c': return -1;
        }
    }
}

static void prot_str(ULONG p, char *s)
{
    /* hsparwed: hspa active high, rwed active low */
    s[0] = p & (1UL << 7) ? 'h' : '-';   /* hold; no FIBF_ name in the SDK */
    s[1] = p & FIBF_SCRIPT ? 's' : '-';
    s[2] = p & FIBF_PURE ? 'p' : '-';
    s[3] = p & FIBF_ARCHIVE ? 'a' : '-';
    s[4] = p & FIBF_READ ? '-' : 'r';
    s[5] = p & FIBF_WRITE ? '-' : 'w';
    s[6] = p & FIBF_EXECUTE ? '-' : 'e';
    s[7] = p & FIBF_DELETE ? '-' : 'd';
    s[8] = 0;
}

static void flags_str(ULONG f, char *s, size_t n)
{
    snprintf(s, n, "%s%s%s%s%s%s%s",
             f & UA_MAP_UNSAFE ? " refused" : "",
             f & UA_MAP_CHARSET ? " charset" : "", f & UA_MAP_LOSSY ? " lossy" : "",
             f & UA_MAP_ILLEGAL ? " illegal-char" : "", f & UA_MAP_ABSOLUTE ? " absolute" : "",
             f & UA_MAP_COLLISION ? " collision" : "", f & UA_MAP_GUESSED ? " guessed-charset" : "");
}

static const char *rule_name(ua_rootkind k)
{
    return k == UA_ROOT_DEST ? "into destination" :
           k == UA_ROOT_TOPDIR ? "archive's own top folder" : "new folder named after the archive";
}

/* Absolute directory holding the archive, so the default destination does not
 * depend on the Shell's current directory. */
static int archive_dir(const char *archive, char *out, LONG n)
{
    BPTR l = Lock((STRPTR)archive, SHARED_LOCK), parent;
    int ok = 0;
    if (!l)
        return 0;
    parent = ParentDir(l);
    if (parent) {
        ok = NameFromLock(parent, out, n);
        UnLock(parent);
    }
    UnLock(l);
    return ok;
}

static void ui_note(void *ctx, const char *fmt, ...)
{
    char buf[UA_PATHMAX + 300];
    va_list ap;
    (void)ctx;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    diag("%s\n", buf);
}

static ua_volumes vs;        /* the volume set of the archive, if it is one */

/* ---- passwords ----
 * The archive handlers take a password as UTF-8. A Limpet window sends UTF-8;
 * CON: and the Shell's own line send ISO-8859-1, which is converted. The
 * password is kept in this buffer only, and cleared before the program ends. */
static char password[512];

static void set_password(const char *in)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)in;
    for (; *p && n + 2 < sizeof(password); p++) {
        if (*p < 0x80 || utf8_stderr) {
            password[n++] = (char)*p;
        } else {
            password[n++] = (char)(0xC0 | (*p >> 6));
            password[n++] = (char)(0x80 | (*p & 0x3F));
        }
    }
    password[n] = 0;
}

/* Asks on the console without echo. 1 with a password, 0 when there is no
 * console to ask or the user gave nothing, -1 on Ctrl-C (stop the job). */
static int ask_password(const char *archive)
{
    BPTR in = Input();
    char line[sizeof(password) / 2];
    size_t n = 0;
    int esc = 0;
    UBYTE c;

    if (!IsInteractive(in))
        return 0;
    diag("Password for %s (not shown): ", FilePart((STRPTR)archive));
    SetMode(in, 1);
    SetSignal(0, SIGBREAKF_CTRL_C);
    for (;;) {
        /* Ctrl-C comes as a signal or, in raw mode on the 2026.09 console, as
         * the byte 3 (below); both stop. */
        if (!WaitForChar(in, 100000)) {
            if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
                n = 0;
                esc = -1;
                break;
            }
            continue;
        }
        if (Read(in, &c, 1) != 1)
            break;
        if (c == 3) {                    /* Ctrl-C, even inside a sequence */
            n = 0;
            esc = -1;
            break;
        }
        if (esc && (c == '\r' || c == '\n')) {
            esc = 0;                     /* Esc then Return: the line ends */
            break;
        }
        if (esc) {                       /* a control sequence: up to its final byte */
            if (c >= 0x40 && c <= 0x7e && !(esc == 1 && c == '['))
                esc = 0;
            else
                esc = 2;
            continue;
        }
        /* 0x9B starts a control sequence in CON:; in Limpet's UTF-8 it is a
         * continuation byte (as in "\xc5\x9b"), so only ESC counts there. */
        if (c == 0x1b || (c == 0x9b && !utf8_stderr)) {
            esc = 1;
            continue;
        }
        if (c == '\r' || c == '\n')
            break;
        if (c == 3) {                    /* Ctrl-C */
            n = 0;
            esc = -1;
            break;
        }
        if (c == 8 || c == 127) {
            if (n)
                n--;
            continue;
        }
        if (c >= 0x20 && n + 1 < sizeof(line))
            line[n++] = (char)c;
    }
    SetMode(in, 0);
    diag("\n");
    line[n] = 0;
    if (n)
        set_password(line);
    memset(line, 0, sizeof(line));
    if (esc < 0)
        return -1;
    return n != 0;
}

static int run(IPTR *args)
{
    const char *archive = (const char *)args[A_ARCHIVE];
    ua_format fmt;
    const ua_engine *eng[4];
    int rc = 20, got, st;
    ua_list l;
    char err[256];
    LONG ioerr;
    ua_op op = args[A_LIST] ? UA_OP_LIST : UA_OP_EXTRACT;
    int recover = args[A_RECOVER] != 0, damaged;
    char planname[UA_PATHMAX];

    DateStamp(&t0);
    stat_point(args[A_STATS], "start");
    memset(&l, 0, sizeof(l));
    /* One volume of a set stands for the whole set, given in any part. */
    memset(&vs, 0, sizeof(vs));
    if (ua_volumes_find(archive, &vs)) {
        if (vs.problem[0] || !vs.count) {
            diag("%s: an incomplete multi-volume set: %s. Nothing was written.\n", archive,
                 vs.problem[0] ? vs.problem : "the other volumes were not found");
            ua_volumes_free(&vs);
            return 20;
        }
        archive = vs.path[0];
        l.volumes = vs.path;
        snprintf(planname, sizeof(planname), "%s", vs.setname);
    } else {
        snprintf(planname, sizeof(planname), "%s", archive);
    }
    if (ua_sniff_file(archive, &fmt, &ioerr) < 0) {
        char why[100];
        Fault(ioerr, NULL, why, sizeof(why));
        diag("%s: %s\n", archive, why);
        return 20;
    }
    if (args[A_ENGINE] && !ua_engine_by_name((const char *)args[A_ENGINE])) {
        diag("unknown engine \"%s\" (libarchive, xad, 7-zip)\n", (char *)args[A_ENGINE]);
        return 20;
    }
    out("Archive: %s\nFormat:  %s\n", archive, ua_format_name(fmt));
    if (vs.count)
        out("Volumes: %d (%s ... %s)\n", vs.count, FilePart((STRPTR)vs.path[0]),
            FilePart((STRPTR)vs.path[vs.count - 1]));
    if (recover)
        out("Mode:    RECOVERY (reading what can be read from a damaged archive)\n");
    if (!args[A_ENGINE] && !ua_engines_for(fmt, op, recover, vs.count > 0, eng, 4)) {
        diag("No engine can %s this format.\n", op == UA_OP_LIST ? "list" : "extract");
        return 20;
    }
    if (args[A_PASSWORD]) {
        char *a = (char *)args[A_PASSWORD];
        set_password(a);
        memset(a, 0, strlen(a));       /* the copy in ReadArgs' buffer */
        l.password = password;
    }
    st = ua_read_list(archive, fmt, op, recover, (const char *)args[A_ENGINE], &l, ui_note, NULL);
    {
        int tries = 0;
        while (st == UA_OPEN_PASSWORD) {
            if (l.pw_state == UA_PW_WRONG)
                diag("That password does not open the list of entries.\n");
            int a = tries++ >= 3 ? 0 : ask_password(archive);
            if (a < 0) {
                diag("Stopped; nothing was written.\n");
                return 10;
            }
            if (!a) {
                diag("The list of entries is encrypted; give the password with PASSWORD.\n"
                     "Nothing was written.\n");
                return 20;
            }
            l.password = password;
            if (vs.count)
                l.volumes = vs.path;
            st = ua_read_list(archive, fmt, op, recover, (const char *)args[A_ENGINE], &l,
                              ui_note, NULL);
        }
    }
    if (st == UA_OPEN_DAMAGED) {
        diag("The archive is damaged. Nothing was written.\n"
            "RECOVER extracts what can still be read, into a new folder.\n");
        return 20;
    }
    if (st == UA_OPEN_CANCELLED) {
        diag("Stopped while reading the archive; nothing was written.\n");
        return 10;
    }
    if (st == UA_OPEN_UNSUPPORTED) {
        char why7[160];
        diag("This archive cannot be read here: %s.\n", l.why);
        if (!ua_7z_available(why7, sizeof(why7)))
            diag("%s\n", why7);
        diag("Nothing was written.\n");
        return 20;
    }
    if (st != UA_OPEN_OK && st != UA_OPEN_PARTIAL)
        return 20;
    got = st == UA_OPEN_PARTIAL ? 1 : 0;
    /* In normal mode a partial list means a damaged archive shown by LIST. */
    damaged = got && !recover;
    stat_point(args[A_STATS], "listed");
    out("Engine:  %s%s\nEntries: %lu\n", l.engine->name,
        got == 1 ? " (archive damaged: listing incomplete)" : "", (unsigned long)l.n);
    if (l.disk_image) {
        diag("This archive holds a disk image; writing disk images is not implemented yet.\n");
        ua_list_free(&l);
        return 10;
    }

    {
        char dest[UA_PATHMAX], perr[256];
        ua_plan p;
        ULONG k;
        if (args[A_TO])
            snprintf(dest, sizeof(dest), "%s", (char *)args[A_TO]);
        else if (!archive_dir(archive, dest, sizeof(dest))) {
            diag("Cannot find the archive's directory.\n");
            ua_list_free(&l);
            return 20;
        }
        /* A recovered extraction always gets its own folder, so its uncertain
         * output never mixes with existing files. */
        if (ua_plan_make(&l, planname, dest, args[A_NOWRAP] != 0 && !recover, &p, perr,
                         sizeof(perr))) {
            diag("Plan failed: %s\n", perr);
            ua_list_free(&l);
            return 20;
        }

        if (op == UA_OP_LIST) {
            for (k = 0; k < l.n && !args[A_QUIET]; k++) {
                ua_entry *e = &l.v[k];
                char ps[10], fl[96], ds[32] = "", *shown;
                prot_str(e->prot_valid ? e->prot : 0, ps);
                if (e->date_valid) {
                    struct DateTime dt;
                    char d[16], t[16];
                    memset(&dt, 0, sizeof(dt));
                    dt.dat_Stamp = e->date;
                    dt.dat_Format = FORMAT_INT;
                    dt.dat_StrDate = d;
                    dt.dat_StrTime = t;
                    if (DateToStr(&dt))
                        snprintf(ds, sizeof(ds), "%s %s", d, t);
                }
                flags_str(e->map_flags, fl, sizeof(fl));
                shown = ua_display_name(e->name_src, e->name_kind);
                out("%10llu %s %-19s %s%s%s\n", (unsigned long long)e->size,
                    e->prot_valid ? ps : "--------", ds,
                    e->type == UA_ET_DIR ? "[dir] " : e->type == UA_ET_LINK ? "[link] " : "",
                    shown ? shown : e->name_src,
                    e->encrypted ? " [encrypted]" : e->skip ? " [not extracted]" : "");
                if (e->comment)
                    out("           : %s\n", e->comment);
                if (e->map_flags)
                    out("           name on disk (%s): \"%s\"\n", fl + 1,
                        e->target ? e->target : "-");
                free(shown);
            }
            out("Would extract to: %s (%s%s)\n", p.root, rule_name(p.kind),
                p.root_renamed ? ", name taken, numbered" : "");
            out("Planned %lu, refused unsafe %lu, encrypted %lu, renamed by collision %lu, "
                "converted %lu, lossy %lu\n", (unsigned long)p.planned, (unsigned long)p.unsafe,
                (unsigned long)p.encrypted, (unsigned long)p.collisions,
                (unsigned long)p.converted, (unsigned long)p.lossy);
            if (damaged)
                out("The archive is damaged; the list above is what could be read before the "
                    "damage.\n");
            rc = got == 1 ? 10 : 0;
        } else {
            ua_extract_opts o;
            ua_report r;
            memset(&o, 0, sizeof(o));
            o.conflict = args[A_OVERWRITE] ? UA_CONFLICT_OVERWRITE :
                         args[A_RENAME] ? UA_CONFLICT_RENAME :
                         args[A_SKIP] ? UA_CONFLICT_SKIP : UA_CONFLICT_ASK;
            if (o.conflict == UA_CONFLICT_ASK && IsInteractive(Input()))
                o.ask = ui_ask;
            o.message = ui_message;
            o.progress = args[A_QUIET] ? NULL : ui_progress;
            if (args[A_CANCELAFTER])
                o.cancel_after = (UQUAD)*(LONG *)args[A_CANCELAFTER];
            /* Encrypted entries: ask now, before anything is written. With no
             * console and no PASSWORD they are left out and reported. */
            if (ua_list_encrypted_wanted(&l) && !l.password) {
                int a = ask_password(archive);
                if (a < 0) {
                    diag("Stopped; nothing was written.\n");
                    ua_list_free(&l);
                    return 10;
                }
                if (a) {
                    l.password = password;
                } else {
                    for (k = 0; k < l.n; k++)
                        if (l.v[k].encrypted && !l.v[k].skip && !l.v[k].excluded) {
                            l.v[k].skip = 1;
                            p.encrypted++;
                            p.with_password--;
                            if (p.planned)
                                p.planned--;
                        }
                }
            }
            out("Extracting to: %s (%s%s)\n", p.root, rule_name(p.kind),
                p.root_renamed ? ", name taken, numbered" : "");
            for (k = 0; k < l.n && !args[A_QUIET]; k++) {
                ua_entry *e = &l.v[k];
                char fl[96], *shown = ua_display_name(e->name_src, e->name_kind);
                const char *nm = shown ? shown : e->name_src;
                if (e->map_flags & UA_MAP_UNSAFE)
                    diag("refused (path leaves the destination): %s\n", nm);
                else if (e->encrypted && e->skip)
                    diag("not extracted (encrypted, %s): %s\n",
                         l.engine->caps(l.format) & UA_CAP_PASSWORD ? "no password" :
                         "this engine cannot decrypt it", nm);
                else if (e->map_flags && e->target) {
                    flags_str(e->map_flags, fl, sizeof(fl));
                    diag("name changed (%s): \"%s\" -> \"%s\"\n", fl + 1, nm, e->target);
                }
                free(shown);
            }
            /* Test aid: replace the archive with another file between the
             * listing and the extraction, as another program could. */
            if (args[A_TESTSWAP]) {
                BPTR in = Open((STRPTR)args[A_TESTSWAP], MODE_OLDFILE);
                BPTR outf = in ? Open((STRPTR)archive, MODE_NEWFILE) : BNULL;
                static UBYTE cb[8192];
                LONG got;
                while (outf && (got = Read(in, cb, sizeof(cb))) > 0)
                    Write(outf, cb, got);
                if (outf)
                    Close(outf);
                if (in)
                    Close(in);
                diag("test: the archive was replaced by %s before extracting\n",
                     (char *)args[A_TESTSWAP]);
            }
            if (ua_extract(archive, &l, &p, &o, &r, err, sizeof(err)) < 0) {
                diag("Failed: %s\n", err);
                if (!r.files && !r.cancelled && !r.failed) {
                    ua_list_free(&l);
                    return 20;
                }
            }
            out("Result: %s%s. Files %lu, folders %lu, skipped %lu, renamed %lu, replaced %lu, "
                "errors %lu, %llu bytes.\n",
                r.cancelled ? "CANCELLED, partial" : r.failed ? "STOPPED by an error, partial: " :
                (r.errors ? "finished with errors" : "complete"), r.failed ? r.why : "",
                (unsigned long)r.files, (unsigned long)r.dirs, (unsigned long)r.skipped,
                (unsigned long)r.renamed, (unsigned long)r.overwritten, (unsigned long)r.errors,
                (unsigned long long)r.bytes);
            if (r.cancelled || r.failed)
                diag("Finished files were kept; the file in progress was removed.\n");
            if (recover)
                diag("Recovered from a damaged archive: check the files before relying on them.\n");
            stat_point(args[A_STATS], "extracted");
            rc = (r.cancelled || r.failed || r.errors || got == 1) ? 10 : recover ? 5 :
                 (r.skipped || r.renamed || p.unsafe || p.encrypted || p.collisions || p.lossy) ? 5 : 0;
        }
    }
    ua_list_free(&l);
    stat_point(args[A_STATS], "freed");
    return rc;
}

/* ---- PACK ---- */

static void pk_progress(void *ctx, const char *name, UQUAD done)
{
    (void)ctx; (void)done;
    diag("  %s\n", name);
}

static int run_pack(IPTR *args)
{
    const char *archive = (const char *)args[A_ARCHIVE];
    char **files = (char **)args[A_FILES];
    int n = 0, rc;
    ua_pack_opts o;
    ua_pack_report r;
    char err[256] = "";

    for (; files && files[n]; n++)
        ;
    memset(&o, 0, sizeof(o));
    if (!n) {
        diag("PACK needs the files to pack after the archive name.\n");
        return 20;
    }
    if (args[A_FORMAT]) {
        const char *f = (const char *)args[A_FORMAT];
        if (!strcasecmp(f, "zip")) o.fmt = UA_PACK_ZIP;
        else if (!strcasecmp(f, "tgz") || !strcasecmp(f, "tar.gz")) o.fmt = UA_PACK_TGZ;
        else if (!strcasecmp(f, "7z")) o.fmt = UA_PACK_7Z;
        else {
            diag("unknown FORMAT \"%s\" (ZIP, TGZ, 7Z)\n", f);
            return 20;
        }
    } else if (!ua_pack_format_from_name(archive, &o.fmt)) {
        diag("Cannot tell the format from \"%s\": name it .zip, .tar.gz or .7z, or give FORMAT.\n",
             archive);
        return 20;
    }
    o.level = args[A_LEVEL] ? *(LONG *)args[A_LEVEL] : -1;
    if (o.level > 9)
        o.level = 9;
    o.replace = args[A_OVERWRITE] != 0;
    o.progress = args[A_QUIET] ? NULL : pk_progress;
    o.message = ui_message;
    if (args[A_CANCELAFTER])
        o.cancel_after = (UQUAD)*(LONG *)args[A_CANCELAFTER];
    out("Archive: %s\nFormat:  %s\n", archive, ua_pack_format_name(o.fmt));
    rc = ua_pack(archive, files, n, &o, &r, err, sizeof(err));
    if (rc < 0 && !r.cancelled && !r.failed) {
        diag("Failed: %s\n", err);
        out("Result: not made. Nothing was written.\n");
        return 20;
    }
    out("Result: %s%s. Files %lu, folders %lu, skipped %lu, errors %lu, %llu bytes read, "
        "%llu bytes written%s.\n",
        r.cancelled ? "CANCELLED, no archive" : r.failed ? "STOPPED, no archive: " :
        r.errors ? "made, with errors" : "made", r.failed ? r.why : "",
        (unsigned long)r.files, (unsigned long)r.dirs, (unsigned long)r.skipped,
        (unsigned long)r.errors, (unsigned long long)r.bytes_in, (unsigned long long)r.bytes_out,
        r.replaced ? ", the old archive replaced" : "");
    if (rc < 0)
        return 10;
    return r.errors ? 10 : r.skipped ? 5 : 0;
}

/* ---- HELP and FORMATS ---- */

static void help(void)
{
    out("unarc-cli: list, extract and make archives.\n\n"
        "  unarc-cli <archive> [TO <drawer>]       extract next to the archive or into TO\n"
        "  unarc-cli <archive> LIST                show what is inside and where it would go\n"
        "  unarc-cli <archive> RECOVER             extract what a damaged archive still holds\n"
        "  unarc-cli <new archive> PACK <files>... make a ZIP, tar.gz or 7z archive\n"
        "  unarc-cli FORMATS                       what each format supports\n\n"
        "Examples:\n"
        "  unarc-cli RAM:Game.lha\n"
        "  unarc-cli \"Work:Downloads/My Docs.zip\" TO RAM:docs\n"
        "  unarc-cli Work:old.zip LIST\n"
        "  unarc-cli RAM:backup.zip PACK Work:Projects S:Startup-Sequence\n"
        "  unarc-cli RAM:src.tar.gz PACK Work:src LEVEL 9\n\n"
        "A multi-volume archive (name.part1.rar ..., name.rar + name.r00 ..., name.7z.001 ...)\n"
        "can be given by any of its volumes; all of them must be in the same drawer.\n\n"
        "Encrypted 7z and RAR archives: the password is asked on the console (not shown),\n"
        "or given with PASSWORD <text> in scripts. Without one, encrypted entries are left\n"
        "out and reported.\n\n"
        "Existing files are never replaced without OVERWRITE, SKIP, RENAME or an answer on\n"
        "the console; in a script, a conflict without one of them is skipped and reported.\n"
        "Results go to standard output, progress and messages to the console (DIAGOUT\n"
        "copies them to standard output). Ctrl-C stops; finished files are kept.\n"
        "Return codes: 0 done, 5 done with notes, 10 partly done or stopped, 20 failed.\n");
}

static void formats(void)
{
    static const ua_format all[] = { UA_FMT_ZIP, UA_FMT_LHA, UA_FMT_LZX, UA_FMT_7Z, UA_FMT_RAR,
                                     UA_FMT_TAR, UA_FMT_GZIP, UA_FMT_BZIP2, UA_FMT_XZ,
                                     UA_FMT_DMS, UA_FMT_ISO9660 };
    unsigned i;
    out("Extracting (what this program has been tested with is in its documentation):\n");
    for (i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        ULONG c = ua_format_caps(all[i]);
        out("  %-8s %s%s%s%s\n", ua_format_name(all[i]),
            c & UA_CAP_EXTRACT ? "list, extract" : c & UA_CAP_LIST ? "list only" : "no",
            c & UA_CAP_AMIGA_PROT ? "; Amiga protection bits and comments" : "",
            c & UA_CAP_PASSWORD ? "; passwords" : "",
            c & UA_CAP_DISKIMAGE ? "; disk images are not written yet" : "");
    }
    out("  Multi-volume: RAR and RAR 5 sets, 7z split into .001, .002, ...\n"
        "  Passwords need unarc-7z next to this program (7z and RAR).\n"
        "Packing:\n"
        "  zip      names (UTF-8), dates, owner rwx as Unix permissions; LEVEL 0-9\n"
        "  tar.gz   names (UTF-8), dates, owner/group/other rwx; LEVEL 1-9\n"
        "  7z       names, dates; LEVEL 0-9\n"
        "  Amiga comments and the h, s, p, a bits are not stored by any of them.\n"
        "  Links are not followed and not stored.\n");
}

static IPTR run_args[A_COUNT];

static LONG run_trampoline(void)
{
    if (run_args[A_PACK])
        return run_pack(run_args);
    {
        int rc = run(run_args);
        ua_volumes_free(&vs);
        return rc;
    }
}

int main(void)
{
    struct RDArgs *rda = ReadArgs(TEMPLATE, (IPTR *)run_args, NULL);
    struct Task *me = FindTask(NULL);
    LONG rc;
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    const IPTR want = 256 * 1024;

    if (!rda) {
        PrintFault(IoErr(), "unarc-cli");
        return 20;
    }
    diag_out = run_args[A_DIAGOUT] != 0;
    detect_consoles();
    ua_7z_init();
    if (run_args[A_HELP] || run_args[A_FORMATS] || !run_args[A_ARCHIVE]) {
        if (run_args[A_FORMATS])
            formats();
        else
            help();
        FreeArgs(rda);
        return run_args[A_ARCHIVE] || run_args[A_HELP] || run_args[A_FORMATS] ? 0 : 20;
    }
    if (run_args[A_FILES] && !run_args[A_PACK]) {
        diag("Files after the archive name are for PACK; to extract, use TO <drawer>.\n");
        FreeArgs(rda);
        return 20;
    }
    /* ABIv11 has no __stack; Shells default to 40 KB. The decoders recurse and
     * keep tables on the stack, so swap to our own when the Shell's is small. */
    if (have < want) {
        struct StackSwapStruct sss;
        struct StackSwapArgs ssa;
        APTR stk = AllocVec(want, MEMF_ANY);
        if (!stk) {
            FreeArgs(rda);
            return 20;
        }
        sss.stk_Lower = stk;
        sss.stk_Upper = (APTR)((IPTR)stk + want);
        sss.stk_Pointer = sss.stk_Upper;
        memset(&ssa, 0, sizeof(ssa));
        rc = NewStackSwap(&sss, (LONG_FUNC)run_trampoline, &ssa);
        FreeVec(stk);
    } else {
        rc = run(run_args);
    }
    memset(password, 0, sizeof(password));
    FreeArgs(rda);
    xad_static_close();
    return rc;
}
