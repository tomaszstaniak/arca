/* SPDX-License-Identifier: MIT */
/* Arca, the Zune front end. Three windows over the core the command line
 * uses: a start window (drop an archive, or open one), a browser (look before
 * writing anything), and a progress window that turns into the result. The
 * work runs in a worker process (job.c); this file only shows, asks and
 * decides. One job at a time per running program. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <exec/tasks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/muimaster.h>
#include <proto/icon.h>
#include <proto/workbench.h>
#include <libraries/mui.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <intuition/intuitionbase.h>
#include <graphics/layers.h>
#include <proto/layers.h>
#include <clib/alib_protos.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <mui/NFloattext_mcc.h>
#include "job.h"
#include "zk_table.h"
#include "zk_steps.h"
#include "zk_icon.h"
#include "zk_drop.h"
#include "../version.h"
#include "../engines/xad_static.h"

#define APPNAME "Arca"
const char version[] = "$VER: Arca " UNARC_VR " (" UNARC_DATE ") " UNARC_RELEASE;

/* From 0x100: Zune's MUI_Request runs the application's input loop and takes
 * return IDs 1..buttons+1 as its own answer (mui_request.c). */
enum { ID_OPEN = 0x100, ID_START_CLOSE, ID_DROP,
       ID_B_FIND, ID_B_SEL, ID_B_ACTIVE, ID_B_EXSEL, ID_B_EXALL, ID_B_INFO, ID_B_CLOSE,
       ID_B_RECOVER, ID_P_CANCEL, ID_P_OPENDIR, ID_P_DETAILS, ID_P_CLOSE, ID_P_RECOVER,
       ID_A_REPLACE, ID_A_KEEP, ID_A_SKIP, ID_A_CANCEL, ID_D_CLOSE,
       ID_PACK_FILES, ID_PACKFMT, ID_LEVEL, ID_SEPARATE, ID_B_REPLAN,
       ID_B_DRAGOUT, ID_Q_REMOVE, ID_Q_CANCELALL, ID_Q_ACTIVE,
       ID_SETTINGS, ID_S_SAVE, ID_S_USE, ID_S_CANCEL, ID_S_WHERE };

static Object *app, *wstart, *wbrowse, *wprog, *wask, *wdetail;
static Object *o_icon, *o_drop, *b_open, *c_packfmt, *b_pack, *b_settings, *sl_level,
              *t_level, *cb_separate, *t_where, *g_fmthint;
static Object *wsettings, *c_sstart, *c_sopen, *cb_sopendir, *cb_sclose, *c_spackwhere,
              *s_spackdir, *p_spackdir, *c_sname, *c_sexwhere, *s_sexdir, *p_sexdir, *c_snewdir,
              *c_sexists, *b_ssave, *b_suse, *b_scancel;
static Object *cb_newfolder;
static Object *s_find, *lv, *lvview, *t_active, *t_bstatus, *s_to, *p_to, *b_exsel, *b_exall,
              *b_info, *b_bclose, *b_brecover;
static Object *t_ptitle, *t_pto, *t_pname, *g_gauge, *t_pcount, *t_presult,
              *b_pcancel, *b_popen, *b_pdetails, *b_pclose, *b_precover;
static Object *lq, *g_qbtns, *b_qremove, *b_qcancelall;
static Object *t_askq, *t_askname, *t_askwhere, *t_askold, *t_asknew, *cb_all, *b_arep, *b_akeep, *b_askip, *b_acancel;
static Object *f_detail;

static struct MsgPort *jport;     /* the worker's messages */
static uj_job *job;               /* the running job, or NULL */
static uj_msg *held_ask;          /* a question the user has not answered yet */
static int quitting;              /* quit once the job has ended */
static int prog_closing;          /* the progress window closes when the job ends */

/* What the browser shows: the list (owned here once the LIST job is done, or
 * lent to an EXTRACT job), rows on screen and the selection by entry index. */
static char b_archive[UA_PATHMAX];
static char b_planname[UA_PATHMAX];      /* names the new folder (a set's name) */
static ua_list blist;
static ua_plan bplan;
static ua_format b_fmt;                 /* the format the listing found */
static int pw_restart_list;             /* read the browser's archive again (password) */
static int b_loaded, b_status = -1;      /* UA_OPEN_* of the listing */
static ULONG b_count;                     /* entries received so far */
static UBYTE *b_sel;                      /* selected, by entry index */
static ULONG b_selcap;
static ULONG *b_rows, b_nrows;            /* entry index of each row on screen */
static char b_filter[128];
static uj_job *b_job;                     /* LIST job of the browser */

/* The last extraction, for the result window and its buttons. */
static char r_root[UA_PATHMAX];
static char r_archive[UA_PATHMAX];
static char *r_detail;
static char r_head[200];                  /* the last result's headline */
static int r_ok;                          /* the last extraction did all it should */
static int r_clean;                       /* the last result has nothing to report */
static int skip_enc_next;                 /* the next extraction leaves out encrypted files */
static int r_damaged;
static int r_from_browser;
static int onopen_browse;                 /* preference: double-click shows contents */

/* The queue of archives to extract, one job at a time. Several archives given
 * at once, or dropped while one is being extracted, wait here; each gets its
 * own line with its own outcome, and the progress window becomes one summary
 * at the end instead of a result to close per archive. */
#define QMAX 64
enum { Q_WAIT, Q_RUN, Q_DONE, Q_REMOVED };
static struct qitem {
    char path[UA_PATHMAX];
    char key[UA_PATHMAX];         /* the set's first volume, or the path */
    int state;
    int ok;                       /* outcome: 1 done, 0 not done or partly */
    char status[80];
    char root[UA_PATHMAX];
    char *detail;
} q[QMAX];
static int nq, qcur = -1, qstop;
/* Preferences (Settings window; the start window remembers its own). */
static int packfmt_pref;                  /* index into pfmts (packing section) */
static int pf_level[4] = { 1, 2, 1, 1 };  /* slider position per pack format */
static int pf_separate;                   /* each item into its own archive */
static int pf_start_open;                 /* at start the open requester, not the pack window */
static int pf_open_after;                 /* open the result's drawer after a clean job */
static int pf_close_after;                /* close the result after a clean job */
static int pf_pack_where;                 /* 0 next to the items, 1 ask, 2 pf_pack_dir */
static char pf_pack_dir[UA_PATHMAX];
static int pf_name_drawer = 1;            /* several items: named after their drawer */
static int pf_ex_where;                   /* 0 next to the archive, 1 ask, 2 pf_ex_dir */
static char pf_ex_dir[UA_PATHMAX];
static int pf_ex_newdir = 1;              /* a new drawer when the archive needs one */
static int pf_ex_exists;                  /* 0 ask, 1 skip, 2 keep both; never replace */

/* ------------------------------------------------------------- helpers */

/* Test aid: with the variable UnarcDebug set to a file name, events are
 * appended there (jobs, results, signals in use at start and exit). */
static char dbg_path[256];
static int test_delay;        /* UnarcTestDelay: ticks between batches */
static int test_quit;         /* TESTQUIT: end when the first job has ended */

static void dbg(const char *fmt, ...)
{
    char buf[UA_PATHMAX + 256];
    va_list ap;
    BPTR fh;
    if (!dbg_path[0])
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fh = Open(dbg_path, MODE_READWRITE);
    if (!fh)
        return;
    Seek(fh, 0, OFFSET_END);
    FPuts(fh, buf);
    FPuts(fh, "\n");
    Close(fh);
}

static void settext(Object *o, const char *fmt, ...)
{
    char buf[UA_PATHMAX + 256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SetAttrs(o, MUIA_Text_Contents, (IPTR)buf, TAG_DONE);
}

/* A long text cut at its end, or a path cut in its middle (the volume and
 * the last names stay), to at most max characters, with "..." where text was
 * left out. For lines that must not grow a window; the whole text is shown
 * elsewhere (Details, a requester). */
static void cut(char *out, size_t n, const char *s, size_t max)
{
    if (max >= n)
        max = n - 1;
    if (strlen(s) <= max || max < 8)
        snprintf(out, n, "%.*s", (int)max, s);
    else
        snprintf(out, n, "%.*s...", (int)(max - 3), s);
}

static void shortpath(char *out, size_t n, const char *path, size_t max)
{
    size_t len = strlen(path), head = 0, tail;
    const char *colon = strchr(path, ':'), *t, *slash;
    if (max >= n)
        max = n - 1;
    if (len <= max || max < 12) {
        cut(out, n, path, max);
        return;
    }
    if (colon && (size_t)(colon - path) + 1 <= max / 3)
        head = colon - path + 1;
    tail = max - head - 3;
    t = path + len - tail;
    /* Start the kept end at a whole name when one starts early in it. */
    slash = strchr(t, '/');
    if (slash && slash[1] && (size_t)(slash - t) < tail / 2)
        t = slash + 1;
    snprintf(out, n, "%.*s...%s", (int)head, path, t);
}

/* Keep the destination readable during a job; the help bubble retains it all. */
static void progress_destination(const char *label, const char *path)
{
    static char full[UA_PATHMAX];
    char brief[49];
    snprintf(full, sizeof(full), "%s", path);
    shortpath(brief, sizeof(brief), full, sizeof(brief) - 1);
    settext(t_pto, "%s: %s", label, brief);
    SetAttrs(t_pto, MUIA_ShortHelp, (IPTR)full, TAG_DONE);
}

/* Result lines are kept to this many characters: the window stays the width
 * it had during the job, and nothing needs wrapping. */
#define RES_COLS 52

/* The short summary under a result's headline: at most three lines of at
 * most RES_COLS characters. The whole text is in Details (detail_text). A
 * summary longer than that loses its ends here, never in a field too small
 * to show it. */
static void setresult(const char *fmt, ...)
{
    char buf[2 * UA_PATHMAX + 512], out[3 * (RES_COLS + 2) + 1], *line, *next;
    int lines = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    out[0] = 0;
    for (line = buf; line && lines < 3; line = next, lines++) {
        char c[RES_COLS + 1];
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        cut(c, sizeof(c), line, RES_COLS);
        snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s", lines ? "\n" : "", c);
    }
    SetAttrs(t_presult, MUIA_Text_Contents, (IPTR)out, TAG_DONE);
}

/* A summary that leaves something out says where the rest is: on a line of
 * its own, or at the end of the third line. */
static void result_more(char *sum, size_t n, int more)
{
    char *nl = strchr(sum, '\n'), *third = nl ? strchr(nl + 1, '\n') : NULL;
    if (!more)
        return;
    if (!third) {
        snprintf(sum + strlen(sum), n - strlen(sum), "\nMore in Details.");
        return;
    }
    third++;
    if (strlen(third) + 17 > RES_COLS) {
        char c[RES_COLS + 1];
        cut(c, sizeof(c), third, RES_COLS - 17);
        snprintf(third, n - (third - sum), "%s", c);
    }
    snprintf(sum + strlen(sum), n - strlen(sum), " More in Details.");
}

static void sizestr(UQUAD n, char *s, size_t sz)
{
    if (n < 10000)
        snprintf(s, sz, "%lu bytes", (unsigned long)n);
    else if (n < 10 * 1024 * 1024)
        snprintf(s, sz, "%lu KB", (unsigned long)((n + 512) / 1024));
    else if (n < 10ULL * 1024 * 1024 * 1024)
        snprintf(s, sz, "%lu.%lu MB", (unsigned long)(n / (1024 * 1024)),
                 (unsigned long)(n % (1024 * 1024) * 10 / (1024 * 1024)));
    else
        snprintf(s, sz, "%lu GB", (unsigned long)(n / (1024ULL * 1024 * 1024)));
}

static const char *leafname(const char *path)
{
    return (const char *)FilePart((STRPTR)path);
}

static int is_open(Object *w)
{
    IPTR o = 0;
    if (w)
        GetAttr(MUIA_Window_Open, w, &o);
    return o != 0;
}

static void open_win(Object *w, int open)
{
    SetAttrs(w, MUIA_Window_Open, open ? TRUE : FALSE, TAG_DONE);
    if (open)
        SetAttrs(w, MUIA_Window_Activate, TRUE, TAG_DONE);
}

/* Absolute path of a file given relative to a lock (Workbench argument). */
static int lockpath(BPTR lock, const char *name, char *out, size_t n)
{
    if (!NameFromLock(lock, out, n))
        return 0;
    if (name && *name)
        return AddPart(out, (STRPTR)name, n) != 0;
    return 1;
}

/* A requester or a typed field can give a name relative to this process's
 * current drawer ("//sub" after Parent, an empty drawer for the one Arca was
 * started in). The worker has no current drawer, so such a name would be
 * looked up from the boot volume's root there: "object not found". Every
 * name is made absolute here, before a job or the settings keep it. A name
 * that does not exist yet (a drawer an extraction makes) is resolved through
 * the nearest drawer that does. */
static void full_path(const char *in, char *out, size_t n)
{
    char head[UA_PATHMAX];
    const char *rest = "";
    BPTR l = BNULL;
    size_t hl;
    snprintf(out, n, "%s", in);
    if (in[0] != ':' && strchr(in, ':'))
        return;
    snprintf(head, sizeof(head), "%s", in);
    for (;;) {
        char *slash;
        l = Lock((STRPTR)head, SHARED_LOCK);
        if (l || !head[0])
            break;
        slash = strrchr(head, '/');
        /* "a/b" goes up to "a", but in "//b" the slashes are the parents. */
        if (!slash)
            hl = head[0] == ':' ? 1 : 0;
        else if (slash == head || slash[-1] == '/')
            hl = slash - head + 1;
        else
            hl = slash - head;
        if (hl >= strlen(head))
            break;
        rest = in + (slash && hl == (size_t)(slash - head) ? hl + 1 : hl);
        head[hl] = 0;
    }
    if (!l || !NameFromLock(l, out, n) || (*rest && !AddPart((STRPTR)out, (STRPTR)rest, n)))
        snprintf(out, n, "%s", in);
    if (l)
        UnLock(l);
}

static int is_dir(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    struct FileInfoBlock *fib;
    int d = 0;
    if (!l)
        return 0;
    fib = AllocDosObject(DOS_FIB, NULL);
    if (fib && Examine(l, fib))
        d = fib->fib_DirEntryType > 0;
    FreeDosObject(DOS_FIB, fib);
    UnLock(l);
    return d;
}

static int is_archive(const char *path)
{
    ua_format f;
    LONG e;
    if (is_dir(path) || ua_sniff_file(path, &f, &e) < 0)
        return 0;
    return f != UA_FMT_UNKNOWN;
}

static void archive_drawer(const char *archive, char *out, size_t n)
{
    BPTR l = Lock((STRPTR)archive, SHARED_LOCK), parent = l ? ParentDir(l) : BNULL;
    out[0] = 0;
    if (!parent || !NameFromLock(parent, out, n))
        out[0] = 0;
    if (parent)
        UnLock(parent);
    if (l)
        UnLock(l);
}

/* A drawer we cannot write to: a CD, a write-protected disk, a volume being
 * validated. Checked before a job starts, so the user picks another place
 * before anything happens. */
static int writable(const char *dir)
{
    BPTR l = Lock((STRPTR)dir, SHARED_LOCK);
    struct InfoData *id;
    int ok = 0;
    if (!l) {
        /* A drawer that does not exist yet is made by the extraction (as
         * UnZip -d does): what counts is the nearest one that exists. */
        char up[UA_PATHMAX];
        char *slash, *colon;
        snprintf(up, sizeof(up), "%s", dir);
        slash = strrchr(up, '/');
        colon = strchr(up, ':');
        if (slash)
            *slash = 0;
        else if (colon && colon[1])
            colon[1] = 0;
        else
            return 0;
        return strcmp(up, dir) != 0 && writable(up);
    }
    id = AllocVec(sizeof(*id), MEMF_ANY);
    if (id && Info(l, id))
        ok = id->id_DiskState == ID_VALIDATED;
    FreeVec(id);
    UnLock(l);
    return ok;
}

static int ask_drawer(const char *title, const char *initial, char *out, size_t n)
{
    struct FileRequester *fr;
    int ok = 0;
    fr = MUI_AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (IPTR)title,
                                 ASLFR_DrawersOnly, TRUE, ASLFR_DoSaveMode, TRUE,
                                 ASLFR_InitialDrawer, (IPTR)initial, TAG_DONE);
    if (!fr)
        return 0;
    if (MUI_AslRequestTags(fr, TAG_DONE)) {
        full_path((char *)fr->fr_Drawer, out, n);
        ok = 1;
    }
    MUI_FreeAslRequest(fr);
    return ok;
}

static int ask_archive(const char *title, char *out, size_t n)
{
    struct FileRequester *fr;
    int ok = 0;
    fr = MUI_AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (IPTR)title, TAG_DONE);
    if (!fr)
        return 0;
    if (MUI_AslRequestTags(fr, TAG_DONE) && fr->fr_File[0]) {
        full_path((char *)fr->fr_Drawer, out, n);
        ok = AddPart(out, fr->fr_File, n) != 0;
    }
    MUI_FreeAslRequest(fr);
    return ok;
}

