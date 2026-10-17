/* Host shim: the DOS calls volumes.c makes, on POSIX. A lock is an index into
 * a table of paths, a file handle a FILE pointer. Seek behaves like ABIv11's:
 * a LONG position in, the old position cut to a LONG out.
 * Test hooks: shim_sfs (as SFS on ABIv11: any seek in a file over 2 GB
 * fails), shim_exnext_fail (ExNext fails with an error after that many
 * names), shim_seek_calls / shim_seek_max (largest step asked for). */
#ifndef SHIM_PROTO_DOS_H
#define SHIM_PROTO_DOS_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dos/dos.h>
static char *shim_paths[256];
static DIR *shim_dirs[256];
static int shim_n;
static char shim_cwd[4096] = ".";
static BPTR shim_cur;
static LONG shim_ioerr;
static int shim_exnext_fail = -1, shim_exnext_count;
static int shim_sfs;          /* behave like SFS: no seek at all in a file over 2 GB */
static long shim_seek_calls;
static void SetIoErr(LONG e) { shim_ioerr = e; }
static LONG IoErr(void) { return shim_ioerr; }
static BPTR Lock(STRPTR n, LONG m) { struct stat st; (void)m;
    if (stat(*n ? n : ".", &st)) return 0; shim_paths[++shim_n] = strdup(*n ? n : "."); return shim_n; }
static void UnLock(BPTR l) { (void)l; }
static BPTR CurrentDir(BPTR l) { BPTR o = shim_cur; shim_cur = l;
    if (l) snprintf(shim_cwd, sizeof(shim_cwd), "%s", shim_paths[l]); return o; }
static BPTR Open(STRPTR n, LONG m) { char p[4096]; (void)m;
    if (n[0] == '/') snprintf(p, sizeof(p), "%s", n);
    else snprintf(p, sizeof(p), "%s/%s", shim_cwd, n);
    return (BPTR)fopen(p, "rb"); }
static int ExamineFH(BPTR f, struct FileInfoBlock *fib) { struct stat st;
    if (fstat(fileno((FILE *)f), &st)) return 0; fib->fib_Size = st.st_size;
    fib->fib_Date.ds_Days = (LONG)(st.st_mtime / 86400); fib->fib_Date.ds_Minute = (LONG)(st.st_mtime % 86400 / 60);
    fib->fib_Date.ds_Tick = (LONG)(st.st_mtime % 60 * 50); return 1; }
static LONG Read(BPTR f, void *b, LONG n) { return (LONG)fread(b, 1, n, (FILE *)f); }
static LONG Seek(BPTR f, LONG p, LONG m) {
    FILE *fp = (FILE *)f; off_t old = ftello(fp), to;
    struct stat st;
    shim_seek_calls++;
    fstat(fileno(fp), &st);
    if (shim_sfs && st.st_size > 0x7fffffffLL) { shim_ioerr = ERROR_SEEK_ERROR; return -1; }
    to = m == OFFSET_BEGINNING ? (off_t)p : m == OFFSET_CURRENT ? old + p : st.st_size + p;
    if (to < 0 || to > st.st_size || fseeko(fp, to, SEEK_SET)) { shim_ioerr = ERROR_SEEK_ERROR; return -1; }
    return (LONG)old; }
static LONG Close(BPTR f) { fclose((FILE *)f); return 1; }
static LONG FPuts(BPTR f, const char *s) { return fputs(s, (FILE *)f) < 0; }
static int DeleteFile(STRPTR n) { return remove(n) == 0; }
static void *AllocDosObject(LONG t, void *x) { (void)t; (void)x; return calloc(1, sizeof(struct FileInfoBlock)); }
static void FreeDosObject(LONG t, void *o) { (void)t; free(o); }
static int Examine(BPTR l, struct FileInfoBlock *f) { (void)f; shim_exnext_count = 0;
    shim_dirs[l] = opendir(shim_paths[l]); return shim_dirs[l] != 0; }
static int ExNext(BPTR l, struct FileInfoBlock *f) { struct dirent *d;
    if (shim_exnext_fail >= 0 && shim_exnext_count++ >= shim_exnext_fail) {
        closedir(shim_dirs[l]); shim_ioerr = 218; return 0; }
    while ((d = readdir(shim_dirs[l]))) { if (d->d_name[0] == '.') continue;
        snprintf(f->fib_FileName, sizeof(f->fib_FileName), "%s", d->d_name);
        f->fib_DirEntryType = d->d_type == DT_DIR ? 2 : -3; return 1; }
    closedir(shim_dirs[l]); shim_ioerr = ERROR_NO_MORE_ENTRIES; return 0; }
static STRPTR FilePart(STRPTR p) { char *s = strrchr(p, '/'); return s ? s + 1 : p; }
#endif
