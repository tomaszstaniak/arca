// SPDX-License-Identifier: MIT
// unarc-7z: probe front end for the 7-Zip archive handlers (7z, RAR, RAR5)
// linked statically. Prints the machine protocol of PROTOCOL.md on stdout.
//
//   unarc-7z list <archive>
//   unarc-7z extract <archive> <index>
//
// A password, when the archive asks for one, is read as one line from stdin.

#include "Common/Common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Common/MyWindows.h"
// Defines every IID_* and IID_IUnknown in this translation unit; it must come
// before the interface headers.
#include "Common/MyInitGuid.h"

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/UTFConvert.h"
#include "Windows/PropVariant.h"

#include "7zip/IStream.h"
#include "7zip/IPassword.h"
#include "7zip/ICoder.h"
#include "7zip/PropID.h"
#include "7zip/Archive/IArchive.h"

// ArchiveExports.cpp refers to this class id; upstream defines it in
// DllExports2.cpp, which is not part of this build.
Z7_DEFINE_GUID(CLSID_CArchiveHandler,
    k_7zip_GUID_Data1,
    k_7zip_GUID_Data2,
    k_7zip_GUID_Data3_Common,
    0x10, 0x00, 0x00, 0x01, 0x10, 0x00, 0x00, 0x00);

// Static registration table, ArchiveExports.cpp.
STDAPI CreateArchiver(const GUID *clsid, const GUID *iid, void **outObject);
STDAPI GetHandlerProperty2(UInt32 formatIndex, PROPID propID, PROPVARIANT *value);
STDAPI GetNumberOfFormats(UINT32 *numFormats);

using namespace NWindows;

enum
{
  kCodeOk = 0,
  kCodeNeedPassword = 1,
  kCodeWrongPassword = 2,
  kCodeDataError = 3,
  kCodeMissingVolume = 4,
  kCodeUnsupported = 5,
  kCodeOther = 9
};

// ---------------------------------------------------------------- output

static void Out(const char *s) { fputs(s, stdout); }

static void OutU64(UInt64 v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
  Out(buf);
}

// The message must stay on one line and must not contain the field separator.
static void Finish(int code, const char *msg)
{
  printf("R\t%d\t", code);
  for (const char *p = msg; *p; p++)
    fputc((*p == '\t' || *p == '\n' || *p == '\r') ? ' ' : *p, stdout);
  fputc('\n', stdout);
  // Redirected stdout on AROS can lose unflushed stdio output at exit.
  fflush(stdout);
}

static void OutHexUtf8(const UString &u)
{
  AString a;
  ConvertUnicodeToUTF8(u, a);
  static const char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < a.Len(); i++)
  {
    const unsigned char c = (unsigned char)a[i];
    fputc(hex[c >> 4], stdout);
    fputc(hex[c & 15], stdout);
  }
}

// ---------------------------------------------------------------- paths

static AString ToUtf8(const UString &u)
{
  AString a;
  ConvertUnicodeToUTF8(u, a);
  return a;
}

static UString FromUtf8(const char *s)
{
  AString a(s);
  UString u;
  ConvertUTF8ToUnicode(a, u);
  return u;
}

// Directory part including the separator; works for "dir/x", "Vol:x", "Vol:dir/x".
static AString DirPrefix(const AString &path)
{
  int pos = -1;
  for (unsigned i = 0; i < path.Len(); i++)
    if (path[i] == '/' || path[i] == ':')
      pos = (int)i;
  if (pos < 0)
    return AString();
  AString d(path);
  d.DeleteFrom((unsigned)pos + 1);
  return d;
}

static AString BaseName(const AString &path)
{
  const AString d = DirPrefix(path);
  return AString(path.Ptr(d.Len()));
}

// ---------------------------------------------------------------- password

// Shared by the open and the extract callbacks: the line is read from stdin
// at most once, only when a handler asks, and is never echoed or logged.
struct CPasswordState
{
  bool Asked;
  bool Read;
  bool Defined;
  UString Password;
  CPasswordState(): Asked(false), Read(false), Defined(false) {}