static void say(const char *fmt, ...)
{
    char buf[UA_PATHMAX + 256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    MUI_RequestA(app, NULL, 0, APPNAME, "_OK", buf, NULL);
}

/* ------------------------------------------------------------- passwords */

/* The password for one archive, as UTF-8 for the archive handlers. Kept
 * while the program runs so that listing and extracting ask once; never
 * logged, never saved, cleared at the end. */
static char pw[512];
static char pw_for[UA_PATHMAX];

static const char *pw_of(const char *archive)
{
    return pw[0] && !strcmp(pw_for, archive) ? pw : NULL;
}

#define ID_PW_OK     0x7a01
#define ID_PW_CANCEL 0x7a02

/* A small window of its own, asked while no job runs. Zune's string gadget
 * holds ISO-8859-1 text; the handlers take UTF-8. */
#define ID_PW_SKIP   0x7a03

/* 1: a password was given; 2: go on without the encrypted files (offered only
 * when can_skip, that is when other files can be extracted without it);
 * 0: cancelled. */
static int ask_password(const char *archive, int wrong, int can_skip)
{
    Object *w, *str, *ok, *cancel, *skip, *sleepers[3];
    ULONG sigs = 0, id = 0, other[8];
    int i, got = 0, nother = 0;

    /* A new answer replaces the old one; a cancelled window leaves none. */
    memset(pw, 0, sizeof(pw));
    pw_for[0] = 0;

    w = WindowObject,
        MUIA_Window_Title, (IPTR)"Password",
        WindowContents, (IPTR)(VGroup,
            Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)(wrong ?
                "That password does not open it. Try again:" :
                "This archive is encrypted. Password:"), End),
            Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)leafname(archive),
                MUIA_Text_SetMin, FALSE, End),
            Child, (IPTR)(str = StringObject, StringFrame, MUIA_String_Secret, TRUE,
                MUIA_String_MaxLen, 250, MUIA_CycleChain, 1, End),
            Child, (IPTR)(HGroup,
                Child, (IPTR)(ok = SimpleButton("_OK")),
                Child, (IPTR)(skip = SimpleButton("_Skip encrypted files")),
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(cancel = SimpleButton("_Cancel")),
            End),
        End),
    End;
    if (!w)
        return 0;
    DoMethod(app, OM_ADDMEMBER, (IPTR)w);
    SetAttrs(skip, MUIA_ShowMe, can_skip, TAG_DONE);
    DoMethod(skip, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID,
             ID_PW_SKIP);
    DoMethod(ok, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID, ID_PW_OK);
    DoMethod(str, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_PW_OK);
    DoMethod(cancel, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2, MUIM_Application_ReturnID,
             ID_PW_CANCEL);
    DoMethod(w, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_PW_CANCEL);
    sleepers[0] = wstart; sleepers[1] = wbrowse; sleepers[2] = wprog;
    for (i = 0; i < 3; i++)
        SetAttrs(sleepers[i], MUIA_Window_Sleep, TRUE, TAG_DONE);
    SetAttrs(w, MUIA_Window_Open, TRUE, MUIA_Window_ActiveObject, (IPTR)str, TAG_DONE);
    /* Its own event loop while the other windows sleep. Waiting only when
     * nothing came; IDs meant for the main loop (a drop on the start window,
     * say) are kept and given back to it afterwards. */
    for (;;) {
        id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
        if (id == ID_PW_OK || id == ID_PW_CANCEL || id == ID_PW_SKIP ||
            id == (ULONG)MUIV_Application_ReturnID_Quit)
            break;
        if (id) {
            if (nother < 8)
                other[nother++] = id;
            continue;
        }
        if (sigs)
            sigs = Wait(sigs);
    }
    if (id == ID_PW_OK) {
        STRPTR t = NULL;
        size_t n = 0;
        const UBYTE *c;
        GetAttr(MUIA_String_Contents, str, (IPTR *)&t);
        for (c = (const UBYTE *)(t ? (char *)t : ""); *c && n + 2 < sizeof(pw); c++) {
            if (*c < 0x80) {
                pw[n++] = (char)*c;
            } else {
                pw[n++] = (char)(0xC0 | (*c >> 6));
                pw[n++] = (char)(0x80 | (*c & 0x3F));
            }
        }
        pw[n] = 0;
        got = n != 0;
        if (got)
            snprintf(pw_for, sizeof(pw_for), "%s", archive);
    }
    {
        /* Overwrite the gadget's buffer before emptying it: setting "" alone
         * leaves the old bytes behind the terminator. */
        STRPTR t = NULL;
        char blank[256];
        GetAttr(MUIA_String_Contents, str, (IPTR *)&t);
        memset(blank, ' ', sizeof(blank) - 1);
        blank[t ? (strlen((char *)t) < sizeof(blank) ? strlen((char *)t) : sizeof(blank) - 1) : 0] = 0;
        SetAttrs(str, MUIA_String_Contents, (IPTR)blank, TAG_DONE);
        SetAttrs(str, MUIA_String_Contents, (IPTR)"", TAG_DONE);
    }
    SetAttrs(w, MUIA_Window_Open, FALSE, TAG_DONE);
    DoMethod(app, OM_REMMEMBER, (IPTR)w);
    MUI_DisposeObject(w);
    for (i = 0; i < 3; i++)
        SetAttrs(sleepers[i], MUIA_Window_Sleep, FALSE, TAG_DONE);
    if (id == (ULONG)MUIV_Application_ReturnID_Quit)
        quitting = 1;
    for (i = 0; i < nother; i++)
        DoMethod(app, MUIM_Application_ReturnID, other[i]);
    dbg("password asked: %s", got ? "given" : id == ID_PW_SKIP ? "skip" : "not given");
    if (!got && id == ID_PW_SKIP)
        return 2;
    return got;
}

/* ------------------------------------------------------------- preferences */

#define PREFS_NAME "Arca.prefs"
#define LEGACY_PREFS_NAME "Unarc.prefs"

/* One "KEY=value" per line in the variable ENV:Arca.prefs. */
static const char *pref_get(const char *buf, const char *key)
{
    size_t kl = strlen(key);
    const char *p = buf;
    while (p && *p) {
        if (!strncmp(p, key, kl) && p[kl] == '=')
            return p + kl + 1;
        p = strchr(p, '\n');
        if (p)
            p++;
    }
    return NULL;
}

static int pref_int(const char *buf, const char *key, int def, int max)
{
    const char *v = pref_get(buf, key);
    int n = v ? atoi(v) : def;
    return n < 0 || n > max ? def : n;
}

static void pref_str(const char *buf, const char *key, char *out, size_t n)
{
    const char *v = pref_get(buf, key);
    size_t l = 0;
    out[0] = 0;
    if (!v)
        return;
    while (v[l] && v[l] != '\n')
        l++;
    snprintf(out, n, "%.*s", (int)l, v);
}

static void prefs_load(void)
{
    char buf[2 * UA_PATHMAX + 400];
    if (GetVar(PREFS_NAME, buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0 &&
        GetVar(LEGACY_PREFS_NAME, buf, sizeof(buf), GVF_GLOBAL_ONLY) <= 0)
        return;
    onopen_browse = strstr(buf, "ONOPEN=BROWSE") != NULL;
    packfmt_pref = strstr(buf, "PACK=ZIP") ? 1 : strstr(buf, "PACK=7Z") ? 2 :
                   strstr(buf, "PACK=TGZ") ? 3 : 0;
    pf_level[0] = pref_int(buf, "LEVELLHA", 1, 1);
    pf_level[1] = pref_int(buf, "LEVELZIP", 2, 3);
    pf_level[2] = pref_int(buf, "LEVEL7Z", 1, 2);
    pf_level[3] = pref_int(buf, "LEVELTGZ", 1, 2);
    pf_separate = pref_int(buf, "SEPARATE", 0, 1);
    pf_start_open = pref_int(buf, "STARTOPEN", 0, 1);
    pf_open_after = pref_int(buf, "OPENAFTER", 0, 1);
    pf_close_after = pref_int(buf, "CLOSEAFTER", 0, 1);
    pf_pack_where = pref_int(buf, "PACKWHERE", 0, 2);
    pref_str(buf, "PACKDIR", pf_pack_dir, sizeof(pf_pack_dir));
    pf_name_drawer = pref_int(buf, "NAMEDRAWER", 1, 1);
    pf_ex_where = pref_int(buf, "EXWHERE", 0, 2);
    pref_str(buf, "EXDIR", pf_ex_dir, sizeof(pf_ex_dir));
    pf_ex_newdir = pref_int(buf, "NEWDIR", 1, 1);
    pf_ex_exists = pref_int(buf, "EXISTS", 0, 2);
}

/* keep: also in ENVARC: so that it survives a reboot ("Save"); otherwise ENV:
 * only, until the next reboot ("Use"). */
static void prefs_store(int keep)
{
    char v[2 * UA_PATHMAX + 400];
    static const char *const fmtkey[4] = { "LHA", "ZIP", "7Z", "TGZ" };
    snprintf(v, sizeof(v), "%s\nPACK=%s\nLEVELLHA=%d\nLEVELZIP=%d\nLEVEL7Z=%d\nLEVELTGZ=%d\n"
             "SEPARATE=%d\nSTARTOPEN=%d\nOPENAFTER=%d\nCLOSEAFTER=%d\nPACKWHERE=%d\n"
             "PACKDIR=%s\n",
             onopen_browse ? "ONOPEN=BROWSE" : "ONOPEN=EXTRACT", fmtkey[packfmt_pref & 3],
             pf_level[0], pf_level[1], pf_level[2], pf_level[3], pf_separate, pf_start_open,
             pf_open_after, pf_close_after, pf_pack_where, pf_pack_dir);
    snprintf(v + strlen(v), sizeof(v) - strlen(v),
             "NAMEDRAWER=%d\nEXWHERE=%d\nEXDIR=%s\nNEWDIR=%d\nEXISTS=%d\n", pf_name_drawer,
             pf_ex_where, pf_ex_dir, pf_ex_newdir, pf_ex_exists);
    SetVar(PREFS_NAME, v, strlen(v), GVF_GLOBAL_ONLY | (keep ? GVF_SAVE_VAR : 0));
}

static void prefs_save(void)
{
    prefs_store(1);
}

/* ------------------------------------------------------------- browser */

static void b_note_selection(void);
static void start_note(const char *title, const char *text);

#define DROP_TITLE "Drop files or an archive here"
#define DROP_TEXT "Files are packed, an archive is extracted."

/* Protection bits as List shows them ("hspa" set, "rwed" granted). */
static void protstr(const ua_entry *e, char *prot, size_t n)
{
    ULONG p = e->prot;
    prot[0] = 0;
    if (e->prot_valid)
        snprintf(prot, n, "%c%c%c%c%c%c%c%c",
                 p & (1UL << 7) ? 'h' : '-', p & FIBF_SCRIPT ? 's' : '-',
                 p & FIBF_PURE ? 'p' : '-', p & FIBF_ARCHIVE ? 'a' : '-',
                 p & FIBF_READ ? '-' : 'r', p & FIBF_WRITE ? '-' : 'w',
                 p & FIBF_EXECUTE ? '-' : 'e', p & FIBF_DELETE ? '-' : 'd');
}

/* The four column strings of an entry (static buffers, valid until the next
 * call); NULL entry: the titles. Protection and comment are shown for the
 * active entry under the table, not as columns. */
static void make_cols(ua_entry *e, char **col)
{
    static char name[UA_PATHMAX], size[32], date[32], note[64];
    if (!e) {
        col[0] = "Name";
        col[1] = "Size";
        col[2] = "Date";
        col[3] = "Note";
        return;
    }
    {
        char *shown = ua_display_name(e->name_src, e->name_kind);
        const char *nm = shown ? shown : e->name_src;
        size_t nl = strlen(nm);
        /* Most archives store directories with the slash already. */
        snprintf(name, sizeof(name), "%s%s", nm,
                 e->type == UA_ET_DIR && (!nl || nm[nl - 1] != '/') ? "/" : "");
        free(shown);
        if (e->type == UA_ET_DIR)
            size[0] = 0;
        else if (!e->size_valid)
            snprintf(size, sizeof(size), "?");
        else
            snprintf(size, sizeof(size), "%llu", (unsigned long long)e->size);
        date[0] = 0;
        if (e->date_valid) {
            struct DateTime dt;
            char d[16], t[16];
            memset(&dt, 0, sizeof(dt));
            dt.dat_Stamp = e->date;
            dt.dat_Format = FORMAT_DOS;
            dt.dat_StrDate = d;
            dt.dat_StrTime = t;
            if (DateToStr(&dt))
                snprintf(date, sizeof(date), "%s %.5s", d, t);
        }
        note[0] = 0;
        if (e->map_flags & UA_MAP_UNSAFE)
            snprintf(note, sizeof(note), "refused: unsafe path");
        else if (e->encrypted)
            snprintf(note, sizeof(note), "encrypted");
        else if (e->skip)
            snprintf(note, sizeof(note), "not extracted");
        else if (e->map_flags)
            snprintf(note, sizeof(note), "name changes");
        if (e->comment && *e->comment)
            snprintf(note + strlen(note), sizeof(note) - strlen(note), "%scomment",
                     note[0] ? ", " : "");
        col[0] = name;
        col[1] = size;
        col[2] = date;
        col[3] = note;
    }
}

static void table_columns(void *ctx, APTR row, char **col)
{
    (void)ctx;
    make_cols(row ? &blist.v[(IPTR)row - 1] : NULL, col);
}

/* Copy the list's selection into b_sel for the rows on screen. Rows hidden by
 * the filter keep what they had. */
static void b_sync_selection(void)
{
    LONG pos = -1;
    ULONG r;
    for (r = 0; r < b_nrows; r++)
        b_sel[b_rows[r]] = 0;
    while ((pos = zk_table_next_selected(lv, pos)) >= 0)
        if ((ULONG)pos < b_nrows)
            b_sel[b_rows[pos]] = 1;
}

static int b_matches(const ua_entry *e)
{
    const char *h, *n;
    size_t fl = strlen(b_filter);
    if (!fl)
        return 1;
    for (h = e->name_src; *h; h++) {
        size_t i;
        for (i = 0, n = h; i < fl && n[i]; i++) {
            char a = n[i], b = b_filter[i];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b)
                break;
        }
        if (i == fl)
            return 1;
    }
    return 0;
}

static int b_grow(ULONG n)
{
    if (n <= b_selcap)
        return 1;
    {
        ULONG c = n + 1024;
        UBYTE *s = realloc(b_sel, c);
        ULONG *r = realloc(b_rows, c * sizeof(*r));
        if (s)
            b_sel = s;
        if (r)
            b_rows = r;
        if (!s || !r)
            return 0;
        memset(b_sel + b_selcap, 0, c - b_selcap);
        b_selcap = c;
    }
    return 1;
}

/* Add entries [first, first+count) as rows, those the filter lets through. */
static void b_add(ULONG first, ULONG count)
{
    static APTR buf[UJ_BATCH];
    ULONG k, nb = 0;
    if (!b_grow(first + count))
        return;
    for (k = first; k < first + count; k++) {
        if (!b_matches(&blist.v[k]))
            continue;
        b_rows[b_nrows++] = k;
        buf[nb++] = (APTR)(IPTR)(k + 1);
        if (nb == UJ_BATCH) {
            zk_table_append(lv, buf, nb);
            nb = 0;
        }
    }
    zk_table_append(lv, buf, nb);
}

static void b_refilter(void)
{
    ULONG r;
    STRPTR s = NULL;
    b_sync_selection();
    GetAttr(MUIA_String_Contents, s_find, (IPTR *)&s);
    snprintf(b_filter, sizeof(b_filter), "%s", s ? (char *)s : "");
    zk_table_quiet(lv, 1);
    zk_table_clear(lv);
    b_nrows = 0;
    b_add(0, b_count);
    for (r = 0; r < b_nrows; r++)
        if (b_sel[b_rows[r]])
            zk_table_select(lv, r, 1);
    zk_table_quiet(lv, 0);
    b_note_selection();
}

/* Entries an "Extract selected" would write: the selected ones, and for a
 * selected directory everything under it. */
static ULONG b_mark_selection(void)
{
    ULONG k, n = 0, d;
    ULONG *dirs = NULL, nd = 0;
    for (k = 0; k < blist.n; k++)
        if (b_sel[k] && blist.v[k].type == UA_ET_DIR)
            nd++;
    /* Without the list of selected drawers their contents would silently be
     * left out; extract nothing instead (the caller says so). */
    if (nd && !(dirs = malloc(nd * sizeof(*dirs))))
        return 0;
    nd = 0;
    for (k = 0; k < blist.n && dirs; k++)
        if (b_sel[k] && blist.v[k].type == UA_ET_DIR)
            dirs[nd++] = k;
    for (k = 0; k < blist.n; k++) {
        ua_entry *e = &blist.v[k];
        int in = b_sel[k];
        for (d = 0; !in && d < nd; d++) {
            const char *dn = blist.v[dirs[d]].name_src;
            size_t l = strlen(dn);
            while (l && dn[l - 1] == '/')
                l--;
            if (l && strncmp(e->name_src, dn, l) == 0 && e->name_src[l] == '/')
                in = 1;
        }
        e->excluded = !in;
        if (in && !e->skip)
            n++;
    }
    free(dirs);
    return n;
}

static void b_note_selection(void)
{
    ULONG k, sel = 0, hidden = 0, changes = 0;
    UQUAD bytes = 0;
    char sz[32], extra[160] = "";
    if (!b_loaded && b_status < 0) {
        settext(t_bstatus, "Reading the archive... %lu entries", (unsigned long)b_count);
        return;
    }
    b_sync_selection();
    for (k = 0; k < b_count; k++) {
        if (blist.v[k].map_flags && !(blist.v[k].map_flags & UA_MAP_UNSAFE))
            changes++;
        if (!b_sel[k])
            continue;
        sel++;
        bytes += blist.v[k].size;
    }
    if (sel && b_nrows < b_count) {
        ULONG r;
        hidden = sel;
        for (r = 0; r < b_nrows; r++)
            if (b_sel[b_rows[r]])
                hidden--;
    }
    sizestr(bytes, sz, sizeof(sz));
    if (hidden)
        snprintf(extra, sizeof(extra), ", %lu of them not shown by the filter",
                 (unsigned long)hidden);
    if (changes)
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 ". Names that change on disk: %lu", (unsigned long)changes);
    settext(t_bstatus, "%lu %s, %lu selected (%s)%s.", (unsigned long)b_count,
            b_count == 1 ? "entry" : "entries", (unsigned long)sel, sz, extra);
    SetAttrs(b_exsel, MUIA_Disabled, !b_loaded || !sel || b_status == UA_OPEN_PARTIAL ||
             b_selcap < blist.n, TAG_DONE);
}

