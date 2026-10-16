# 7-Zip backend probe

A statically linked build of the 7-Zip 26.04 archive handlers for 7z, RAR
(1.5 to 4.x) and RAR5, extract only, for AROS ABIv11 x86_64. It comes with a
small front end, `unarc-7z`, that lists an archive or writes one entry to
stdout using the machine protocol in [PROTOCOL.md](PROTOCOL.md). The purpose
is to show that the handlers build, link and work outside 7-Zip's own UI and
file system layer, before an engine for aros-unarc is written on top of them.

Status, 2026-10-16:
- The AROS build compiles and links with no undefined symbols.
- It has **not been run on AROS**.
- The same sources built for macOS pass all host checks (below).

## Files

| File | What it is |
|---|---|
| `build.sh` | Rebuilds everything from clean: `build.sh [aros\|host\|all]` (default `all`; `host` needs clang++) |
| `sources.txt` | The 7-Zip translation units in the library (111, paths relative to the 7-Zip tree) |
| `unarc7z.cpp` | The front end (MIT); also defines the interface GUIDs |
| `aros_system.cpp` | AROS replacement for `NWindows::NSystem::GetRamSize` (MIT) |
| `shim/aros_shim.h` | Force-included in the AROS build; see "Workarounds" |
| `host_test.py` | Host checks against 7zz, lsar and unar: `python3 -I host_test.py` |
| `PROTOCOL.md` | The stdout protocol |

Outputs (in `build/7z-backend/`, ignored by git):

| Path | What it is |
|---|---|
| `aros/lib7zbackend.a` | 112 objects, 1,550,732 B |
| `aros/bin/unarc-7z` | the AROS program |
| `host/lib7zbackend.a`, `host/bin/unarc-7z` | macOS arm64 build of the same sources |
| `test/` | generated test archives (`enc.7z`, `hdr.7z`, `rar-enc/*.rar`) |
| `expected/*.list` | host `list` output (TZ=UTC) for comparing the AROS run |

## Source and toolchain

- 7-Zip 26.04, ip7z/7zip commit 9128b80, from `../deps/7zip`. It is used
  read-only, and `SZ_SRC=<dir>` overrides the location.
- GCC 10.5.0 for ABIv11: `UNARC_TOOLCHAIN_ABIV11` in `local.env`. Sysroot:
  `UNARC_SDK_ABIV11` (SDK-202609), passed as `--sysroot`.
- Host: Apple clang, `-std=c++11`.

## Build

```
experiments/7z-backend/build.sh          # AROS, then host
experiments/7z-backend/build.sh aros     # AROS only
```

Every unit is compiled with:

```
-O2 -Wall -Wno-unknown-pragmas
-DZ7_ST -DZ7_EXTRACT_ONLY -DNDEBUG -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE
```

The AROS build adds `--sysroot=$UNARC_SDK_ABIV11 -I shim -include shim/aros_shim.h`.
C++ units also get `-I <7zip>/CPP`. The objects go into `lib7zbackend.a`.
The program is then linked like this:

```
x86_64-aros-g++ --sysroot=<SDK> -o unarc-7z unarc7z.o \
    -Wl,--whole-archive lib7zbackend.a -Wl,--no-whole-archive
```

On the host, `-Wl,-force_load,lib7zbackend.a` is used instead. After linking,
the script runs `x86_64-aros-nm -u` and fails if anything other than a weak
symbol is undefined. The reason: collect-aros can exit 0 with unresolved
symbols. A clean `build.sh all` takes about 16 s on this Mac and gives no
compiler warnings at `-Wall`.

## What is compiled

The list is in `sources.txt`. It follows `Bundles/Format7zF/Arc_gcc.mak`, cut
down to what these handlers need:

- **Handlers:** 7z (`7zDecode`, `7zExtract`, `7zHandler`, `7zHeader`, `7zIn`,
  `7zProperties`, `7zRegister`, `7zCompressionMode`), `RarHandler`,
  `Rar5Handler`, and the registry `ArchiveExports`.
