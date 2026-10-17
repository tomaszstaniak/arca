/* Host shim: the DOS definitions volumes.c and ua.h use. */
#ifndef SHIM_DOS_DOS_H
#define SHIM_DOS_DOS_H
#include <exec/types.h>
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define SHARED_LOCK -2
#define OFFSET_BEGINNING -1
#define OFFSET_CURRENT 0
#define OFFSET_END 1
#define DOS_FIB 2
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_SEEK_ERROR 219
#define FIBF_OTR_READ (1<<15)
#define FIBF_OTR_WRITE (1<<14)
#define FIBF_OTR_EXECUTE (1<<13)
#define FIBF_OTR_DELETE (1<<12)
#define FIBF_GRP_READ (1<<11)
#define FIBF_GRP_WRITE (1<<10)
#define FIBF_GRP_EXECUTE (1<<9)
#define FIBF_GRP_DELETE (1<<8)
#define FIBF_READ (1<<3)
#define FIBF_WRITE (1<<2)
#define FIBF_EXECUTE (1<<1)
#define FIBF_DELETE (1<<0)
struct DateStamp { LONG ds_Days, ds_Minute, ds_Tick; };
struct FileInfoBlock { LONG fib_DirEntryType; char fib_FileName[108]; UQUAD fib_Size; struct DateStamp fib_Date; };
#endif