static void b_show_active(void)
{
    LONG pos = zk_table_active(lv);
    IPTR ent = pos >= 0 ? (IPTR)zk_table_row(lv, pos) : 0;
    ua_entry *e;
    char *shown;
    char on[UA_PATHMAX + 64] = "", prot[12], about[200];
    if (!ent) {
        settext(t_active, "");
        return;
    }
    e = &blist.v[ent - 1];
    shown = ua_display_name(e->name_src, e->name_kind);
    if (e->map_flags & UA_MAP_UNSAFE)
        snprintf(on, sizeof(on), "Not extracted: the path would leave the destination.");
    else if (e->encrypted)
        snprintf(on, sizeof(on), e->skip ? "Not extracted: encrypted, and this format's reader "
                 "takes no password." : "Encrypted: the password is asked before extracting.");
    else if (e->target && b_loaded)
        snprintf(on, sizeof(on), "On disk: %s%s%s%s", e->target[0] ? e->target : "(the folder itself)",
                 e->map_flags & UA_MAP_COLLISION ? " (renamed: another entry has the same name)" :
                 e->map_flags & UA_MAP_LOSSY ? " (characters this system cannot show were replaced)" :
                 e->map_flags & UA_MAP_ILLEGAL ? " (characters not allowed in names were replaced)" :
                 "", e->map_flags & UA_MAP_GUESSED ? " (name encoding guessed)" : "",
                 e->map_flags & UA_MAP_ABSOLUTE ? " (leading / removed)" : "");
    protstr(e, prot, sizeof(prot));
    snprintf(about, sizeof(about), "%s%s%s%s", prot[0] ? "Protection " : "", prot,
             e->comment && *e->comment ? (prot[0] ? ", comment: " : "Comment: ") : "",
             e->comment && *e->comment ? e->comment : "");
    settext(t_active, "%s\n%s", on, about);
    free(shown);
}

static void b_reset(void)
{
    ua_list_free(&blist);
    memset(&bplan, 0, sizeof(bplan));
    free(b_sel);
    free(b_rows);
    b_sel = NULL;
    b_rows = NULL;
    b_selcap = b_nrows = b_count = 0;
    b_loaded = 0;
    b_status = -1;
    b_filter[0] = 0;
}

static void start_list(const char *archive);
static void queue_next(void);
static void queue_after(void);
static void pack_done(uj_job *j);
static void k_next(void);
static void result_followups(void);
static ULONG plain_wanted(const ua_list *l);
static void open_folder(const char *path);
static void b_replan(void);
static void start_extract(const char *archive, const char *dest, int recover, int from_browser);

/* A name typed into a requester may be a drawer, a volume, or nothing at
 * all ("RAM" without its colon is a name in the current drawer). Say so
 * here instead of opening an empty browser. */
static int archive_usable(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    if (!l) {
        char why[100];
        Fault(IoErr(), NULL, why, sizeof(why));
        say("Cannot open \"%s\":\n%s", path, why);
        return 0;
    }
    UnLock(l);
    if (is_dir(path)) {
        say("\"%s\" is a drawer or a volume, not an archive.", path);
        return 0;
    }
    return 1;
}

