/* SPDX-License-Identifier: MIT */
/* Guest-side helper for GUI tests, so that no association has to be installed
 * in the test system:
 *   uatool icon <file> <default tool> [TOOLTYPE=...]...  project icon for a file
 *   uatool open <path>          what a Workbench double-click does (workbench.library)
 *   uatool drop <title> <path>... an AppWindow drop on the first window whose title
 *                               starts with <title>, as Wanderer sends it
 *   uatool waitwin <title> <s>  wait until such a window exists (RC 5 if not)
 *   uatool gonewin <title> <s>  wait until no such window exists (RC 5 if one stays)
 *   uatool running <name>       RC 0 if a process runs the command <name> (the file
 *                               part of a Shell command, or a task of that name) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>
#include <intuition/intuitionbase.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include <proto/workbench.h>

static struct Window *find_win(const char *title)
{
    struct Screen *s;
    struct Window *w, *found = NULL;
    ULONG lock = LockIBase(0);
    size_t n = strlen(title);
    for (s = IntuitionBase->FirstScreen; s && !found; s = s->NextScreen)
        for (w = s->FirstWindow; w; w = w->NextWindow)
            if (w->Title && strncmp((char *)w->Title, title, n) == 0) {
                found = w;
                break;
            }
    UnlockIBase(lock);
    return found;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "icon")) {
        struct DiskObject *dob = GetDefDiskObject(WBPROJECT);
        char *tt[16];
        int i, n = 0;
        if (!dob) {
            printf("no default project icon\n");
            return 20;
        }
        for (i = 4; i < argc && n < 15; i++)
            tt[n++] = argv[i];
        tt[n] = NULL;
        dob->do_DefaultTool = argv[3];
        dob->do_ToolTypes = (STRPTR *)tt;
        dob->do_CurrentX = NO_ICON_POSITION;
        dob->do_CurrentY = NO_ICON_POSITION;
        i = PutDiskObject(argv[2], dob);
        printf("icon %s -> %s: %s\n", argv[2], argv[3], i ? "ok" : "FAILED");
        /* Only the strings we put in; the rest is the default object's. */
        dob->do_DefaultTool = NULL;
        dob->do_ToolTypes = NULL;
        FreeDiskObject(dob);
        return i ? 0 : 20;
    }
    if (argc == 3 && !strcmp(argv[1], "open")) {
        BOOL ok = OpenWorkbenchObject(argv[2], TAG_DONE);
        printf("open %s: %s\n", argv[2], ok ? "queued" : "FAILED");
        return ok ? 0 : 20;
    }
    if (argc >= 4 && !strcmp(argv[1], "drop")) {
        struct Window *w = find_win(argv[2]);
        BOOL ok;
        if (!w) {
            printf("no window \"%s\"\n", argv[2]);
            return 20;
        }
        ok = SendAppWindowMessage(w, argc - 3, &argv[3], 0, w->Width / 2, w->Height / 2, 0, 0);
        printf("drop %d on \"%s\": %s\n", argc - 3, argv[2], ok ? "sent" : "FAILED");
        return ok ? 0 : 20;
    }
    if (argc == 4 && (!strcmp(argv[1], "waitwin") || !strcmp(argv[1], "gonewin"))) {
        int want = !strcmp(argv[1], "waitwin"), t;
        int ticks = atoi(argv[3]) * 50;
        for (t = 0; t <= ticks; t += 10) {
            if ((find_win(argv[2]) != NULL) == want) {
                printf("%s \"%s\": after %d s\n", argv[1], argv[2], t / 50);
                return 0;
            }
            Delay(10);
        }
        printf("%s \"%s\": NOT within %s s\n", argv[1], argv[2], argv[3]);
        return 5;
    }
    if (argc == 3 && !strcmp(argv[1], "running")) {
        /* A program started with Run runs in a CLI process named after the
         * CLI, not the program; compare the command name too. */
        struct List *lists[2];
        struct Node *n;
        int i, found = 0;
        lists[0] = &SysBase->TaskReady;
        lists[1] = &SysBase->TaskWait;
        Forbid();
        for (i = 0; i < 2 && !found; i++)
            for (n = lists[i]->lh_Head; n->ln_Succ && !found; n = n->ln_Succ) {
                struct Task *t = (struct Task *)n;
                if (n->ln_Name && !strcmp(n->ln_Name, argv[2]))
                    found = 1;
                else if (t->tc_Node.ln_Type == NT_PROCESS && ((struct Process *)t)->pr_CLI) {
                    struct CommandLineInterface *cli = BADDR(((struct Process *)t)->pr_CLI);
                    UBYTE *b = BADDR(cli->cli_CommandName);
                    char name[256];
                    if (b && b[0]) {
                        memcpy(name, b + 1, b[0]);
                        name[b[0]] = 0;
                        if (!strcmp((char *)FilePart(name), argv[2]))
                            found = 1;
                    }
                }
            }
        Permit();
        printf("running \"%s\": %s\n", argv[2], found ? "yes" : "no");
        return found ? 0 : 5;
    }
    printf("usage: uatool icon|open|drop|waitwin|gonewin|running ...\n");
    return 20;
}
