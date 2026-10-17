/* SPDX-License-Identifier: MIT */
/* Unarc, the Zune front end. Three windows over the core the command line
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
#include "job.h"
#include "../engines/xad_static.h"

#define APPNAME "Unarc"
const char version[] = "$VER: Unarc 0.0.3 (17.10.2026)";

/* From 0x100: Zune's MUI_Request runs the application's input loop and takes
 * return IDs 1..buttons+1 as its own answer (mui_request.c). */
enum { ID_OPEN = 0x100, ID_EXTRACT_ASK, ID_ONOPEN, ID_START_CLOSE, ID_DROP,
       ID_B_FIND, ID_B_SEL, ID_B_ACTIVE, ID_B_EXSEL, ID_B_EXALL, ID_B_INFO, ID_B_CLOSE,
       ID_B_RECOVER, ID_P_CANCEL, ID_P_OPENDIR, ID_P_DETAILS, ID_P_CLOSE, ID_P_RECOVER,
       ID_A_REPLACE, ID_A_KEEP, ID_A_SKIP, ID_A_CANCEL, ID_D_CLOSE,
       ID_PACK_FILES, ID_PACK_DRAWER, ID_K_FMT, ID_K_GO, ID_K_CANCEL, ID_PACKFMT, ID_B_REPLAN,
       ID_B_DRAGOUT, ID_Q_REMOVE, ID_Q_CANCELALL, ID_Q_ACTIVE };

static Object *app, *wstart, *wbrowse, *wprog, *wask, *wdetail;
static Object *t_drop, *t_startnote, *c_onopen, *b_open, *b_extract, *c_packfmt, *b_pack,
              *b_packdir;
static Object *wpack, *t_kitems, *t_knote, *s_kname, *c_kfmt, *c_klevel, *c_klevel3, *b_kgo,
              *b_kcancel;
static Object *cb_newfolder;
static Object *s_find, *lv, *t_active, *t_bstatus, *s_to, *p_to, *b_exsel, *b_exall,
              *b_info, *b_bclose, *b_brecover;
static Object *t_ptitle, *t_pto, *t_pname, *g_gauge, *t_pcount, *t_presult,
              *b_pcancel, *b_popen, *b_pdetails, *b_pclose, *b_precover;
static Object *lq, *g_qbtns, *b_qremove, *b_qcancelall;
static Object *t_askq, *t_askinfo, *cb_all, *b_arep, *b_akeep, *b_askip, *b_acancel;
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
/* Appending rows below what is on screen: Zune's MUIM_List_Insert always asks
 * for a full redraw, which first paints the whole list background
 * (list.c, UPDATEMODE_ALL) and showed as a white flash for every batch of
 * entries. While rows are only appended and no column gets wider, the rows on
 * screen are drawn again exactly as they were, so that background pass is
 * skipped; a batch that widens a column is drawn the normal way. */
static int append_noclear;
static WORD colmax[5];
static char coltitle[5][200];             /* titles, padded to the widest text */
static struct RastPort colrp;
static int colrp_ready;
static uj_job *b_job;                     /* LIST job of the browser */

/* The last extraction, for the result window and its buttons. */
static char r_root[UA_PATHMAX];
static char r_archive[UA_PATHMAX];
static char *r_detail;
static char r_head[200];                  /* the last result's headline */
static int r_ok;                          /* the last extraction did all it should */
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
static int packfmt_pref;                  /* preference: 0 ZIP, 1 tar.gz, 2 7z */

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
        snprintf(out, n, "%s", fr->fr_Drawer);
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
        snprintf(out, n, "%s", fr->fr_Drawer);
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
static int ask_password(const char *archive, int wrong)
{
    Object *w, *str, *ok, *cancel, *sleepers[3];
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
                Child, (IPTR)HSpace(0),
                Child, (IPTR)(cancel = SimpleButton("_Cancel")),
            End),
        End),
    End;
    if (!w)
        return 0;
    DoMethod(app, OM_ADDMEMBER, (IPTR)w);
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
        if (id == ID_PW_OK || id == ID_PW_CANCEL || id == (ULONG)MUIV_Application_ReturnID_Quit)
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
    dbg("password asked: %s", got ? "given" : "not given");
    return got;
}

/* ------------------------------------------------------------- preferences */

#define PREFS_NAME "Unarc.prefs"

static void prefs_load(void)
{
    char buf[64];
    if (GetVar(PREFS_NAME, buf, sizeof(buf), GVF_GLOBAL_ONLY) > 0) {
        onopen_browse = strstr(buf, "ONOPEN=BROWSE") != NULL;
        packfmt_pref = strstr(buf, "PACK=TGZ") ? 1 : strstr(buf, "PACK=7Z") ? 2 : 0;
    }
}

/* ENV: for now, ENVARC: so that it survives a reboot. */
static void prefs_save(void)
{
    char v[64];
    snprintf(v, sizeof(v), "%s\nPACK=%s\n", onopen_browse ? "ONOPEN=BROWSE" : "ONOPEN=EXTRACT",
             packfmt_pref == 1 ? "TGZ" : packfmt_pref == 2 ? "7Z" : "ZIP");
    SetVar(PREFS_NAME, v, strlen(v), GVF_GLOBAL_ONLY | GVF_SAVE_VAR);
}

/* ------------------------------------------------------------- browser */

static void b_note_selection(void);

/* The five column strings of an entry (static buffers, valid until the next
 * call); NULL entry: the titles. */
static void make_cols(ua_entry *e, char **col)
{
    static char name[UA_PATHMAX], size[32], date[32], prot[12], note[64];
    if (!e) {
        static const char *const t[5] = { "Name", "Size", "Date", "Protection", "Note" };
        int c;
        for (c = 0; c < 5; c++) {
            if (!coltitle[c][0])
                snprintf(coltitle[c], sizeof(coltitle[c]), "\33b%s", t[c]);
            col[c] = coltitle[c];
        }
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
        prot[0] = 0;
        if (e->prot_valid) {
            ULONG p = e->prot;
            snprintf(prot, sizeof(prot), "%c%c%c%c%c%c%c%c",
                     p & (1UL << 7) ? 'h' : '-', p & FIBF_SCRIPT ? 's' : '-',
                     p & FIBF_PURE ? 'p' : '-', p & FIBF_ARCHIVE ? 'a' : '-',
                     p & FIBF_READ ? '-' : 'r', p & FIBF_WRITE ? '-' : 'w',
                     p & FIBF_EXECUTE ? '-' : 'e', p & FIBF_DELETE ? '-' : 'd');
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
        col[3] = prot;
        col[4] = note;
    }
}

AROS_UFH3S(void, display_func,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(char **, col, A2),
    AROS_UFHA(APTR, ent, A1))
{
    AROS_USERFUNC_INIT
    (void)hook;
    make_cols(ent ? &blist.v[(IPTR)ent - 1] : NULL, col);
    AROS_USERFUNC_EXIT
}
static struct Hook display_hook;

/* Copy the list's selection into b_sel for the rows on screen. Rows hidden by
 * the filter keep what they had. */
static void b_sync_selection(void)
{
    LONG pos = MUIV_List_NextSelected_Start;
    ULONG r;
    for (r = 0; r < b_nrows; r++)
        b_sel[b_rows[r]] = 0;
    for (;;) {
        DoMethod(lv, MUIM_List_NextSelected, (IPTR)&pos);
        if (pos == MUIV_List_NextSelected_End)
            break;
        if ((ULONG)pos < b_nrows)
            b_sel[b_rows[pos]] = 1;
    }
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
            DoMethod(lv, MUIM_List_Insert, (IPTR)buf, nb, MUIV_List_Insert_Bottom);
            nb = 0;
        }
    }
    if (nb)
        DoMethod(lv, MUIM_List_Insert, (IPTR)buf, nb, MUIV_List_Insert_Bottom);
}

