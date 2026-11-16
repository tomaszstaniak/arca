#!/bin/bash
# Rebuild the 7-Zip backend probe from clean: lib7zbackend.a and unarc-7z,
# for AROS ABIv11 x86_64 (aros), mainline AROS ABIv1 x86_64 (aros-v1) and,
# if clang++ is present, for the host.
# usage: build.sh [aros|aros-v1|aros-raspi|host|all]   (default all: aros and host)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
. "$ROOT/local.env"
SZ=${SZ_SRC:-$ROOT/deps/7zip}   # pinned in deps.lock; scripts/fetch-deps.sh
OUT=$ROOT/build/7z-backend
WHAT=${1:-all}

# Single-threaded and extract-only (no thread or encoder code); the other
# defines are upstream's makefile.gcc ones.
DEFS=(-DZ7_ST -DZ7_EXTRACT_ONLY -DNDEBUG -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE)
COMMON=(-O2 -Wall -Wno-unknown-pragmas)

# Upstream units from sources.txt (relative to $SZ), plus one per-target unit
# given as an absolute path.
list_sources() {
  grep -v '^[[:space:]]*#' "$HERE/sources.txt" | grep -v '^[[:space:]]*$' | sed "s|^|$SZ/|"
  case $1 in
    aros*) echo "$HERE/aros_system.cpp" ;;     # GetRamSize via exec AvailMem
    host) echo "$SZ/CPP/Windows/System.cpp" ;;  # upstream, builds on macOS
  esac
  echo "$HERE/zip_out_stub.cpp"
}


