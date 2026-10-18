/* SPDX-License-Identifier: MIT */
/* Test aid: which process keeps a drawer "in use". For every process it
 * compares the current directory, the home directory and the CLI's command
 * path with the drawer given, and prints the matches.
 *   whohas <drawer>
 * The task lists are copied under Forbid (pointers only) and compared after
 * Permit, so a process that ends in between can give a stale line; this is
 * a diagnostic, run on a quiet system. Locks held in other ways (an open
 * file, a lock a program keeps in its own data) are not seen. */
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/execbase.h>
#include <dos/dosextens.h>

#define MAXP 64

struct seen {
    char name[64];
    BPTR cd, home;
    BPTR path[8];
    int npath;
};

static int grab(struct List *l, struct seen *s, int n)
{
    struct Node *nd;
    for (nd = l->lh_Head; nd->ln_Succ && n < MAXP; nd = nd->ln_Succ) {
        struct Process *pr = (struct Process *)nd;
        struct CommandLineInterface *cli;
        if (nd->ln_Type != NT_PROCESS)
            continue;
        memset(&s[n], 0, sizeof(s[n]));
        snprintf(s[n].name, sizeof(s[n].name), "%s", nd->ln_Name ? nd->ln_Name : "?");
        s[n].cd = pr->pr_CurrentDir;
        s[n].home = pr->pr_HomeDir;
        cli = BADDR(pr->pr_CLI);
        if (cli) {
            BPTR *pe = BADDR(cli->cli_CommandDir);
            while (pe && s[n].npath < 8) {
                s[n].path[s[n].npath++] = pe[1];
                pe = BADDR(pe[0]);
            }
            if (cli->cli_CommandName) {
                UBYTE *b = BADDR(cli->cli_CommandName);
                if (b && b[0])
                    snprintf(s[n].name + strlen(s[n].name), sizeof(s[n].name) - strlen(s[n].name),
                             " [%.*s]", b[0], b + 1);
            }
        }
        n++;
    }
    return n;
}

int main(int argc, char **argv)
{
    static struct seen s[MAXP];
    BPTR d;
    int n = 0, i, k, hits = 0;
    if (argc < 2) {
        printf("usage: whohas <drawer>\n");
        return 20;
    }
    if (!(d = Lock((STRPTR)argv[1], SHARED_LOCK))) {
        printf("cannot lock %s\n", argv[1]);
        return 20;
    }
    Forbid();
    n = grab(&SysBase->TaskReady, s, n);
    n = grab(&SysBase->TaskWait, s, n);
    Permit();
    for (i = 0; i < n; i++) {
        if (s[i].cd && SameLock(s[i].cd, d) == LOCK_SAME) {
            printf("current dir of %s\n", s[i].name);
            hits++;
        }
        if (s[i].home && SameLock(s[i].home, d) == LOCK_SAME) {
            printf("home dir of %s\n", s[i].name);
            hits++;
        }
        for (k = 0; k < s[i].npath; k++)
            if (s[i].path[k] && SameLock(s[i].path[k], d) == LOCK_SAME) {
                printf("command path of %s\n", s[i].name);
                hits++;
            }
    }
    printf("%d processes looked at, %d matches\n", n, hits);
    UnLock(d);
    return 0;
}
