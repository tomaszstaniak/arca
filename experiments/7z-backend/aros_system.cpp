// SPDX-License-Identifier: MIT
// AROS replacement for the one function of CPP/Windows/System.cpp that the
// handlers in this build reference. Upstream's System.cpp needs sysinfo(),
// sys/sysinfo.h and _SC_NPROCESSORS_CONF, none of which the ABIv11 SDK has.
// Only Rar5Handler's "memx" property (memory limit given as a share of RAM)
// calls it, and unarc-7z never sets that property.

// No 7-Zip header here: exec's headers bring their own BSTR (dos/bptr.h) and
// BYTE/WORD/BOOL, which conflict with MyWindows.h. The declaration matches
// CPP/Windows/System.h.
#include <stddef.h>
#include <proto/exec.h>
#include <exec/memory.h>

namespace NWindows {
namespace NSystem {

bool GetRamSize(size_t &size)
{
  size = (size_t)AvailMem(MEMF_TOTAL);
  return size != 0;
}

}}