static void browse(const char *archive)
{
    if (job) {
        say("Arca is busy with another archive. Wait for it to finish, or cancel it.");
        return;
    }
    if (!archive_usable(archive))
        return;
    /* Rows first: the display hook reads blist. */
    zk_table_clear(lv);
    b_reset();
    SetAttrs(cb_newfolder, MUIA_NoNotify, TRUE, MUIA_Selected, pf_ex_newdir, TAG_DONE);
    snprintf(b_archive, sizeof(b_archive), "%s", archive);
    SetAttrs(wbrowse, MUIA_Window_Title, (IPTR)leafname(b_archive), TAG_DONE);
    SetAttrs(s_find, MUIA_NoNotify, TRUE, MUIA_String_Contents, (IPTR)"", TAG_DONE);
    settext(t_active, "");
    SetAttrs(b_exsel, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(b_exall, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(b_brecover, MUIA_ShowMe, FALSE, TAG_DONE);
    open_win(wbrowse, 1);
    start_list(archive);
}

/* ------------------------------------------------------------- jobs */

static void start_list(const char *archive)
{
    uj_job *j = uj_new(UJ_LIST, archive, jport);
    if (j) {
        j->test_delay = test_delay;
        if (pw_of(archive))
            snprintf(j->password, sizeof(j->password), "%s", pw);
    }
    if (!j || uj_start(j) < 0) {
        uj_free(j);
        settext(t_bstatus, "Not enough memory to read the archive.");
        return;
    }
    job = b_job = j;
    dbg("job list start %s", archive);
    settext(t_bstatus, "Reading the archive...");
}

static void queue_reset(void);
static int queue_busy(void);

static void prog_reset(const char *archive, const char *dest)
{
    if (!queue_busy())
        queue_reset();
    settext(t_ptitle, "Extracting \33b%s", leafname(archive));
    progress_destination("To", dest);
    settext(t_pname, "");
    settext(t_pcount, "");
    setresult("");
    SetAttrs(g_gauge, MUIA_Gauge_Current, 0, MUIA_Gauge_InfoText, (IPTR)"", MUIA_ShowMe, TRUE,
             TAG_DONE);
    SetAttrs(b_pcancel, MUIA_ShowMe, TRUE, MUIA_Disabled, FALSE, TAG_DONE);
    SetAttrs(b_popen, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_pdetails, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_pclose, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_precover, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_pname, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(t_pto, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(t_pcount, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(t_presult, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(wprog, MUIA_Window_Title, (IPTR)leafname(archive), TAG_DONE);
    open_win(wprog, 1);
}

static void start_extract(const char *archive, const char *dest, int recover, int from_browser)
{
    uj_job *j;
    if (job) {
        say("Arca is busy with another archive. Wait for it to finish, or cancel it.");
        return;
    }
    j = uj_new(UJ_EXTRACT, archive, jport);
    if (!j) {
        say("Not enough memory.");
        return;
    }
    snprintf(j->dest, sizeof(j->dest), "%s", dest);
    j->recover = recover;
    j->skip_encrypted = skip_enc_next;
    skip_enc_next = 0;
    /* The settings for an extraction straight from a drop or a double-click;
     * the browser has its own controls and asks as before. */
    if (!from_browser) {
        j->nowrap = !pf_ex_newdir;
        j->conflict = pf_ex_exists == 1 ? UA_CONFLICT_SKIP :
                      pf_ex_exists == 2 ? UA_CONFLICT_RENAME : UA_CONFLICT_ASK;
    }
    if (pw_of(archive))
        snprintf(j->password, sizeof(j->password), "%s", pw);
    if (from_browser) {
        uj_lend_list(j, &blist, &bplan);
        j->fmt = b_fmt;
        j->list.password = j->password[0] ? j->password : NULL;
    }
    if (uj_start(j) < 0) {
        if (from_browser)
            uj_take_list(j, &blist, &bplan);
        uj_free(j);
        say("Could not start the extraction (not enough memory).");
        return;
    }
    job = j;
    dbg("job extract start %s to %s recover=%d browser=%d", archive, dest, recover, from_browser);
    r_from_browser = from_browser;
    /* The window title keeps the pointer: give it our copy, not the
     * caller's string (a drop's paths are freed right after). */
    if (archive != r_archive)
        snprintf(r_archive, sizeof(r_archive), "%s", archive);
    if (from_browser)
        SetAttrs(wbrowse, MUIA_Window_Sleep, TRUE, TAG_DONE);
    prog_reset(r_archive, dest);
    if (qcur >= 0 && nq > 1)
        settext(t_ptitle, "Extracting \33b%s\33n (%d of %d)", leafname(r_archive), qcur + 1, nq);
    SetAttrs(lq, MUIA_ShowMe, qcur >= 0 && nq > 1, TAG_DONE);
    SetAttrs(g_qbtns, MUIA_ShowMe, qcur >= 0 && nq > 1, TAG_DONE);
}

/* Extract an archive the way a double-click does: next to it, after asking
 * for another place only when that drawer cannot be written. */
static void extract_now(const char *archive)
{
    char dest[UA_PATHMAX];
    archive_drawer(archive, dest, sizeof(dest));
    if (!dest[0]) {
        say("Cannot find the drawer of\n%s", archive);
        return;
    }
    if (pf_ex_where == 2 && pf_ex_dir[0])
        snprintf(dest, sizeof(dest), "%s", pf_ex_dir);
    if (pf_ex_where == 1) {
        char other[UA_PATHMAX], title[300];
        snprintf(title, sizeof(title), "Extract \"%s\" to", leafname(archive));
        if (!ask_drawer(title, dest, other, sizeof(other)))
            return;
        snprintf(dest, sizeof(dest), "%s", other);
    }
    if (!writable(dest)) {
        char other[UA_PATHMAX];
        if (!ask_drawer("The archive's drawer cannot be written. Extract to:", "RAM:", other,
                        sizeof(other)))
            return;
        snprintf(dest, sizeof(dest), "%s", other);
    }
    start_extract(archive, dest, 0, 0);
}

static int queue_busy(void)
{
    int i;
    for (i = 0; i < nq; i++)
        if (q[i].state == Q_WAIT || q[i].state == Q_RUN)
            return 1;
    return 0;
}

/* Forget a finished queue: its rows, and the window parts that show it. A
 * later job must not show them, nor have "Open folder" or "Details..." pick
 * one of its rows. */
static void queue_reset(void)
{
    int i;
    for (i = 0; i < nq; i++)
        free(q[i].detail);
    nq = 0;
    qcur = -1;
    qstop = 0;
    DoMethod(lq, MUIM_List_Clear);
    SetAttrs(lq, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(g_qbtns, MUIA_ShowMe, FALSE, TAG_DONE);
}

/* Add an archive; a finished queue is cleared first. 0 when full. */
static int queue_add(const char *path)
{
    if (!queue_busy() && !job) {
        queue_reset();
        prog_closing = 0;
    }
    {
        /* All volumes of one set are one job: the parts given after the
         * first are not added again. */
        ua_volumes v;
        char key[UA_PATHMAX];
        int i;
        snprintf(key, sizeof(key), "%s", path);
        if (ua_volumes_find(path, &v) && v.count && v.path)
            snprintf(key, sizeof(key), "%s", v.path[0]);
        ua_volumes_free(&v);
        for (i = 0; i < nq; i++)
            if (q[i].state != Q_REMOVED && !strcasecmp(q[i].key, key))
                return 1;
        if (nq == QMAX)
            return 0;
        memset(&q[nq], 0, sizeof(q[nq]));
        snprintf(q[nq].key, sizeof(q[nq].key), "%s", key);
    }
    snprintf(q[nq].path, sizeof(q[nq].path), "%s", path);
    q[nq].state = Q_WAIT;
    nq++;
    DoMethod(lq, MUIM_List_InsertSingle, (IPTR)nq, MUIV_List_Insert_Bottom);
    return 1;
}

static void queue_summary(void)
{
    int i, ok = 0, bad = 0, cancelled = 0;
    for (i = 0; i < nq; i++) {
        if (q[i].state == Q_REMOVED || (q[i].state == Q_DONE && !strncmp(q[i].status, "Cancelled", 9)))
            cancelled++;
        else if (q[i].ok)
            ok++;
        else
            bad++;
    }
    settext(t_ptitle, "\33b%d archives", nq);
    SetAttrs(wprog, MUIA_Window_Title, (IPTR)APPNAME, TAG_DONE);
    if (bad || cancelled)
        setresult("%d extracted, %d with problems, %d cancelled or removed.\n"
                "Select an archive for its folder and details.", ok, bad, cancelled);
    else
        setresult("All %d extracted.\nSelect an archive for its folder and details.", ok);
    SetAttrs(lq, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(g_qbtns, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_precover, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_popen, MUIA_Disabled, TRUE, TAG_DONE);
    SetAttrs(lq, MUIA_List_Active, MUIV_List_Active_Off, TAG_DONE);
    DoMethod(lq, MUIM_List_Redraw, MUIV_List_Redraw_All);
    dbg("queue done: %d archives, %d extracted, %d with problems, %d cancelled", nq, ok, bad,
        cancelled);
}

/* Start the next waiting archive, if nothing runs. */
static void queue_next(void)
{
    int i;
    if (job)
        return;
    for (i = 0; i < nq; i++) {
        if (q[i].state != Q_WAIT)
            continue;
        if (qstop) {
            q[i].state = Q_DONE;
            snprintf(q[i].status, sizeof(q[i].status), "Cancelled (not started)");
            continue;
        }
        q[i].state = Q_RUN;
        qcur = i;
        DoMethod(lq, MUIM_List_Redraw, MUIV_List_Redraw_All);
        extract_now(q[i].path);
        if (job)
            return;
        /* Not started: no destination chosen, or no memory. */
        q[i].state = Q_DONE;
        snprintf(q[i].status, sizeof(q[i].status), "Not extracted");
        qcur = -1;
    }
    qcur = -1;
    if (nq > 1)
        queue_summary();
}

/* The current archive has its result (extract_done): note it, go on. */
static void queue_after(void)
{
    struct qitem *it = &q[qcur];
    it->state = Q_DONE;
    it->ok = r_ok;
    snprintf(it->status, sizeof(it->status), "%s", r_head);
    snprintf(it->root, sizeof(it->root), "%s", r_root);
    free(it->detail);
    it->detail = r_detail ? strdup(r_detail) : NULL;
    qcur = -1;
    if (prog_closing)
        qstop = 1;
    if (queue_busy())
        queue_next();
    else if (nq > 1)
        queue_summary();
    DoMethod(lq, MUIM_List_Redraw, MUIV_List_Redraw_All);
}

/* The archive a queue row stands for, or the last one when the queue has
 * one entry: for "Open folder" and "Details...". */
static struct qitem *queue_selected(void)
{
    IPTR ent = 0;
    if (nq <= 1)
        return NULL;
    DoMethod(lq, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (IPTR)&ent);
    return ent ? &q[ent - 1] : NULL;
}

/* Recovery is only ever asked for: into a new folder next to the archive, or
 * somewhere else when that drawer cannot be written. */
static void recover(const char *archive)
{
    char dest[UA_PATHMAX];
    if (job)
        return;
    archive_drawer(archive, dest, sizeof(dest));
    if (!dest[0] || !writable(dest)) {
        if (!ask_drawer("The archive's drawer cannot be written. Recover to:", "RAM:", dest,
                        sizeof(dest)))
            return;
    }
    start_extract(archive, dest, 1, 0);
}

static void cancel_job(void)
{
    if (!job)
        return;
    dbg("cancel (question open: %d)", held_ask != NULL);
    if (held_ask) {
        held_ask->answer = -1;
        held_ask->all = 0;
        ReplyMsg(&held_ask->m);
        held_ask = NULL;
        open_win(wask, 0);
    }
    uj_cancel(job);
    if (job->kind != UJ_LIST) {
        SetAttrs(b_pcancel, MUIA_Disabled, TRUE, TAG_DONE);
        settext(t_pname, "Cancelling...");
    }
}

static void datestr(const struct DateStamp *ds, char *out, size_t n)
{
    struct DateTime dt;
    char d[16], t[16];
    memset(&dt, 0, sizeof(dt));
    dt.dat_Stamp = *ds;
    dt.dat_Format = FORMAT_DOS;
    dt.dat_StrDate = d;
    dt.dat_StrTime = t;
    if (DateToStr(&dt))
        snprintf(out, n, "%s %.5s", d, t);
    else
        out[0] = 0;
}

/* The question names the file and its drawer on lines of their own, each
 * cut to fit, so a long path does not make the window wide or push the
 * buttons out of view. The whole path is the drawer line's help bubble. */
static void show_ask(uj_msg *m)
{
    BPTR l = Lock((STRPTR)m->path, SHARED_LOCK);
    char old[80] = "unknown", sz[32], ds[40], name[RES_COLS + 1], where[RES_COLS + 1];
    static char dir[UA_PATHMAX], fullpath[UA_PATHMAX];
    held_ask = m;
    if (l) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib && Examine(l, fib)) {
            sizestr(UA_FIB_SIZE(fib), sz, sizeof(sz));
            datestr(&fib->fib_Date, ds, sizeof(ds));
            snprintf(old, sizeof(old), "%s, %s", sz, ds);
        }
        FreeDosObject(DOS_FIB, fib);
        UnLock(l);
    }
    if (m->esize_valid)
        sizestr(m->esize, sz, sizeof(sz));
    else
        snprintf(sz, sizeof(sz), "size unknown");
    if (m->edate_valid)
        datestr(&m->edate, ds, sizeof(ds));
    else
        snprintf(ds, sizeof(ds), "no date");
    snprintf(dir, sizeof(dir), "%.*s", (int)(PathPart((STRPTR)m->path) - (STRPTR)m->path), m->path);
    cut(name, sizeof(name), leafname(m->path), RES_COLS);
    shortpath(where, sizeof(where), dir, RES_COLS);
    settext(t_askname, "\33b%s", name);
    settext(t_askwhere, "%s", where);
    snprintf(fullpath, sizeof(fullpath), "%s", m->path);
    SetAttrs(t_askwhere, MUIA_ShortHelp, (IPTR)fullpath, TAG_DONE);
    SetAttrs(t_askname, MUIA_ShortHelp, (IPTR)fullpath, TAG_DONE);
    settext(t_askold, "%s", old);
    settext(t_asknew, "%s, %s", sz, ds);
    SetAttrs(cb_all, MUIA_Selected, FALSE, TAG_DONE);
    open_win(wask, 1);
    /* Return answers Skip; Replace needs its key or a click. */
    SetAttrs(wask, MUIA_Window_ActiveObject, (IPTR)b_askip, MUIA_Window_DefaultObject,
             (IPTR)b_askip, TAG_DONE);
}

static void answer(LONG a)
{
    IPTR all = 0;
    if (!held_ask)
        return;
    GetAttr(MUIA_Selected, cb_all, &all);
    dbg("answer %ld all=%ld", (long)a, (long)all);
    held_ask->answer = a;
    held_ask->all = all != 0;
    ReplyMsg(&held_ask->m);
    held_ask = NULL;
    open_win(wask, 0);
}

static ULONG n_progress, n_batches;

/* Packing: the share of the input found while preparing, by bytes (by
 * files when every file is empty). Below 100 until the archive is made: the
 * result replaces the gauge then. Files that grew are packed only as far as
 * their size then, but a count can still pass the total. */
static ULONG pack_pct(const uj_msg *m)
{
    UQUAD pct;
    if (m->total_bytes)
        pct = m->total_bytes >= ((UQUAD)1 << 56) ? m->bytes / (m->total_bytes / 100)
                                                 : m->bytes * 100 / m->total_bytes;
    else if (m->total_files)
        pct = (UQUAD)m->files * 100 / m->total_files;
    else
        pct = 0;
    return pct > 99 ? 99 : (ULONG)pct;
}

static void on_pack_progress(uj_msg *m)
{
    char done[32], total[32];
    if (m->phase == UA_PACK_PH_PREPARING) {
        settext(t_pname, "Preparing... %lu file%s found", (unsigned long)m->total_files,
                m->total_files == 1 ? "" : "s");
        SetAttrs(g_gauge, MUIA_Gauge_Current, 0, MUIA_Gauge_InfoText, (IPTR)"", TAG_DONE);
        sizestr(m->total_bytes, total, sizeof(total));
        settext(t_pcount, "%s", total);
        return;
    }
    if (m->phase == UA_PACK_PH_FINISHING)
        settext(t_pname, "Finishing...");
    else if (m->name[0])
        settext(t_pname, "%s", m->name);
    sizestr(m->bytes, done, sizeof(done));
    sizestr(m->total_bytes, total, sizeof(total));
    /* The percentage in the line below, not in the gauge: the gauge draws
     * its text in the shine pen, which on a light background shows only
     * over the filled part. */
    SetAttrs(g_gauge, MUIA_Gauge_Current, pack_pct(m), MUIA_Gauge_InfoText, (IPTR)"", TAG_DONE);
    settext(t_pcount, "%lu %%: %s of %s, %lu of %lu files completed",
            (unsigned long)pack_pct(m), done, total, (unsigned long)m->files,
            (unsigned long)m->total_files);
}

static void on_progress(uj_msg *m)
{
    uj_job *j = m->job;
    if (j->kind == UJ_PACK) {
        on_pack_progress(m);
        return;
    }
    if (m->reading) {
        if (j->kind == UJ_LIST)
            settext(t_bstatus, "Reading the archive... %lu entries", (unsigned long)m->listed);
        else
            settext(t_pname, "Reading the archive... %lu entries", (unsigned long)m->listed);
        return;
    }
    if (n_progress++ < 3 || !(n_progress % 20))
        dbg("progress #%lu at %ld ticks: %llu bytes, %lu files", (unsigned long)n_progress,
            (long)uj_ticks_since(&j->started), (unsigned long long)m->bytes, (unsigned long)m->files);
    char done[32], total[32];
    sizestr(m->bytes, done, sizeof(done));
    if (m->name[0])
        settext(t_pname, "%s", m->name);
    if (j->total_known && j->total) {
        ULONG pct = (ULONG)(m->bytes * 100 / j->total);
        if (pct > 100)
            pct = 100;
        sizestr(j->total, total, sizeof(total));
        /* The percentage below the gauge, as when packing (on_pack_progress). */
        SetAttrs(g_gauge, MUIA_ShowMe, TRUE, MUIA_Gauge_Current, pct,
                 MUIA_Gauge_InfoText, (IPTR)"", TAG_DONE);
        settext(t_pcount, "%lu %%: %s of %s, %lu files completed", (unsigned long)pct, done,
                total, (unsigned long)m->files);
    } else {
        /* Total size unknown (a stream that learns sizes as it goes): no
         * percentage rather than an invented one. */
        SetAttrs(g_gauge, MUIA_ShowMe, FALSE, TAG_DONE);
        settext(t_pcount, "%s, %lu files completed", done, (unsigned long)m->files);
    }
}

static void on_batch(uj_msg *m)
{
    uj_job *j = m->job;
    if (n_batches++ < 3 || !(n_batches % 10))
        dbg("batch #%lu at %ld ticks (listed at %ld): first %lu", (unsigned long)n_batches,
            (long)uj_ticks_since(&j->started), (long)j->t_listed, (unsigned long)m->first);
    /* The worker's list does not change after the first batch; the browser
     * reads it through blist, which is a copy of the job's header. */
    blist = j->list;
    if (!b_grow(m->first + m->count))
        return;
    b_add(m->first, m->count);
    b_count = m->first + m->count;
    settext(t_bstatus, "Reading the archive... %lu entries", (unsigned long)b_count);
}

/* Details: the headline and the whole result text, then what the archive
 * and the engine were, then every message the job kept. */
static void detail_text(uj_job *j, const char *title, const char *body)
{
    size_t n = (j->log ? j->loglen : 0) + strlen(j->archive) + strlen(j->err) +
               (body ? strlen(body) : 0) + 512;
    char *s;
    free(r_detail);
    r_detail = s = malloc(n);
    if (!s)
        return;
    snprintf(s, n, "%s\n\n%s%s", title, body ? body : "", body ? "\n\n" : "");
    if (j->kind == UJ_PACK)
        /* Packing reads no archive: the format is the one written, and
         * libarchive's writer is the only engine. */
        snprintf(s + strlen(s), n - strlen(s), "Archive: %s\nFormat: %s\n%s\n", j->archive,
                 ua_pack_format_name(j->pack.fmt), j->err[0] ? j->err : "");
    else
        snprintf(s + strlen(s), n - strlen(s), "Archive: %s\nFormat: %s\nEngine: %s\n%s%s\n",
                 j->archive, ua_format_name(j->fmt), j->list.engine ? j->list.engine->name : "none",
                 j->recover ? "Mode: recovery\n" : "", j->err[0] ? j->err : "");
    if (j->log)
        strncat(s, j->log, n - strlen(s) - 1);
    if (j->logdropped)
        snprintf(s + strlen(s), n - strlen(s), "\n(%lu more messages not kept)\n",
                 (unsigned long)j->logdropped);
}

static void list_done(uj_job *j)
{
    dbg("job list done: open=%d entries=%lu rows=%lu", j->open_status,
        (unsigned long)j->list.n, (unsigned long)b_nrows);
    if (!is_open(wbrowse)) {
        /* The browser was closed while it was reading: nothing to show. The
         * job's list is freed with the job. */
        zk_table_clear(lv);
        /* blist is only a view of the job's list during reading (on_batch);
         * forget it before b_reset so that it is freed once, by uj_free. */
        memset(&blist, 0, sizeof(blist));
        b_reset();
        return;
    }
    b_status = j->open_status;
    if (j->open_status == UA_OPEN_PASSWORD) {
        int wrong = j->list.pw_state == UA_PW_WRONG;
        uj_take_list(j, &blist, &bplan);
        if (ask_password(b_archive, wrong, 0) == 1) {
            pw_restart_list = 1;     /* on_done starts it again once this job is freed */
            settext(t_bstatus, "Reading the archive...");
        } else {
            settext(t_bstatus, "The list of entries is encrypted; nothing can be shown "
                    "without its password.");
        }
        return;
    }
    if (j->open_status == UA_OPEN_OK || j->open_status == UA_OPEN_PARTIAL) {
        uj_take_list(j, &blist, &bplan);
        snprintf(b_planname, sizeof(b_planname), "%s", j->planname);
        b_fmt = j->fmt;
        b_loaded = 1;
        b_count = blist.n;
        b_grow(blist.n);
        /* Rows from batches the worker did not get to send (cancelled). */
        if (b_nrows < b_count && !b_filter[0]) {
            ULONG have = b_nrows ? b_rows[b_nrows - 1] + 1 : 0;
            if (have < b_count)
                b_add(have, b_count - have);
        }
        SetAttrs(s_to, MUIA_String_Contents, (IPTR)bplan.dest, TAG_DONE);
        SetAttrs(b_exall, MUIA_Disabled, j->open_status != UA_OPEN_OK || blist.disk_image, TAG_DONE);
        if (j->open_status == UA_OPEN_PARTIAL) {
            SetAttrs(b_brecover, MUIA_ShowMe, TRUE, TAG_DONE);
            b_note_selection();
            settext(t_active, "The archive is damaged: this list is what could be read before "
                    "the damage. Nothing is extracted from it as it is;\n\"Recover...\" extracts "
                    "what can be read into a new folder.");
        } else {
            b_note_selection();
            settext(t_active, "Will extract to %s", bplan.root);
        }
        detail_text(j, "Listing", NULL);
    } else {
        detail_text(j, "Listing failed", NULL);
        uj_take_list(j, &blist, &bplan);
        if (j->open_status == UA_OPEN_DAMAGED) {
            settext(t_bstatus, "The archive is damaged and nothing could be listed.");
            SetAttrs(b_brecover, MUIA_ShowMe, TRUE, TAG_DONE);
        } else if (j->open_status == UA_OPEN_NOENGINE)
            settext(t_bstatus, "This is not an archive Arca can read (%s).", ua_format_name(j->fmt));
        else if (j->open_status == UA_OPEN_UNSUPPORTED)
            settext(t_bstatus, "Arca cannot read this archive: %s. See Details.", blist.why);
        else
            settext(t_bstatus, "Cannot read the archive. %s", j->err[0] ? j->err : "See Details.");
    }
}

static void extract_done(uj_job *j)
{
    ua_report *r = &j->rep;
    char sz[32], fc[32], head[200], body[UA_PATHMAX + 600], where[RES_COLS + 1], c[RES_COLS + 1];
    /* The summary: up to three short lines (setresult). The full text, with
     * the whole path and every note, is body, shown in Details. */
    char sum[3 * (RES_COLS + 2) + 1];
    int extracted_any, more = 0;

    snprintf(r_root, sizeof(r_root), "%s", j->have_plan ? j->plan.root : "");
    r_damaged = j->open_status == UA_OPEN_DAMAGED;
    sizestr(r->bytes, sz, sizeof(sz));
    snprintf(fc, sizeof(fc), "%lu file%s", (unsigned long)r->files, r->files == 1 ? "" : "s");
    shortpath(where, sizeof(where), r_root, RES_COLS);
    extracted_any = r->files || r->dirs;
    if (j->open_status == UA_OPEN_DAMAGED) {
        snprintf(head, sizeof(head), "The archive is damaged");
        snprintf(body, sizeof(body), "Nothing was written.\n\"Recover...\" extracts what can still be read, "
                 "into a new folder.");
        snprintf(sum, sizeof(sum), "Nothing was written.\n\"Recover...\" extracts what can still be read.");
    } else if (j->open_status == UA_OPEN_CANCELLED) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "Stopped while reading the archive. Nothing was written.");
        snprintf(sum, sizeof(sum), "Stopped while reading the archive.\nNothing was written.");
    } else if (j->open_status == UA_OPEN_PASSWORD) {
        snprintf(head, sizeof(head), "Password needed");
        snprintf(body, sizeof(body), "The archive is encrypted and no password was given.\n"
                 "Nothing was written.");
        snprintf(sum, sizeof(sum), "%s", body);
    } else if (j->open_status == UA_OPEN_NOENGINE) {
        snprintf(head, sizeof(head), "Not an archive Arca can read");
        snprintf(body, sizeof(body), "Format: %s. Nothing was written.", ua_format_name(j->fmt));
        snprintf(sum, sizeof(sum), "Format: %s.\nNothing was written.", ua_format_name(j->fmt));
    } else if (j->open_status == UA_OPEN_UNSUPPORTED) {
        char why7[160];
        if (ua_7z_available(why7, sizeof(why7)))
            why7[0] = 0;
        else
            strncat(why7, "\n", sizeof(why7) - strlen(why7) - 1);
        snprintf(head, sizeof(head), "Not supported");
        snprintf(body, sizeof(body), "Arca cannot read this archive: %s.\n%sNothing was written.",
                 j->list.why, why7);
        cut(c, sizeof(c), j->list.why, RES_COLS);
        snprintf(sum, sizeof(sum), "%s\nNothing was written.", c);
        more = strlen(j->list.why) > RES_COLS || why7[0];
    } else if (j->open_status == UA_OPEN_ERROR && !strncmp(j->err, "Incomplete multi-volume", 23)) {
        snprintf(head, sizeof(head), "Incomplete set");
        snprintf(body, sizeof(body), "%s\nNothing was written.", j->err);
        cut(c, sizeof(c), j->err, RES_COLS);
        snprintf(sum, sizeof(sum), "%s\nNothing was written.", c);
        more = strlen(j->err) > RES_COLS;
    } else if (j->open_status == UA_OPEN_ERROR || (j->extract_rc < 0 && !extracted_any &&
                                                   !r->cancelled && !r->failed)) {
        snprintf(head, sizeof(head), "Failed");
        snprintf(body, sizeof(body), "%s\nNothing was written.", j->err[0] ? j->err : "See Details.");
        cut(c, sizeof(c), j->err[0] ? j->err : "The reason is in Details.", RES_COLS);
        snprintf(sum, sizeof(sum), "%s\nNothing was written.", c);
        more = strlen(j->err) > RES_COLS;
    } else if (r->cancelled && !extracted_any) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "Nothing was extracted.");
        snprintf(sum, sizeof(sum), "%s", body);
    } else if (r->cancelled) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "%s (%s) were extracted to\n%s\nand kept; the file in "
                 "progress was removed.", fc, sz, r_root);
        snprintf(sum, sizeof(sum), "%s (%s) kept in\n%s\nThe file in progress was removed.", fc, sz,
                 where);
    } else if (r->failed) {
        snprintf(head, sizeof(head), "Stopped: %s", r->why);
        snprintf(body, sizeof(body), "%s (%s) were extracted to\n%s\nbefore it stopped. "
                 "Nothing that was there before was lost.", fc, sz, r_root);
        snprintf(sum, sizeof(sum), "%s (%s) in\n%s\nNothing that was there before was lost.", fc, sz,
                 where);
        more = strlen(head) > RES_COLS;
    } else if (r->errors) {
        snprintf(head, sizeof(head), "Partly done");
        snprintf(body, sizeof(body), "%s (%s) in\n%s\n%lu could not be written: see "
                 "Details.", fc, sz, r_root, (unsigned long)r->errors);
        snprintf(sum, sizeof(sum), "%s (%s) in\n%s\n%lu could not be written.", fc, sz, where,
                 (unsigned long)r->errors);
        more = 1;
    } else if (r->skipped || r->renamed || j->plan.unsafe || j->plan.encrypted ||
               j->plan.collisions || j->plan.lossy || j->recover) {
        snprintf(head, sizeof(head), "Done, with notes");
        snprintf(body, sizeof(body), "%s (%s) in\n%s\nSkipped %lu, renamed %lu%s%s%s.",
                 fc, sz, r_root, (unsigned long)r->skipped,
                 (unsigned long)(r->renamed + j->plan.collisions),
                 j->plan.unsafe ? ", unsafe paths refused" : "",
                 j->plan.encrypted ? ", encrypted files not extracted" : "",
                 j->recover ? ".\nRecovered from a damaged archive: check the files before "
                 "relying on them" : "");
        snprintf(sum, sizeof(sum), "%s (%s) in\n%s\nSkipped %lu, renamed %lu.", fc, sz, where,
                 (unsigned long)r->skipped, (unsigned long)(r->renamed + j->plan.collisions));
        more = j->plan.unsafe || j->plan.encrypted || j->recover;
    } else {
        snprintf(head, sizeof(head), "Done");
        snprintf(body, sizeof(body), "%s (%s) in\n%s", fc, sz, r_root);
        snprintf(sum, sizeof(sum), "%s (%s) in\n%s", fc, sz, where);
    }
    if (j->skipped_encrypted) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 "\n%lu encrypted file%s left out (no password).",
                 (unsigned long)j->skipped_encrypted,
                 j->skipped_encrypted == 1 ? " was" : "s were");
        more = 1;
    }
    /* Something to report: say so in the headline as well. */
    if (!strcmp(head, "Done") && (j->skipped_encrypted ||
                                  (extracted_any && j->have_plan && j->plan.root_renamed)))
        snprintf(head, sizeof(head), "Done, with notes");
    if (extracted_any && j->have_plan && j->plan.root_renamed) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 "\nA drawer of the archive's name was already there, so a new one was made.");
        more = 1;
    }
    if (r->overwritten) {
        snprintf(body + strlen(body), sizeof(body) - strlen(body), "\n%lu files were replaced.",
                 (unsigned long)r->overwritten);
        more = 1;
    }
    if ((r->errors || r->failed) && j->plan.with_password && pw_of(j->archive)) {
        memset(pw, 0, sizeof(pw));
        pw_for[0] = 0;
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 "\nThe password may be wrong; it will be asked again.");
        more = 1;
    }
    result_more(sum, sizeof(sum), more);
    detail_text(j, head, body);
    snprintf(r_head, sizeof(r_head), "%s", head);
    r_ok = !strcmp(head, "Done") || !strcmp(head, "Done, with notes");
    r_clean = !strcmp(head, "Done") && !j->skipped_encrypted &&
              !(j->have_plan && j->plan.root_renamed);
    dbg("job extract done: %s | files=%lu dirs=%lu skipped=%lu renamed=%lu replaced=%lu "
        "errors=%lu bytes=%llu cancelled=%d failed=%d open=%d root=%s", head,
        (unsigned long)r->files, (unsigned long)r->dirs, (unsigned long)r->skipped,
        (unsigned long)r->renamed, (unsigned long)r->overwritten, (unsigned long)r->errors,
        (unsigned long long)r->bytes, r->cancelled, r->failed, j->open_status, r_root);
    cut(c, sizeof(c), head, RES_COLS);
    settext(t_ptitle, "\33b%s", c);
    setresult("%s", sum);
    SetAttrs(t_pname, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(g_gauge, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_pcount, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_presult, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(t_pto, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_pcancel, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_pclose, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(b_pdetails, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(b_popen, MUIA_ShowMe, TRUE, MUIA_Disabled, !extracted_any || !r_root[0], TAG_DONE);
    SetAttrs(b_precover, MUIA_ShowMe, r_damaged, TAG_DONE);
    SetAttrs(wprog, MUIA_Window_DefaultObject, (IPTR)b_pclose, TAG_DONE);
    if (r_from_browser) {
        uj_take_list(j, &blist, &bplan);
        SetAttrs(wbrowse, MUIA_Window_Sleep, FALSE, TAG_DONE);
        /* A drag-out changed the targets; show the browser's own place again. */
        b_replan();
    }
}

static int jobs_done;

static void on_done(uj_job *j)
{
    int pw_answer;
    dbg("done at %ld ticks; %lu batches, %lu progress messages", (long)uj_ticks_since(&j->started),
        (unsigned long)n_batches, (unsigned long)n_progress);
    n_batches = n_progress = 0;
    uj_wait_gone(j);
    job = NULL;
    jobs_done++;
    start_note(NULL, NULL);
    if (held_ask) {          /* cannot happen: the worker waited for it */
        held_ask = NULL;
        open_win(wask, 0);
    }
    if (j->kind == UJ_LIST) {
        b_job = NULL;
        list_done(j);
    } else if (j->kind == UJ_PACK) {
        pack_done(j);
        uj_free(j);
        k_next();
        if (prog_closing && !job) {
            open_win(wprog, 0);
            prog_closing = 0;
        } else if (!job) {
            result_followups();
        }
        return;
    } else if (j->open_status == UA_OPEN_PASSWORD && !j->lent && !quitting &&
               (pw_answer = ask_password(j->archive, j->list.pw_state == UA_PW_WRONG,
                                         j->list.n && plain_wanted(&j->list) > 0)) != 0) {
        /* Nothing was written; the same extraction again, with the password
         * or without the encrypted files. */
        char a[UA_PATHMAX], d[UA_PATHMAX];
        int rec = j->recover;
        snprintf(a, sizeof(a), "%s", j->archive);
        snprintf(d, sizeof(d), "%s", j->dest);
        uj_free(j);
        skip_enc_next = pw_answer == 2;
        start_extract(a, d, rec, 0);
        if (!job && qcur >= 0)
            queue_after();
        return;
    } else {
        SetAttrs(t_pcount, MUIA_ShowMe, TRUE, TAG_DONE);
        extract_done(j);
        if (qcur >= 0)
            queue_after();
        if (prog_closing && !job) {
            open_win(wprog, 0);
            prog_closing = 0;
        } else if (!job) {
            result_followups();
        }
    }
    uj_free(j);
    if (pw_restart_list) {
        pw_restart_list = 0;
        start_list(b_archive);
    }
}

static void poll_jobs(void)
{
    uj_msg *m;
    while ((m = (uj_msg *)GetMsg(jport))) {
        switch (m->type) {
        case UJM_BATCH:
            on_batch(m);
            ReplyMsg(&m->m);
            break;
        case UJM_PROGRESS:
            on_progress(m);
            ReplyMsg(&m->m);
            break;
        case UJM_ASK:
            dbg("ask %s", m->path);
            if (quitting || prog_closing) {
                m->answer = -1;
                ReplyMsg(&m->m);
            } else {
                show_ask(m);
            }
            break;
        case UJM_DONE:
            on_done(m->job);
            break;
        }
    }
}

/* ------------------------------------------------------------- packing */

/* Packing starts from a drop on the start window or from "Pack...", with the
 * format and level set there; where the archive goes and how it is named
 * come from the settings. With "each item into its own archive" every item
 * is a request of its own; requests run one after another. */
#define KMAX 64
static struct kreq {
    char **items;
    int n;
} kq[KMAX];
static int nk, kcur;
static int k_total, k_made, k_bad;     /* archives in this run, made, with problems */
static char k_lines[1200];             /* one line per archive, for the summary */
static char k_lastdir[UA_PATHMAX];

static void kreq_free(struct kreq *r)
{
    while (r->n)
        free(r->items[--r->n]);
    free(r->items);
    r->items = NULL;
}

/* The formats in the order of the start window's button, with the levels
 * its slider offers. gzip has no "store only"; 7z's is not offered (ZIP
 * stores as well and is read everywhere). LhA: lh0 or lh5, as Amiga LhA's
 * usual choice. */
static const struct pfmt {
    const char *name;
    ua_packfmt fmt;
    const char *ext;
    int nlv;
    const char *lv[4];
    int val[4];
    const char *hint;
} pfmts[4] = {
    { "LhA", UA_PACK_LHA, ".lha", 2, { "Store only", "Normal" }, { 0, -1 },
      "Keeps Amiga comments and protection bits." },
    { "ZIP", UA_PACK_ZIP, ".zip", 4, { "Store only", "Fast", "Normal", "Best" }, { 0, 1, -1, 9 },
      "Opens almost everywhere; no Amiga comments." },
    { "7z", UA_PACK_7Z, ".7z", 3, { "Fast", "Normal", "Best" }, { 1, -1, 9 },
      "Smallest archives; opens with 7-Zip or Arca." },
    { "tar.gz", UA_PACK_TGZ, ".tar.gz", 3, { "Fast", "Normal", "Best" }, { 1, -1, 9 },
      "For Unix systems; no Amiga comments." },
};

static const char *pack_ext(int f)
{
    return pfmts[f & 3].ext;
}

static int lvl_count(int f)
{
    return pfmts[f & 3].nlv;
}

static int cur_fmt(void)
{
    IPTR v = 0;
    GetAttr(MUIA_Cycle_Active, c_packfmt, &v);
    return v > 3 ? 0 : (int)v;
}

static void start_show_level(void)
{
    int f = cur_fmt();
    settext(t_level, "\33b%s", pfmts[f].lv[pf_level[f]]);
}

/* The start window follows the format: what it keeps, the level slider's
 * range and position. The format itself is named once, on its button. */
static void start_format_changed(void)
{
    int f = cur_fmt();
    SetAttrs(g_fmthint, MUIA_Group_ActivePage, f, TAG_DONE);
    SetAttrs(sl_level, MUIA_NoNotify, TRUE, MUIA_Numeric_Max, lvl_count(f) - 1,
             MUIA_Numeric_Value, pf_level[f], TAG_DONE);
    start_show_level();
}

static void start_show_where(void)
{
    char where[RES_COLS + 1];
    if (pf_pack_where == 2 && pf_pack_dir[0]) {
        shortpath(where, sizeof(where), pf_pack_dir, 40);
        settext(t_where, "In %s", where);
    } else if (pf_pack_where == 1)
        settext(t_where, "Asked for each archive");
    else
        settext(t_where, "Next to the items");
}

/* The drop area says what a drop does; while a drop cannot be taken, or
 * after one was taken only in part, it says that instead, until the job
 * ends (on_done). */
static void start_note(const char *title, const char *text)
{
    if (title)
        zk_drop_set(o_drop, title, text);
    else
        zk_drop_set(o_drop, DROP_TITLE, DROP_TEXT);
}

/* The name of a volume or assign ("Work" for "Work:"), or the last part. */
static void item_name(const char *path, char *out, size_t n)
{
    const char *leaf = leafname(path);
    if (*leaf) {
        snprintf(out, n, "%s", leaf);
        return;
    }
    {
        const char *c = strchr(path, ':');
        snprintf(out, n, "%.*s", c ? (int)(c - path) : (int)strlen(path), path);
    }
    if (!out[0])
        snprintf(out, n, "Archive");
}

static int path_exists(const char *path)
{
    BPTR l = Lock((STRPTR)path, SHARED_LOCK);
    if (l)
        UnLock(l);
    return l != BNULL;
}

/* "dir/base 2.ext", "dir/base 3.ext" ...: the first name that is free. */
static int free_name(const char *dir, const char *base, const char *ext, char *out, size_t n)
{
    int i;
    for (i = 2; i < 100; i++) {
        char leaf[300];
        snprintf(leaf, sizeof(leaf), "%s %d%s", base, i, ext);
        snprintf(out, n, "%s", dir);
        if (!AddPart((STRPTR)out, (STRPTR)leaf, n))
            return 0;
        if (!path_exists(out))
            return 1;
    }
    return 0;
}

static int ask_save(const char *dir, const char *leaf, char *out, size_t n)
{
    struct FileRequester *fr;
    int ok = 0;
    fr = MUI_AllocAslRequestTags(ASL_FileRequest, ASLFR_TitleText, (IPTR)"Save the archive as",
                                 ASLFR_DoSaveMode, TRUE, ASLFR_InitialDrawer, (IPTR)dir,
                                 ASLFR_InitialFile, (IPTR)leaf, TAG_DONE);
    if (!fr)
        return 0;
    if (MUI_AslRequestTags(fr, TAG_DONE) && fr->fr_File[0]) {
        full_path((char *)fr->fr_Drawer, out, n);
        ok = AddPart((STRPTR)out, fr->fr_File, n) != 0;
    }
    MUI_FreeAslRequest(fr);
    return ok;
}

/* Where the archive for these items goes. 0 when the user cancelled; *replace
 * set when an existing archive of that name may be replaced. */
static int pack_target(char **items, int n, int fmt, char *out, size_t on, int *replace)
{
    char dir[UA_PATHMAX], base[256], leaf[300], parent[UA_PATHMAX];
    const char *ext = pack_ext(fmt);
    *replace = 0;
    archive_drawer(items[0], parent, sizeof(parent));
    if (n == 1)
        item_name(items[0], base, sizeof(base));
    else if (pf_name_drawer && parent[0])
        item_name(parent, base, sizeof(base));
    else
        snprintf(base, sizeof(base), "Archive");
    snprintf(leaf, sizeof(leaf), "%s%s", base, ext);
    if (pf_pack_where == 2 && pf_pack_dir[0])
        snprintf(dir, sizeof(dir), "%s", pf_pack_dir);
    else
        snprintf(dir, sizeof(dir), "%s", parent[0] ? parent : "RAM:");
    if (pf_pack_where == 1 || !writable(dir)) {
        if (pf_pack_where != 1)
            say("\"%s\" cannot be written.\nChoose where to save the archive.", dir);
        if (!ask_save(writable(dir) ? dir : "RAM:", leaf, out, on))
            return 0;
    } else {
        snprintf(out, on, "%s", dir);
        if (!AddPart((STRPTR)out, (STRPTR)leaf, on))
            return 0;
    }
    if (path_exists(out)) {
        char alt[UA_PATHMAX], adir[UA_PATHMAX], abase[256], label[400];
        const char *fp = (const char *)FilePart((STRPTR)out);
        size_t bl = strlen(fp), el = strlen(ext);
        snprintf(adir, sizeof(adir), "%.*s", (int)(fp - out), out);
        /* "Work:Demo/" shown as "Work:Demo" (the name is joined back with AddPart). */
        if (strlen(adir) > 1 && adir[strlen(adir) - 1] == '/')
            adir[strlen(adir) - 1] = 0;
        snprintf(abase, sizeof(abase), "%.*s", (int)(bl > el && !strcasecmp(fp + bl - el, ext) ?
                                                      bl - el : bl), fp);
        if (!free_name(adir, abase, ext, alt, sizeof(alt))) {
            say("\"%s\" exists, and no free name was found next to it.", out);
            return 0;
        }
        if (is_dir(out)) {
            if (MUI_Request(app, wstart, 0, APPNAME, "*_Save as new name|_Cancel",
                            "\"%s\" is a drawer.\nSave the archive as \"%s\"?", out,
                            leafname(alt)) != 1)
                return 0;
            snprintf(out, on, "%s", alt);
            return 1;
        }
        /* A new name is the first button; replacing needs its own key or a
         * click. The old archive stays until the new one is complete. */
        /* "*": the active button, which Return presses (mui_request.c). */
        snprintf(label, sizeof(label), "*_Save as \"%s\"|_Replace|_Cancel", leafname(alt));
        switch (MUI_Request(app, wstart, 0, APPNAME, label,
                            "\"%s\" already exists in %s.\nThe old archive is removed only "
                            "after the new one is complete.", leafname(out), adir)) {
        case 1:
            snprintf(out, on, "%s", alt);
            break;
        case 2:
            *replace = 1;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static void pack_start_one(struct kreq *r);

/* Start the next waiting request; after the last of several, the summary. */
static void k_next(void)
{
    while (!job && kcur < nk) {
        struct kreq *r = &kq[kcur++];
        pack_start_one(r);
        kreq_free(r);
    }
    if (!job && kcur >= nk) {
        nk = kcur = 0;
        if (k_total > 1) {
            /* Two lines like any result; one line per archive in Details. */
            char where[RES_COLS + 1];
            shortpath(where, sizeof(where), k_lastdir[0] ? k_lastdir : "their drawers", RES_COLS);
            settext(t_ptitle, "\33b%d archives made", k_made);
            if (k_bad)
                setresult("%d with problems: see Details.\nThe others are in\n%s", k_bad, where);
            else
                setresult("Each item in its own archive, in\n%s", where);
            free(r_detail);
            r_detail = strdup(k_lines);
            SetAttrs(wprog, MUIA_Window_Title, (IPTR)APPNAME, TAG_DONE);
            snprintf(r_root, sizeof(r_root), "%s", k_lastdir);
            SetAttrs(b_popen, MUIA_ShowMe, TRUE, MUIA_Disabled, !k_made, TAG_DONE);
            r_clean = k_made && !k_bad;
        }
    }
}

static void pack_start_one(struct kreq *r)
{
    char name[UA_PATHMAX];
    uj_job *j;
    int replace, f = cur_fmt();

    if (!pack_target(r->items, r->n, f, name, sizeof(name), &replace)) {
        k_total--;
        return;
    }
    j = uj_new(UJ_PACK, name, jport);
    if (!j) {
        say("Not enough memory.");
        return;
    }
    j->inputs = r->items;
    j->ninputs = r->n;
    r->items = NULL;
    r->n = 0;
    j->pack.fmt = pfmts[f].fmt;
    j->pack.level = pfmts[f].val[pf_level[f]];
    j->pack.replace = replace;
    if (uj_start(j) < 0) {
        uj_free(j);
        say("Could not start packing (not enough memory).");
        return;
    }
    job = j;
    dbg("job pack start %s fmt=%d level=%d items=%d", name, (int)j->pack.fmt, j->pack.level,
        j->ninputs);
    snprintf(r_archive, sizeof(r_archive), "%s", name);
    r_from_browser = 0;
    prog_closing = 0;
    prog_reset(r_archive, "");
    if (k_total > 1)
        settext(t_ptitle, "Packing \33b%s\33n (%d of %d)", leafname(r_archive), kcur, k_total);
    else
        settext(t_ptitle, "Packing \33b%s", leafname(r_archive));
    progress_destination("Into", r_archive);
    settext(t_pname, "Preparing...");
}

/* Pack what was dropped or chosen. Names that do not exist are dropped
 * first (they would make an empty archive). The items become the requests'. */
static void pack_now(char **items, int n)
{
    int i;
    if (job || nk) {
        say("Arca is busy with another job. Wait for it to finish, or cancel it.");
        for (i = 0; i < n; i++)
            free(items[i]);
        free(items);
        return;
    }
    {
        char missing[300] = "";
        int kept = 0, gone = 0;
        for (i = 0; i < n; i++) {
            if (path_exists(items[i])) {
                items[kept++] = items[i];
                continue;
            }
            if (gone++ < 3)
                snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), "%s\"%s\"",
                         gone > 1 ? ", " : "", items[i]);
            free(items[i]);
        }
        n = kept;
        if (gone > 3)
            snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), " and %d more",
                     gone - 3);
        if (gone)
            say("%s %s not found and will not be packed.%s", missing,
                gone == 1 ? "was" : "were", n ? "" : "\nThere is nothing left to pack.");
        if (!n) {
            free(items);
            return;
        }
    }
    nk = kcur = 0;
    k_made = k_bad = 0;
    k_lines[0] = 0;
    k_lastdir[0] = 0;
    if (pf_separate && n > 1) {
        for (i = 0; i < n; i++) {
            if (nk < KMAX && (kq[nk].items = malloc(sizeof(char *)))) {
                kq[nk].items[0] = items[i];
                kq[nk].n = 1;
                nk++;
            } else {
                free(items[i]);
            }
        }
        free(items);
        if (n > KMAX)
            say("Only %d archives are made in one go; %d items were left out.", KMAX, n - KMAX);
    } else {
        kq[0].items = items;
        kq[0].n = n;
        nk = 1;
    }
    k_total = nk;
    k_next();
}

static void pack_done(uj_job *j)
{
    ua_pack_report *r = &j->prep;
    char sz[32], out[32], fc[32], head[200], body[2 * UA_PATHMAX + 400];
    char name[RES_COLS + 1], where[RES_COLS + 1], c[RES_COLS + 1], sum[3 * (RES_COLS + 2) + 1];
    int more = 0;
    sizestr(r->bytes_in, sz, sizeof(sz));
    sizestr(r->bytes_out, out, sizeof(out));
    snprintf(fc, sizeof(fc), "%lu file%s", (unsigned long)r->files, r->files == 1 ? "" : "s");
    archive_drawer(j->archive, r_root, sizeof(r_root));
    /* The summary names the archive and its drawer on lines of their own,
     * so a long path does not push the counts out of view. */
    cut(name, sizeof(name), leafname(j->archive), RES_COLS);
    shortpath(where, sizeof(where), r_root, RES_COLS - 3);
    if (j->extract_rc < 0 && r->cancelled) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "No archive was created.%s", r->replaced ? "" :
                 "\nAn archive that was there before is unchanged.");
        snprintf(sum, sizeof(sum), "%s", body);
    } else if (j->extract_rc < 0) {
        snprintf(head, sizeof(head), "Not created: %s", r->why[0] ? r->why : j->err);
        snprintf(body, sizeof(body), "No archive was created.\nAn archive that was there "
                 "before is unchanged.");
        snprintf(sum, sizeof(sum), "%s", body);
        more = strlen(head) > RES_COLS;
    } else if (r->errors) {
        snprintf(head, sizeof(head), "Archive created, with errors");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s.\n%lu could not be packed as "
                 "they were: see Details.", j->archive, fc, sz, out, (unsigned long)r->errors);
        snprintf(sum, sizeof(sum), "%s\nin %s\n%lu could not be packed as they were.", name, where,
                 (unsigned long)r->errors);
        more = 1;
    } else if (r->skipped || r->kept[0]) {
        snprintf(head, sizeof(head), "Archive created, with notes");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s.", j->archive, fc, sz, out);
        snprintf(sum, sizeof(sum), "%s\nin %s\n%s (%s) packed into %s.", name, where, fc, sz, out);
        if (r->skipped)
            snprintf(body + strlen(body), sizeof(body) - strlen(body),
                     "\n%lu links were not followed.", (unsigned long)r->skipped);
        more = 1;
    } else {
        snprintf(head, sizeof(head), "Archive created");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s.", j->archive, fc, sz, out);
        snprintf(sum, sizeof(sum), "%s\nin %s\n%s (%s) packed into %s.", name, where, fc, sz, out);
    }
    if (j->extract_rc == 0 && r->kept[0])
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 "\nThe old archive could not be deleted (something had it open);\n"
                 "it is kept as %s.", r->kept);
    else if (j->extract_rc == 0 && r->replaced) {
        strncat(body, "\nThe old archive was replaced.", sizeof(body) - strlen(body) - 1);
        more = 1;
    }
    result_more(sum, sizeof(sum), more);
    detail_text(j, head, body);
    dbg("job pack done: %s | files=%lu dirs=%lu skipped=%lu errors=%lu in=%llu out=%llu "
        "cancelled=%d failed=%d rc=%d", head, (unsigned long)r->files, (unsigned long)r->dirs,
        (unsigned long)r->skipped, (unsigned long)r->errors, (unsigned long long)r->bytes_in,
        (unsigned long long)r->bytes_out, r->cancelled, r->failed, j->extract_rc);
    cut(c, sizeof(c), head, RES_COLS);
    settext(t_ptitle, "\33b%s", c);
    setresult("%s", sum);
    SetAttrs(t_pname, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(g_gauge, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_pcount, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_pto, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(t_presult, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(b_pcancel, MUIA_ShowMe, FALSE, TAG_DONE);
    SetAttrs(b_pclose, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(b_pdetails, MUIA_ShowMe, TRUE, TAG_DONE);
    SetAttrs(b_popen, MUIA_ShowMe, TRUE, MUIA_Disabled, j->extract_rc < 0 || !r_root[0], TAG_DONE);
    SetAttrs(b_precover, MUIA_ShowMe, FALSE, TAG_DONE);
    snprintf(r_head, sizeof(r_head), "%s", head);
    r_clean = j->extract_rc == 0 && !r->errors && !r->skipped && !r->kept[0];
    if (j->extract_rc == 0)
        k_made++;
    if (j->extract_rc < 0 || r->errors || r->kept[0])
        k_bad++;
    if (j->extract_rc == 0)
        snprintf(k_lastdir, sizeof(k_lastdir), "%s", r_root);
    if (k_total > 1)
        snprintf(k_lines + strlen(k_lines), sizeof(k_lines) - strlen(k_lines), "%s%s: %s",
                 k_lines[0] ? "\n" : "", leafname(j->archive), head);
    /* Cancel stops the whole run, not only this archive. */
    if (r->cancelled)
        while (kcur < nk)
            kreq_free(&kq[kcur++]);
}

/* Requesters: the way to pack without dragging anything. */
static void pack_pick(void)
{
    struct FileRequester *fr;
    char **items = NULL;
    int n = 0;
    /* One requester for files and drawers: the files chosen in a drawer, or
     * with none chosen, the drawer itself. */
    fr = MUI_AllocAslRequestTags(ASL_FileRequest,
                                 ASLFR_TitleText,
                                 (IPTR)"Pack: choose files, or OK in a drawer to pack all of it",
                                 ASLFR_DoMultiSelect, TRUE, TAG_DONE);
    if (!fr)
        return;
    if (MUI_AslRequestTags(fr, TAG_DONE)) {
        LONG i, cnt = fr->fr_NumArgs ? fr->fr_NumArgs : (fr->fr_File[0] ? 1 : 0);
        items = calloc(cnt ? cnt : 1, sizeof(char *));
        if (items && !cnt) {
            /* The drawer itself, by its full name (an empty drawer is the
             * one shown first, Arca's current one). */
            char p[UA_PATHMAX];
            full_path((char *)fr->fr_Drawer, p, sizeof(p));
            if (p[0] && (items[n] = strdup(p)))
                n++;
        }
        for (i = 0; items && i < cnt; i++) {
            char p[UA_PATHMAX];
            struct WBArg *wa = fr->fr_NumArgs ? &fr->fr_ArgList[i] : NULL;
            if (!wa || !wa->wa_Lock || !lockpath(wa->wa_Lock, (char *)wa->wa_Name, p, sizeof(p))) {
                full_path((char *)fr->fr_Drawer, p, sizeof(p));
                AddPart((STRPTR)p, wa ? (STRPTR)wa->wa_Name : fr->fr_File, sizeof(p));
            }
            if (p[0] && (items[n] = strdup(p)))
                n++;
        }
    }
    MUI_FreeAslRequest(fr);
    if (n)
        pack_now(items, n);
    else
        free(items);
}

/* ------------------------------------------------------------- drops */

#define MAXDROP 64
static char *dropped[MAXDROP];
static int ndropped;

static void drops_clear(void)
{
    while (ndropped)
        free(dropped[--ndropped]);    /* free(NULL) for items handed on */
}

AROS_UFH3S(void, drop_func,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2),
    AROS_UFHA(struct AppMessage **, pmsg, A1))
{
    AROS_USERFUNC_INIT
    struct AppMessage *am = *pmsg;
    LONG i;
    (void)hook;
    (void)obj;
    /* Copy and return: Zune replies to the message right after this hook,
     * and Wanderer waits for that reply. The work starts later. */
    drops_clear();
    for (i = 0; i < am->am_NumArgs && ndropped < MAXDROP; i++) {
        char p[UA_PATHMAX];
        if (lockpath(am->am_ArgList[i].wa_Lock, (char *)am->am_ArgList[i].wa_Name, p, sizeof(p)))
            dropped[ndropped++] = strdup(p);
    }
    DoMethod(app, MUIM_Application_ReturnID, ID_DROP);
    AROS_USERFUNC_EXIT
}
static struct Hook drop_hook;

/* One archive: extract it. Several archives: this version does one at a time
 * and says so. Anything else: that is packing, which is not available yet;
 * the files are listed so the user sees what was understood. */
static void handle_drop(void)
{
    int i, archives = 0;
    for (i = 0; i < ndropped; i++)
        if (dropped[i] && is_archive(dropped[i]))
            archives++;
    if (!ndropped)
        return;
    if (archives == ndropped) {
        /* Archives only: one after another, each its own job. */
        int added = 0;
        char note[80];
        start_note(NULL, NULL);
        for (i = 0; i < ndropped; i++)
            added += queue_add(dropped[i]);
        if (added < ndropped) {
            snprintf(note, sizeof(note), "Only %d archives can wait at a time; %d were not added.",
                     QMAX, ndropped - added);
            start_note("Not all were added", note);
        }
        queue_next();
    } else if (job) {
        start_note("Busy", "Files to pack can be dropped when this job is done.");
    } else {
        /* Files and drawers, or archives mixed with them: that is packing;
         * the pack window shows what and where before anything starts. */
        char **items = calloc(ndropped, sizeof(char *));
        int n = 0;
        start_note(NULL, NULL);
        for (i = 0; items && i < ndropped; i++)
            if (dropped[i]) {
                items[n++] = dropped[i];
                dropped[i] = NULL;
            }
        if (n)
            pack_now(items, n);
        else
            free(items);
    }
    drops_clear();
}

/* ------------------------------------------------------------- windows */

static Object *button(const char *label)
{
    Object *o = SimpleButton(label);
    if (o)
        SetAttrs(o, MUIA_CycleChain, 1, TAG_DONE);
    return o;
}

static const char *onopen_labels[] = { "Extract next to the archive", "Show their contents", NULL };
static const char *packfmt_labels[] = { "LhA", "ZIP", "7z", "tar.gz", NULL };
static const char *reg_titles[] = { "General", "Packing", "Extracting", NULL };
static const char *start_labels[] = { "The start window", "A requester to open an archive", NULL };
static const char *packwhere_labels[] = { "Next to the items", "Ask each time", "In this drawer:",
                                          NULL };
static const char *name_labels[] = { "After the drawer they are in", "Archive", NULL };
static const char *exwhere_labels[] = { "Next to the archive", "Ask each time", "In this drawer:",
                                        NULL };
static const char *newdir_labels[] = { "When the archive needs one", "Never", NULL };
static const char *exists_labels[] = { "Ask", "Skip the file", "Keep both", NULL };

/* ------------------------------------------------------------- queue */

AROS_UFH3S(void, qdisplay_func,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(char **, col, A2),
    AROS_UFHA(APTR, ent, A1))
{
    AROS_USERFUNC_INIT
    static char st[96];
    (void)hook;
    if (!ent) {
        col[0] = "\33bArchive";
        col[1] = "\33bStatus";
    } else {
        struct qitem *it = &q[(IPTR)ent - 1];
        col[0] = (char *)leafname(it->path);
        snprintf(st, sizeof(st), "%s", it->state == Q_WAIT ? "Waiting" :
                 it->state == Q_RUN ? "Extracting..." :
                 it->state == Q_REMOVED ? "Removed" : it->status);
        col[1] = st;
    }
    AROS_USERFUNC_EXIT
}
static struct Hook qdisplay_hook;

/* ------------------------------------------------------------- drag out */

/* Rows dragged out of the browser and dropped on a Wanderer drawer window are
 * extracted into that drawer (zk_table finds the drawer); any other target,
 * the desktop included, extracts nothing. The table calls back from the
 * window's input handling; the work is done in the main loop. */
static char drag_target[UA_PATHMAX];
static char drag_why[160];

static void table_drag_started(void *ctx)
{
    (void)ctx;
    settext(t_active, "Release over a drawer window to extract the selected entries there.");
}

static void table_drag_ended(void *ctx, const char *target, const char *why)
{
    (void)ctx;
    snprintf(drag_target, sizeof(drag_target), "%s", target ? target : "");
    snprintf(drag_why, sizeof(drag_why), "%s", why ? why : "");
    DoMethod(app, MUIM_Application_ReturnID, ID_B_DRAGOUT);
}

static void table_selected_all(void *ctx)
{
    (void)ctx;
    DoMethod(app, MUIM_Application_ReturnID, ID_B_SEL);
}

/* The program's own icon, under whatever name it was started. */
static const char *icon_name(void)
{
    static char n[UA_PATHMAX];
    char p[256];
    if (!GetProgramName(p, sizeof(p)))
        snprintf(p, sizeof(p), "%s", FindTask(NULL)->tc_Node.ln_Name);
    snprintf(n, sizeof(n), "PROGDIR:%s", FilePart(p));
    return n;
}

static int build(void)
{
    static const struct zk_table_cfg tcfg = {
        /* NList sizes a column to its widest text: the name column may take
         * at most 70 % so that size and date stay in view. */
        "MAW=70 BAR,P=\33r BAR,BAR,", table_columns, NULL, table_drag_started,
        table_drag_ended, table_selected_all
    };
    if (!zk_table_init()) {
        say("Arca needs the class NList.mcc, which AROS normally includes,\n"
            "and it could not be opened.");
        return 0;
    }
    {
        /* The level slider: one step per level of the format, the level's
         * name shown above it (start_show_level). */
        static struct TagItem chain[] = { { MUIA_CycleChain, 1 }, { TAG_DONE, 0 } };
        static struct TagItem back[] = { { MUIA_Background, MUII_TextBack }, { TAG_DONE, 0 } };
        if (!zk_steps_init() || !(sl_level = zk_steps_new(4, 0, chain)))
            return 0;
        if (!zk_icon_init() || !zk_drop_init() || !(o_icon = zk_icon_new(icon_name(), NULL)) ||
            !(o_drop = zk_drop_new(DROP_TITLE, DROP_TEXT, back)))
            return 0;
    }
    lvview = zk_table_new(&tcfg, &lv, NULL);
    if (!lvview)
        return 0;
    qdisplay_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(qdisplay_func);
    drop_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(drop_func);

    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)APPNAME,
        MUIA_Application_Version, (IPTR)version,
        MUIA_Application_Description, (IPTR)"Extracts archives",
        MUIA_Application_Base, (IPTR)"UNARC",
        SubWindow, (IPTR)(wstart = WindowObject,
            MUIA_Window_Title, (IPTR)APPNAME,
            MUIA_Window_ID, MAKE_ID('U','A','S','T'),
            /* Zune opened it at its smallest width, which left the level
             * slider a few steps' width; a window the user sized is kept
             * by its ID. */
            MUIA_Window_Width, MUIV_Window_Width_Visible(45),
            MUIA_Window_AppWindow, TRUE,
            WindowContents, (IPTR)(VGroup,
                /* Header: the program's icon and name, and the format the
                 * next drop packs to. */
                Child, (IPTR)(HGroup,
                    Child, (IPTR)o_icon,
                    Child, (IPTR)(TextObject,
                        MUIA_Font, MUIV_Font_Big,
                        MUIA_Text_Contents, (IPTR)"\33b" APPNAME,
                    End),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)Label1("_Format"),
                    Child, (IPTR)(c_packfmt = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)packfmt_labels,
                        MUIA_ControlChar, 'f',
                        MUIA_CycleChain, 1,
                        /* Wide enough for every format: the window does not
                         * change size when the format does. */
                        MUIA_FixWidthTxt, (IPTR)"MMtar.gzMM",
                    End),
                End),
                Child, (IPTR)o_drop,
                /* What the next pack does, one row each, labels on the left. The
                 * empty labels are labels, not HSpace: a space has weight and
                 * would widen the label column at the slider's cost. */
                Child, (IPTR)(ColGroup(2),
                    Child, (IPTR)Label1("Compression"),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)"Faster",
                            MUIA_Text_SetMax, TRUE, End),
                        Child, (IPTR)sl_level,
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)"Smaller",
                            MUIA_Text_SetMax, TRUE, End),
                        /* The level's name, as wide as the longest. */
                        Child, (IPTR)(t_level = TextObject,
                            MUIA_FixWidthTxt, (IPTR)"MStore onlyM",
                        End),
                    End),
                    Child, (IPTR)Label1(""),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)(cb_separate = MUI_MakeObject(MUIO_Checkmark,
                                                                  (IPTR)"_Each item into its own archive")),
                        Child, (IPTR)LLabel1("_Each item into its own archive"),
                        Child, (IPTR)HSpace(0),
                    End),
                    Child, (IPTR)Label1(""),
                    /* One page per format (pfmts' order): a page group is as
                     * wide as its widest page, so no note is cut and the
                     * window does not change size with the format. */
                    Child, (IPTR)(g_fmthint = VGroup,
                        MUIA_Group_PageMode, TRUE,
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)pfmts[0].hint, End),
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)pfmts[1].hint, End),
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)pfmts[2].hint, End),
                        Child, (IPTR)(TextObject, MUIA_Text_Contents, (IPTR)pfmts[3].hint, End),
                    End),
                    Child, (IPTR)Label1("Save to"),
                    Child, (IPTR)(t_where = TextObject, MUIA_Text_SetMin, FALSE, End),
                End),
                Child, (IPTR)MUI_MakeObject(MUIO_HBar, 2),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_open = button("_Open archive...")),
                    Child, (IPTR)(b_pack = button("_Pack...")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_settings = button("Se_ttings...")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wbrowse = WindowObject,
            MUIA_Window_Title, (IPTR)APPNAME,
            MUIA_Window_ID, MAKE_ID('U','A','B','R'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(55),
            MUIA_Window_Height, MUIV_Window_Height_Screen(60),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(HGroup,
                    Child, (IPTR)Label2("_Find"),
                    Child, (IPTR)(s_find = StringObject,
                        StringFrame,
                        MUIA_String_MaxLen, sizeof(b_filter),
                        MUIA_ControlChar, 'f',
                        MUIA_CycleChain, 1,
                    End),
                End),
                Child, (IPTR)lvview,
                Child, (IPTR)(t_active = TextObject,
                    MUIA_Text_Contents, (IPTR)"\n",
                    MUIA_Text_SetMin, FALSE,
                    /* Two lines. Zune clamps FixHeightTxt to the text's own
                     * maximum, which SetVMax (default TRUE) makes the height of
                     * the first contents (area.c, __area_finish_minmax). */
                    MUIA_Text_SetVMax, FALSE,
                    MUIA_FixHeightTxt, (IPTR)"X\nX",
                End),
                Child, (IPTR)(t_bstatus = TextObject,
                    TextFrame,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_SetMin, FALSE,
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(cb_newfolder = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_New folder")),
                    Child, (IPTR)LLabel1("_New folder when the archive needs one"),
                    Child, (IPTR)HSpace(0),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)Label2("_To"),
                    Child, (IPTR)(p_to = PopaslObject,
                        MUIA_Popstring_String, (IPTR)(s_to = StringObject,
                            StringFrame,
                            MUIA_String_MaxLen, UA_PATHMAX,
                            MUIA_ControlChar, 't',
                            MUIA_CycleChain, 1,
                        End),
                        MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopDrawer),
                        MUIA_Popasl_Type, ASL_FileRequest,
                        ASLFR_DrawersOnly, TRUE,
                        ASLFR_TitleText, (IPTR)"Extract to",
                    End),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_exsel = button("Extract _selected")),
                    Child, (IPTR)(b_exall = button("Extract _all")),
                    Child, (IPTR)(b_brecover = button("_Recover...")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_info = button("_Details...")),
                    Child, (IPTR)(b_bclose = button("_Close")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wprog = WindowObject,
            MUIA_Window_Title, (IPTR)APPNAME,
            /* No MUIA_Window_ID: Zune reopens a window with an ID at the size
             * it had when closed, even below the new minimum (window.c,
             * MUIA_Application_GetWinPos before WindowLimits). Closed after a
             * result, this window then hid Cancel on the next extraction. */
            MUIA_Window_Width, MUIV_Window_Width_Screen(40),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(t_ptitle = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(t_pto = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(t_pname = TextObject, TextFrame, MUIA_Background, MUII_TextBack,
                    MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(g_gauge = GaugeObject,
                    GaugeFrame,
                    MUIA_Gauge_Horiz, TRUE,
                    MUIA_Gauge_Max, 100,
                    MUIA_FixHeightTxt, (IPTR)"0",
                End),
                Child, (IPTR)(t_pcount = TextObject, MUIA_Text_SetMin, FALSE, End),
                /* The summary: three short lines (setresult); the whole
                 * text is in Details. Three lines always, so the window does
                 * not change height with the result. Zune clamps FixHeightTxt
                 * to the first contents' height unless SetVMax is off
                 * (area.c, __area_finish_minmax). */
                Child, (IPTR)(t_presult = TextObject,
                    MUIA_Text_Contents, (IPTR)"",
                    MUIA_Text_SetVMax, FALSE,
                    MUIA_FixHeightTxt, (IPTR)"X\nX\nX",
                End),
                Child, (IPTR)(lq = ListviewObject,
                    MUIA_ShowMe, FALSE,
                    MUIA_CycleChain, 1,
                    MUIA_Listview_List, (IPTR)(ListObject,
                        InputListFrame,
                        MUIA_List_Format, (IPTR)"D=8 BAR,D=8",
                        MUIA_List_DisplayHook, (IPTR)&qdisplay_hook,
                        MUIA_List_MinLineHeight, 0,
                    End),
                End),
                Child, (IPTR)(g_qbtns = HGroup,
                    Child, (IPTR)(b_qremove = button("Re_move")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_qcancelall = button("Cancel _all")),
                    MUIA_ShowMe, FALSE,
                End),
                /* No upper limit on the window's height: when progress turns
                 * into the result, rows are hidden, and a window whose content
                 * had a smaller maximum would be made smaller by Zune. The
                 * browser under it then kept the uncovered part unpainted (a
                 * ghost Cancel button). The window now only grows. */
                Child, (IPTR)VSpace(0),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_popen = button("_Open folder")),
                    Child, (IPTR)(b_precover = button("_Recover...")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_pdetails = button("_Details...")),
                    Child, (IPTR)(b_pcancel = button("_Cancel")),
                    Child, (IPTR)(b_pclose = button("C_lose")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wask = WindowObject,
            MUIA_Window_Title, (IPTR)"File exists",
            /* No MUIA_Window_ID, as for the progress window: each question
             * sizes the window to its own names. */
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(t_askq = TextObject,
                    MUIA_Text_Contents, (IPTR)"A file of this name is already there.",
                End),
                Child, (IPTR)(ColGroup(2),
                    GroupFrame,
                    MUIA_Background, MUII_GroupBack,
                    Child, (IPTR)Label1("File"),
                    Child, (IPTR)(t_askname = TextObject, End),
                    Child, (IPTR)Label1("In"),
                    Child, (IPTR)(t_askwhere = TextObject, End),
                    Child, (IPTR)Label1("On disk"),
                    Child, (IPTR)(t_askold = TextObject, End),
                    Child, (IPTR)Label1("In the archive"),
                    Child, (IPTR)(t_asknew = TextObject, End),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(cb_all = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_Do the same for the rest")),
                    Child, (IPTR)LLabel1("_Do the same for the rest"),
                    Child, (IPTR)HSpace(0),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_acancel = button("S_top")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_akeep = button("_Keep both")),
                    Child, (IPTR)(b_arep = button("_Replace")),
                    Child, (IPTR)(b_askip = button("_Skip")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wsettings = WindowObject,
            MUIA_Window_Title, (IPTR)"Arca settings",
            MUIA_Window_ID, MAKE_ID('U','A','S','E'),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(RegisterGroup(reg_titles),
                    MUIA_CycleChain, 1,
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(ColGroup(2),
                            Child, (IPTR)Label1("At start, show"),
                            Child, (IPTR)(c_sstart = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)start_labels, MUIA_CycleChain, 1, End),
                            Child, (IPTR)Label1("Double-clicked archives"),
                            Child, (IPTR)(c_sopen = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)onopen_labels, MUIA_CycleChain, 1, End),
                        End),
                        Child, (IPTR)(VGroup,
                            GroupFrameT("When a job ends"),
                            Child, (IPTR)(HGroup,
                                Child, (IPTR)(cb_sopendir = MUI_MakeObject(MUIO_Checkmark,
                                    (IPTR)"_Open the drawer with the result")),
                                Child, (IPTR)LLabel1("_Open the drawer with the result"),
                                Child, (IPTR)HSpace(0),
                            End),
                            Child, (IPTR)(HGroup,
                                Child, (IPTR)(cb_sclose = MUI_MakeObject(MUIO_Checkmark,
                                    (IPTR)"_Close the result when there is nothing to report")),
                                Child, (IPTR)LLabel1("_Close the result when there is nothing to report"),
                                Child, (IPTR)HSpace(0),
                            End),
                            Child, (IPTR)(TextObject,
                                MUIA_Text_Contents, (IPTR)"Both only after a job without problems; "
                                    "warnings and errors always stay open.",
                                MUIA_Text_SetMin, FALSE,
                            End),
                        End),
                        Child, (IPTR)VSpace(0),
                    End),
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(ColGroup(2),
                            Child, (IPTR)Label1("Save archives"),
                            Child, (IPTR)(c_spackwhere = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)packwhere_labels, MUIA_CycleChain, 1, End),
                            Child, (IPTR)HSpace(0),
                            Child, (IPTR)(p_spackdir = PopaslObject,
                                MUIA_Popstring_String, (IPTR)(s_spackdir = StringObject,
                                    StringFrame, MUIA_String_MaxLen, UA_PATHMAX, MUIA_CycleChain, 1,
                                End),
                                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopDrawer),
                                MUIA_Popasl_Type, ASL_FileRequest,
                                ASLFR_DrawersOnly, TRUE,
                                ASLFR_TitleText, (IPTR)"Save archives in",
                            End),
                            Child, (IPTR)Label1("Name of several items"),
                            Child, (IPTR)(c_sname = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)name_labels, MUIA_CycleChain, 1, End),
                        End),
                        Child, (IPTR)VSpace(0),
                    End),
                    Child, (IPTR)(VGroup,
                        Child, (IPTR)(ColGroup(2),
                            Child, (IPTR)Label1("Extract to"),
                            Child, (IPTR)(c_sexwhere = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)exwhere_labels, MUIA_CycleChain, 1, End),
                            Child, (IPTR)HSpace(0),
                            Child, (IPTR)(p_sexdir = PopaslObject,
                                MUIA_Popstring_String, (IPTR)(s_sexdir = StringObject,
                                    StringFrame, MUIA_String_MaxLen, UA_PATHMAX, MUIA_CycleChain, 1,
                                End),
                                MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopDrawer),
                                MUIA_Popasl_Type, ASL_FileRequest,
                                ASLFR_DrawersOnly, TRUE,
                                ASLFR_TitleText, (IPTR)"Extract to",
                            End),
                            Child, (IPTR)Label1("New drawer"),
                            Child, (IPTR)(c_snewdir = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)newdir_labels, MUIA_CycleChain, 1, End),
                            Child, (IPTR)Label1("When a file exists"),
                            Child, (IPTR)(c_sexists = CycleObject,
                                MUIA_Cycle_Entries, (IPTR)exists_labels, MUIA_CycleChain, 1, End),
                        End),
                        Child, (IPTR)(TextObject,
                            MUIA_Text_Contents, (IPTR)"Files on disk are never replaced without "
                                "asking.",
                            MUIA_Text_SetMin, FALSE,
                        End),
                        Child, (IPTR)VSpace(0),
                    End),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_ssave = button("_Save")),
                    Child, (IPTR)(b_suse = button("_Use")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(TextObject,
                        MUIA_Text_Contents, (IPTR)"Arca " UNARC_RELEASE,
                        MUIA_Text_SetMin, TRUE,
                    End),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_scancel = button("_Cancel")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wdetail = WindowObject,
            MUIA_Window_Title, (IPTR)"Details",
            MUIA_Window_ID, MAKE_ID('U','A','D','T'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(50),
            MUIA_Window_Height, MUIV_Window_Height_Screen(40),
            /* NList's Floattext: Zune's wraps lines under its own scroller
             * (floattext.c SetText wraps to the width of the whole Listview),
             * and the end of a long path was hidden there. NFloattext wraps to
             * the visible width and copies the text it is given. */
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)NListviewObject,
                    MUIA_NListview_NList, (IPTR)(f_detail = NFloattextObject,
                        ReadListFrame,
                    End),
                End,
            End),
        End),
    End;
    if (!app)
        return 0;
    return 1;
}

