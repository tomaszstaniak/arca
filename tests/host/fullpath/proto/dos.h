/* Host shim for src/gui/fullpath.c: the DOS calls it makes, over a fixed
 * table of names as a process sees them (current drawer RAM:oa/app/Arca,
 * program drawer RAM:oa/app/Arca). Test hooks: fp_shim_nfl_fail makes
 * NameFromLock fail with ERROR_OBJECT_NOT_FOUND, fp_shim_locks counts the
 * locks not unlocked. */
#ifndef FP_SHIM_PROTO_DOS_H
#define FP_SHIM_PROTO_DOS_H
#include <stdio.h>
#include <string.h>
#include <dos/dos.h>
#ifndef ERROR_OBJECT_NOT_FOUND
#define ERROR_OBJECT_NOT_FOUND 205
#endif
#ifndef ERROR_LINE_TOO_LONG
#define ERROR_LINE_TOO_LONG 120
#endif
static const char *const fp_shim_names[][2] = {
    { "", "RAM:oa/app/Arca" }, { "/", "RAM:oa/app" }, { "//", "RAM:oa" },
    { "//sub", "RAM:oa/sub" }, { "//sub/small.7z", "RAM:oa/sub/small.7z" },
    { "Licences", "RAM:oa/app/Arca/Licences" }, { ":", "RAM:" },
    { "PROGDIR:", "RAM:oa/app/Arca" }, { "PROGDIR:Licences", "RAM:oa/app/Arca/Licences" },
    { "progdir:", "RAM:oa/app/Arca" },
};
static int fp_shim_nfl_fail, fp_shim_locks;
static LONG fp_shim_ioerr;
static LONG IoErr(void) { return fp_shim_ioerr; }
static BPTR Lock(STRPTR n, LONG m)
{
    size_t i;
    (void)m;
    for (i = 0; i < sizeof(fp_shim_names) / sizeof(fp_shim_names[0]); i++)
        if (!strcmp(fp_shim_names[i][0], n)) {
            fp_shim_locks++;
            return (BPTR)(i + 1);
        }
    fp_shim_ioerr = ERROR_OBJECT_NOT_FOUND;
    return 0;
}
static void UnLock(BPTR l) { if (l) fp_shim_locks--; }
static LONG NameFromLock(BPTR l, STRPTR out, LONG n)
{
    if (fp_shim_nfl_fail) {
        fp_shim_ioerr = ERROR_OBJECT_NOT_FOUND;
        return 0;
    }
    if ((LONG)strlen(fp_shim_names[l - 1][1]) >= n) {
        fp_shim_ioerr = ERROR_LINE_TOO_LONG;
        return 0;
    }
    snprintf(out, n, "%s", fp_shim_names[l - 1][1]);
    return 1;
}
/* As dos.library: FALSE when the result does not fit, out then unchanged. */
static LONG AddPart(STRPTR out, STRPTR part, ULONG n)
{
    size_t k = strlen(out), need = k + (k && out[k - 1] != ':' ? 1 : 0) + strlen(part) + 1;
    if (need > n)
        return 0;
    if (k && out[k - 1] != ':')
        strcat(out, "/");
    strcat(out, part);
    return 1;
}
static LONG Fault(LONG code, STRPTR hdr, STRPTR buf, LONG n)
{
    (void)hdr;
    return snprintf(buf, n, code == ERROR_OBJECT_NOT_FOUND ? "object not found" :
                    code == ERROR_LINE_TOO_LONG ? "line is too long" : "error %ld",
                    (long)code);
}
#endif