- **Codecs:**
  - LZMA, LZMA2, PPMd (7z variant H and RAR3's `Ppmd7aDec`);
  - BCJ, BCJ2 and the branch filters (`BranchMisc`: ARM, ARMT, ARM64, PPC,
    SPARC, IA64, RISCV), ByteSwap, Delta, Copy;
  - Deflate, Deflate64 and BZip2 decoders, which 7z archives may also use;
  - RAR 1.5, 2.x, 3.x/4.x (including the VM filters) and 5.
- **Crypto:** 7zAES, AES (`MyAes`), RAR 2.0 crypto, RAR 3/4 AES, RAR5 AES,
  PBKDF2-HMAC-SHA1/SHA256, SHA-1, SHA-256 and BLAKE2sp.
- **Support:**
  - `CPP/Common` strings, CRC and UTF conversion, and `MyWindows`
    (BSTR/PROPVARIANT on POSIX);
  - `CPP/Windows/PropVariant*` and `TimeUtils`;
  - the stream helpers in `CPP/7zip/Common`;
  - `CoderMixer2`, `FindSignature`, `HandlerOut`, `ItemNameUtils`,
    `OutStreamWithCRC` and `ParseProperties`.

Not compiled: the whole `CPP/Windows` file system layer (`FileDir`,
`FileFind`, `FileIO`, `FileName`, `System`) and `FileStreams`. It uses POSIX
`*at` calls that AROS lacks. `unarc-7z` has its own input stream over stdio.
`DllExports2`/`CodecExports` and every multithreading unit are also left out.

## Workarounds, and why

1. **Own compiler command lines, not upstream's makefile.gcc.** Upstream adds
   `-fPIC`, which redefines `__PIC__` on this compiler, and `-Werror`, which
   turns that into a failure. Neither flag is used here. AROS executables are
   not PIC.
2. **`-DZ7_ST`** (single-threaded) and **`-DZ7_EXTRACT_ONLY`**. These leave
   out the thread and encoder code.
3. **`shim/aros_shim.h`, pthread.h.** `C/Threads.h` includes `<pthread.h>`
   even with `Z7_ST`. It is reached through `Windows/System.h` from
   `Archive/Common/HandlerOut.h`. The SDK's `pthread.h` pulls in:
   - `exec/types.h`, whose `BYTE` (signed char), `WORD` (signed) and `BOOL`
     (short) conflict with 7-Zip's `MyWindows.h`;
   - `exec/lists.h`, whose `GetHead`/`GetTail`/... macros break 7-Zip
     identifiers.

   The shim includes `pthread.h` once, first, with those three names
   renamed, and then undefines the list macros. The AROS headers' include
   guards stop them from being read again. Nothing in this build calls
   pthreads.
4. **`shim/aros_shim.h`, `towupper`/`towlower`.** The SDK's `<wctype.h>` is
   empty. The stand-ins map ASCII only. 7-Zip uses them in `MyString.cpp` for
   case-insensitive comparisons, not on the decode path.
5. **`aros_system.cpp`.** `Rar5Handler.cpp` and `HandlerOut.h` reference
   `NWindows::NSystem::GetRamSize`. Upstream `System.cpp` needs `sysinfo()`
   and `_SC_NPROCESSORS_CONF`, which the SDK lacks. The replacement returns
   `AvailMem(MEMF_TOTAL)`. It includes no 7-Zip headers, because exec's
   headers bring their own `BSTR` (`dos/bptr.h`). It is compiled without the
   shim. Only the RAR5 `memx` property calls it, and `unarc-7z` never sets
   that property. The host build compiles upstream `System.cpp` instead.
6. **`C/Lzma2DecMt.c` is needed even with `Z7_ST`.** `Lzma2Decoder.cpp`
   always uses its handle API. The real multithreaded paths stay disabled.
7. **GUIDs.** `unarc7z.cpp` includes `Common/MyInitGuid.h`, which defines
   every `IID_*`, and it defines `CLSID_CArchiveHandler`. Upstream does both
   in `DllExports2.cpp`, which is not built here.
8. **Whole-archive link.** Every handler and codec registers itself from a
   static constructor in an object that nothing references (`REGISTER_ARC_*`,
   `REGISTER_CODEC_*`). A plain link of the `.a` would drop them all, and
   `GetNumberOfFormats` would return 0.
9. **Handler lookup without dynamic loading.** The front end uses three calls
   from `ArchiveExports.cpp`: `GetNumberOfFormats`, `GetHandlerProperty2`
   (name, class id, signatures) and `CreateArchiver`. It picks the handler by
   matching the registered signatures against the first 64 bytes of the file,
   so the file is opened by its own handler only. The archive must start at
   offset 0: SFX archives are not searched.
10. **stdout.** Output goes through stdio, and stdout is flushed before exit,
    because on mainline AROS built before 2026-09-24 a redirected stdout can lose
    stdio output that was not flushed.

## AROS binary

- `build/7z-backend/aros/bin/unarc-7z`: 1,886,496 B, not stripped.
- SHA-256 `4db6247169b38191bb2b2b1564515ec66cfd20c68b8f91692e6e5715f4454ffc`.
  Two clean builds gave the same bytes.
- `size`: text 620,572 B, data 32,980 B, bss 1,608,664 B.
- `x86_64-aros-nm -u`: empty.
- Libraries the startup code opens: crt and stdlib (`__aros_libset_CrtBase`,
  `__aros_libset_StdlibBase`), plus exec and DOS.
- 32 static constructors (`_GLOBAL__sub_I_*`). collect-aros puts them in
  `__CTORS_LIST__` (272 bytes: 32 entries plus count and terminator), the
  same layout a one-line C++ test program gets from this toolchain.
- The SDK's static pthread code (`__pthread_Init_Func`/`__pthread_Exit_Func`)
  is linked in through libstdc++, as in any C++ program from this toolchain.
  7-Zip itself creates no threads.
- A safe strip (`--strip-unneeded --remove-section .comment`; a full strip
  produced executables whose relocations AROS One did not apply) gives 1,433,192 B. The build does not
  strip.
- The largest stack frame in the library, from a one-off `-fstack-usage`
  build, is 2,112 B (`ReadZeroTail`). The RAR table readers use about 1.5 KB.

## Host results (macOS, 2026-10-16)

`python3 -I host_test.py`: 65 checks, 0 failures. They cover:

- `build/corpus/tree.7z` (solid LZMA2, 6 entries): names, directory flags and
  sizes equal `7zz l -slt`. All 3 files extract byte-identical to
  `7zz e -so`.
- `r5.part01.rar` (8-part RAR5 set):
  - `A Rar5 8 multivolume`;
  - names, directory flags and sizes equal `lsar -j` (2 entries);
  - both files are byte-identical to `unar`'s output.
- `r4.part1.rar` (4-part RAR4 set):
  - `A Rar 4 multivolume`;
  - 7 entries, all equal to `lsar`;
  - 4 of 4 regular files are byte-identical to `unar`. One is 241,647,978 B
    and spans every volume.
  - The symlink `testlink` is listed with attribute `a1ff8000` and was not
    compared, because unar writes a link.
- `old.rar` (`old.rar`, `old.r00`, `old.r01`, `old.r02`): same results as
  `r4.part1.rar`. Old-style volume names work.
- `gap.part1.rar` (part3 missing): `R 4 Missing volume : gap.part3.rar`, after
  the entry from part1 and part2.
- `tree.tar.gz`: `R 5 unsupported format`. tar and gz are not in this build.
- `test/enc.7z` (7zz, `-psecret`, encrypted data, plain headers):
  - list with no password: entries, `R 0`, flags `entries-encrypted`;
  - extract with no password: `R 1`;
  - extract with a wrong password: `R 2 data error in encrypted entry (wrong password?)`;
  - with `secret`: all files byte-identical to 7zz.
- `test/hdr.7z` (7zz, `-psecret -mhe=on`):
  - list with no password: `R 1 password required`;
  - list with a wrong password: `R 2 cannot open encrypted archive (wrong password?)`;
  - with `secret`: listing equals 7zz, and the files are byte-identical.
- RAR4 and RAR5 encrypted archives from libarchive's test suite
  (`test_read_format_rar{4,5}_[solid_]encrypted[_filenames].rar.uu`, decoded
  into `test/rar-enc/`, password `password`):
  - header-encrypted ones: list gives `R 1` with no password and `R 2` with a
    wrong one;
  - data-encrypted ones: list `R 0` without a password;
  - extract b.txt: `R 1` with no password, `R 2` with a wrong one, the right
    bytes with `password`;
  - d.txt with its own password: the right bytes.
  - RAR5 reports `wrong password` directly. RAR4 gives the data-error form.

For RAR, the reference is lsar/unar (The Unarchiver), because Homebrew's 7zz
has no RAR codecs. Comparisons cover name, directory flag and size, plus
content via SHA-256. Times and attributes were not compared.

RAR 1.5-4.x times are local times converted with the process's time zone:
the same archive lists different `mtime` values under different `TZ`.
`expected/*.list` was produced with `TZ=UTC`. On AROS, expect RAR4 times to
differ from it by the offset AROS applies.

## Results on AROS (2026-10-16)

`tests/guest/make_7z_cases.py` stages the AROS binary with 38 cases and the
host's output for each: listing without a password, with a wrong one and with
the right one, then extracting one entry. The cases cover 7z, encrypted 7z,
7z with encrypted headers, encrypted RAR 4 and RAR 5 (names and data),
solid RAR 4, RAR 5 and RAR 4 multi-volume sets (part names and old `.rNN`
names) and a RAR 4 set with volume 3 missing, opened at parts 1, 2 and 4.
Passwords come from files on stdin; the script runs in a Shell with
`Stack 40960`.

On both systems (ABIv11 2026.09 base, pool v11-3; AROS One 1.3, pool v11-1)
all 38 outputs are byte-identical to the host's, return codes included.
The first comparison differed in 9 cases, all by exactly 7,200 seconds and
only for archives that store local DOS times (RAR 4): 7-Zip turns those
into UTC with the process's time zone, which was CEST on the host and none
on AROS. With the host run in UTC the outputs match, so the script now
does that. The guest therefore reports the stored wall-clock time, which is
what an AROS file date needs; the adapter must not convert it again.

What the missing-volume cases show, on AROS and on the host alike: opened
at part 1 or 2, 7-Zip lists the entry that runs into the gap and ends with
record `R 4 Missing volume : gap.part3.rar`; opened at part 4, it lists the
entries that start in part 4 and ends with `R 0 ok`, and extracting one of
them succeeds. 7-Zip does not look backwards for earlier volumes. The
adapter must therefore resolve the set with aros-unarc's own finder
(`src/core/volumes.c`, which reports volume 3 missing for any part) before
it calls this program, and always pass the first volume.

## Not verified, and what is missing

- **Exceptions, stack and memory.** The wrong-password and missing-volume
  cases return the host's codes, so the error paths run, but memory use over
  repeated runs and the stack high-water mark were not measured.
- **Stack.** Shells on a default AROS One have 40,960 bytes of stack. The
  frame sizes above suggest that is enough, but this has not been measured.
  Test at `Stack 40960`.
- **Large files.** Offsets use `fseeko`/`ftello` (`fseeko64` in the SDK).
  Archives over 2 or 4 GiB on AROS file systems have not been tried.
- **CPU features.** AES, SHA and BLAKE2sp choose AES-NI, SHA-NI, SSE or AVX2
  code at run time. AVX2 is used only if CPUID reports OSXSAVE and XCR0 allows
  it. Under AROS this has not been checked.
- **RAR4 names without the Unicode flag** are decoded as UTF-8
  (`g_ForceToUTF8` is true on non-Windows). OEM/CP437 names come out wrong.
  The test sets have ASCII names only.
- **Case-insensitive comparisons** are ASCII-only (workaround 4).
- **An empty password line** counts as "no password", so an archive with an
  empty password cannot be opened.
- **Formats.** No 7z split volumes (`name.7z.001`; the Split handler is not
  built), no SFX search, and nothing beyond 7z/RAR/RAR5.
- **Codec coverage.** Deflate, Deflate64, BZip2, PPMd and the non-x86 branch
  filters inside 7z are compiled but not exercised by these tests.
- **Extract to stdout only.** `extract` buffers in memory when the size is
  unknown. Writing files, times and protection bits is left to aros-unarc's
  own file layer.
- **Other targets.** Mainline (ABIv1) and aarch64 builds were not attempted.

## Licensing

`unarc7z.cpp`, `aros_system.cpp`, `shim/aros_shim.h` and `host_test.py` are
ours, MIT.

The 7-Zip sources are GNU LGPL 2.1 or later, and some files are public domain
(`DOC/License.txt`). The RAR decoders (`CPP/7zip/Compress/Rar*`) are LGPL with
the **unRAR license restriction**: the code may not be used to develop a
RAR-compatible archiver, that is, to create RAR archives. This build only
extracts, which the restriction allows. Dropping `RarHandler`, `Rar5Handler`,
`Rar*Decoder`, `Rar3Vm`, `RarCodecsRegister` and the `Rar*` crypto units gives
a build without that restriction. A binary that ships these sources must carry
the 7-Zip licence text and the unRAR restriction.