static void notify(void)
{
    struct { Object *o; ULONG id; } press[] = {
        { b_open, ID_OPEN }, { b_pack, ID_PACK_FILES }, { b_settings, ID_SETTINGS },
        { b_ssave, ID_S_SAVE }, { b_suse, ID_S_USE }, { b_scancel, ID_S_CANCEL },
        { b_exsel, ID_B_EXSEL }, { b_exall, ID_B_EXALL }, { b_info, ID_B_INFO },
        { b_bclose, ID_B_CLOSE }, { b_brecover, ID_B_RECOVER },
        { b_pcancel, ID_P_CANCEL }, { b_popen, ID_P_OPENDIR }, { b_pdetails, ID_P_DETAILS },
        { b_pclose, ID_P_CLOSE }, { b_precover, ID_P_RECOVER },
        { b_arep, ID_A_REPLACE }, { b_akeep, ID_A_KEEP },
        { b_askip, ID_A_SKIP }, { b_acancel, ID_A_CANCEL },
    };
    size_t i;
    for (i = 0; i < sizeof(press) / sizeof(press[0]); i++)
        DoMethod(press[i].o, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2,
                 MUIM_Application_ReturnID, press[i].id);
    DoMethod(wstart, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_START_CLOSE);
    DoMethod(wbrowse, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_CLOSE);
    DoMethod(wprog, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_P_CLOSE);
    DoMethod(wask, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_A_CANCEL);
    DoMethod(wdetail, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_D_CLOSE);
    DoMethod(b_qremove, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_Q_REMOVE);
    DoMethod(b_qcancelall, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_Q_CANCELALL);
    DoMethod(lq, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_Q_ACTIVE);
    DoMethod(cb_newfolder, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_REPLAN);
    DoMethod(s_to, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_REPLAN);
    SetAttrs(cb_newfolder, MUIA_Selected, pf_ex_newdir, MUIA_CycleChain, 1, TAG_DONE);
    DoMethod(s_find, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_FIND);
    DoMethod(lv, MUIM_Notify, MUIA_NList_SelectChange, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_SEL);
    DoMethod(lv, MUIM_Notify, MUIA_NList_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_ACTIVE);
    DoMethod(wstart, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime, (IPTR)wstart, 3,
             MUIM_CallHook, (IPTR)&drop_hook, MUIV_TriggerValue);
    DoMethod(c_packfmt, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_PACKFMT);
    DoMethod(sl_level, MUIM_Notify, MUIA_Numeric_Value, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_LEVEL);
    DoMethod(cb_separate, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_SEPARATE);
    DoMethod(c_spackwhere, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_S_WHERE);
    DoMethod(c_sexwhere, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_S_WHERE);
    DoMethod(wsettings, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_S_CANCEL);
    SetAttrs(c_packfmt, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, packfmt_pref, TAG_DONE);
    SetAttrs(cb_separate, MUIA_NoNotify, TRUE, MUIA_Selected, pf_separate, MUIA_CycleChain, 1,
             TAG_DONE);
    start_format_changed();
    start_show_where();
}

