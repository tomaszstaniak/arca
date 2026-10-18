/* SPDX-License-Identifier: MIT */
/* Zune List against NList under the pattern of an archive browser: a
 * five-column list with a title row, filled in batches that arrive while the
 * window is open, each batch inserted inside Quiet. No archive code.
 *
 *   listbench MODE zune|nlist [ROWS n] [BATCH n] [DELAY ticks] [WIDEN] [LOG file]
 *             [START ticks] [HOLD ticks]
 *
 * WIDEN makes the names longer as rows go on, so that a column has to grow
 * part way through, as with real archives. Names are ISO-8859-1.
 *
 * Logged per run: the time of each batch (insert and the drawing Zune does
 * inside it), the total, select all, jump to the last row, and for Zune List
 * the number of MUIM_DrawBackground calls on the list that cover at least
 * three rows' height (the clears seen as empty frames). NList draws its
 * background its own way, so for both the screen recording is the neutral
 * measure; the log gives the times. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <proto/timer.h>
#include <libraries/mui.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <devices/timer.h>
#include <clib/alib_protos.h>
#include <aros/asmcall.h>

struct Device *TimerBase;
static struct timerequest tr;
static ULONG efreq;
static BPTR logfh;
static int nlist, widen;
static ULONG clears, bgcalls;
static WORD minclear = 30;

static void lg(const char *fmt, ...)
{
    char b[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (logfh) {
        FPuts(logfh, b);
        Flush(logfh);
    }
    PutStr(b);
}

static UQUAD now(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return ((UQUAD)e.ev_hi << 32) | e.ev_lo;
}

static ULONG ms(UQUAD a, UQUAD b)
{
    return (ULONG)((b - a) * 1000 / efreq);
}

/* ---- rows ---- */

static char c_name[200], c_size[24], c_date[24], c_prot[12], c_note[24];

static void make_cols(ULONG i, char **col)
{
    if (i == 0) {
        col[0] = "Name";
        col[1] = "Size";
        col[2] = "Date";
        col[3] = "Prot";
        col[4] = "Note";
        return;
    }
    i--;
    {
        int extra = widen ? (int)(i / 2000) * 6 : 0;
        snprintf(c_name, sizeof(c_name), "Caf\xe9 \xc4rger/%s%05lu f\xfcr \xf1o%.*s.txt",
                 (i % 7) ? "docs/" : "", (unsigned long)i, extra,
                 "______________________________________________________________");
    }
    snprintf(c_size, sizeof(c_size), "%lu", (unsigned long)(i * 37 % 100000));
    snprintf(c_date, sizeof(c_date), "%02lu-Oct-26 12:%02lu", (unsigned long)(i % 28 + 1),
             (unsigned long)(i % 60));
    snprintf(c_prot, sizeof(c_prot), "----rwed");
    snprintf(c_note, sizeof(c_note), "%s", (i % 11) ? "" : "comment");
    col[0] = c_name;
    col[1] = c_size;
    col[2] = c_date;
    col[3] = c_prot;
    col[4] = c_note;
}

AROS_UFH3S(void, zdisplay,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(char **, col, A2),
    AROS_UFHA(APTR, ent, A1))
{
    AROS_USERFUNC_INIT
    (void)hook;
    make_cols((ULONG)(IPTR)ent, col);
    AROS_USERFUNC_EXIT
}

AROS_UFH3S(void, ndisplay,
    AROS_UFHA(struct Hook *, hook, A0),
    AROS_UFHA(Object *, obj, A2),
    AROS_UFHA(struct NList_DisplayMessage *, m, A1))
{
    AROS_USERFUNC_INIT
    (void)hook;
    (void)obj;
    make_cols((ULONG)(IPTR)m->entry, m->strings);
    AROS_USERFUNC_EXIT
}

static struct Hook zhook, nhook;

/* ---- Zune List subclass that counts background clears ---- */

static struct MUI_CustomClass *cls;

IPTR count_dispatcher(struct IClass *cl, Object *obj, Msg msg)
{
    if (msg->MethodID == MUIM_DrawBackground) {
        struct MUIP_DrawBackground *d = (struct MUIP_DrawBackground *)msg;
        bgcalls++;
        if (d->height >= minclear)
            clears++;
    }
    return DoSuperMethodA(cl, obj, msg);
}

