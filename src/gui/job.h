/* SPDX-License-Identifier: MIT */
/* One job (list or extract) run by a worker process over the core, reporting
 * to the GUI through exec messages. The worker never touches a Zune object and
 * never opens a requester; it asks, and the GUI answers.
 *
 * Ownership and lifetime
 * - The GUI allocates the job with uj_new() and frees it with uj_free() only
 *   after it has received UJM_DONE. Every message is embedded in the job.
 * - A message belongs to the GUI from GetMsg() until it is replied, and to the
 *   worker otherwise. UJM_DONE is never replied: the worker sends it under
 *   Forbid() and exits, and the GUI waits for the task to be gone
 *   (uj_wait_gone) before the program can unload the code.
 * - Every message except UJM_DONE must be replied, also when the job is
 *   cancelled or the window closes: the worker waits for them.
 * - LIST: the worker builds job->list and job->plan. They do not change after
 *   the first UJM_BATCH; the GUI may read the entries a batch names as soon
 *   as it has the batch. With UJM_DONE both belong to the GUI, which takes
 *   them with uj_take_list().
 * - EXTRACT: the GUI lends its list and plan to the job (uj_lend_list) and
 *   must not change or free them until UJM_DONE; then they come back.
 * - Strings in a message are copies inside the message, valid until reply.
 * - job->log is written by the worker only and read by the GUI after
 *   UJM_DONE; it is freed by uj_free.
 * - PACK: job->inputs (strings and array) belong to the job, set by the GUI
 *   before uj_start and freed by uj_free; job->archive is the new archive.
 *
 * Flow: at most UJ_BATCHES batches are out at a time, so a fast engine cannot
 * fill memory with messages; one progress message at a time, and not more
 * often than every UJ_TICKS ticks; one question at a time. */
#ifndef UA_JOB_H
#define UA_JOB_H

#include <exec/ports.h>
#include "../core/ua.h"

#define UJ_BATCH    400
#define UJ_BATCHES  2
#define UJ_TICKS    5          /* 1/50 s each: progress at most 10 times a second */
#define UJ_LOGMAX   (64 * 1024)

typedef enum { UJ_LIST, UJ_EXTRACT, UJ_PACK } uj_kind;
enum { UJM_BATCH = 1, UJM_PROGRESS, UJM_ASK, UJM_DONE };

struct uj_job;

typedef struct uj_msg {
    struct Message m;
    ULONG type;
    struct uj_job *job;
    ULONG first, count;          /* BATCH: entries [first, first+count) */
    UQUAD bytes;                 /* PROGRESS: written so far */
    ULONG files;                 /* PROGRESS: files finished */
    ULONG listed;                /* PROGRESS while reading: entries read so far */
    int reading;                 /* PROGRESS: still reading the archive */
    char name[256];              /* PROGRESS: the file being written (display form) */
    char path[UA_PATHMAX];       /* ASK: the existing file */
    UQUAD esize;                 /* ASK: the entry that would replace it */
    int esize_valid, edate_valid;
    struct DateStamp edate;
    LONG answer;                 /* ASK, set by the GUI: UA_CONFLICT_* or -1 = cancel */
    LONG all;                    /* ASK, set by the GUI: the answer is for the rest too */
} uj_msg;

typedef struct uj_job {
    uj_kind kind;
    char archive[UA_PATHMAX];
    char dest[UA_PATHMAX];       /* where to extract; "" = the archive's drawer */
    int recover;                 /* explicit recovery, never automatic */
    int nowrap;                  /* straight into dest, no new folder */
    int test_delay;              /* test aid: ticks to wait before each batch */
    ua_conflict conflict;        /* ASK unless the GUI decided for every file */
    char **inputs;               /* PACK: files and drawers to pack */
    int ninputs;
    ua_pack_opts pack;           /* PACK: format, level, replace */
    ua_pack_report prep;         /* PACK: the result */
    ua_volumes vols;             /* the archive's volume set, if any (the worker's) */
    char planname[UA_PATHMAX];   /* names the new folder: the set's name or the archive */
    char password[512];          /* UTF-8, "" for none; cleared by uj_free, never logged */

    /* Results */
    int open_status;             /* UA_OPEN_* */
    ua_format fmt;
    ua_list list;
    ua_plan plan;
    int have_plan;
    UQUAD total;                 /* bytes to write, when every size is known */
    int total_known;
    ua_report rep;
    int extract_rc;              /* ua_extract's return */
    char err[256];
    char *log;                   /* engine notes, warnings, errors: one per line */
    ULONG loglen, loglines, logdropped;

    /* Messages */
    struct MsgPort *gui;
    struct MsgPort *wport;       /* the worker's reply port, made by the worker */
    uj_msg batch[UJ_BATCHES], progress, ask, done;
    int batch_out[UJ_BATCHES], progress_out;
    struct DateStamp last_tick;
    struct DateStamp started;    /* test aid: times in the debug log */
    LONG t_listed;               /* ticks from start to the end of listing */
    struct Process *proc;
    volatile int exiting;        /* set under Forbid just before UJM_DONE is sent */
    char pname[40];              /* the worker's task name, unique per job */
    int lent;                    /* list and plan belong to the GUI (EXTRACT) */
} uj_job;

uj_job *uj_new(uj_kind kind, const char *archive, struct MsgPort *gui);
/* EXTRACT: give the job the GUI's list and plan for its duration. */
void uj_lend_list(uj_job *j, ua_list *l, ua_plan *p);
/* After UJM_DONE: take the list and plan (LIST) or have them back (EXTRACT). */
void uj_take_list(uj_job *j, ua_list *l, ua_plan *p);
/* Starts the worker. 0, or -1 when no process could be made (nothing runs). */
int uj_start(uj_job *j);
/* Asks the worker to stop; it ends with UJM_DONE as always. */
void uj_cancel(uj_job *j);
/* Waits until the worker task has left the system (after UJM_DONE). */
void uj_wait_gone(uj_job *j);
void uj_free(uj_job *j);
/* 1/50 s since a date stamp; for throttling and test timings. */
int uj_ticks_since(const struct DateStamp *a);

#endif
