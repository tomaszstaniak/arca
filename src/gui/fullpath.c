/* SPDX-License-Identifier: MIT */
/* fullpath: see fullpath.h. */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <proto/dos.h>
#include "../core/ua.h"
#include "fullpath.h"

/* A requester or a typed field can give a name relative to this process's
 * current drawer ("//sub" after Parent, an empty drawer for the one Arca was
 * started in), and PROGDIR: is this process's program drawer. The worker has
 * neither, so such a name would be looked up from the boot volume's root
 * there: "object not found", or for a save another place. Every name is made
 * absolute here, before a job or the settings keep it. A name that does not
 * exist yet (a drawer an extraction makes) is resolved through the nearest
 * drawer that does. 0 when that fails: the caller must not go on with the name
 * as given (full_path_why says why). */
char fp_why[120];
int fp_test_fail;

static int needs_resolving(const char *in)
{
    const char *colon = strchr(in, ':');
    return !colon || colon == in || (colon - in == 7 && !strncasecmp(in, "PROGDIR", 7));
}

int full_path(const char *in, char *out, size_t n)
{
    char head[UA_PATHMAX];
    const char *rest = "";
    BPTR l = BNULL;
    size_t hl;
    int ok;
    fp_why[0] = 0;
    if (!needs_resolving(in)) {
        if ((size_t)snprintf(out, n, "%s", in) < n)
            return 1;
        snprintf(fp_why, sizeof(fp_why), "The name is too long.");
        return 0;
    }
    if (fp_test_fail) {
        snprintf(fp_why, sizeof(fp_why), "Its drawer could not be named (UnarcTestNoFullPath).");
        return 0;
    }
    if ((size_t)snprintf(head, sizeof(head), "%s", in) >= sizeof(head)) {
        snprintf(fp_why, sizeof(fp_why), "The name is too long.");
        return 0;
    }
    for (;;) {
        char *slash;
        l = Lock((STRPTR)head, SHARED_LOCK);
        if (l || !head[0])
            break;
        slash = strrchr(head, '/');
        /* "a/b" goes up to "a", "PROGDIR:a" to "PROGDIR:", but in "//b" the
         * slashes are the parents. */
        if (!slash) {
            char *colon = strchr(head, ':');
            hl = colon ? (size_t)(colon - head) + 1 : 0;
        } else if (slash == head || slash[-1] == '/' || slash[-1] == ':')
            hl = slash - head + 1;
        else
            hl = slash - head;
        if (hl >= strlen(head))
            break;
        rest = in + (slash && hl == (size_t)(slash - head) ? hl + 1 : hl);
        head[hl] = 0;
    }
    if (!l) {
        Fault(IoErr(), NULL, fp_why, sizeof(fp_why));
        if (!fp_why[0])
            snprintf(fp_why, sizeof(fp_why), "No part of it could be found.");
        return 0;
    }
    ok = NameFromLock(l, out, n);
    if (!ok)
        Fault(IoErr(), NULL, fp_why, sizeof(fp_why));
    else if (*rest && !AddPart((STRPTR)out, (STRPTR)rest, n)) {
        ok = 0;
        snprintf(fp_why, sizeof(fp_why), "The name is too long.");
    }
    UnLock(l);
    if (!ok && !fp_why[0])
        snprintf(fp_why, sizeof(fp_why), "Its drawer could not be named.");
    return ok;
}
