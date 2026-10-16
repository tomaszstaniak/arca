/* Host shim: the DOS calls volumes.c makes, on POSIX. A lock is an index into
 * a table of paths, a file handle a FILE pointer. */
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
static BPTR Lock(STRPTR n, LONG m) { struct stat st; (void)m;
    if (stat(*n ? n : ".", &st)) return 0; shim_paths[++shim_n] = strdup(*n ? n : "."); return shim_n; }
static void UnLock(BPTR l) { (void)l; }
static BPTR CurrentDir(BPTR l) { BPTR o = shim_cur; shim_cur = l;
    if (l) snprintf(shim_cwd, sizeof(shim_cwd), "%s", shim_paths[l]); return o; }
static BPTR Open(STRPTR n, LONG m) { char p[4096]; (void)m;
    snprintf(p, sizeof(p), "%s/%s", shim_cwd, n); return (BPTR)fopen(p, "rb"); }
static LONG Read(BPTR f, void *b, LONG n) { return (LONG)fread(b, 1, n, (FILE *)f); }
static LONG Seek(BPTR f, LONG p, LONG m) { (void)m; return fseek((FILE *)f, p, SEEK_SET) ? -1 : 0; }
static LONG Close(BPTR f) { fclose((FILE *)f); return 1; }
static void *AllocDosObject(LONG t, void *x) { (void)t; (void)x; return calloc(1, sizeof(struct FileInfoBlock)); }
static void FreeDosObject(LONG t, void *o) { (void)t; free(o); }
static int Examine(BPTR l, struct FileInfoBlock *f) { (void)f; shim_dirs[l] = opendir(shim_paths[l]); return shim_dirs[l] != 0; }
static int ExNext(BPTR l, struct FileInfoBlock *f) { struct dirent *d;
    while ((d = readdir(shim_dirs[l]))) { if (d->d_name[0] == '.') continue;
        snprintf(f->fib_FileName, sizeof(f->fib_FileName), "%s", d->d_name);
        f->fib_DirEntryType = d->d_type == DT_DIR ? 2 : -3; return 1; }
    closedir(shim_dirs[l]); return 0; }
static STRPTR FilePart(STRPTR p) { char *s = strrchr(p, '/'); return s ? s + 1 : p; }
#endif
