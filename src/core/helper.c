/* SPDX-License-Identifier: MIT */
/* Running a helper program (see helper.h).
 *
 * The program is started with LoadSeg and CreateNewProc, not through a
 * Shell: a Shell would read quotes and semicolons in the argument line, and
 * would keep a lock on our drawer (SystemTags passes no NP_HomeDir). Its
 * output comes through a PIPE: handle. Reads are sent as packets, so that
 * Ctrl-C reaches the program even while it works for minutes without
 * writing. AROS puts a process's arguments into its input stream, so input
 * is NIL: and nothing is passed that way. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/dosextens.h>
#include <dos/stdio.h>
#include "helper.h"

int ua_helper_find(const char *name, char *path, size_t n)
{
    BPTR d = GetProgramDir(), l;
    if (!d || !NameFromLock(d, (STRPTR)path, n) || !AddPart((STRPTR)path, (STRPTR)name, n))
        return 0;
    if (!(l = Lock((STRPTR)path, SHARED_LOCK)))
        return 0;
    UnLock(l);
    return 1;
}

/* Runs in the program's process when it ends, before DOS frees it. Only
 * this function's own return is left to run in our code after the signal;
 * ua_helper_end waits a tick before the caller may go on. */
static void helper_exit(IPTR rc, IPTR data)
{
    ua_helper *h = (ua_helper *)data;
    h->rc = (LONG)rc;
    h->exited = 1;
    Signal(h->parent, 1UL << h->sig);
}

void ua_helper_poke(ua_helper *h)
{
    /* helper_exit sets exited before DOS frees the process, so under Forbid
     * the process is valid while it is unset. */
    Forbid();
    if (h->proc && !h->exited)
        Signal(&h->proc->pr_Task, SIGBREAKF_CTRL_C);
    Permit();
}

static void helper_free(ua_helper *h)
{
    if (h->sig >= 0)
        FreeSignal(h->sig);
    if (h->dp)
        FreeDosObject(DOS_STDPKT, h->dp);
    DeleteMsgPort(h->port);
    h->sig = -1;
    h->dp = NULL;
    h->port = NULL;
    h->proc = NULL;
    h->rd = BNULL;
}

static int start(ua_helper *h, const char *path, const char *name, const char *args,
                 BPTR curdir, int err_to_out, int poll, char *err, int errlen)
{
    static ULONG serial;
    char out[64];
    char tv[4];
    BPTR seg, wr, in, er, cd = BNULL;
    int vbuf = 1;

    memset(h, 0, sizeof(*h));
    h->sig = -1;
    serial++;
    snprintf(out, sizeof(out), "%s:unarch-%08lx-%lu", poll ? "T" : "PIPE",
             (unsigned long)(IPTR)FindTask(NULL), (unsigned long)serial);
    if (poll) {
        h->poll = 1;
        snprintf(h->outname, sizeof(h->outname), "%s", out);
    }
    h->port = CreateMsgPort();
    h->dp = AllocDosObject(DOS_STDPKT, NULL);
    h->sig = AllocSignal(-1);
    h->parent = FindTask(NULL);
    if (!h->port || !h->dp || h->sig < 0) {
        snprintf(err, errlen, "out of memory");
        helper_free(h);
        return -1;
    }
    if (!(seg = LoadSeg((STRPTR)path))) {
        snprintf(err, errlen, "cannot load %s", name);
        helper_free(h);
        return -1;
    }
    /* A file: both ends shared, the writer's made first (and emptied, in
     * case one of that name was left behind). */
    wr = Open((STRPTR)out, poll ? MODE_READWRITE : MODE_NEWFILE);
    if (wr && poll && SetFileSize(wr, 0, OFFSET_BEGINNING) != 0) {
        Close(wr);
        wr = BNULL;
    }
    h->rd = wr ? Open((STRPTR)out, MODE_OLDFILE) : BNULL;
    in = Open("NIL:", MODE_OLDFILE);
    er = err_to_out ? BNULL : Open("NIL:", MODE_NEWFILE);
    if (curdir)
        cd = DupLock(curdir);
    /* Mainline's PIPE: handles are interactive, so DOS makes them line
     * buffered: every LF, CR or NUL in the program's binary output became a
     * packet of its own (5.5 million for one 241 MB text file, 280 s where
     * full buffering takes 3 s). Nothing in the protocol waits for us to read
     * a line before the program goes on, so full buffering loses nothing. */
    if (wr && !poll && SetVBuf(wr, NULL, BUF_FULL, -1) != 0)
        vbuf = 0;
    if (!wr || !h->rd || !in || (!err_to_out && !er) || (curdir && !cd) || !vbuf) {
        snprintf(err, errlen, poll ? "cannot write the helper's output to T:"
                              : !vbuf ? "cannot set the helper's output pipe to full buffering"
                              : "cannot open a pipe (is PIPE: mounted?)");
        UnLoadSeg(seg);
        if (wr) Close(wr);
        if (h->rd) Close(h->rd);
        if (in) Close(in);
        if (er) Close(er);
        if (cd) UnLock(cd);
        if (poll)
            DeleteFile((STRPTR)out);
        helper_free(h);
        return -1;
    }
    SetSignal(0, 1UL << h->sig);
    /* Polled: the output is ours to close, after the program has ended, so
     * that everything it wrote is in the file before we read the end.
     * Test aid: UnarcTestHelperNoProc makes the start fail here. */
    h->wr = poll ? wr : BNULL;
    h->proc = GetVar("UnarcTestHelperNoProc", tv, sizeof(tv), 0) > 0 ? NULL :
              CreateNewProcTags(NP_Seglist, (IPTR)seg, NP_FreeSeglist, TRUE,
                                NP_Name, (IPTR)name, NP_CommandName, (IPTR)name,
                                NP_Cli, TRUE, NP_Arguments, (IPTR)args,
                                NP_Input, (IPTR)in, NP_Output, (IPTR)wr,
                                /* Errors into the output pipe as well: the
                                 * same handle, closed once, with the output. */
                                NP_Error, (IPTR)(err_to_out ? wr : er),
                                NP_CloseInput, TRUE, NP_CloseOutput, !poll,
                                NP_CloseError, err_to_out ? FALSE : TRUE,
                                NP_StackSize, 262144,
                                /* Our drawer must not stay locked by a helper. */
                                NP_HomeDir, (IPTR)BNULL,
                                cd ? NP_CurrentDir : TAG_IGNORE, (IPTR)cd,
                                NP_ExitCode, (IPTR)helper_exit, NP_ExitData, (IPTR)h,
                                TAG_END);
    if (!h->proc) {
        snprintf(err, errlen, "cannot start %s", name);
        UnLoadSeg(seg);
        Close(wr);
        Close(h->rd);
        Close(in);
        if (er) Close(er);
        if (cd) UnLock(cd);
        if (poll)
            DeleteFile((STRPTR)h->outname);
        helper_free(h);
        return -1;
    }
    return 0;
}

