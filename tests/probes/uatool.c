/* SPDX-License-Identifier: MIT */
/* Guest-side helper for GUI tests, so that no association has to be installed
 * in the test system:
 *   uatool icon <file> <default tool> [TOOLTYPE=...]...  project icon for a file
 *   uatool open <path>          what a Workbench double-click does (workbench.library)
 *   uatool drop <title> <path>... an AppWindow drop on the first window whose title
 *                               starts with <title>, as Wanderer sends it
 *   uatool waitwin <title> <s>  wait until such a window exists (RC 5 if not)
 *   uatool gonewin <title> <s>  wait until no such window exists (RC 5 if one stays)
 *   uatool sum <drawer>         SHA-256 of every file under the drawer, one per line
 *   uatool running <name>       RC 0 if a process runs the command <name> (the file
 *                               part of a Shell command, or a task of that name) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <intuition/intuitionbase.h>
#include <workbench/workbench.h>
#include <workbench/icon.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/icon.h>
#include <proto/workbench.h>

/* ---- SHA-256 (FIPS 180-4), for checking extracted files in the guest ---- */
typedef struct { ULONG h[8]; UBYTE buf[64]; UQUAD len; ULONG n; } sha;
static const ULONG K[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha *c, const UBYTE *p)
{
    ULONG w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (ULONG)p[i * 4] << 24 | (ULONG)p[i * 4 + 1] << 16 | (ULONG)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (i = 16; i < 64; i++)
        w[i] = (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10)) + w[i - 7] +
               (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 16];
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
static void sha_init(sha *c)
{
    static const ULONG iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(c->h, iv, sizeof(iv));
    c->len = 0;
    c->n = 0;
}
static void sha_add(sha *c, const UBYTE *p, ULONG n)
{
    c->len += n;
    while (n--) {
        c->buf[c->n++] = *p++;
        if (c->n == 64) {
            sha_block(c, c->buf);
            c->n = 0;
        }
    }
}
static void sha_hex(sha *c, char *out)
{
    UQUAD bits = c->len * 8;
    UBYTE pad = 0x80, z = 0, lb[8];
    int i;
    sha_add(c, &pad, 1);
    while (c->n != 56)
        sha_add(c, &z, 1);
    for (i = 0; i < 8; i++)
        lb[i] = (UBYTE)(bits >> (56 - 8 * i));
    sha_add(c, lb, 8);
    for (i = 0; i < 8; i++)
        sprintf(out + i * 8, "%08lx", (unsigned long)c->h[i]);
}

static void sum_tree(BPTR dir, const char *rel)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    static UBYTE buf[65536];
    if (!fib || !Examine(dir, fib)) {
        FreeDosObject(DOS_FIB, fib);
        return;
    }
    while (ExNext(dir, fib)) {
        char sub[1024];
        BPTR old = CurrentDir(dir), l;
        snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "", fib->fib_FileName);
        if (fib->fib_DirEntryType == ST_SOFTLINK || fib->fib_DirEntryType == ST_LINKDIR ||
            fib->fib_DirEntryType == ST_LINKFILE) {
            CurrentDir(old);
            printf("link %s\n", sub);
            continue;
        }
        if (fib->fib_DirEntryType > 0) {
            l = Lock(fib->fib_FileName, SHARED_LOCK);
            CurrentDir(old);
            if (l) {
                sum_tree(l, sub);
                UnLock(l);
            }
        } else {
            BPTR fh = Open(fib->fib_FileName, MODE_OLDFILE);
            CurrentDir(old);
            if (fh) {
                sha c;
                char hex[65];
                LONG n;
                sha_init(&c);
                while ((n = Read(fh, buf, sizeof(buf))) > 0)
                    sha_add(&c, buf, n);
                Close(fh);
                sha_hex(&c, hex);
                printf("%s  %s\n", hex, sub);
            }
        }
    }
    FreeDosObject(DOS_FIB, fib);
}

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
    if (argc == 3 && !strcmp(argv[1], "sum")) {
        BPTR l = Lock(argv[2], SHARED_LOCK);
        if (!l) {
            printf("cannot open %s\n", argv[2]);
            return 20;
        }
        sum_tree(l, "");
        UnLock(l);
        return 0;
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
