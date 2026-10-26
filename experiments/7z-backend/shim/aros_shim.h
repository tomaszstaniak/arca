/* SPDX-License-Identifier: MIT */
/* Force-included (-include) into every unit of the AROS builds of the 7-Zip
 * sources. Two problems of the SDK headers are handled here (the second is
 * ABIv11's only: build.sh defines UNARC_SDK_HAS_TOWUPPER for mainline). */
#ifndef UNARC_AROS_SHIM_H
#define UNARC_AROS_SHIM_H

/* 1. C/Threads.h includes <pthread.h> even with Z7_ST (it is reached through
 * Windows/System.h from Archive/Common/HandlerOut.h). The SDK's pthread.h
 * pulls in exec/types.h, whose BYTE (signed), WORD (signed) and BOOL (short)
 * conflict with 7-Zip's MyWindows.h typedefs, and exec/lists.h, whose GetHead/
 * GetTail/... macros break 7-Zip identifiers. Include pthread.h once, first,
 * with the AROS names renamed, then drop the list macros. The include guards
 * keep the AROS headers from being read again later. Nothing in the
 * single-threaded build calls into pthreads. */
#define BYTE aros_exec_BYTE
#define WORD aros_exec_WORD
#define BOOL aros_exec_BOOL
#include <pthread.h>
#undef BYTE
#undef WORD
#undef BOOL
#undef GetHead
#undef GetTail
#undef GetSucc
#undef GetPred
#undef IsListEmpty
#undef REMOVE

/* 2. The SDK's <wctype.h> is empty, so towupper/towlower are missing. These
 * stand-ins map ASCII only; 7-Zip uses them for case-insensitive name
 * comparison in MyString.cpp (MyCharUpper/MyCharLower), not for decoding. */
#if defined(__cplusplus) && !defined(UNARC_SDK_HAS_TOWUPPER)
#include <stddef.h>
#include <wchar.h>
static inline wint_t towupper(wint_t c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static inline wint_t towlower(wint_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
#endif

#endif
