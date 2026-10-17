// SPDX-License-Identifier: MIT
// The ZIP handler always declares the writing interface (IOutArchive and
// ISetProperties); upstream has no extract-only ZIP build. unarc-7z only
// reads, so the three methods ZipHandlerOut.cpp would bring are stubs here
// (see also zip_extract_only.h).
#include "Common/Common.h"

#include "7zip/Archive/Zip/ZipHandler.h"

namespace NArchive {
namespace NZip {

Z7_COM7F_IMF(CHandler::GetFileTimeType(UInt32 *timeType))
{
  *timeType = NFileTimeType::kDOS;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::UpdateItems(ISequentialOutStream *, UInt32, IArchiveUpdateCallback *))
{
  return E_NOTIMPL;
}

Z7_COM7F_IMF(CHandler::SetProperties(const wchar_t * const *, const PROPVARIANT *, UInt32))
{
  return S_OK;
}

}}