/* ------------------------------------------------------------- settings */

static void settings_where_changed(void)
{
    IPTR p = 0, e = 0;
    GetAttr(MUIA_Cycle_Active, c_spackwhere, &p);
    GetAttr(MUIA_Cycle_Active, c_sexwhere, &e);
    SetAttrs(p_spackdir, MUIA_Disabled, p != 2, TAG_DONE);
    SetAttrs(p_sexdir, MUIA_Disabled, e != 2, TAG_DONE);
}

static void settings_show(void)
{
    SetAttrs(c_sstart, MUIA_Cycle_Active, pf_start_open, TAG_DONE);
    SetAttrs(c_sopen, MUIA_Cycle_Active, onopen_browse, TAG_DONE);
    SetAttrs(cb_sopendir, MUIA_Selected, pf_open_after, TAG_DONE);
    SetAttrs(cb_sclose, MUIA_Selected, pf_close_after, TAG_DONE);
    SetAttrs(c_spackwhere, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, pf_pack_where, TAG_DONE);
    SetAttrs(s_spackdir, MUIA_String_Contents, (IPTR)pf_pack_dir, TAG_DONE);
    SetAttrs(c_sname, MUIA_Cycle_Active, pf_name_drawer ? 0 : 1, TAG_DONE);
    SetAttrs(c_sexwhere, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, pf_ex_where, TAG_DONE);
    SetAttrs(s_sexdir, MUIA_String_Contents, (IPTR)pf_ex_dir, TAG_DONE);
    SetAttrs(c_snewdir, MUIA_Cycle_Active, pf_ex_newdir ? 0 : 1, TAG_DONE);
    SetAttrs(c_sexists, MUIA_Cycle_Active, pf_ex_exists, TAG_DONE);
    settings_where_changed();
    open_win(wsettings, 1);
}

