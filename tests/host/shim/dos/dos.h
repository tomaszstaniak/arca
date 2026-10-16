/* Host shim: the DOS definitions volumes.c and ua.h use. */
#ifndef SHIM_DOS_DOS_H
#define SHIM_DOS_DOS_H
#include <exec/types.h>
#define MODE_OLDFILE 1005
#define SHARED_LOCK -2
#define OFFSET_BEGINNING -1
#define DOS_FIB 2
struct DateStamp { LONG ds_Days, ds_Minute, ds_Tick; };
struct FileInfoBlock { LONG fib_DirEntryType; char fib_FileName[108]; UQUAD fib_Size; };
#endif
