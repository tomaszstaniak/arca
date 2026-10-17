// SPDX-License-Identifier: MIT
// Included first in the ZIP handler's units (build.sh). Upstream has no
// extract-only ZIP build: its options class derives from CMultiMethodProps,
// which HandlerOut.h leaves out under Z7_EXTRACT_ONLY. Every unit here is
// compiled with Z7_EXTRACT_ONLY, because that macro changes shared classes
// (FilterCoder.h, MyAes.h) and they must look the same in every unit. The
// handler reads only the common decoding options, so this stands in for it.
#ifndef UNARC_ZIP_EXTRACT_ONLY_H
#define UNARC_ZIP_EXTRACT_ONLY_H
#include "7zip/Archive/Common/HandlerOut.h"
namespace NArchive {
class CMultiMethodProps: public CCommonMethodProps
{
public:
  void Init() { InitCommon(); }
};
}
#endif