static int cycle_of(Object *o)
{
    IPTR v = 0;
    GetAttr(MUIA_Cycle_Active, o, &v);
    return (int)v;
}

/* keep: "Save" (survives a reboot) rather than "Use". */
static void settings_take(int keep)
{
    STRPTR s = NULL;
    IPTR v = 0;
    pf_start_open = cycle_of(c_sstart);
    onopen_browse = cycle_of(c_sopen) == 1;
    GetAttr(MUIA_Selected, cb_sopendir, &v);
    pf_open_after = v != 0;
    GetAttr(MUIA_Selected, cb_sclose, &v);
    pf_close_after = v != 0;
    pf_pack_where = cycle_of(c_spackwhere);
    GetAttr(MUIA_String_Contents, s_spackdir, (IPTR *)&s);
    if (s && *s)
        full_path((char *)s, pf_pack_dir, sizeof(pf_pack_dir));
    else
        pf_pack_dir[0] = 0;
    pf_name_drawer = cycle_of(c_sname) == 0;
    pf_ex_where = cycle_of(c_sexwhere);
    GetAttr(MUIA_String_Contents, s_sexdir, (IPTR *)&s);
    if (s && *s)
        full_path((char *)s, pf_ex_dir, sizeof(pf_ex_dir));
    else
        pf_ex_dir[0] = 0;
    pf_ex_newdir = cycle_of(c_snewdir) == 0;
    pf_ex_exists = cycle_of(c_sexists);
    /* "In this drawer" with no drawer named means next to the items. */
    if (pf_pack_where == 2 && !pf_pack_dir[0])
        pf_pack_where = 0;
    if (pf_ex_where == 2 && !pf_ex_dir[0])
        pf_ex_where = 0;
    prefs_store(keep);
    start_show_where();
    open_win(wsettings, 0);
}

/* After a result: what the settings ask for, only when there is nothing to
 * report. Never for warnings or errors, and not in the middle of a queue. */
static void result_followups(void)
{
    if (!r_clean || job || nk || nq > 1)
        return;
    if (pf_open_after && r_root[0])
        open_folder(r_root);
    if (pf_close_after)
        open_win(wprog, 0);
}

static void show_details(void)
{
    SetAttrs(f_detail, MUIA_Floattext_Text, (IPTR)(r_detail ? r_detail : "No details."), TAG_DONE);
    open_win(wdetail, 1);
}

static int b_newfolder(void)
{
    IPTR v = 1;
    GetAttr(MUIA_Selected, cb_newfolder, &v);
    return v != 0;
}

/* The place shown under the list follows "To" and "New folder"; planning
 * again also refreshes the names on disk (they depend on the folder rule). */
