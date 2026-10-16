// SPDX-License-Identifier: MIT
// AROS replacement for the one function of CPP/Windows/System.cpp that the
// handlers in this build reference, and the Ctrl-C check for unarc7z.cpp. Upstream's System.cpp needs sysinfo(),
// sys/sysinfo.h and _SC_NPROCESSORS_CONF, none of which the ABIv11 SDK has.
// Only Rar5Handler's "memx" property (memory limit given as a share of RAM)
// calls it, and unarc-7z never sets that property.

// No 7-Zip header here: exec's headers bring their own BSTR (dos/bptr.h) and
// BYTE/WORD/BOOL, which conflict with MyWindows.h. The declaration matches
// CPP/Windows/System.h.
#include <stddef.h>
#include <proto/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>

// Ctrl-C, sent by the Shell or by unarc when the user cancels. Kept set, so
// that every later check in this run sees it too.
extern "C" int unarc_break_requested(void)
{
  return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) != 0;
}

namespace NWindows {
namespace NSystem {

bool GetRamSize(size_t &size)
{
  size = (size_t)AvailMem(MEMF_TOTAL);
  return size != 0;
}

}}