  HRESULT Get(BSTR *password)
  {
    Asked = true;
    if (!Read)
    {
      Read = true;
      char buf[1024];
      if (fgets(buf, sizeof(buf), stdin))
      {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
          buf[--n] = 0;
        if (n > 0)
        {
          Password = FromUtf8(buf);
          Defined = true;
        }
      }
      memset(buf, 0, sizeof(buf));
    }
    if (!Defined)
      return E_ABORT;
    return StringToBstr(Password, password);
  }
};

static CPasswordState g_Password;

// ---------------------------------------------------------------- input stream

// IInStream over C stdio with 64-bit offsets (fseeko/ftello).
Z7_CLASS_IMP_IInStream(CStdioInStream)
  FILE *_f;
public:
  CStdioInStream(): _f(NULL) {}
  ~CStdioInStream() { if (_f) fclose(_f); }
  bool Open(const char *path)
  {
    _f = fopen(path, "rb");
    return _f != NULL;
  }
};

Z7_COM7F_IMF(CStdioInStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0)
    return S_OK;
  const size_t n = fread(data, 1, size, _f);
  if (processedSize)
    *processedSize = (UInt32)n;
  if (n == 0 && ferror(_f))
    return E_FAIL;
  return S_OK;
}

Z7_COM7F_IMF(CStdioInStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  int whence;
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: whence = SEEK_SET; break;
    case STREAM_SEEK_CUR: whence = SEEK_CUR; break;
    case STREAM_SEEK_END: whence = SEEK_END; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (fseeko(_f, (off_t)offset, whence) != 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  const off_t pos = ftello(_f);
  if (pos < 0)
    return E_FAIL;
  if (newPosition)
    *newPosition = (UInt64)pos;
  return S_OK;
}

// ---------------------------------------------------------------- open callback

class COpenCallback Z7_final:
  public IArchiveOpenCallback,
  public IArchiveOpenVolumeCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_3(IArchiveOpenCallback, IArchiveOpenVolumeCallback, ICryptoGetTextPassword)
public:
  AString Dir;        // directory of the first volume, with separator
  UString FirstName;  // file name of the first volume
  unsigned NumVolumes;
  bool PasswordAskedOnOpen;
  COpenCallback(): NumVolumes(1), PasswordAskedOnOpen(false) {}
};