static void b_replan(void)
{
    STRPTR to = NULL;
    char dest[UA_PATHMAX], err[256];
    if (!b_loaded || job || b_status != UA_OPEN_OK)
        return;
    GetAttr(MUIA_String_Contents, s_to, (IPTR *)&to);
    full_path(to && *to ? (char *)to : bplan.dest, dest, sizeof(dest));
    if (ua_plan_make(&blist, b_planname, dest, !b_newfolder(), &bplan, err, sizeof(err)))
        settext(t_active, "Cannot extract there: %s", err);
    else
        settext(t_active, "Will extract to %s%s", bplan.root,
                bplan.kind == UA_ROOT_DEST ? "" : bplan.kind == UA_ROOT_TOPDIR ?
                " (the archive's own folder)" : " (a new folder)");
    DoMethod(lv, MUIM_NList_Redraw, MUIV_NList_Redraw_All);
    b_note_selection();
}

/* Encrypted entries among those about to be extracted: a password first.
 * 0 when the user gave none (nothing is started). */
/* Entries about to be written that need no password. */
static ULONG plain_wanted(const ua_list *l)
{
    ULONG i, n = 0;
    for (i = 0; i < l->n; i++)
        if (!l->v[i].encrypted && !l->v[i].excluded && !l->v[i].skip && l->v[i].target &&
            l->v[i].type != UA_ET_DIR)
            n++;
    return n;
}

static int b_password_ready(void)
{
    int a;
    ULONG k;
    if (!ua_list_encrypted_wanted(&blist) || pw_of(b_archive))
        return 1;
    a = ask_password(b_archive, 0, plain_wanted(&blist) > 0);
    if (a != 2)
        return a;
    for (k = 0; k < blist.n; k++)
        if (blist.v[k].encrypted)
            blist.v[k].excluded = 1;
    return 1;
}

/* Browser: extract the selection or everything, to the folder in "To". */
static void b_start_extract(int all)
{
    STRPTR to = NULL;
    char dest[UA_PATHMAX], err[256];
    ULONG k, n;
    if (!b_loaded || job)
        return;
    GetAttr(MUIA_String_Contents, s_to, (IPTR *)&to);
    full_path(to && *to ? (char *)to : bplan.dest, dest, sizeof(dest));
    if (!writable(dest)) {
        char other[UA_PATHMAX];
        if (!ask_drawer("That drawer cannot be written. Extract to:", "RAM:", other, sizeof(other)))
            return;
        snprintf(dest, sizeof(dest), "%s", other);
        SetAttrs(s_to, MUIA_String_Contents, (IPTR)dest, TAG_DONE);
    }
    /* Plan again: the destination may have changed, and a folder name that
     * was free when the list was read may not be now. */
    if (ua_plan_make(&blist, b_planname, dest, !b_newfolder(), &bplan, err, sizeof(err))) {
        say("Cannot extract there:\n%s", err);
        return;
    }
    if (all) {
        for (k = 0; k < blist.n; k++)
            blist.v[k].excluded = 0;
    } else {
        b_sync_selection();
        n = b_mark_selection();
        if (!n) {
            say("Nothing selected can be extracted.");
            return;
        }
        ua_plan_unique(&blist, &bplan);   /* counts the collisions of what is extracted */
    }
    if (!b_password_ready())
        return;
    start_extract(b_archive, dest, 0, 1);
}

/* Rows dropped on a Wanderer drawer: the selected entries go into that
 * drawer without the folders that all of them share. One file comes out
 * alone; a selected folder keeps its name and brings its contents; files from
 * different folders keep the folders that tell them apart, and everything
 * selected keeps its tree. Stripping each entry's own folders instead would
 * flatten a tree and turn a/x.txt and b/x.txt into x.txt and "x 2.txt".
 * Same rules as any extraction: conflicts are asked, nothing is overwritten
 * unasked. */
static void b_drag_extract(void)
{
    char err[256];
    ULONG n;

    if (!drag_target[0]) {
        settext(t_active, "Nothing extracted: %s.\nDrop on an open drawer window to extract "
                "the selected entries there.", drag_why[0] ? drag_why : "no drawer there");
        dbg("drag out: no target (%s)", drag_why);
        return;
    }
    if (!b_loaded || job || b_status != UA_OPEN_OK) {
        settext(t_active, "Nothing extracted: the archive is not ready.");
        return;
    }
    if (!writable(drag_target)) {
        settext(t_active, "Nothing extracted: \"%s\" cannot be written.", drag_target);
        return;
    }
    if (ua_plan_make(&blist, b_planname, drag_target, 1, &bplan, err, sizeof(err))) {
        say("Cannot extract there:\n%s", err);
        return;
    }
    b_sync_selection();
    n = b_mark_selection();
    if (!n) {
        settext(t_active, "Nothing extracted: no entry selected that can be extracted.");
        b_replan();
        return;
    }
    if (ua_plan_strip_common(&blist) < 0) {
        say("Not enough memory.");
        b_replan();
        return;
    }
    if (ua_plan_unique(&blist, &bplan)) {
        say("Not enough memory.");
        b_replan();
        return;
    }
    if (!b_password_ready()) {
        b_replan();
        return;
    }
    settext(t_active, "");
    dbg("drag out: %lu entries to %s", (unsigned long)n, drag_target);
    start_extract(b_archive, drag_target, 0, 1);
}

static void quit_check(int *quit)
{
    if (!job && !nk && !is_open(wstart) && !is_open(wbrowse) && !is_open(wprog) &&
        !is_open(wsettings))
        *quit = 1;
}

static void open_folder(const char *path)
{
    if (!OpenWorkbenchObject((STRPTR)path, TAG_DONE))
        say("Could not open the folder\n%s", path);
}

/* ------------------------------------------------------------- start-up */

#define MAXARGS 64
static char *args[MAXARGS];
static int nargs, mode;    /* mode: 0 = preference, 1 = browse, 2 = extract */

static void mode_from_icon(const char *path)
{
    struct DiskObject *dob = GetDiskObject((STRPTR)path);
    if (dob) {
        STRPTR v = FindToolType((CONST STRPTR *)dob->do_ToolTypes, "MODE");
        if (v) {
            if (MatchToolValue(v, "BROWSE"))
                mode = 1;
            else if (MatchToolValue(v, "EXTRACT"))
                mode = 2;
        }
        FreeDiskObject(dob);
    }
}

static void read_wb_args(struct WBStartup *wbs)
{
    LONG i;
    for (i = 0; i < wbs->sm_NumArgs; i++) {
        char p[UA_PATHMAX];
        struct WBArg *wa = &wbs->sm_ArgList[i];
        if (!lockpath(wa->wa_Lock, (char *)wa->wa_Name, p, sizeof(p)))
            continue;
        mode_from_icon(p);
        /* The program's own icon, and project icons that only carry a MODE
         * (a "Browse" icon), are not archives. */
        if (i > 0 && nargs < MAXARGS && is_archive(p))
            args[nargs++] = strdup(p);
    }
}

static int read_shell_args(void)
{
    IPTR a[4] = { 0, 0, 0, 0 };
    struct RDArgs *rda = ReadArgs("FILES/M,BROWSE/S,EXTRACT/S,TESTQUIT/S", a, NULL);
    if (!rda) {
        PrintFault(IoErr(), APPNAME);
        return 0;
    }
    if (a[0]) {
        STRPTR *f = (STRPTR *)a[0];
        for (; *f && nargs < MAXARGS; f++) {
            char p[UA_PATHMAX];
            BPTR l = Lock(*f, SHARED_LOCK);
            if (l && NameFromLock(l, p, sizeof(p)))
                args[nargs++] = strdup(p);
            else
                args[nargs++] = strdup((char *)*f);
            if (l)
                UnLock(l);
        }
    }
    if (a[1])
        mode = 1;
    if (a[2])
        mode = 2;
    test_quit = a[3] != 0;
    FreeArgs(rda);
    return 1;
}

static int gui_main(int argc, char **argv)
{
    ULONG sigs = 0;
    int quit = 0, i;

    if (argc == 0) {
        read_wb_args((struct WBStartup *)argv);
    } else if (!read_shell_args()) {
        return 20;
    }
    if (GetVar("UnarcDebug", dbg_path, sizeof(dbg_path), 0) <= 0)
        dbg_path[0] = 0;
    {
        char v[16];
        if (GetVar("UnarcTestDelay", v, sizeof(v), 0) > 0)
            test_delay = atoi(v);
    }
    dbg("start sigalloc=%08lx avail=%lu args=%d mode=%d", (unsigned long)FindTask(NULL)->tc_SigAlloc,
        (unsigned long)AvailMem(MEMF_ANY), nargs, mode);
    prefs_load();
    jport = CreateMsgPort();
    if (!jport || !build()) {
        if (jport)
            DeleteMsgPort(jport);
        if (argc)
            printf("Arca: cannot create the application (is MUI/Zune installed?)\n");
        return 20;
    }
    notify();

    if (nargs == 0) {
        open_win(wstart, 1);
        if (pf_start_open) {
            char p[UA_PATHMAX];
            if (ask_archive("Open archive", p, sizeof(p)))
                browse(p);
        }
    } else {
        int browse_it = mode == 1 || (mode == 0 && onopen_browse);
        if (nargs > 1 && browse_it)
            say("%d archives were given. The browser shows one archive at a time;\n"
                "only \"%s\" is shown.", nargs, leafname(args[0]));
        if (browse_it)
            browse(args[0]);
        else {
            int k, added = 0;
            for (k = 0; k < nargs; k++) {
                if (!is_archive(args[k]))
                    say("\"%s\" is not an archive Arca can read.", args[k]);
                else
                    added += queue_add(args[k]);
            }
            if (added)
                queue_next();
            else
                open_win(wstart, 1);
        }
        quit_check(&quit);
    }

    while (!quit) {
        IPTR id = DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
        switch (id) {
        case MUIV_Application_ReturnID_Quit:
            quitting = 1;
            break;
        case ID_DROP:
            dbg("drop %d items, first %s", ndropped, ndropped ? dropped[0] : "-");
            if (job && qcur < 0)
                start_note("Busy", "Drop again when this archive is done.");
            else
                handle_drop();
            DoMethod(lq, MUIM_List_Redraw, MUIV_List_Redraw_All);
            if (qcur >= 0 && nq > 1) {
                SetAttrs(lq, MUIA_ShowMe, TRUE, TAG_DONE);
                SetAttrs(g_qbtns, MUIA_ShowMe, TRUE, TAG_DONE);
                settext(t_ptitle, "Extracting \33b%s\33n (%d of %d)", leafname(r_archive),
                        qcur + 1, nq);
            }
            drops_clear();
            break;
        case ID_OPEN: {
            char p[UA_PATHMAX];
            if (ask_archive("Open archive", p, sizeof(p)))
                browse(p);
        } break;
        case ID_START_CLOSE:
            open_win(wstart, 0);
            break;
        case ID_B_FIND:
            b_refilter();
            break;
        case ID_B_REPLAN:
            b_replan();
            break;
        case ID_B_DRAGOUT:
            b_drag_extract();
            break;
        case ID_B_SEL:
            b_note_selection();
            break;
        case ID_B_ACTIVE:
            b_show_active();
            break;
        case ID_B_EXSEL:
            b_start_extract(0);
            break;
        case ID_B_EXALL:
            b_start_extract(1);
            break;
        case ID_B_RECOVER:
            recover(b_archive);
            break;
        case ID_B_INFO:
            show_details();
            break;
        case ID_B_CLOSE:
            if (job && job == b_job)
                cancel_job();      /* the list still arrives with DONE */
            open_win(wbrowse, 0);
            break;
        case ID_P_CANCEL:
            cancel_job();
            break;
        case ID_P_CLOSE:
            if (job && job->kind != UJ_LIST) {
                /* Closing while it runs: cancel, clean up, then close; the
                 * archives still waiting are not started. */
                prog_closing = 1;
                qstop = 1;
                cancel_job();
            } else {
                open_win(wprog, 0);
            }
            break;
        case ID_P_OPENDIR: {
            struct qitem *it = queue_selected();
            const char *root = it ? it->root : r_root;
            if (root[0])
                open_folder(root);
        } break;
        case ID_P_DETAILS: {
            struct qitem *it = queue_selected();
            if (it && !job) {
                SetAttrs(f_detail, MUIA_Floattext_Text,
                         (IPTR)(it->detail ? it->detail : "No details."), TAG_DONE);
                open_win(wdetail, 1);
            } else
                show_details();
        } break;
        case ID_Q_ACTIVE: {
            struct qitem *it = queue_selected();
            if (!job)
                SetAttrs(b_popen, MUIA_Disabled, !it || !it->root[0] || !it->ok, TAG_DONE);
        } break;
        case ID_Q_REMOVE: {
            struct qitem *it = queue_selected();
            if (it && it->state == Q_WAIT) {
                it->state = Q_REMOVED;
                DoMethod(lq, MUIM_List_Redraw, MUIV_List_Redraw_All);
            }
        } break;
        case ID_Q_CANCELALL:
            qstop = 1;
            cancel_job();
            break;
        case ID_P_RECOVER:
            recover(r_archive);
            break;
        case ID_A_REPLACE: answer(UA_CONFLICT_OVERWRITE); break;
        case ID_A_KEEP:    answer(UA_CONFLICT_RENAME); break;
        case ID_A_SKIP:    answer(UA_CONFLICT_SKIP); break;
        case ID_A_CANCEL:  cancel_job(); break;
        case ID_D_CLOSE:   open_win(wdetail, 0); break;
        case ID_PACK_FILES:  pack_pick(); break;
        case ID_PACKFMT:
            packfmt_pref = cur_fmt();
            start_format_changed();
            prefs_save();
            break;
        case ID_LEVEL: {
            IPTR v = 0;
            int f = cur_fmt();
            GetAttr(MUIA_Numeric_Value, sl_level, &v);
            if ((int)v < lvl_count(f) && (int)v != pf_level[f]) {
                pf_level[f] = (int)v;
                start_show_level();
                prefs_save();
            }
        } break;
        case ID_SEPARATE: {
            IPTR v = 0;
            GetAttr(MUIA_Selected, cb_separate, &v);
            pf_separate = v != 0;
            prefs_save();
        } break;
        case ID_SETTINGS:    settings_show(); break;
        case ID_S_WHERE:     settings_where_changed(); break;
        case ID_S_SAVE:      settings_take(1); break;
        case ID_S_USE:       settings_take(0); break;
        case ID_S_CANCEL:    open_win(wsettings, 0); break;
        default: break;
        }
        if (quitting) {
            if (job)
                cancel_job();
            else
                quit = 1;
        }
        /* Wait only when Zune has nothing queued: it returns one ID per call
         * and asks for no wait (sigs 0) only in MUI, not in Zune, so after an
         * ID go round once more without waiting. The signals that arrived go
         * back to NewInput as they are: Zune ignores bits that are not its
         * own, and a 0 would make it service every source as if all had
         * fired (that lost memory in another program; see the Dockbar
         * finding in AROS/CLAUDE.md). */
        if (!quit && sigs && !id) {
            ULONG got = Wait(sigs | (1UL << jport->mp_SigBit) | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C)
                quitting = 1;
            sigs = got & ~(1UL << jport->mp_SigBit);
            if (!sigs)
                sigs = 1UL << jport->mp_SigBit;  /* not Zune's: NewInput does nothing */
        } else if (id) {
            sigs = SetSignal(0, sigs) & sigs;   /* take what arrived meanwhile */
            if (!sigs)
                sigs = 1UL << jport->mp_SigBit;
        }
        poll_jobs();
        if (test_quit && jobs_done && !job)
            quitting = 1;
        if (quitting && !job)
            quit = 1;
        if (!quit)
            quit_check(&quit);
    }

    /* A job still running here would be a bug in the loop above; never
     * unload code a worker runs. */
    while (job) {
        cancel_job();
        WaitPort(jport);
        poll_jobs();
    }
    MUI_DisposeObject(app);
    zk_drop_exit();
    zk_icon_exit();
    zk_steps_exit();
    zk_table_exit();
    DeleteMsgPort(jport);
    b_reset();
    free(r_detail);
    for (i = 0; i < nq; i++)
        free(q[i].detail);
    drops_clear();
    while (kcur < nk)
        kreq_free(&kq[kcur++]);
    for (i = 0; i < nargs; i++)
        free(args[i]);
    xad_static_close();
    dbg("exit sigalloc=%08lx", (unsigned long)FindTask(NULL)->tc_SigAlloc);
    return 0;
}

/* ------------------------------------------------------------- stack */

static int s_argc;
static char **s_argv;

static LONG trampoline(void)
{
    return gui_main(s_argc, s_argv);
}

int main(int argc, char **argv)
{
    struct Task *me = FindTask(NULL);
    IPTR have = (IPTR)me->tc_SPUpper - (IPTR)me->tc_SPLower;
    const IPTR want = 128 * 1024;
    s_argc = argc;
    s_argv = argv;
    ua_7z_init();
    ua_lha_init();
    /* Started from an icon with a small stack, or from a 40 KB Shell. */
    if (have < want) {
        struct StackSwapStruct sss;
        struct StackSwapArgs ssa;
        APTR stk = AllocVec(want, MEMF_ANY);
        LONG rc;
        if (!stk)
            return 20;
        sss.stk_Lower = stk;
        sss.stk_Upper = (APTR)((IPTR)stk + want);
        sss.stk_Pointer = sss.stk_Upper;
        memset(&ssa, 0, sizeof(ssa));
        rc = NewStackSwap(&sss, (LONG_FUNC)trampoline, &ssa);
        FreeVec(stk);
        memset(pw, 0, sizeof(pw));
        return rc;
    }
    {
        int rc = gui_main(argc, argv);
        memset(pw, 0, sizeof(pw));
        return rc;
    }
}
