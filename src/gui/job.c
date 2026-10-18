/* SPDX-License-Identifier: MIT */
/* The worker side of a job; see job.h for who owns what.
 *
 * The worker is a DOS process of its own, made by CreateNewProc with the code
 * of this program. On ABIv11 it then shares the program's C library base:
 * malloc and free use one pool, which is semaphore protected (compiler/crt/
 * stdc/malloc.c), so lists built here can be freed by the GUI. The posixc
 * file calls libarchive makes are not protected, so only the worker uses
 * them; the GUI process sticks to dos.library. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include "job.h"

#define WORKER_NAME "Unarc worker"

int uj_ticks_since(const struct DateStamp *a);

uj_job *uj_new(uj_kind kind, const char *archive, struct MsgPort *gui)
{
    uj_job *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->kind = kind;
    snprintf(j->archive, sizeof(j->archive), "%s", archive);
    j->gui = gui;
    j->conflict = UA_CONFLICT_ASK;
    return j;
}

void uj_lend_list(uj_job *j, ua_list *l, ua_plan *p)
{
    j->list = *l;
    j->plan = *p;
    j->have_plan = 1;
    j->lent = 1;
}

void uj_take_list(uj_job *j, ua_list *l, ua_plan *p)
{
    *l = j->list;
    *p = j->plan;
    l->password = NULL;          /* pointed at j->password, freed with the job */
    memset(&j->list, 0, sizeof(j->list));
    j->have_plan = 0;
    j->lent = 0;
}

void uj_free(uj_job *j)
{
    int i;
    if (!j)
        return;
    for (i = 0; i < j->ninputs; i++)
        free(j->inputs[i]);
    free(j->inputs);
    if (!j->lent)
        ua_list_free(&j->list);
    ua_volumes_free(&j->vols);
    free(j->log);
    memset(j->password, 0, sizeof(j->password));
    free(j);
}

/* The log keeps the first UJ_LOGMAX bytes and counts the rest: a damaged
 * archive with 10 000 bad names must not turn into 10 000 messages. */
static void logv(uj_job *j, const char *fmt, va_list ap)
{
    char line[UA_PATHMAX + 200];
    size_t n;
    vsnprintf(line, sizeof(line), fmt, ap);
    n = strlen(line);
    if (!j->log)
        j->log = malloc(UJ_LOGMAX);
    if (!j->log || j->loglen + n + 2 > UJ_LOGMAX) {
        j->logdropped++;
        return;
    }
    memcpy(j->log + j->loglen, line, n);
    j->loglen += n;
    j->log[j->loglen++] = '\n';
    j->log[j->loglen] = 0;
    j->loglines++;
}

static void note(void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv(ctx, fmt, ap);
    va_end(ap);
}

static void send(uj_job *j, uj_msg *m, ULONG type)
{
    m->type = type;
    m->job = j;
    m->m.mn_Length = sizeof(*m);
    m->m.mn_ReplyPort = j->wport;
    PutMsg(j->gui, &m->m);
}

/* Take back every replied message; with wait, block until at least one. */
static void collect(uj_job *j, int wait)
{
    struct Message *m;
    if (wait)
        WaitPort(j->wport);
    while ((m = GetMsg(j->wport))) {
        int i;
        for (i = 0; i < UJ_BATCHES; i++)
            if (m == &j->batch[i].m)
                j->batch_out[i] = 0;
        if (m == &j->progress.m)
            j->progress_out = 0;
    }
}

static int batches_out(uj_job *j)
{
    int i, n = 0;
    for (i = 0; i < UJ_BATCHES; i++)
        n += j->batch_out[i];
    return n;
}