int ua_helper_start(ua_helper *h, const char *path, const char *name, const char *args,
                    BPTR curdir, int err_to_out, char *err, int errlen)
{
    return start(h, path, name, args, curdir, err_to_out, 0, err, errlen);
}

int ua_helper_start_polled(ua_helper *h, const char *path, const char *name, const char *args,
                           BPTR curdir, int err_to_out, char *err, int errlen)
{
    return start(h, path, name, args, curdir, err_to_out, 1, err, errlen);
}

/* The file grows while the program writes; 0 from Read means "not yet"
 * until the program has ended and its output is closed. Ctrl-C to us is
 * passed on whatever the read brought. */
static LONG read_polled(ua_helper *h, void *buf, LONG len)
{
    for (;;) {
        LONG n;
        if ((SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) && !h->cancelled) {
            h->cancelled = 1;
            ua_helper_poke(h);
        }
        n = Read(h->rd, buf, len);
        if (n != 0 || !h->wr)
            return n;
        if (h->exited) {
            /* helper_exit has run; let it return out of our code, then
             * close the output: what it held is in the file after that. */
            Delay(1);
            Close(h->wr);
            h->wr = BNULL;
            continue;
        }
        Delay(2);
    }
}

LONG ua_helper_read(ua_helper *h, void *buf, LONG len)
{
    struct FileHandle *fh = BADDR(h->rd);
    if (h->poll)
        return read_polled(h, buf, len);
    h->dp->dp_Type = ACTION_READ;
    h->dp->dp_Arg1 = fh->fh_Arg1;
    h->dp->dp_Arg2 = (SIPTR)buf;
    h->dp->dp_Arg3 = len;
    SendPkt(h->dp, fh->fh_Type, h->port);
    for (;;) {
        ULONG got = Wait(1UL << h->port->mp_SigBit | SIGBREAKF_CTRL_C);
        if ((got & SIGBREAKF_CTRL_C) && !h->cancelled) {
            h->cancelled = 1;
            ua_helper_poke(h);
        }
        if (GetMsg(h->port))
            return (LONG)h->dp->dp_Res1;
    }
}

LONG ua_helper_end(ua_helper *h)
{
    LONG rc;
    if (h->rd) {
        /* Read to the end, whatever is left. Closing the reader early would
         * leave the program waiting for ever on a full pipe: PIPE: holds a
         * writer until someone reads. */
        UBYTE junk[1024];
        while (ua_helper_read(h, junk, sizeof(junk)) > 0)
            ;
        Close(h->rd);
        h->rd = BNULL;
    }
    if (h->proc) {
        while (!h->exited)
            Wait(1UL << h->sig);
        Delay(1);      /* let helper_exit return out of our code */
    }
    /* Polled, after a failed read: the output is still ours to close. */
    if (h->wr) {
        Close(h->wr);
        h->wr = BNULL;
    }
    rc = h->rc;
    if (h->poll)
        DeleteFile((STRPTR)h->outname);
    helper_free(h);
    return rc;
}