Z7_COM7F_IMF(COpenCallback::SetTotal(const UInt64 *, const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(COpenCallback::SetCompleted(const UInt64 *, const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(COpenCallback::GetProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  if (propID == kpidName)
    prop = FirstName;
  prop.Detach(value);
  return S_OK;
}

// The RAR handlers compute the next volume name (name.part2.rar, name.r00, ...)
// from kpidName and ask for it here; S_FALSE means "not there".
Z7_COM7F_IMF(COpenCallback::GetStream(const wchar_t *name, IInStream **inStream))
{
  *inStream = NULL;
  AString path = Dir;
  path += ToUtf8(UString(name));
  CStdioInStream *spec = new CStdioInStream;
  CMyComPtr<IInStream> s = spec;
  if (!spec->Open(path))
    return S_FALSE;
  NumVolumes++;
  *inStream = s.Detach();
  return S_OK;
}

Z7_COM7F_IMF(COpenCallback::CryptoGetTextPassword(BSTR *password))
{
  PasswordAskedOnOpen = true;
  return g_Password.Get(password);
}

// ---------------------------------------------------------------- stdout sink

// Writes the D record before the first byte. With a known size the record
// carries that size and the stream is kept to exactly that many bytes (cut,
// or zero-padded at the end; the R line then reports the error), so that a
// reader always finds the R line. With an unknown size the data is buffered.
Z7_CLASS_IMP_COM_1(CStdoutSink, ISequentialOutStream)
public:
  UInt32 Index;
  bool SizeKnown;
  UInt64 Size;
  UInt64 Written;      // bytes the decoder produced
  bool HeaderOut;
  bool Overflow;
  Byte *Buf;           // only for unknown size
  size_t BufLen, BufCap;

  CStdoutSink(): Index(0), SizeKnown(false), Size(0), Written(0), HeaderOut(false),
      Overflow(false), Buf(NULL), BufLen(0), BufCap(0) {}
  ~CStdoutSink() { free(Buf); }

  void Header(UInt64 size)
  {
    printf("D\t%u\t", (unsigned)Index);
    OutU64(size);
    fputc('\n', stdout);
    HeaderOut = true;
  }

  // Called after the handler returned; false if the bytes were not what the
  // record promised.
  bool Close()
  {
    if (!SizeKnown)
    {
      if (Written == 0)
        return false;
      Header(BufLen);
      fwrite(Buf, 1, BufLen, stdout);
      return true;
    }
    if (!HeaderOut)
    {
      if (Size != 0)
        return false;      // nothing decoded: no D record at all
      Header(0);
      return true;
    }
    bool ok = !Overflow;
    if (Written < Size)
    {
      ok = false;
      static const char zeros[4096] = { 0 };
      UInt64 rem = Size - Written;
      while (rem)
      {
        const size_t n = rem < sizeof(zeros) ? (size_t)rem : sizeof(zeros);
        fwrite(zeros, 1, n, stdout);
        rem -= n;
      }
    }
    return ok;
  }
};

Z7_COM7F_IMF(CStdoutSink::Write(const void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = size;
  if (size == 0)
    return S_OK;
  if (!SizeKnown)
  {
    if (BufLen + size > BufCap)
    {
      size_t cap = BufCap ? BufCap * 2 : (1 << 16);
      while (cap < BufLen + size)
        cap *= 2;
      Byte *p = (Byte *)realloc(Buf, cap);
      if (!p)
        return E_OUTOFMEMORY;
      Buf = p;
      BufCap = cap;
    }
    memcpy(Buf + BufLen, data, size);
    BufLen += size;
    Written += size;
    return S_OK;
  }
  if (!HeaderOut)
    Header(Size);
  UInt64 take = size;
  if (Written >= Size)
    take = 0;
  else if (Written + take > Size)
    take = Size - Written;
  if (take < size)
    Overflow = true;
  if (take)
    fwrite(data, 1, (size_t)take, stdout);
  Written += size;
  return S_OK;
}

// ---------------------------------------------------------------- extract callback

class CExtractCallback Z7_final:
  public IArchiveExtractCallback,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IArchiveExtractCallback, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
public:
  UInt32 Wanted;
  CStdoutSink *SinkSpec;
  CMyComPtr<ISequentialOutStream> Sink;
  bool Seen;
  Int32 OpRes;
  CExtractCallback(): Wanted(0), SinkSpec(NULL), Seen(false), OpRes(-1) {}
};

Z7_COM7F_IMF(CExtractCallback::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CExtractCallback::SetCompleted(const UInt64 *)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::GetStream(UInt32 index, ISequentialOutStream **outStream, Int32 askExtractMode))
{
  *outStream = NULL;
  // The result that follows belongs to this index, whatever the mode: a
  // handler that already knows the data is corrupt (a solid 7z block after a
  // wrong password) asks for the wanted entry in test mode, not extract mode.
  Seen = (index == Wanted);
  if (!Seen || askExtractMode != NArchive::NExtract::NAskMode::kExtract)
    return S_OK;
  CMyComPtr<ISequentialOutStream> s = Sink;
  *outStream = s.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::PrepareOperation(Int32)) { return S_OK; }

Z7_COM7F_IMF(CExtractCallback::SetOperationResult(Int32 opRes))
{
  if (Seen && OpRes < 0)
    OpRes = opRes;
  Seen = false;
  return S_OK;
}

Z7_COM7F_IMF(CExtractCallback::CryptoGetTextPassword(BSTR *password))
{
  return g_Password.Get(password);
}

// ---------------------------------------------------------------- helpers

static bool GetBool(IInArchive *arc, UInt32 index, PROPID id, bool &v)
{
  NCOM::CPropVariant prop;
  v = false;
  if (arc->GetProperty(index, id, &prop) != S_OK || prop.vt != VT_BOOL)
    return false;
  v = (prop.boolVal != VARIANT_FALSE);
  return true;
}

static bool GetU64(IInArchive *arc, UInt32 index, PROPID id, UInt64 &v)
{
  NCOM::CPropVariant prop;
  v = 0;
  if (arc->GetProperty(index, id, &prop) != S_OK)
    return false;
  switch (prop.vt)
  {
    case VT_UI8: v = prop.uhVal.QuadPart; return true;
    case VT_UI4: v = prop.ulVal; return true;
    case VT_UI2: v = prop.uiVal; return true;
    case VT_UI1: v = prop.bVal; return true;
    default: return false;
  }
}

static bool ArcPropBool(IInArchive *arc, PROPID id)
{
  NCOM::CPropVariant prop;
  return arc->GetArchiveProperty(id, &prop) == S_OK && prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

static UInt32 ArcPropU32(IInArchive *arc, PROPID id)
{
  NCOM::CPropVariant prop;
  if (arc->GetArchiveProperty(id, &prop) == S_OK && prop.vt == VT_UI4)
    return prop.ulVal;
  return 0;
}

static UString ArcPropStr(IInArchive *arc, PROPID id)
{
  NCOM::CPropVariant prop;
  if (arc->GetArchiveProperty(id, &prop) == S_OK && prop.vt == VT_BSTR)
    return UString(prop.bstrVal);
  return UString();
}

// ---------------------------------------------------------------- format choice

struct CFormat
{
  UInt32 Index;
  AString Name;
  GUID ClassId;
};

static bool GetFormat(UInt32 i, CFormat &f)
{
  f.Index = i;
  NCOM::CPropVariant prop;
  if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kName, &prop) != S_OK || prop.vt != VT_BSTR)
    return false;
  f.Name = ToUtf8(UString(prop.bstrVal));
  prop.Clear();
  if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kClassID, &prop) != S_OK || prop.vt != VT_BSTR
      || ::SysStringByteLen(prop.bstrVal) != sizeof(GUID))
    return false;
  memcpy(&f.ClassId, prop.bstrVal, sizeof(GUID));
  return true;
}

