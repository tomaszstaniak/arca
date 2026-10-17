/* Host shim for the host tests: AROS types on POSIX, with AROS's widths
 * (LONG is 32 bits there; a 64-bit long here would hide overflows). */
#ifndef SHIM_EXEC_TYPES_H
#define SHIM_EXEC_TYPES_H
#include <stdint.h>
typedef unsigned char UBYTE;
typedef unsigned short UWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef int64_t QUAD;
typedef uint64_t UQUAD;
typedef intptr_t IPTR;
typedef char *STRPTR;
typedef intptr_t BPTR;
#define BNULL 0
#endif