static void b_refilter(void)
{
    ULONG r;
    STRPTR s = NULL;
    b_sync_selection();
    GetAttr(MUIA_String_Contents, s_find, (IPTR *)&s);
    snprintf(b_filter, sizeof(b_filter), "%s", s ? (char *)s : "");
    SetAttrs(lv, MUIA_List_Quiet, TRUE, TAG_DONE);
    DoMethod(lv, MUIM_List_Clear);
    b_nrows = 0;
    b_add(0, b_count);
    for (r = 0; r < b_nrows; r++)
        if (b_sel[b_rows[r]])
            DoMethod(lv, MUIM_List_Select, r, MUIV_List_Select_On, NULL);
    SetAttrs(lv, MUIA_List_Quiet, FALSE, TAG_DONE);
    /* Zune left rows of the previous contents on screen after a Clear and
     * Insert inside Quiet (AROS One 1.3); draw everything again. */
    DoMethod(lv, MUIM_List_Redraw, MUIV_List_Redraw_All);
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
    IPTR ent = 0;
    ua_entry *e;
    char *shown;
    char on[UA_PATHMAX + 64] = "";
    DoMethod(lv, MUIM_List_GetEntry, MUIV_List_GetEntry_Active, (IPTR)&ent);
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
    settext(t_active, "%s\n%s%s%s", on, e->comment && *e->comment ? "Comment: " : "",
            e->comment && *e->comment ? e->comment : "", shown ? "" : "");
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
static void b_replan(void);
static void start_extract(const char *archive, const char *dest, int recover, int from_browser);

static void browse(const char *archive)
{
    if (job) {
        say("Unarc is busy with another archive. Wait for it to finish, or cancel it.");
        return;
    }
    /* Rows first: the display hook reads blist. */
    DoMethod(lv, MUIM_List_Clear);
    b_reset();
    memset(colmax, 0, sizeof(colmax));
    memset(coltitle, 0, sizeof(coltitle));
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

static void prog_reset(const char *archive, const char *dest)
{
    settext(t_ptitle, "Extracting \33b%s", leafname(archive));
    settext(t_pto, "To: %s", dest);
    settext(t_pname, "");
    settext(t_pcount, "");
    settext(t_presult, "");
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
        say("Unarc is busy with another archive. Wait for it to finish, or cancel it.");
        return;
    }
    j = uj_new(UJ_EXTRACT, archive, jport);
    if (!j) {
        say("Not enough memory.");
        return;
    }
    snprintf(j->dest, sizeof(j->dest), "%s", dest);
    j->recover = recover;
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

/* Add an archive; a finished queue is cleared first. 0 when full. */
static int queue_add(const char *path)
{
    if (!queue_busy() && !job) {
        int i;
        for (i = 0; i < nq; i++)
            free(q[i].detail);
        nq = 0;
        qcur = -1;
        qstop = 0;
        prog_closing = 0;
        DoMethod(lq, MUIM_List_Clear);
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
        settext(t_presult, "%d extracted, %d with problems, %d cancelled or removed.\n"
                "Select an archive for its folder and details.", ok, bad, cancelled);
    else
        settext(t_presult, "All %d extracted.\nSelect an archive for its folder and details.", ok);
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

static void show_ask(uj_msg *m)
{
    BPTR l = Lock((STRPTR)m->path, SHARED_LOCK);
    char info[160] = "", incoming[160] = "", sz[32], ds[40];
    held_ask = m;
    if (l) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        if (fib && Examine(l, fib)) {
            struct DateTime dt;
            char d[16], t[16], sz[32];
            memset(&dt, 0, sizeof(dt));
            dt.dat_Stamp = fib->fib_Date;
            dt.dat_Format = FORMAT_DOS;
            dt.dat_StrDate = d;
            dt.dat_StrTime = t;
            DateToStr(&dt);
            sizestr(UA_FIB_SIZE(fib), sz, sizeof(sz));
            snprintf(info, sizeof(info), "Existing file: %s, %s %.5s", sz, d, t);
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
    snprintf(incoming, sizeof(incoming), "From the archive: %s, %s", sz, ds);
    settext(t_askq, "\33b%s\33n already exists.", m->path);
    settext(t_askinfo, "%s\n%s", info, incoming);
    SetAttrs(cb_all, MUIA_Selected, FALSE, TAG_DONE);
    open_win(wask, 1);
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

static void on_progress(uj_msg *m)
{
    uj_job *j = m->job;
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
        sizestr(j->total, total, sizeof(total));
        SetAttrs(g_gauge, MUIA_ShowMe, TRUE, MUIA_Gauge_Current, pct > 100 ? 100 : pct,
                 MUIA_Gauge_InfoText, (IPTR)"%ld %%", TAG_DONE);
        settext(t_pcount, "%s of %s, %lu files", done, total, (unsigned long)m->files);
    } else {
        /* Total size unknown (a stream that learns sizes as it goes): no
         * percentage rather than an invented one. */
        SetAttrs(g_gauge, MUIA_ShowMe, FALSE, TAG_DONE);
        settext(t_pcount, "%s, %lu files", done, (unsigned long)m->files);
    }
}

/* The worker reads the whole list before it sends the first rows. So the
 * widest text of each column can be found then, from a few candidates (the
 * longest names, the largest sizes, one entry of each fixed-width column,
 * the first entries with each kind of note), and the column titles padded to
 * it: Zune sizes a column by its widest row, the title row included. The
 * first rows are then drawn at the final widths and no later batch widens a
 * column, which would need a full redraw (an empty list for a moment). An
 * entry wider than every candidate still widens its column, drawn normally. */
static void presize_columns(void)
{
    enum { K = 12, KN = 24 };
    ULONG name_c[KN], size_c[K], other_c[16];
    size_t name_l[KN];
    UQUAD size_v[K];
    int nn = 0, ns = 0, no = 0, c, k;
    int have_date = 0, have_prot = 0;
    ULONG notes = 0, i;
    WORD tw, sw;
    for (i = 0; i < blist.n; i++) {
        const ua_entry *e = &blist.v[i];
        size_t l = e->name_src ? strlen(e->name_src) : 0;
        ULONG kind;
        /* the KN longest names */
        if (nn < KN || l > name_l[nn - 1]) {
            k = nn < KN ? nn++ : KN - 1;
            while (k > 0 && name_l[k - 1] < l) {
                name_l[k] = name_l[k - 1];
                name_c[k] = name_c[k - 1];
                k--;
            }
            name_l[k] = l;
            name_c[k] = i;
        }
        /* the K largest sizes */
        if (e->type != UA_ET_DIR && e->size_valid && (ns < K || e->size > size_v[ns - 1])) {
            k = ns < K ? ns++ : K - 1;
            while (k > 0 && size_v[k - 1] < e->size) {
                size_v[k] = size_v[k - 1];
                size_c[k] = size_c[k - 1];
                k--;
            }
            size_v[k] = e->size;
            size_c[k] = i;
        }
        kind = (e->map_flags & UA_MAP_UNSAFE ? 1 : e->encrypted ? 2 : e->skip ? 4 :
                e->map_flags ? 8 : 0) | (e->comment && *e->comment ? 16 : 0) |
               (!e->size_valid && e->type != UA_ET_DIR ? 32 : 0);
        if (((e->date_valid && !have_date) || (e->prot_valid && !have_prot) ||
             (kind && !(notes & (1UL << kind)))) && no < 16) {
            have_date |= e->date_valid;
            have_prot |= e->prot_valid;
            notes |= 1UL << kind;
            other_c[no++] = i;
        }
    }
    for (k = 0; k < nn + ns + no; k++) {
        ULONG idx = k < nn ? name_c[k] : k < nn + ns ? size_c[k - nn] : other_c[k - nn - ns];
        char *col[5];
        make_cols(&blist.v[idx], col);
        for (c = 0; c < 5; c++) {
            WORD w = TextLength(&colrp, col[c], strlen(col[c]));
            if (w > colmax[c])
                colmax[c] = w;
        }
    }
    /* Pad each title with spaces to its column's width and a little more:
     * the candidates are chosen by length, and in a proportional font a name
     * of the same length can be a few pixels wider (digits differ). Entries
     * up to the padded width then fit the column; colmax becomes that width,
     * so that only an entry wider still counts as widening. Size is
     * right-aligned, so its padding goes in front. */
    sw = TextLength(&colrp, " ", 1);
    for (c = 0; c < 5 && sw > 0; c++) {
        static const char *const t[5] = { "Name", "Size", "Date", "Protection", "Note" };
        char pad[160];
        int np;
        if (colmax[c])
            colmax[c] += 4 * sw;
        tw = TextLength(&colrp, (STRPTR)t[c], strlen(t[c]));
        np = colmax[c] > tw ? (colmax[c] - tw) / sw + 1 : 0;
        if (np > (int)sizeof(pad) - 1)
            np = sizeof(pad) - 1;
        memset(pad, ' ', np);
        pad[np] = 0;
        if (c == 1)
            snprintf(coltitle[c], sizeof(coltitle[c]), "\33b%s%s", pad, t[c]);
        else
            snprintf(coltitle[c], sizeof(coltitle[c]), "\33b%s%s", t[c], pad);
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
    {
        /* Does this batch widen a column? Measured with the list's font. */
        ULONG k;
        int widened = 0, c;
        Object *inner = NULL;
        GetAttr(MUIA_Listview_List, lv, (IPTR *)&inner);
        if (!colrp_ready && inner && _font(inner)) {
            InitRastPort(&colrp);
            SetFont(&colrp, _font(inner));
            colrp_ready = 1;
        }
        int presized = colrp_ready && m->first == 0 && !b_filter[0];
        if (presized)
            presize_columns();
        for (k = m->first; colrp_ready && k < m->first + m->count; k++) {
            char *col[5];
            make_cols(&blist.v[k], col);
            for (c = 0; c < 5; c++) {
                WORD w = TextLength(&colrp, col[c], strlen(col[c]));
                if (w > colmax[c]) {
                    colmax[c] = w;
                    widened = 1;
                }
            }
        }
        /* The first batch is drawn normally; later ones only append. */
        append_noclear = colrp_ready && !widened && m->first > 0 && !b_filter[0];
        SetAttrs(lv, MUIA_List_Quiet, TRUE, TAG_DONE);
        if (presized)       /* the title row measured again: still the only row */
            DoMethod(lv, MUIM_List_Redraw, MUIV_List_Redraw_All);
        b_add(m->first, m->count);
        SetAttrs(lv, MUIA_List_Quiet, FALSE, TAG_DONE);
        append_noclear = 0;
    }
    b_count = m->first + m->count;
    settext(t_bstatus, "Reading the archive... %lu entries", (unsigned long)b_count);
}

static void detail_text(uj_job *j, const char *title)
{
    size_t n = (j->log ? j->loglen : 0) + strlen(j->archive) + strlen(j->err) + 512;
    char *s;
    free(r_detail);
    r_detail = s = malloc(n);
    if (!s)
        return;
    snprintf(s, n, "%s\n\nArchive: %s\nFormat: %s\nEngine: %s\n%s%s\n", title, j->archive,
             ua_format_name(j->fmt), j->list.engine ? j->list.engine->name : "none",
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
        DoMethod(lv, MUIM_List_Clear);
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
        if (ask_password(b_archive, wrong)) {
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
        detail_text(j, "Listing");
    } else {
        detail_text(j, "Listing failed");
        uj_take_list(j, &blist, &bplan);
        if (j->open_status == UA_OPEN_DAMAGED) {
            settext(t_bstatus, "The archive is damaged and nothing could be listed.");
            SetAttrs(b_brecover, MUIA_ShowMe, TRUE, TAG_DONE);
        } else if (j->open_status == UA_OPEN_NOENGINE)
            settext(t_bstatus, "This is not an archive Unarc can read (%s).", ua_format_name(j->fmt));
        else if (j->open_status == UA_OPEN_UNSUPPORTED)
            settext(t_bstatus, "Unarc cannot read this archive: %s. See Details.", blist.why);
        else
            settext(t_bstatus, "Cannot read the archive. %s", j->err[0] ? j->err : "See Details.");
    }
}

static void extract_done(uj_job *j)
{
    ua_report *r = &j->rep;
    char sz[32], fc[32], head[200], body[UA_PATHMAX + 600];
    int extracted_any;

    snprintf(r_root, sizeof(r_root), "%s", j->have_plan ? j->plan.root : "");
    r_damaged = j->open_status == UA_OPEN_DAMAGED;
    sizestr(r->bytes, sz, sizeof(sz));
    snprintf(fc, sizeof(fc), "%lu file%s", (unsigned long)r->files, r->files == 1 ? "" : "s");
    extracted_any = r->files || r->dirs;
    if (j->open_status == UA_OPEN_DAMAGED) {
        snprintf(head, sizeof(head), "The archive is damaged");
        snprintf(body, sizeof(body), "Nothing was written.\n\"Recover...\" extracts what can still "
                 "be read, into a new folder.");
    } else if (j->open_status == UA_OPEN_CANCELLED) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "Stopped while reading the archive. Nothing was written.");
    } else if (j->open_status == UA_OPEN_PASSWORD) {
        snprintf(head, sizeof(head), "Password needed");
        snprintf(body, sizeof(body), "The archive is encrypted and no password was given.\n"
                 "Nothing was written.");
    } else if (j->open_status == UA_OPEN_NOENGINE) {
        snprintf(head, sizeof(head), "Not an archive Unarc can read");
        snprintf(body, sizeof(body), "Format: %s. Nothing was written.", ua_format_name(j->fmt));
    } else if (j->open_status == UA_OPEN_UNSUPPORTED) {
        char why7[160];
        if (ua_7z_available(why7, sizeof(why7)))
            why7[0] = 0;
        else
            strncat(why7, "\n", sizeof(why7) - strlen(why7) - 1);
        snprintf(head, sizeof(head), "Not supported");
        snprintf(body, sizeof(body), "Unarc cannot read this archive: %s.\n%sNothing was written.",
                 j->list.why, why7);
    } else if (j->open_status == UA_OPEN_ERROR && !strncmp(j->err, "Incomplete multi-volume", 23)) {
        snprintf(head, sizeof(head), "Incomplete set");
        snprintf(body, sizeof(body), "%s\nNothing was written.", j->err);
    } else if (j->open_status == UA_OPEN_ERROR || (j->extract_rc < 0 && !extracted_any &&
                                                   !r->cancelled && !r->failed)) {
        snprintf(head, sizeof(head), "Failed");
        snprintf(body, sizeof(body), "%s\nNothing was written.", j->err[0] ? j->err : "See Details.");
    } else if (r->cancelled) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "%s (%s) were extracted to\n%s\nand kept; the file in "
                 "progress was removed.", fc, sz, r_root);
    } else if (r->failed) {
        snprintf(head, sizeof(head), "Stopped: %s", r->why);
        snprintf(body, sizeof(body), "%s (%s) were extracted to\n%s\nbefore it stopped; "
                 "nothing that was there before was lost.", fc, sz, r_root);
    } else if (r->errors) {
        snprintf(head, sizeof(head), "Partly done");
        snprintf(body, sizeof(body), "%s (%s) in\n%s\n%lu could not be written: see "
                 "Details.", fc, sz, r_root, (unsigned long)r->errors);
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
    } else {
        snprintf(head, sizeof(head), "Done");
        snprintf(body, sizeof(body), "%s (%s) in\n%s", fc, sz, r_root);
    }
    if (r->overwritten)
        snprintf(body + strlen(body), sizeof(body) - strlen(body), "\n%lu files were replaced.",
                 (unsigned long)r->overwritten);
    if ((r->errors || r->failed) && j->plan.with_password && pw_of(j->archive)) {
        memset(pw, 0, sizeof(pw));
        pw_for[0] = 0;
        snprintf(body + strlen(body), sizeof(body) - strlen(body),
                 "\nThe password may be wrong; it will be asked again.");
    }
    detail_text(j, head);
    snprintf(r_head, sizeof(r_head), "%s", head);
    r_ok = !strcmp(head, "Done") || !strcmp(head, "Done, with notes");
    dbg("job extract done: %s | files=%lu dirs=%lu skipped=%lu renamed=%lu replaced=%lu "
        "errors=%lu bytes=%llu cancelled=%d failed=%d open=%d root=%s", head,
        (unsigned long)r->files, (unsigned long)r->dirs, (unsigned long)r->skipped,
        (unsigned long)r->renamed, (unsigned long)r->overwritten, (unsigned long)r->errors,
        (unsigned long long)r->bytes, r->cancelled, r->failed, j->open_status, r_root);
    settext(t_ptitle, "\33b%s", head);
    settext(t_presult, "%s", body);
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
    dbg("done at %ld ticks; %lu batches, %lu progress messages", (long)uj_ticks_since(&j->started),
        (unsigned long)n_batches, (unsigned long)n_progress);
    n_batches = n_progress = 0;
    uj_wait_gone(j);
    job = NULL;
    jobs_done++;
    if (held_ask) {          /* cannot happen: the worker waited for it */
        held_ask = NULL;
        open_win(wask, 0);
    }
    if (j->kind == UJ_LIST) {
        b_job = NULL;
        list_done(j);
    } else if (j->kind == UJ_PACK) {
        pack_done(j);
        if (prog_closing) {
            open_win(wprog, 0);
            prog_closing = 0;
        }
    } else if (j->open_status == UA_OPEN_PASSWORD && !j->lent && !quitting &&
               ask_password(j->archive, j->list.pw_state == UA_PW_WRONG)) {
        /* Nothing was written; the same extraction again, with the password. */
        char a[UA_PATHMAX], d[UA_PATHMAX];
        int rec = j->recover;
        snprintf(a, sizeof(a), "%s", j->archive);
        snprintf(d, sizeof(d), "%s", j->dest);
        uj_free(j);
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

static char **k_items;         /* what the pack window offers to pack */
static int k_n;

static void k_clear(void)
{
    while (k_n)
        free(k_items[--k_n]);
    free(k_items);
    k_items = NULL;
}

static const char *pack_ext(int fmt)
{
    return fmt == 1 ? ".tar.gz" : fmt == 2 ? ".7z" : ".zip";
}

/* Replace the extension of the name in the window with the format's. */
static void k_fix_ext(void)
{
    STRPTR cur = NULL;
    IPTR f = 0;
    char name[UA_PATHMAX];
    size_t n;
    GetAttr(MUIA_String_Contents, s_kname, (IPTR *)&cur);
    GetAttr(MUIA_Cycle_Active, c_kfmt, &f);
    snprintf(name, sizeof(name), "%s", cur ? (char *)cur : "");
    n = strlen(name);
    if (n > 7 && !strcasecmp(name + n - 7, ".tar.gz"))
        name[n - 7] = 0;
    else if (n > 4 && (!strcasecmp(name + n - 4, ".zip") || !strcasecmp(name + n - 4, ".tgz")))
        name[n - 4] = 0;
    else if (n > 3 && !strcasecmp(name + n - 3, ".7z"))
        name[n - 3] = 0;
    strncat(name, pack_ext((int)f), sizeof(name) - strlen(name) - 1);
    SetAttrs(s_kname, MUIA_String_Contents, (IPTR)name, TAG_DONE);
    /* Settings the format has: gzip cannot store without compressing. */
    SetAttrs(c_klevel, MUIA_ShowMe, f != 1, TAG_DONE);
    SetAttrs(c_klevel3, MUIA_ShowMe, f == 1, TAG_DONE);
}

/* Show what would be packed and where; nothing happens before "Pack". The
 * items become the window's; archives among them are packed as files. */
static void pack_intent(char **items, int n)
{
    char name[UA_PATHMAX], list[400] = "";
    int i, archives = 0;
    if (job) {
        say("Unarc is busy with another job. Wait for it to finish, or cancel it.");
        for (i = 0; i < n; i++)
            free(items[i]);
        free(items);
        return;
    }
    k_clear();
    k_items = items;
    k_n = n;
    for (i = 0; i < n; i++) {
        if (is_archive(items[i]))
            archives++;
        if (i < 4)
            snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%s", i ? ", " : "",
                     leafname(items[i]));
    }
    if (n > 4)
        snprintf(list + strlen(list), sizeof(list) - strlen(list), " and %d more", n - 4);
    settext(t_kitems, "Pack %d item%s: %s", n, n == 1 ? "" : "s", list);
    settext(t_knote, "%s", archives ? "Archives among them are packed as they are, as files." : "");
    SetAttrs(t_knote, MUIA_ShowMe, archives != 0, TAG_DONE);
    /* Next to the first item, named after it (or "Archive" for several). */
    archive_drawer(items[0], name, sizeof(name));
    if (!name[0] || !writable(name))
        snprintf(name, sizeof(name), "RAM:");
    AddPart(name, n == 1 ? (STRPTR)leafname(items[0]) : (STRPTR)"Archive", sizeof(name));
    SetAttrs(c_kfmt, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, packfmt_pref, TAG_DONE);
    SetAttrs(s_kname, MUIA_String_Contents, (IPTR)name, TAG_DONE);
    k_fix_ext();
    open_win(wpack, 1);
}

static void pack_go(void)
{
    STRPTR cur = NULL;
    IPTR f = 0, lv = 0;
    char name[UA_PATHMAX];
    uj_job *j;
    BPTR x;
    int replace = 0;
    static const int levels[] = { -1, 1, 9, 0 };

    if (job) {
        say("Unarc is busy with another job. Wait for it to finish, or cancel it.");
        return;
    }
    GetAttr(MUIA_String_Contents, s_kname, (IPTR *)&cur);
    GetAttr(MUIA_Cycle_Active, c_kfmt, &f);
    GetAttr(MUIA_Cycle_Active, f == 1 ? c_klevel3 : c_klevel, &lv);
    snprintf(name, sizeof(name), "%s", cur ? (char *)cur : "");
    if (!name[0] || !k_n)
        return;
    if ((x = Lock(name, SHARED_LOCK))) {
        UnLock(x);
        if (is_dir(name)) {
            say("\"%s\" is a drawer. Give the archive a file name.", name);
            return;
        }
        if (MUI_Request(app, wpack, 0, APPNAME, "_Replace|_Cancel",
                        "\"%s\" already exists.\nReplace it with the new archive?", name) != 1)
            return;
        replace = 1;
    }
    j = uj_new(UJ_PACK, name, jport);
    if (!j) {
        say("Not enough memory.");
        return;
    }
    j->inputs = k_items;
    j->ninputs = k_n;
    k_items = NULL;
    k_n = 0;
    j->pack.fmt = f == 1 ? UA_PACK_TGZ : f == 2 ? UA_PACK_7Z : UA_PACK_ZIP;
    j->pack.level = levels[lv < 4 ? lv : 0];
    if (f == 1 && j->pack.level == 0)
        j->pack.level = 1;
    j->pack.replace = replace;
    if (uj_start(j) < 0) {
        uj_free(j);
        say("Could not start packing (not enough memory).");
        return;
    }
    job = j;
    dbg("job pack start %s fmt=%d level=%d items=%d", name, (int)j->pack.fmt, j->pack.level,
        j->ninputs);
    open_win(wpack, 0);
    snprintf(r_archive, sizeof(r_archive), "%s", name);
    r_from_browser = 0;
    prog_closing = 0;
    prog_reset(r_archive, "");
    settext(t_ptitle, "Packing \33b%s", leafname(r_archive));
    settext(t_pto, "Into: %s", r_archive);
}

static void pack_done(uj_job *j)
{
    ua_pack_report *r = &j->prep;
    char sz[32], out[32], fc[32], head[200], body[UA_PATHMAX + 400];
    sizestr(r->bytes_in, sz, sizeof(sz));
    sizestr(r->bytes_out, out, sizeof(out));
    snprintf(fc, sizeof(fc), "%lu file%s", (unsigned long)r->files, r->files == 1 ? "" : "s");
    archive_drawer(j->archive, r_root, sizeof(r_root));
    if (j->extract_rc < 0 && r->cancelled) {
        snprintf(head, sizeof(head), "Cancelled");
        snprintf(body, sizeof(body), "No archive was made%s.", r->replaced ? "" :
                 "; an archive that was there before is unchanged");
    } else if (j->extract_rc < 0) {
        snprintf(head, sizeof(head), "Not made: %s", r->why[0] ? r->why : j->err);
        snprintf(body, sizeof(body), "No archive was made; an archive that was there before "
                 "is unchanged.");
    } else if (r->errors) {
        snprintf(head, sizeof(head), "Made, with errors");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s; %lu could not be packed as "
                 "they were: see Details.", j->archive, fc, sz, out, (unsigned long)r->errors);
    } else if (r->skipped) {
        snprintf(head, sizeof(head), "Made, with notes");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s; %lu links were not followed.",
                 j->archive, fc, sz, out, (unsigned long)r->skipped);
    } else {
        snprintf(head, sizeof(head), "Made");
        snprintf(body, sizeof(body), "%s\n%s (%s) packed into %s.", j->archive, fc, sz, out);
    }
    if (j->extract_rc == 0 && r->replaced)
        strncat(body, "\nThe old archive was replaced.", sizeof(body) - strlen(body) - 1);
    detail_text(j, head);
    dbg("job pack done: %s | files=%lu dirs=%lu skipped=%lu errors=%lu in=%llu out=%llu "
        "cancelled=%d failed=%d rc=%d", head, (unsigned long)r->files, (unsigned long)r->dirs,
        (unsigned long)r->skipped, (unsigned long)r->errors, (unsigned long long)r->bytes_in,
        (unsigned long long)r->bytes_out, r->cancelled, r->failed, j->extract_rc);
    settext(t_ptitle, "\33b%s", head);
    settext(t_presult, "%s", body);
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
}

/* Requesters: the way to pack without dragging anything. */
static void pack_pick(int drawers)
{
    struct FileRequester *fr;
    char **items = NULL;
    int n = 0;
    fr = MUI_AllocAslRequestTags(ASL_FileRequest,
                                 ASLFR_TitleText, (IPTR)(drawers ? "Pack drawer" : "Pack files"),
                                 ASLFR_DoMultiSelect, !drawers, ASLFR_DrawersOnly, drawers,
                                 TAG_DONE);
    if (!fr)
        return;
    if (MUI_AslRequestTags(fr, TAG_DONE)) {
        LONG i, cnt = drawers ? 1 : (fr->fr_NumArgs ? fr->fr_NumArgs : (fr->fr_File[0] ? 1 : 0));
        items = calloc(cnt ? cnt : 1, sizeof(char *));
        for (i = 0; items && i < cnt; i++) {
            char p[UA_PATHMAX];
            snprintf(p, sizeof(p), "%s", fr->fr_Drawer);
            if (!drawers)
                AddPart(p, fr->fr_NumArgs ? (STRPTR)fr->fr_ArgList[i].wa_Name : fr->fr_File,
                        sizeof(p));
            if (p[0] && (items[n] = strdup(p)))
                n++;
        }
    }
    MUI_FreeAslRequest(fr);
    if (n)
        pack_intent(items, n);
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
        settext(t_startnote, "");
        for (i = 0; i < ndropped; i++)
            added += queue_add(dropped[i]);
        if (added < ndropped)
            settext(t_startnote, "Only %d archives can wait at a time; %d were not added.", QMAX,
                    ndropped - added);
        queue_next();
    } else if (job) {
        settext(t_startnote, "Busy; files to pack can be dropped when the current job is done.");
    } else {
        /* Files and drawers, or archives mixed with them: that is packing;
         * the pack window shows what and where before anything starts. */
        char **items = calloc(ndropped, sizeof(char *));
        int n = 0;
        settext(t_startnote, "");
        for (i = 0; items && i < ndropped; i++)
            if (dropped[i]) {
                items[n++] = dropped[i];
                dropped[i] = NULL;
            }
        if (n)
            pack_intent(items, n);
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

static const char *onopen_labels[] = { "Extract", "Show contents", NULL };
static const char *packfmt_labels[] = { "ZIP", "tar.gz", "7z", NULL };
static const char *level_labels[] = { "Normal", "Fast", "Best", "None (store only)", NULL };
static const char *level3_labels[] = { "Normal", "Fast", "Best", NULL };

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
 * extracted into that drawer. Zune drags only between windows of one
 * application, and when a drop lands elsewhere it tells the dragged object
 * with MUIM_UnknownDropDestination (window.c, HandleDragging), passing the
 * IntuiMessage of the button release. The window under the pointer is then
 * found here: it must belong to the Wanderer task, and Wanderer titles a
 * drawer window with the drawer's path (iconwindow.c), which must name an
 * existing drawer. workbench.library has no call that says which drawer is
 * open where (WBCTRLA_GetOpenDrawerList is not implemented on AROS). Any other
 * target, the desktop included, extracts nothing. */
static struct MUI_CustomClass *dragcls;
static char drag_target[UA_PATHMAX];
static char drag_why[160];

/* The window under the pointer belongs to another program and can close at
 * any moment, so nothing of it is used after the locks are let go: its
 * owner's name and its title are copied while they are held, and the drawer
 * is checked on disk afterwards. intuition frees a closing window while it
 * holds the screen's LayerInfo lock (closewindow.c, LOCK_REFRESH), and the
 * window's port goes after that, so holding the lock from WhichLayer to the
 * last read keeps both. The port and the title are read under Forbid as
 * well: Zune takes a window's shared port away under Forbid before closing
 * it, and SetWindowTitles takes no lock at all. */
static int wanderer_drawer_at(struct Window *from, WORD mx, WORD my, char *out, size_t n)
{
    struct Screen *scr = from->WScreen;
    struct Layer *layer;
    char owner[64] = "", title[UA_PATHMAX] = "";
    int found = 0, have_title = 0;
    BPTR l;
    int ok = 0;

    LockLayerInfo(&scr->LayerInfo);
    layer = WhichLayer(&scr->LayerInfo, from->LeftEdge + mx, from->TopEdge + my);
    Forbid();
    if (layer && layer->Window) {
        struct Window *w = layer->Window;
        struct MsgPort *port = w->UserPort;
        struct Task *t = port ? port->mp_SigTask : NULL;
        if (t && t->tc_Node.ln_Name) {
            snprintf(owner, sizeof(owner), "%s", t->tc_Node.ln_Name);
            found = 1;
        }
        if (w->Title) {
            snprintf(title, sizeof(title), "%s", (char *)w->Title);
            have_title = 1;
        }
    }
    Permit();
    UnlockLayerInfo(&scr->LayerInfo);

    if (!found) {
        snprintf(drag_why, sizeof(drag_why), "not dropped on a window");
        return 0;
    }
    if (!strstr(owner, "Wanderer")) {
        snprintf(drag_why, sizeof(drag_why), "dropped on a window of \"%s\"", owner);
        return 0;
    }
    if (!have_title || !strchr(title, ':')) {
        snprintf(drag_why, sizeof(drag_why), "dropped on Wanderer, but not on a drawer");
        return 0;
    }
    snprintf(out, n, "%s", title);
    l = Lock((STRPTR)out, SHARED_LOCK);
    if (l) {
        struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
        ok = fib && Examine(l, fib) && fib->fib_DirEntryType > 0;
        FreeDosObject(DOS_FIB, fib);
        UnLock(l);
    }
    if (!ok)
        snprintf(drag_why, sizeof(drag_why), "\"%s\" is not a drawer", out);
    return ok;
}

/* Zune does not start a drag from list entries: the list eats the mouse
 * events and treats a move with the button down as range selection (list.c,
 * List__MUIM_HandleEvent). So the browser list has an event handler of its
 * own, ahead of the list's: a press on an entry that is already selected
 * arms it; moving 6 pixels starts a drag-out (the list keeps its selection);
 * releasing outside the window looks for the drawer there. A press without
 * a move acts as a plain click on that entry. Everything else goes to the
 * list as before. */
#define RAWKEY_A 0x20

static Object *active_object(Object *win)
{
    IPTR o = 0;
    GetAttr(MUIA_Window_ActiveObject, win, &o);
    return (Object *)o;
}

struct dragdata {
    struct MUI_EventHandlerNode ehn;
    int armed, dragging, added;
    WORD x, y;
    LONG entry;
};

static void drag_events(Object *obj, struct dragdata *d, ULONG events)
{
    if (d->added)
        DoMethod(_win(obj), MUIM_Window_RemEventHandler, (IPTR)&d->ehn);
    d->ehn.ehn_Events = events;
    DoMethod(_win(obj), MUIM_Window_AddEventHandler, (IPTR)&d->ehn);
    d->added = 1;
}

static IPTR drag_handle(struct IClass *cl, Object *obj, struct MUIP_HandleEvent *msg)
{
    struct dragdata *d = INST_DATA(cl, obj);
    struct IntuiMessage *im = msg->imsg;
    if (!im)
        return 0;
    if (im->Class == IDCMP_MOUSEBUTTONS && im->Code == SELECTDOWN) {
        struct MUI_List_TestPos_Result pos;
        LONG state = 0;
        if (im->MouseX < _mleft(obj) || im->MouseX > _mright(obj) || im->MouseY < _mtop(obj) ||
            im->MouseY > _mbottom(obj) || !b_loaded)
            return 0;
        /* A click in the list moves the keyboard to it. Zune keeps a string
         * field as the window's active object after the click, and Amiga+A
         * below would then still be taken for that field's. */
        if (active_object(_win(obj)) != lv)
            SetAttrs(_win(obj), MUIA_Window_ActiveObject, (IPTR)lv, TAG_DONE);
        DoMethod(obj, MUIM_List_TestPos, im->MouseX, im->MouseY, (IPTR)&pos);
        if (pos.entry < 0)
            return 0;
        DoMethod(obj, MUIM_List_Select, pos.entry, MUIV_List_Select_Ask, (IPTR)&state);
        if (!state || (im->Qualifier & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)))
            return 0;                       /* the list's own click */
        d->armed = 1;
        d->dragging = 0;
        d->x = im->MouseX;
        d->y = im->MouseY;
        d->entry = pos.entry;
        drag_events(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_MOUSEMOVE | IDCMP_RAWKEY);
        return MUI_EventHandlerRC_Eat;
    }
    /* Amiga+A selects every entry: the list has no key for it, and dragging
     * many entries out needs it. Not while typing in the search or the
     * destination field: the key belongs to the text there. */
    if (im->Class == IDCMP_RAWKEY && im->Code == RAWKEY_A &&
        (im->Qualifier & (IEQUALIFIER_LCOMMAND | IEQUALIFIER_RCOMMAND)) && b_loaded &&
        active_object(_win(obj)) != s_find && active_object(_win(obj)) != s_to) {
        DoMethod(obj, MUIM_List_Select, MUIV_List_Select_All, MUIV_List_Select_On, NULL);
        DoMethod(app, MUIM_Application_ReturnID, ID_B_SEL);
        return MUI_EventHandlerRC_Eat;
    }
    if (!d->armed)
        return 0;
    if (im->Class == IDCMP_MOUSEMOVE) {
        if (!d->dragging && (abs(im->MouseX - d->x) >= 6 || abs(im->MouseY - d->y) >= 6)) {
            d->dragging = 1;
            settext(t_active, "Release over a drawer window to extract the selected entries "
                    "there.");
        }
        return MUI_EventHandlerRC_Eat;
    }
    if (im->Class == IDCMP_MOUSEBUTTONS && (im->Code == SELECTUP || im->Code == MENUDOWN)) {
        struct Window *w = im->IDCMPWindow;
        int outside = im->MouseX < 0 || im->MouseY < 0 || im->MouseX >= w->Width ||
                      im->MouseY >= w->Height;
        d->armed = 0;
        drag_events(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY);
        if (!d->dragging) {
            /* A plain click on a selected entry: that entry alone. */
            DoMethod(obj, MUIM_List_Select, MUIV_List_Select_All, MUIV_List_Select_Off, NULL);
            DoMethod(obj, MUIM_List_Select, d->entry, MUIV_List_Select_On, NULL);
            SetAttrs(obj, MUIA_List_Active, d->entry, TAG_DONE);
            return MUI_EventHandlerRC_Eat;
        }
        d->dragging = 0;
        drag_target[0] = 0;
        snprintf(drag_why, sizeof(drag_why), "released inside the browser");
        if (im->Code == SELECTUP && outside &&
            !wanderer_drawer_at(w, im->MouseX, im->MouseY, drag_target, sizeof(drag_target)))
            drag_target[0] = 0;
        if (im->Code == MENUDOWN)
            snprintf(drag_why, sizeof(drag_why), "the drag was cancelled");
        DoMethod(app, MUIM_Application_ReturnID, ID_B_DRAGOUT);
        return MUI_EventHandlerRC_Eat;
    }
    return 0;
}

BOOPSI_DISPATCHER(IPTR, drag_dispatcher, cl, obj, msg)
{
    struct dragdata *d;
    switch (msg->MethodID) {
    case MUIM_DrawBackground:
        if (append_noclear) {
            struct MUIP_DrawBackground *db = (struct MUIP_DrawBackground *)msg;
            if (db->width >= _mwidth(obj) && db->height >= _mheight(obj) - 2)
                return 0;
        }
        break;
    case MUIM_Setup: {
        IPTR r = DoSuperMethodA(cl, obj, msg);
        if (!r)
            return 0;
        d = INST_DATA(cl, obj);
        memset(d, 0, sizeof(*d));
        d->ehn.ehn_Priority = 1;          /* ahead of the list's own handler */
        d->ehn.ehn_Object = obj;
        d->ehn.ehn_Class = cl;
        drag_events(obj, d, IDCMP_MOUSEBUTTONS | IDCMP_RAWKEY);
        return r;
    }
    case MUIM_Cleanup:
        d = INST_DATA(cl, obj);
        if (d->added)
            DoMethod(_win(obj), MUIM_Window_RemEventHandler, (IPTR)&d->ehn);
        d->added = 0;
        break;
    case MUIM_HandleEvent: {
        IPTR r = drag_handle(cl, obj, (struct MUIP_HandleEvent *)msg);
        if (r)
            return r;
        break;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

static int build(void)
{
    dragcls = MUI_CreateCustomClass(NULL, MUIC_List, NULL, sizeof(struct dragdata), drag_dispatcher);
    if (!dragcls)
        return 0;
    display_hook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(display_func);
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
            MUIA_Window_AppWindow, TRUE,
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(t_drop = TextObject,
                    TextFrame,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_PreParse, (IPTR)"\33c",
                    MUIA_Text_Contents, (IPTR)"\nDrop an archive here to extract it,\nor files and drawers to pack them.\n\n",
                End),
                Child, (IPTR)(t_startnote = TextObject,
                    MUIA_Text_PreParse, (IPTR)"\33c",
                    MUIA_Text_Contents, (IPTR)"",
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_open = button("_Open archive...")),
                    Child, (IPTR)(b_extract = button("_Extract archive...")),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)Label1("Pack as"),
                    Child, (IPTR)(c_packfmt = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)packfmt_labels,
                        MUIA_CycleChain, 1,
                    End),
                    Child, (IPTR)(b_pack = button("_Pack files...")),
                    Child, (IPTR)(b_packdir = button("Pack _drawer...")),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)Label1("Double-clicked archives:"),
                    Child, (IPTR)(c_onopen = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)onopen_labels,
                        MUIA_CycleChain, 1,
                    End),
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
                Child, (IPTR)(lv = ListviewObject,
                    MUIA_CycleChain, 1,
                    MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_Default,
                    MUIA_Listview_List, (IPTR)(NewObject(dragcls->mcc_Class, NULL,
                        InputListFrame,
                        /* Zune sizes columns by their widest text and ignores MINWIDTH and
                         * WEIGHT (list.c, CalcDimsOfEntry); DELTA is the gap it does use. */
                        MUIA_List_Format, (IPTR)"D=8 BAR,D=12 P=\33r BAR,D=12 BAR,D=12 BAR,D=8",
                        MUIA_List_Title, TRUE,
                        MUIA_List_DisplayHook, (IPTR)&display_hook,
                    TAG_DONE)),
                End),
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
                Child, (IPTR)(t_presult = TextObject, MUIA_Text_SetMin, FALSE, End),
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
            MUIA_Window_ID, MAKE_ID('U','A','A','S'),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(t_askq = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(t_askinfo = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(cb_all = MUI_MakeObject(MUIO_Checkmark, (IPTR)"_Do the same for the rest")),
                    Child, (IPTR)LLabel1("_Do the same for the rest"),
                    Child, (IPTR)HSpace(0),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_arep = button("_Replace")),
                    Child, (IPTR)(b_akeep = button("_Keep both")),
                    Child, (IPTR)(b_askip = button("_Skip")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_acancel = button("_Cancel")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wpack = WindowObject,
            MUIA_Window_Title, (IPTR)"Pack",
            MUIA_Window_ID, MAKE_ID('U','A','P','K'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(40),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)(t_kitems = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(t_knote = TextObject, MUIA_Text_SetMin, FALSE, End),
                Child, (IPTR)(ColGroup(2),
                    Child, (IPTR)Label2("_Archive"),
                    Child, (IPTR)(PopaslObject,
                        MUIA_Popstring_String, (IPTR)(s_kname = StringObject,
                            StringFrame,
                            MUIA_String_MaxLen, UA_PATHMAX,
                            MUIA_ControlChar, 'a',
                            MUIA_CycleChain, 1,
                        End),
                        MUIA_Popstring_Button, (IPTR)PopButton(MUII_PopFile),
                        MUIA_Popasl_Type, ASL_FileRequest,
                        ASLFR_DoSaveMode, TRUE,
                        ASLFR_TitleText, (IPTR)"Archive name",
                    End),
                    Child, (IPTR)Label1("_Format"),
                    Child, (IPTR)(c_kfmt = CycleObject,
                        MUIA_Cycle_Entries, (IPTR)packfmt_labels,
                        MUIA_ControlChar, 'f',
                        MUIA_CycleChain, 1,
                    End),
                    Child, (IPTR)Label1("_Compression"),
                    Child, (IPTR)(HGroup,
                        Child, (IPTR)(c_klevel = CycleObject,
                            MUIA_Cycle_Entries, (IPTR)level_labels,
                            MUIA_ControlChar, 'c',
                            MUIA_CycleChain, 1,
                        End),
                        /* gzip has no "store only": a shorter list for tar.gz. */
                        Child, (IPTR)(c_klevel3 = CycleObject,
                            MUIA_Cycle_Entries, (IPTR)level3_labels,
                            MUIA_CycleChain, 1,
                            MUIA_ShowMe, FALSE,
                        End),
                    End),
                End),
                Child, (IPTR)(HGroup,
                    Child, (IPTR)(b_kgo = button("_Pack")),
                    Child, (IPTR)HSpace(0),
                    Child, (IPTR)(b_kcancel = button("Ca_ncel")),
                End),
            End),
        End),
        SubWindow, (IPTR)(wdetail = WindowObject,
            MUIA_Window_Title, (IPTR)"Details",
            MUIA_Window_ID, MAKE_ID('U','A','D','T'),
            MUIA_Window_Width, MUIV_Window_Width_Screen(50),
            MUIA_Window_Height, MUIV_Window_Height_Screen(40),
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)ListviewObject,
                    MUIA_Listview_Input, FALSE,
                    MUIA_Listview_List, (IPTR)(f_detail = FloattextObject,
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
        { b_open, ID_OPEN }, { b_extract, ID_EXTRACT_ASK },
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
    DoMethod(c_onopen, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_ONOPEN);
    DoMethod(cb_newfolder, MUIM_Notify, MUIA_Selected, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_REPLAN);
    DoMethod(s_to, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_REPLAN);
    SetAttrs(cb_newfolder, MUIA_Selected, TRUE, MUIA_CycleChain, 1, TAG_DONE);
    DoMethod(s_find, MUIM_Notify, MUIA_String_Contents, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_FIND);
    DoMethod(lv, MUIM_Notify, MUIA_Listview_SelectChange, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_SEL);
    DoMethod(lv, MUIM_Notify, MUIA_List_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_B_ACTIVE);
    DoMethod(wstart, MUIM_Notify, MUIA_AppMessage, MUIV_EveryTime, (IPTR)wstart, 3,
             MUIM_CallHook, (IPTR)&drop_hook, MUIV_TriggerValue);
    SetAttrs(c_onopen, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, onopen_browse, TAG_DONE);
    {
        struct { Object *o; ULONG id; } more[] = {
            { b_pack, ID_PACK_FILES }, { b_packdir, ID_PACK_DRAWER },
            { b_kgo, ID_K_GO }, { b_kcancel, ID_K_CANCEL },
        };
        for (i = 0; i < sizeof(more) / sizeof(more[0]); i++)
            DoMethod(more[i].o, MUIM_Notify, MUIA_Pressed, FALSE, (IPTR)app, 2,
                     MUIM_Application_ReturnID, more[i].id);
    }
    DoMethod(c_kfmt, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_K_FMT);
    DoMethod(c_packfmt, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_PACKFMT);
    DoMethod(wpack, MUIM_Notify, MUIA_Window_CloseRequest, TRUE, (IPTR)app, 2,
             MUIM_Application_ReturnID, ID_K_CANCEL);
    SetAttrs(c_packfmt, MUIA_NoNotify, TRUE, MUIA_Cycle_Active, packfmt_pref, TAG_DONE);
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
    snprintf(dest, sizeof(dest), "%s", to && *to ? (char *)to : bplan.dest);
    if (ua_plan_make(&blist, b_planname, dest, !b_newfolder(), &bplan, err, sizeof(err)))
        settext(t_active, "Cannot extract there: %s", err);
    else
        settext(t_active, "Will extract to %s%s", bplan.root,
                bplan.kind == UA_ROOT_DEST ? "" : bplan.kind == UA_ROOT_TOPDIR ?
                " (the archive's own folder)" : " (a new folder)");
    DoMethod(lv, MUIM_List_Redraw, MUIV_List_Redraw_All);
    b_note_selection();
}

/* Encrypted entries among those about to be extracted: a password first.
 * 0 when the user gave none (nothing is started). */
static int b_password_ready(void)
{
    if (!ua_list_encrypted_wanted(&blist) || pw_of(b_archive))
        return 1;
    return ask_password(b_archive, 0);
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
    snprintf(dest, sizeof(dest), "%s", to && *to ? (char *)to : bplan.dest);
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
    if (!job && !is_open(wstart) && !is_open(wbrowse) && !is_open(wprog) && !is_open(wpack))
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
            printf("Unarc: cannot create the application (is MUI/Zune installed?)\n");
        return 20;
    }
    notify();

    if (nargs == 0) {
        open_win(wstart, 1);
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
                    say("\"%s\" is not an archive Unarc can read.", args[k]);
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
                settext(t_startnote, "Busy with another archive; drop again when it is done.");
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
        case ID_EXTRACT_ASK: {
            char p[UA_PATHMAX];
            if (ask_archive("Extract archive", p, sizeof(p))) {
                if (is_archive(p))
                    extract_now(p);
                else
                    say("\"%s\" is not an archive Unarc can read.", p);
            }
        } break;
        case ID_ONOPEN: {
            IPTR v = 0;
            GetAttr(MUIA_Cycle_Active, c_onopen, &v);
            onopen_browse = v == 1;
            prefs_save();
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
        case ID_PACK_FILES:  pack_pick(0); break;
        case ID_PACK_DRAWER: pack_pick(1); break;
        case ID_K_FMT:       k_fix_ext(); break;
        case ID_K_GO:        pack_go(); break;
        case ID_K_CANCEL:    open_win(wpack, 0); k_clear(); break;
        case ID_PACKFMT: {
            IPTR v = 0;
            GetAttr(MUIA_Cycle_Active, c_packfmt, &v);
            packfmt_pref = (int)v;
            prefs_save();
        } break;
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
    if (dragcls)
        MUI_DeleteCustomClass(dragcls);
    DeleteMsgPort(jport);
    b_reset();
    free(r_detail);
    for (i = 0; i < nq; i++)
        free(q[i].detail);
    drops_clear();
    k_clear();
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