// Match the registered signatures against the start of the file, so that an
// archive is only ever opened by the handler it belongs to (a wrong password
// on 7z must not turn into "unsupported" after the RAR handlers refuse it).
static bool SignatureMatches(UInt32 i, const Byte *head, size_t headLen)
{
  NCOM::CPropVariant prop;
  UInt32 offs = 0;
  if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kSignatureOffset, &prop) == S_OK && prop.vt == VT_UI4)
    offs = prop.ulVal;
  prop.Clear();
  if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kSignature, &prop) == S_OK && prop.vt == VT_BSTR)
  {
    const UInt32 len = ::SysStringByteLen(prop.bstrVal);
    return len != 0 && offs + len <= headLen && memcmp(head + offs, prop.bstrVal, len) == 0;
  }
  prop.Clear();
  if (GetHandlerProperty2(i, NArchive::NHandlerPropID::kMultiSignature, &prop) == S_OK && prop.vt == VT_BSTR)
  {
    // A sequence of [length byte][signature bytes].
    const Byte *p = (const Byte *)prop.bstrVal;
    UInt32 rem = ::SysStringByteLen(prop.bstrVal);
    while (rem > 0)
    {
      const UInt32 len = p[0];
      if (len + 1 > rem)
        break;
      if (len != 0 && offs + len <= headLen && memcmp(head + offs, p + 1, len) == 0)
        return true;
      p += len + 1;
      rem -= len + 1;
    }
  }
  return false;
}

// ---------------------------------------------------------------- open

struct COpened
{
  CMyComPtr<IInArchive> Arc;
  COpenCallback *CbSpec;
  CMyComPtr<IArchiveOpenCallback> Cb;
  CFormat Format;
  COpened(): CbSpec(NULL) {}
};

