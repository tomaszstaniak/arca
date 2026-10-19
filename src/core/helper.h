/* SPDX-License-Identifier: MIT */
/* Helper programs (unarc-7z, unarc-lha): programs of their own next to Unarc,
 * run as child processes, never linked in (their licences differ from
 * Unarc's). helper.c starts one and reads its output. */
#ifndef UA_HELPER_H
#define UA_HELPER_H

#include <exec/types.h>
#include <dos/dos.h>
#include <stddef.h>

typedef struct ua_helper {
    struct Process *proc;
    struct Task *parent;
    BYTE sig;                 /* signalled by the exit code */
    volatile int exited;
    volatile LONG rc;         /* the program's return code, once exited */
    BPTR rd;                  /* our end of the output pipe */
    struct MsgPort *port;
    struct DosPacket *dp;
    int cancelled;            /* Ctrl-C was passed on */
    int poll;                 /* output through a file, read as it grows */
    int drained;              /* polled: read once more after the end */
    char outname[64];         /* that file, deleted at the end */
} ua_helper;

/* PROGDIR:name, if that file is there: 1 and its full path in path. */
int ua_helper_find(const char *name, char *path, size_t n);

/* Start the program at path. args: its argument line, ending in "\n";
 * curdir: a lock for its current directory (duplicated; BNULL: none);
 * err_to_out: its error output goes to the same pipe as its output,
 * otherwise to NIL:. Input is NIL:. 0, or -1 with the reason in err. */
int ua_helper_start(ua_helper *h, const char *path, const char *name, const char *args,
                    BPTR curdir, int err_to_out, char *err, int errlen);
/* The same, with the output going to a file in T: that reads return as it
 * grows. PIPE: answers a read only when it can fill the whole buffer, so
 * output written a few bytes at a time (a running indicator) would arrive
 * only in large pieces. */
int ua_helper_start_polled(ua_helper *h, const char *path, const char *name, const char *args,
                           BPTR curdir, int err_to_out, char *err, int errlen);

/* One read of the output (0 at its end). Ctrl-C to us while waiting is
 * passed on to the program once, and sets h->cancelled. */
LONG ua_helper_read(ua_helper *h, void *buf, LONG len);

/* Ctrl-C to the program, if it still runs. */
void ua_helper_poke(ua_helper *h);

/* Reads the output to its end, waits for the program to end and frees what
 * start allocated. Returns the program's return code (h->cancelled stays
 * readable until the next start). */
LONG ua_helper_end(ua_helper *h);

#endif