static int broken(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

static void send_batches(uj_job *j)
{
    ULONG at = 0;
    while (at < j->list.n && !broken()) {
        int i;
        for (i = 0; i < UJ_BATCHES && j->batch_out[i]; i++)
            ;
        if (i == UJ_BATCHES) {
            collect(j, 1);
            continue;
        }
        if (j->test_delay)
            Delay(j->test_delay);
        j->batch[i].first = at;
        j->batch[i].count = j->list.n - at < UJ_BATCH ? j->list.n - at : UJ_BATCH;
        at += j->batch[i].count;
        j->batch_out[i] = 1;
        send(j, &j->batch[i], UJM_BATCH);
    }
    while (batches_out(j))
        collect(j, 1);
}

int uj_ticks_since(const struct DateStamp *a)
{
    /* In 64 bits: days since 1978 times ticks per day does not fit 32 bits,
     * and an overflowed difference made every progress message look too
     * early, so none was sent until the end. */
    struct DateStamp now;
    long long t;
    DateStamp(&now);
    t = ((long long)now.ds_Days - a->ds_Days) * 86400LL * TICKS_PER_SECOND +
        ((long long)now.ds_Minute - a->ds_Minute) * 60LL * TICKS_PER_SECOND +
        ((long long)now.ds_Tick - a->ds_Tick);
    return t > 0x7FFFFFFFLL ? 0x7FFFFFFF : t < 0 ? 0 : (int)t;
}

static void progress(uj_job *j, const ua_entry *e, UQUAD done, ULONG files, int force)
{
    collect(j, 0);
    if (j->progress_out || (!force && uj_ticks_since(&j->last_tick) < UJ_TICKS))
        return;
    DateStamp(&j->last_tick);
    j->progress.reading = 0;
    j->progress.bytes = done;
    j->progress.files = files;
    if (e) {
        char *shown = ua_display_name(e->name_src, e->name_kind);
        snprintf(j->progress.name, sizeof(j->progress.name), "%s", shown ? shown : e->name_src);
        free(shown);
    }
    j->progress_out = 1;
    send(j, &j->progress, UJM_PROGRESS);
}

/* While the engine reads the archive: how many entries so far. */
static void on_reading(void *ctx, ULONG n)
{
    uj_job *j = ctx;
    collect(j, 0);
    if (j->progress_out || uj_ticks_since(&j->last_tick) < UJ_TICKS)
        return;
    DateStamp(&j->last_tick);
    j->progress.reading = 1;
    j->progress.listed = n;
    j->progress.bytes = 0;
    j->progress.files = 0;
    j->progress.name[0] = 0;
    j->progress_out = 1;
    send(j, &j->progress, UJM_PROGRESS);
}

static void on_tick(void *ctx, const ua_entry *e, UQUAD done)
{
    uj_job *j = ctx;
    progress(j, e, done, j->rep.files, 0);
}

static void on_file(void *ctx, const ua_entry *e, const char *written, UQUAD done)
{
    uj_job *j = ctx;
    (void)written;
    /* ua_extract has not counted this file yet when it calls us. */
    progress(j, e, done, j->rep.files, 0);
}

static void on_message(void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    logv(ctx, fmt, ap);
    va_end(ap);
}

/* The worker waits for the answer; the GUI answers -1 when the job is being
 * cancelled or the window closes, so this always returns. */
static int on_ask(void *ctx, const char *path, const ua_entry *e, int *all)
{
    uj_job *j = ctx;
    struct Message *m;
    snprintf(j->ask.path, sizeof(j->ask.path), "%s", path);
    j->ask.esize = e ? e->size : 0;
    j->ask.esize_valid = e && e->size_valid;
    j->ask.edate_valid = e && e->date_valid;
    if (e && e->date_valid)
        j->ask.edate = e->date;
    j->ask.answer = -1;
    j->ask.all = 0;
    send(j, &j->ask, UJM_ASK);
    for (;;) {
        WaitPort(j->wport);
        while ((m = GetMsg(j->wport))) {
            int i;
            for (i = 0; i < UJ_BATCHES; i++)
                if (m == &j->batch[i].m)
                    j->batch_out[i] = 0;
            if (m == &j->progress.m)
                j->progress_out = 0;
            if (m == &j->ask.m) {
                *all = j->ask.all != 0;
                return j->ask.answer;
            }
        }
    }
}

/* List and plan. An EXTRACT job started straight from an archive (double-click,
 * drop) comes through here too, without sending the entries. */
static int do_list(uj_job *j, int batches)
{
    LONG ioerr = 0;
    ULONG k;
    char dest[UA_PATHMAX];
    const char *first = j->archive;

    /* One volume of a set stands for the set; an incomplete set is refused
     * before anything is read, naming the volume that is missing. */
    snprintf(j->planname, sizeof(j->planname), "%s", j->archive);
    if (ua_volumes_find(j->archive, &j->vols)) {
        if (j->vols.problem[0] || !j->vols.count) {
            snprintf(j->err, sizeof(j->err), "Incomplete multi-volume set: %s.",
                     j->vols.problem[0] ? j->vols.problem : "the other volumes were not found");
            j->open_status = UA_OPEN_ERROR;
            return -1;
        }
        first = j->vols.path[0];
        j->list.volumes = j->vols.path;
        snprintf(j->planname, sizeof(j->planname), "%s", j->vols.setname);
        note(j, "Volumes: %d, from %s", j->vols.count, j->vols.path[0]);
    }
    if (ua_sniff_file(first, &j->fmt, &ioerr) < 0) {
        Fault(ioerr, NULL, j->err, sizeof(j->err));
        j->open_status = UA_OPEN_ERROR;
        return -1;
    }
    j->list.reading = on_reading;
    j->list.reading_ctx = j;
    j->list.password = j->password[0] ? j->password : NULL;
    j->open_status = ua_read_list(first, j->fmt, batches ? UA_OP_LIST : UA_OP_EXTRACT,
                                  j->recover, NULL, &j->list, note, j);
    j->t_listed = uj_ticks_since(&j->started);
    if (j->open_status != UA_OPEN_OK && j->open_status != UA_OPEN_PARTIAL)
        return -1;
    if (j->dest[0])
        snprintf(dest, sizeof(dest), "%s", j->dest);
    else {
        BPTR l = Lock((STRPTR)j->archive, SHARED_LOCK), parent = l ? ParentDir(l) : BNULL;
        if (!parent || !NameFromLock(parent, dest, sizeof(dest)))
            dest[0] = 0;
        if (parent)
            UnLock(parent);
        if (l)
            UnLock(l);
    }
    if (dest[0] && ua_plan_make(&j->list, j->planname, dest, j->nowrap && !j->recover, &j->plan, j->err,
                                sizeof(j->err)) == 0)
        j->have_plan = 1;
    j->total_known = 1;
    for (k = 0; k < j->list.n; k++) {
        ua_entry *e = &j->list.v[k];
        if (e->type == UA_ET_FILE && !e->skip) {
            if (!e->size_valid)
                j->total_known = 0;
            j->total += e->size;
        }
    }
    if (batches)
        send_batches(j);
    return j->have_plan ? 0 : -1;
}

static void do_extract(uj_job *j)
{
    ua_extract_opts o;
    ULONG k;

    if (!j->have_plan && do_list(j, 0) < 0)
        return;
    /* The user chose to go on without the password: the encrypted entries
     * are left out, and counted for the result. */
    if (j->skip_encrypted && !j->list.password) {
        for (k = 0; k < j->list.n; k++)
            if (j->list.v[k].encrypted && !j->list.v[k].excluded) {
                j->list.v[k].excluded = 1;
                j->skipped_encrypted++;
            }
    }
    /* Encrypted entries and no password: stop before anything is written;
     * the GUI asks and starts the job again. */
    if (!j->lent && ua_list_encrypted_wanted(&j->list) && !j->list.password) {
        j->open_status = UA_OPEN_PASSWORD;
        j->list.pw_state = UA_PW_NEEDED;
        j->extract_rc = -1;
        return;
    }
    if (j->list.disk_image) {
        snprintf(j->err, sizeof(j->err), "this archive holds a disk image; writing disk images "
                 "is not implemented yet");
        j->extract_rc = -1;
        return;
    }
    memset(&o, 0, sizeof(o));
    o.conflict = j->conflict;
    if (o.conflict == UA_CONFLICT_ASK) {
        o.ask = on_ask;
        o.ask_ctx = j;
    }
    o.progress = on_file;
    o.tick = on_tick;
    o.message = on_message;
    o.ui_ctx = j;
    j->total = 0;
    j->total_known = 1;
    for (k = 0; k < j->list.n; k++) {
        ua_entry *e = &j->list.v[k];
        if (e->type == UA_ET_FILE && !e->skip && !e->excluded) {
            if (!e->size_valid)
                j->total_known = 0;
            j->total += e->size;
        }
    }
    j->extract_rc = ua_extract(j->list.volumes ? j->list.volumes[0] : j->archive, &j->list,
                               &j->plan, &o, &j->rep, j->err,
                               sizeof(j->err));
    progress(j, NULL, j->rep.bytes, j->rep.files, 1);
}

static void pk_progress(void *ctx, const char *name, UQUAD done)
{
    uj_job *j = ctx;
    collect(j, 0);
    if (j->progress_out || uj_ticks_since(&j->last_tick) < UJ_TICKS)
        return;
    DateStamp(&j->last_tick);
    j->progress.bytes = done;
    j->progress.files = j->prep.files;
    snprintf(j->progress.name, sizeof(j->progress.name), "%s", name);
    j->progress_out = 1;
    send(j, &j->progress, UJM_PROGRESS);
}

static void do_pack(uj_job *j)
{
    int i;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    /* The total for the gauge: the size of plain files given directly; a
     * drawer's size is not known without walking it, so then there is none. */
    j->total = 0;
    j->total_known = 1;
    for (i = 0; i < j->ninputs && fib; i++) {
        BPTR l = Lock((STRPTR)j->inputs[i], SHARED_LOCK);
        if (l && Examine(l, fib) && fib->fib_DirEntryType < 0)
            j->total += UA_FIB_SIZE(fib);
        else
            j->total_known = 0;
        if (l)
            UnLock(l);
    }
    FreeDosObject(DOS_FIB, fib);
    j->pack.progress = pk_progress;
    j->pack.tick = pk_progress;
    j->pack.message = on_message;
    j->pack.ui_ctx = j;
    j->extract_rc = ua_pack(j->archive, j->inputs, j->ninputs, &j->pack, &j->prep, j->err,
                            sizeof(j->err));
    j->open_status = UA_OPEN_OK;
}

static void worker_main(void)
{
    uj_job *j = (uj_job *)FindTask(NULL)->tc_UserData;

    /* The parent has stored our Process pointer before we run anything that
     * could end the job (see uj_start). */
    Wait(SIGBREAKF_CTRL_F);
    j->wport = CreateMsgPort();
    if (!j->wport) {
        snprintf(j->err, sizeof(j->err), "not enough memory");
        j->open_status = UA_OPEN_ERROR;
    } else if (j->kind == UJ_LIST) {
        do_list(j, 1);
    } else if (j->kind == UJ_PACK) {
        do_pack(j);
    } else {
        do_extract(j);
    }
    if (j->wport) {
        while (batches_out(j) || j->progress_out)
            collect(j, 1);
        DeleteMsgPort(j->wport);
        j->wport = NULL;
    }
    /* Forbid, send, return: the GUI may unload the program as soon as it has
     * this message, and must not get the CPU before this task has gone. From
     * here on nobody may Signal this task (uj_cancel checks 'exiting'). */
    Forbid();
    j->exiting = 1;
    j->done.type = UJM_DONE;
    j->done.job = j;
    j->done.m.mn_Length = sizeof(j->done);
    j->done.m.mn_ReplyPort = NULL;
    PutMsg(j->gui, &j->done.m);
}

int uj_start(uj_job *j)
{
    struct Process *p;
    DateStamp(&j->started);
    j->last_tick = j->started;
    /* No console: the core reports through the job, never with printf.
     * NP_HomeDir and NP_CurrentDir are given so that the worker does not
     * duplicate the program's locks (the drawer could not be deleted while it
     * ran, and an unreleased copy would pin it for good). WindowPtr -1: a
     * missing volume is an error in the result, not a requester the worker
     * would wait on with nobody able to see why. */
    /* A name per job: a second Unarc running its own job must not be taken
     * for ours by uj_wait_gone. */
    snprintf(j->pname, sizeof(j->pname), "%s %p", WORKER_NAME, (void *)j);
    p = CreateNewProcTags(NP_Entry, (IPTR)worker_main, NP_Name, (IPTR)j->pname,
                          NP_StackSize, 262144, NP_Priority, 0, NP_UserData, (IPTR)j,
                          NP_Input, 0, NP_Output, 0, NP_Error, 0, NP_CloseInput, FALSE,
                          NP_CloseOutput, FALSE, NP_CurrentDir, 0, NP_HomeDir, 0,
                          NP_WindowPtr, (IPTR)-1, TAG_DONE);
    if (!p)
        return -1;
    j->proc = p;
    Signal(&p->pr_Task, SIGBREAKF_CTRL_F);
    return 0;
}

void uj_cancel(uj_job *j)
{
    /* The process exists until UJM_DONE has been sent, and the GUI calls this
     * only before it has received UJM_DONE. */
    if (!j || !j->proc)
        return;
    /* The task is gone as soon as it has sent UJM_DONE, which may be queued
     * here unread; 'exiting' is set under the same Forbid as that send. */
    Forbid();
    if (!j->exiting)
        Signal(&j->proc->pr_Task, SIGBREAKF_CTRL_C);
    Permit();
}

void uj_wait_gone(uj_job *j)
{
    for (;;) {
        struct Task *t;
        Forbid();
        t = FindTask((STRPTR)j->pname);
        Permit();
        if (!t)
            return;
        Delay(1);
    }
}