// Returns kCodeOk, or an error code with msg set.
static int OpenArchive(const char *path, COpened &o, AString &msg)
{
  CStdioInStream *inSpec = new CStdioInStream;
  CMyComPtr<IInStream> in = inSpec;
  if (!inSpec->Open(path))
  {
    msg = "cannot open file";
    return kCodeOther;
  }
  Byte head[64];
  UInt32 got = 0;
  if (in->Read(head, sizeof(head), &got) != S_OK)
  {
    msg = "read error";
    return kCodeOther;
  }

  UInt32 num = 0;
  GetNumberOfFormats(&num);
  int found = -1;
  for (UInt32 i = 0; i < num; i++)
    if (SignatureMatches(i, head, got)) { found = (int)i; break; }
  if (found < 0 || !GetFormat((UInt32)found, o.Format))
  {
    msg = "unsupported format";
    return kCodeUnsupported;
  }

  if (CreateArchiver(&o.Format.ClassId, &IID_IInArchive, (void **)&o.Arc) != S_OK || !o.Arc)
  {
    msg = "cannot create handler";
    return kCodeOther;
  }

  o.CbSpec = new COpenCallback;
  o.Cb = o.CbSpec;
  const AString p(path);
  o.CbSpec->Dir = DirPrefix(p);
  o.CbSpec->FirstName = FromUtf8(BaseName(p));

  UInt64 pos;
  in->Seek(0, STREAM_SEEK_SET, &pos);
  const UInt64 maxCheck = 0;
  const HRESULT res = o.Arc->Open(in, &maxCheck, o.Cb);
  if (res == S_OK)
    return kCodeOk;

  if (o.CbSpec->PasswordAskedOnOpen)
  {
    if (!g_Password.Defined)
    {
      msg = "password required";
      return kCodeNeedPassword;
    }
    // 7-Zip itself can only say "cannot open encrypted archive. Wrong password?".
    msg = "cannot open encrypted archive (wrong password?)";
    return kCodeWrongPassword;
  }
  if (res == S_FALSE)
  {
    msg = "cannot open as archive";
    return kCodeDataError;
  }
  if (res == E_OUTOFMEMORY)
  {
    // 7zHandler::Open maps every exception, broken headers included, to this.
    msg = "out of memory or broken headers";
    return kCodeDataError;
  }
  msg = "open failed";
  return kCodeOther;
}

// Archive-wide state after a successful open.
static int ArchiveState(IInArchive *arc, AString &msg)
{
  const UString err = ArcPropStr(arc, kpidError);
  const UInt32 flags = ArcPropU32(arc, kpidErrorFlags);
  if (err.IsPrefixedBy_Ascii_NoCase("Missing volume"))
  {
    msg = ToUtf8(err);
    return kCodeMissingVolume;
  }
  if (flags & kpv_ErrorFlags_EncryptedHeadersError)
  {
    msg = "encrypted headers error (wrong password?)";
    return kCodeWrongPassword;
  }
  if (flags & kpv_ErrorFlags_UnexpectedEnd)
  {
    msg = "unexpected end of archive";
    return kCodeDataError;
  }
  if (flags & kpv_ErrorFlags_HeadersError)
  {
    msg = "headers error";
    return kCodeDataError;
  }
  if (flags & (kpv_ErrorFlags_UnsupportedMethod | kpv_ErrorFlags_UnsupportedFeature))
  {
    msg = "unsupported feature";
    return kCodeUnsupported;
  }
  if (flags & kpv_ErrorFlags_IsNotArc)
  {
    msg = "not an archive";
    return kCodeDataError;
  }
  if (!err.IsEmpty())
  {
    msg = ToUtf8(err);
    return kCodeDataError;
  }
  return kCodeOk;
}

// ---------------------------------------------------------------- commands