build_target() {
  local t=$1 CC CXX AR NM extra_c extra_cxx tc sdk unwind=() link_flags=()
  case $t in
    aros|aros-v1)
      if [ "$t" = aros ]; then
        : "${UNARC_TOOLCHAIN_ABIV11:?set in local.env}" "${UNARC_SDK_ABIV11:?set in local.env}"
        tc=$UNARC_TOOLCHAIN_ABIV11 sdk=$UNARC_SDK_ABIV11
      else
        : "${UNARC_TOOLCHAIN_V1:?set in local.env}" "${UNARC_SDK_V1:?set in local.env}"
        tc=$UNARC_TOOLCHAIN_V1 sdk=$UNARC_SDK_V1
        # Mainline's libgcc catches no throw; 7-Zip throws on bad data
        # (scripts/build-unwind-fix-v1.sh).
        unwind=("$ROOT/build/v1-x86_64/unwind-fix/unwind-dw2.o")
        [ -f "${unwind[0]}" ] || { echo "missing ${unwind[0]}: run scripts/build-unwind-fix-v1.sh" >&2; exit 1; }
      fi
      CC="$tc/x86_64-aros-gcc --sysroot=$sdk"
      CXX="$tc/x86_64-aros-g++ --sysroot=$sdk"
      AR="$tc/x86_64-aros-ar"
      NM="$tc/x86_64-aros-nm"
      # No -fPIC: upstream adds it, and on this compiler it redefines __PIC__,
      # which -Werror turns into a failure; AROS executables are not PIC anyway.
      # aros_shim.h: pthread.h (still reached through C/Threads.h) included with
      # exec's BYTE/WORD/BOOL renamed, and towupper/towlower, which the SDK's
      # empty wctype.h lacks. See README.md.
      extra_c=(-I"$HERE/shim" -include "$HERE/shim/aros_shim.h")
      extra_cxx=(-I"$HERE/shim" -include "$HERE/shim/aros_shim.h")
      # Mainline's wctype.h declares towupper/towlower; ABIv11's does not.
      [ "$t" = aros-v1 ] && extra_cxx+=(-DUNARC_SDK_HAS_TOWUPPER)
      ;;
    aros-raspi)
      : "${UNARC_TOOLCHAIN_RASPI:?set in local.env}" "${UNARC_SDK_RASPI:?set in local.env}"
      : "${UNARC_CXXRT_RASPI:?set to the maintained LLVM 20.1 AROS runtime}"
      tc=$UNARC_TOOLCHAIN_RASPI sdk=$UNARC_SDK_RASPI
      export RPI_SDK="$sdk"
      CC="$tc/aarch64-aros-clang"
      CXX="$tc/aarch64-aros-clang++"
      AR="$tc/aarch64-aros-ar"
      NM="$tc/aarch64-aros-nm"
      [ -f "$UNARC_CXXRT_RASPI/lib/libc++.a" ] || { echo "missing Pi libc++ runtime" >&2; exit 1; }
      # Keep plain-char semantics consistent with the x86 builds. Native TLS
      # is not supported by the Pi loader; exceptions use LLVM's AROS unwinder.
      extra_c=(-fsigned-char -femulated-tls -Werror=implicit-function-declaration
        -I"$HERE/shim" -include "$HERE/shim/aros_shim.h")
      extra_cxx=(-fsigned-char -femulated-tls -nostdinc++
        -isystem "$UNARC_CXXRT_RASPI/include/c++/v1"
        -I"$HERE/shim" -include "$HERE/shim/aros_shim.h" -DUNARC_SDK_HAS_TOWUPPER)
      link_flags=(-L"$UNARC_CXXRT_RASPI/lib" -stdlib=libc++ -femulated-tls)
      ;;
    host)
      CC="clang"; CXX="clang++"; AR="ar"; NM="nm"
      # Keep names that are not UTF-8 byte for byte, as on AROS (UTFConvert.cpp
      # leaves this off on macOS only).
      extra_c=(); extra_cxx=(-std=c++11 -DMY_UTF8_RAW_NON_UTF8_SUPPORTED)
      ;;
  esac
  local B=$OUT/$t
  rm -rf "$B"; mkdir -p "$B/obj" "$B/bin"
  local objs=() n=0
  while IFS= read -r src; do
    local base; base=$(basename "$src"); base=${base%.*}
    local o=$B/obj/$base.o
    case $src in
      *.c)   $CC  "${COMMON[@]}" "${DEFS[@]}" ${extra_c[@]+"${extra_c[@]}"}   -c "$src" -o "$o" ;;
      */zip_out_stub.cpp|*/Archive/Zip/*)
             # No extract-only ZIP upstream: zip_extract_only.h fills the gap.
             $CXX "${COMMON[@]}" "${DEFS[@]}" ${extra_cxx[@]+"${extra_cxx[@]}"} -I"$SZ/CPP" \
               -include "$HERE/zip_extract_only.h" -c "$src" -o "$o" ;;
      */aros_system.cpp)  # plain AROS code, no 7-Zip headers, no shim
             $CXX "${COMMON[@]}" -c "$src" -o "$o" ;;
      *.cpp) $CXX "${COMMON[@]}" "${DEFS[@]}" ${extra_cxx[@]+"${extra_cxx[@]}"} -I"$SZ/CPP" -c "$src" -o "$o" ;;
    esac
    objs+=("$o"); n=$((n+1))
  done < <(list_sources "$t")
  rm -f "$B/lib7zbackend.a"
  $AR rcs "$B/lib7zbackend.a" "${objs[@]}"
  echo "$t: $n translation units -> $B/lib7zbackend.a"

  $CXX "${COMMON[@]}" "${DEFS[@]}" ${extra_cxx[@]+"${extra_cxx[@]}"} -I"$SZ/CPP" \
    -c "$HERE/unarc7z.cpp" -o "$B/obj/unarc7z.o"
  # The handlers and codecs register themselves from static constructors in
  # objects nothing else references, so the archive must be linked whole.
  if [ "$t" = host ]; then
    $CXX -o "$B/bin/unarc-7z" "$B/obj/unarc7z.o" -Wl,-force_load,"$B/lib7zbackend.a"
  else
    $CXX ${link_flags[@]+"${link_flags[@]}"} -o "$B/bin/unarc-7z" "$B/obj/unarc7z.o" ${unwind[@]+"${unwind[@]}"} \
      -Wl,--whole-archive "$B/lib7zbackend.a" -Wl,--no-whole-archive
  fi
  # collect-aros can exit 0 with unresolved symbols left; check ourselves.
  if [ "$t" != host ]; then
    local undef; undef=$($NM -u "$B/bin/unarc-7z" | grep -v ' w ' || true)
    if [ -n "$undef" ]; then echo "unresolved symbols in unarc-7z:"; echo "$undef"; exit 1; fi
  fi
  if [ "$t" = aros-raspi ]; then
    # A native TLS section or relocation makes an otherwise linked program
    # unloadable on the native Pi target. GetTLSValues calls are ordinary calls.
    local elf_sections elf_relocs
    elf_sections=$("$tc/aarch64-aros-readelf" -SW "$B/bin/unarc-7z")
    elf_relocs=$("$tc/aarch64-aros-readelf" -rW "$B/bin/unarc-7z")
    if grep -Eq '\.(tdata|tbss)([[:space:].]|$)' <<< "$elf_sections" ||
       grep -Eq 'R_AARCH64_TLS' <<< "$elf_relocs"; then
      echo "native TLS in Pi unarc-7z is unsupported" >&2; exit 1
    fi
  fi
  ls -l "$B/bin/unarc-7z"
}

case $WHAT in
  aros) build_target aros ;;
  aros-v1) build_target aros-v1 ;;
  aros-raspi) build_target aros-raspi ;;
  host) build_target host ;;
  all)  build_target aros; if command -v clang++ >/dev/null; then build_target host; fi ;;
  *) echo "usage: $0 [aros|aros-v1|aros-raspi|host|all]" >&2; exit 2 ;;
esac