AROS_UFH3S(IPTR, count_entry,
    AROS_UFHA(struct IClass *, cl, A0),
    AROS_UFHA(Object *, obj, A2),
    AROS_UFHA(Msg, msg, A1))
{
    AROS_USERFUNC_INIT
    return count_dispatcher(cl, obj, msg);
    AROS_USERFUNC_EXIT
}

/* ---- main ---- */

/* Let Zune handle what is pending (refreshes, input) without waiting. Not
 * NewInput with 0 signals: that services every source and costs memory on
 * ABIv11. */
static void pump(Object *app)
{
    DoMethod(app, MUIM_Application_InputBuffered);
}

int main(void)
{
    enum { A_MODE, A_ROWS, A_BATCH, A_DELAY, A_WIDEN, A_LOG, A_START, A_HOLD, A_N };
    IPTR args[A_N] = { 0 };
    struct RDArgs *rd = ReadArgs("MODE/K/A,ROWS/K/N,BATCH/K/N,DELAY/K/N,WIDEN/S,LOG/K,"
                                 "START/K/N,HOLD/K/N", (IPTR *)args, NULL);
    ULONG rows = 20000, batch = 256, delay = 2, start = 100, hold = 150, i, b, nb = 0;
    ULONG maxb = 0, sumb = 0;
    Object *app, *win, *lst, *lv;
    APTR *buf;
    UQUAD t0, t1, ta, tb;
    ULONG mem0, mem1;

    if (!rd) {
        PrintFault(IoErr(), "listbench");
        return 20;
    }
    nlist = !Stricmp((STRPTR)args[A_MODE], "nlist");
    widen = args[A_WIDEN] != 0;
    if (args[A_ROWS]) rows = *(LONG *)args[A_ROWS];
    if (args[A_BATCH]) batch = *(LONG *)args[A_BATCH];
    if (args[A_DELAY]) delay = *(LONG *)args[A_DELAY];
    if (args[A_START]) start = *(LONG *)args[A_START];
    if (args[A_HOLD]) hold = *(LONG *)args[A_HOLD];
    if (args[A_LOG])
        logfh = Open((STRPTR)args[A_LOG], MODE_NEWFILE);
    if (OpenDevice("timer.device", UNIT_ECLOCK, &tr.tr_node, 0)) {
        PutStr("no timer.device\n");
        return 20;
    }
    TimerBase = tr.tr_node.io_Device;
    {
        struct EClockVal e;
        efreq = ReadEClock(&e);
    }
    buf = malloc(batch * sizeof(APTR));
    zhook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(zdisplay);
    nhook.h_Entry = (HOOKFUNC)AROS_ASMSYMNAME(ndisplay);
    mem0 = AvailMem(MEMF_ANY);

    if (nlist) {
        lst = NListObject,
            InputListFrame,
            MUIA_NList_Format, (IPTR)"BAR,P=\33r BAR,BAR,BAR,",
            MUIA_NList_Title, TRUE,
            MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_Default,
            MUIA_NList_DisplayHook2, (IPTR)&nhook,
        End;
        lv = lst ? NListviewObject, MUIA_NListview_NList, (IPTR)lst, End : NULL;
    } else {
        cls = MUI_CreateCustomClass(NULL, MUIC_List, NULL, 0,
                                    (APTR)AROS_ASMSYMNAME(count_entry));
        lst = cls ? NewObject(cls->mcc_Class, NULL,
            InputListFrame,
            MUIA_List_Format, (IPTR)"D=8 BAR,D=12 P=\33r BAR,D=12 BAR,D=12 BAR,D=8",
            MUIA_List_Title, TRUE,
            MUIA_List_DisplayHook, (IPTR)&zhook,
            TAG_DONE) : NULL;
        lv = lst ? ListviewObject,
            MUIA_Listview_MultiSelect, MUIV_Listview_MultiSelect_Default,
            MUIA_Listview_List, (IPTR)lst,
        End : NULL;
    }
    if (!lv) {
        lg("could not create the %s list (class missing?)\n", nlist ? "NList" : "Zune");
        return 20;
    }
    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"ListBench",
        SubWindow, (IPTR)(win = WindowObject,
            MUIA_Window_Title, (IPTR)(nlist ? "ListBench NList" : "ListBench Zune List"),
            MUIA_Window_LeftEdge, 40,
            MUIA_Window_TopEdge, 60,
            MUIA_Window_Width, 640,
            MUIA_Window_Height, 420,
            WindowContents, (IPTR)(VGroup, Child, (IPTR)lv, End),
        End),
    End;
    if (!app) {
        lg("could not create the application\n");
        return 20;
    }
    SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
    pump(app);
    {
        /* Rows are about one font line high; a clear of three or more is
         * more than a row being redrawn. */
        struct TextFont *f = _font(lst);
        if (f)
            minclear = 3 * f->tf_YSize;
    }
    lg("mode %s rows %lu batch %lu delay %lu widen %d eclock %lu Hz\n",
       nlist ? "nlist" : "zune", (unsigned long)rows, (unsigned long)batch,
       (unsigned long)delay, widen, (unsigned long)efreq);
    Delay(start);
    lg("FILL START\n");
    clears = bgcalls = 0;
    t0 = now();
    for (i = 0; i < rows; i += b) {
        ULONG k, d;
        b = rows - i < batch ? rows - i : batch;
        for (k = 0; k < b; k++)
            buf[k] = (APTR)(IPTR)(i + k + 1);
        ta = now();
        SetAttrs(lst, nlist ? MUIA_NList_Quiet : MUIA_List_Quiet, TRUE, TAG_DONE);
        if (nlist)
            DoMethod(lst, MUIM_NList_Insert, (IPTR)buf, b, MUIV_NList_Insert_Bottom, 0);
        else
            DoMethod(lst, MUIM_List_Insert, (IPTR)buf, b, MUIV_List_Insert_Bottom);
        SetAttrs(lst, nlist ? MUIA_NList_Quiet : MUIA_List_Quiet, FALSE, TAG_DONE);
        pump(app);
        tb = now();
        d = ms(ta, tb);
        sumb += d;
        if (d > maxb)
            maxb = d;
        nb++;
        if (nb <= 3 || d > 200)
            lg("batch %lu at row %lu: %lu ms\n", (unsigned long)nb, (unsigned long)i,
               (unsigned long)d);
        Delay(delay);
    }
    t1 = now();
    lg("FILL END\n");
    lg("fill: %lu ms total, %lu batches, batch avg %lu ms, max %lu ms\n",
       (unsigned long)ms(t0, t1), (unsigned long)nb, (unsigned long)(nb ? sumb / nb : 0),
       (unsigned long)maxb);
    if (!nlist)
        lg("zune: %lu background clears of >= %d px height, %lu DrawBackground calls\n",
           (unsigned long)clears, (int)minclear, (unsigned long)bgcalls);
    Delay(50);

    ta = now();
    if (nlist)
        DoMethod(lst, MUIM_NList_Select, MUIV_NList_Select_All, MUIV_NList_Select_On, NULL);
    else
        DoMethod(lst, MUIM_List_Select, MUIV_List_Select_All, MUIV_List_Select_On, NULL);
    pump(app);
    tb = now();
    lg("select all: %lu ms\n", (unsigned long)ms(ta, tb));
    Delay(50);
    ta = now();
    SetAttrs(lst, nlist ? MUIA_NList_Active : MUIA_List_Active,
             nlist ? MUIV_NList_Active_Bottom : MUIV_List_Active_Bottom, TAG_DONE);
    pump(app);
    tb = now();
    lg("jump to last row: %lu ms\n", (unsigned long)ms(ta, tb));
    Delay(hold);

    SetAttrs(win, MUIA_Window_Open, FALSE, TAG_DONE);
    MUI_DisposeObject(app);
    if (cls)
        MUI_DeleteCustomClass(cls);
    mem1 = AvailMem(MEMF_ANY);
    lg("memory: %ld bytes not back after closing (no flush)\nDONE\n", (long)(mem0 - mem1));
    free(buf);
    CloseDevice(&tr.tr_node);
    if (logfh)
        Close(logfh);
    FreeArgs(rd);
    return 0;
}