static int CmdList(const char *path)
{
  COpened o;
  AString msg;
  int code = OpenArchive(path, o, msg);
  if (code != kCodeOk)
  {
    Finish(code, msg);
    return code;
  }
  IInArchive *arc = o.Arc;
  UInt32 n = 0;
  arc->GetNumberOfItems(&n);

  bool anyEnc = false;
  for (UInt32 i = 0; i < n; i++)
  {
    bool b;
    if (GetBool(arc, i, kpidEncrypted, b) && b)
      anyEnc = true;
  }
  AString flags;
  if (ArcPropBool(arc, kpidSolid)) flags += "solid,";
  if (o.CbSpec->PasswordAskedOnOpen) flags += "headers-encrypted,";
  if (anyEnc) flags += "entries-encrypted,";
  if (o.CbSpec->NumVolumes > 1 || ArcPropBool(arc, kpidIsVolume)) flags += "multivolume,";
  if (flags.IsEmpty()) flags = "-"; else flags.DeleteBack();

  Out("A\t"); Out(o.Format.Name); Out("\t");
  OutU64(o.CbSpec->NumVolumes); Out("\t"); Out(flags); Out("\n");

  for (UInt32 i = 0; i < n; i++)
  {
    bool isDir = false, enc = false;
    GetBool(arc, i, kpidIsDir, isDir);
    GetBool(arc, i, kpidEncrypted, enc);
    UInt64 size, packed;
    const bool hasSize = GetU64(arc, i, kpidSize, size);
    const bool hasPacked = GetU64(arc, i, kpidPackSize, packed);

    NCOM::CPropVariant tprop;
    bool hasTime = false;
    Int64 unixTime = 0;
    if (arc->GetProperty(i, kpidMTime, &tprop) == S_OK && tprop.vt == VT_FILETIME)
    {
      const UInt64 ft = ((UInt64)tprop.filetime.dwHighDateTime << 32) | tprop.filetime.dwLowDateTime;
      // FILETIME: 100 ns units since 1601-01-01 UTC.
      unixTime = (Int64)(ft / 10000000) - (Int64)11644473600LL;
      hasTime = true;
    }
    NCOM::CPropVariant aprop;
    bool hasAttr = false;
    UInt32 attr = 0;
    if (arc->GetProperty(i, kpidAttrib, &aprop) == S_OK && aprop.vt == VT_UI4)
    {
      attr = aprop.ulVal;
      hasAttr = true;
    }
    NCOM::CPropVariant nprop;
    UString name;
    if (arc->GetProperty(i, kpidPath, &nprop) == S_OK && nprop.vt == VT_BSTR)
      name = nprop.bstrVal;

    printf("E\t%u\t%d\t", (unsigned)i, isDir ? 1 : 0);
    if (hasSize) OutU64(size); else Out("-");
    Out("\t");
    if (hasPacked) OutU64(packed); else Out("-");
    Out("\t");
    if (hasTime) printf("%lld", (long long)unixTime); else Out("-");
    Out("\t");
    if (hasAttr) printf("%x", (unsigned)attr); else Out("-");
    printf("\t%d\t", enc ? 1 : 0);
    OutHexUtf8(name);
    Out("\n");
  }

  code = ArchiveState(arc, msg);
  if (code == kCodeOk)
    msg = "ok";
  Finish(code, msg);
  arc->Close();
  return code;
}

