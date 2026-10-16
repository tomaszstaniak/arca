/* Host shim for tests/host/volumes_host.c: AROS types on POSIX. */
#ifndef SHIM_EXEC_TYPES_H
#define SHIM_EXEC_TYPES_H
typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned long long UQUAD;
typedef long IPTR;
typedef char *STRPTR;
typedef long BPTR;
#define BNULL 0
#endif
