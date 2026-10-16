/* SPDX-License-Identifier: MIT */
/* Control for the GUI memory loop: what one run of a small Zune program keeps
 * on this system, so that Unarc's own figure can be told apart from it. It
 * builds an application with a list window and an AppWindow, opens one window,
 * fills the list with N rows (argument, default 0), starts and waits for one
 * worker process, and quits. No archive code. */
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <libraries/mui.h>
#include <dos/dostags.h>
#include <clib/alib_protos.h>

static volatile int worker_done;

static void worker(void)
{
    worker_done = 1;
}

int main(int argc, char **argv)
{
    Object *app, *win, *lst;
    ULONG rows = argc > 1 ? strtoul(argv[1], NULL, 10) : 0, i;
    static const char *row[] = { "row", NULL };
    struct Process *p;

    app = ApplicationObject,
        MUIA_Application_Title, (IPTR)"ZuneControl",
        SubWindow, (IPTR)(win = WindowObject,
            MUIA_Window_Title, (IPTR)"Zune control",
            MUIA_Window_AppWindow, TRUE,
            WindowContents, (IPTR)(VGroup,
                Child, (IPTR)ListviewObject,
                    MUIA_Listview_List, (IPTR)(lst = ListObject, InputListFrame, End),
                End,
                Child, (IPTR)SimpleButton("_Close"),
            End),
        End),
    End;
    if (!app)
        return 20;
    SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
    SetAttrs(lst, MUIA_List_Quiet, TRUE, TAG_DONE);
    for (i = 0; i < rows; i++)
        DoMethod(lst, MUIM_List_InsertSingle, (IPTR)row[0], MUIV_List_Insert_Bottom);
    SetAttrs(lst, MUIA_List_Quiet, FALSE, TAG_DONE);
    p = CreateNewProcTags(NP_Entry, (IPTR)worker, NP_Name, (IPTR)"Zune control worker",
                          NP_Input, 0, NP_Output, 0, NP_CloseInput, FALSE, NP_CloseOutput, FALSE,
                          NP_CurrentDir, 0, NP_HomeDir, 0, TAG_DONE);
    while (p && !worker_done)
        Delay(1);
    for (;;) {
        struct Task *t;
        Forbid();
        t = FindTask((STRPTR)"Zune control worker");
        Permit();
        if (!t)
            break;
        Delay(1);
    }
    Delay(10);
    SetAttrs(win, MUIA_Window_Open, FALSE, TAG_DONE);
    MUI_DisposeObject(app);
    return 0;
}