static int CmdExtract(const char *path, const char *indexStr)
{
  char *end = NULL;
  const unsigned long idx = strtoul(indexStr, &end, 10);
  if (!*indexStr || *end)
  {
    Finish(kCodeOther, "bad index");
    return kCodeOther;
  }
  COpened o;
  AString msg;
  int code = OpenArchive(path, o, msg);
  if (code != kCodeOk)
  {
    Finish(code, msg);
    return code;
  }
  IInArchive *arc = o.Arc;
  UInt32 n = 0;
  arc->GetNumberOfItems(&n);
  if (idx >= n)
  {
    Finish(kCodeOther, "index out of range");
    return kCodeOther;
  }
  const UInt32 index = (UInt32)idx;

  bool enc = false, isDir = false;
  GetBool(arc, index, kpidEncrypted, enc);
  GetBool(arc, index, kpidIsDir, isDir);

  CExtractCallback *cbSpec = new CExtractCallback;
  CMyComPtr<IArchiveExtractCallback> cb = cbSpec;
  cbSpec->Wanted = index;
  cbSpec->SinkSpec = new CStdoutSink;
  cbSpec->Sink = cbSpec->SinkSpec;
  cbSpec->SinkSpec->Index = index;
  cbSpec->SinkSpec->SizeKnown = GetU64(arc, index, kpidSize, cbSpec->SinkSpec->Size);

  const HRESULT res = arc->Extract(&index, 1, 0, cb);
  const bool framed = cbSpec->SinkSpec->Close();

  if (g_Password.Asked && !g_Password.Defined && (res == E_ABORT || cbSpec->OpRes < 0))
  {
    Finish(kCodeNeedPassword, "password required");
    return kCodeNeedPassword;
  }
  if (res != S_OK)
  {
    code = (res == E_OUTOFMEMORY) ? kCodeOther : kCodeDataError;
    Finish(code, res == E_OUTOFMEMORY ? "out of memory" : "extract failed");
    return code;
  }

  // An archive-wide missing volume explains a cut entry better than the entry result.
  AString arcMsg;
  const int arcState = ArchiveState(arc, arcMsg);

  switch (cbSpec->OpRes)
  {
    case NArchive::NExtract::NOperationResult::kOK:
      code = framed ? kCodeOk : kCodeDataError;
      msg = framed ? "ok" : "size mismatch";
      break;
    case NArchive::NExtract::NOperationResult::kUnsupportedMethod:
      code = kCodeUnsupported; msg = "unsupported method"; break;
    case NArchive::NExtract::NOperationResult::kWrongPassword:
      code = kCodeWrongPassword; msg = "wrong password"; break;
    case NArchive::NExtract::NOperationResult::kDataError:
    case NArchive::NExtract::NOperationResult::kCRCError:
      if (enc)
      {
        // What 7-Zip reports for this case: "... in encrypted file. Wrong password?"
        code = kCodeWrongPassword;
        msg = "data error in encrypted entry (wrong password?)";
      }
      else
      {
        code = kCodeDataError;
        msg = cbSpec->OpRes == NArchive::NExtract::NOperationResult::kCRCError ? "CRC error" : "data error";
      }
      break;
    case NArchive::NExtract::NOperationResult::kUnavailable:
    case NArchive::NExtract::NOperationResult::kUnexpectedEnd:
      if (arcState == kCodeMissingVolume)
      {
        code = kCodeMissingVolume; msg = arcMsg;
      }
      else
      {
        code = kCodeDataError;
        msg = cbSpec->OpRes == NArchive::NExtract::NOperationResult::kUnavailable ? "data unavailable" : "unexpected end";
      }
      break;
    case NArchive::NExtract::NOperationResult::kDataAfterEnd:
      code = kCodeDataError; msg = "data after end"; break;
    case NArchive::NExtract::NOperationResult::kIsNotArc:
    case NArchive::NExtract::NOperationResult::kHeadersError:
      code = kCodeDataError; msg = "headers error"; break;
    case -1:
      if (isDir)
      {
        // Handlers do not always ask for a stream for a directory.
        if (!cbSpec->SinkSpec->HeaderOut)
          printf("D\t%u\t0\n", (unsigned)index);
        code = kCodeOk; msg = "ok";
      }
      else
      {
        code = kCodeOther; msg = "entry was not processed";
      }
      break;
    default:
      code = kCodeOther; msg = "unknown operation result"; break;
  }
  Finish(code, msg);
  arc->Close();
  return code;
}

static void Usage()
{
  fputs("usage: unarc-7z list <archive>\n"
        "       unarc-7z extract <archive> <index>\n"
        "A password, if needed, is read as one line from stdin.\n", stderr);
}

int main(int argc, char **argv)
{
  int code;
  try
  {
    if (argc == 3 && strcmp(argv[1], "list") == 0)
      code = CmdList(argv[2]);
    else if (argc == 4 && strcmp(argv[1], "extract") == 0)
      code = CmdExtract(argv[2], argv[3]);
    else
    {
      Usage();
      return 20;
    }
  }
  catch (...)
  {
    // 7-Zip reports allocation failures as CNewException and some internal
    // failures with other throws.
    Finish(kCodeOther, "exception");
    code = kCodeOther;
  }
  fflush(stdout);
  // The protocol carries the result; the exit code only says ok or not ok.
  return code == kCodeOk ? 0 : 10;
}
