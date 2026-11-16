#!/bin/bash
# Build the static dependencies for one ABI into build/<target>-<arch>/prefix.
#   scripts/build-deps.sh abiv11|v1
set -euo pipefail
. "$(dirname "$0")/abi.sh" "${1:-abiv11}"
export UNARC_PREFIX=$B/prefix
TCF=$R/scripts/cmake/x86_64-aros.cmake
LA_CFLAGS=
mkdir -p "$UNARC_PREFIX/lib" "$UNARC_PREFIX/include"
# ABIv11's SDK has GNU libiconv (Arca supplies locale_charset, see
# src/core/compat.c); mainline's SDK has no iconv at all, so libarchive is
# built without it there and Arca converts names itself (src/core/names.c).
ICONV=ON
[ "$UNARC_ABI" != abiv11 ] && ICONV=OFF LA_CFLAGS="-I$R/scripts/include-noiconv"

# xz/liblzma: the SDKs ship only stubs for lzma.library. AROS has no CPU-count
# or physical-memory query that tuklib knows; we use neither (no threads, no memlimit
# from physmem), so mark both as found and let them return 0.
cmake -S "$R/deps/xz" -B "$B/xz" -DCMAKE_TOOLCHAIN_FILE="$TCF" \
  -DCMAKE_INSTALL_PREFIX="$UNARC_PREFIX" -DBUILD_SHARED_LIBS=OFF \
  -DXZ_THREADS=no -DXZ_NLS=OFF -DXZ_DOC=OFF -DXZ_TOOL_XZ=OFF -DXZ_TOOL_XZDEC=OFF \
  -DXZ_TOOL_LZMADEC=OFF -DXZ_TOOL_LZMAINFO=OFF -DXZ_TOOL_SCRIPTS=OFF -DXZ_TOOL_SYMLINKS=OFF \
  -DXZ_TOOL_SYMLINKS_LZMA=OFF -DXZ_SANDBOX=no \
  -DTUKLIB_CPUCORES_FOUND=1 -DTUKLIB_PHYSMEM_FOUND=1 >"$B/xz-configure.log"
cmake --build "$B/xz" -j8 >"$B/xz-build.log"
cmake --install "$B/xz" >"$B/xz-install.log"

# libarchive: CMake's checks link with collect-aros, which can exit 0 with symbols
# left unresolved, so calls AROS does not have are reported as present; force those
# off. arc4random_buf, tzset and the threaded xz encoder only showed up once the
# write side was linked (packing); xz is built without threads.
# CFLAGS from the environment come before the toolchain file's --sysroot.
CFLAGS="$LA_CFLAGS" cmake -S "$R/deps/libarchive" -B "$B/libarchive" -DCMAKE_TOOLCHAIN_FILE="$TCF" \
  -DCMAKE_INSTALL_PREFIX="$UNARC_PREFIX" -DBUILD_SHARED_LIBS=OFF -DENABLE_WERROR=OFF \
  -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
  -DHAVE_OPENAT=0 -DHAVE_FSTATAT=0 -DHAVE_FUTIMENS=0 -DHAVE_TIMEGM=0 -DHAVE_STATVFS=0 \
  -DHAVE_FUTIMES=0 -DHAVE_FUTIMESAT=0 -DHAVE_LUTIMES=0 -DHAVE__FSEEKI64=0 \
  -DHAVE_FSTATVFS=0 -DHAVE_LINKAT=0 -DHAVE_FORK=0 -DHAVE_NL_LANGINFO=0 -DHAVE_GETPWNAM_R=0 \
  -DHAVE_GETGRNAM_R=0 -DHAVE_STATFS=0 -DHAVE_FSTATFS=0 \
  -DHAVE_MKFIFO=0 -DHAVE_LCHMOD=0 -DHAVE_LCHOWN=0 \
  -DHAVE_CHFLAGS=0 -DHAVE_FCHFLAGS=0 -DHAVE_LCHFLAGS=0 -DHAVE_CLOSEFROM=0 -DHAVE_CLOSE_RANGE=0 \
  -DHAVE_ARC4RANDOM_BUF=0 -DHAVE_TZSET=0 -DHAVE_LZMA_STREAM_ENCODER_MT=0 \
  -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF \
  -DENABLE_LZ4=OFF -DENABLE_EXPAT=OFF -DENABLE_LIBXML2=OFF -DENABLE_PCREPOSIX=OFF \
  -DENABLE_PCRE2POSIX=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_LZO=OFF -DENABLE_CNG=OFF \
  -DENABLE_ZSTD=OFF -DENABLE_ICONV=$ICONV \
  -DENABLE_ZLIB=ON -DZLIB_INCLUDE_DIR="$SDK/include" -DZLIB_LIBRARY="$SDK/lib/libz.static.a" \
  -DENABLE_BZip2=ON -DBZIP2_INCLUDE_DIR="$SDK/include" -DBZIP2_LIBRARIES="$SDK/lib/libbz2_nostdio.a" \
  -DENABLE_LZMA=ON -DLIBLZMA_INCLUDE_DIR="$UNARC_PREFIX/include" -DLIBLZMA_LIBRARY="$UNARC_PREFIX/lib/liblzma.a" \
  >"$B/libarchive-configure.log"
# Without iconv libarchive's CMake drops locale_charset(), and the current
# charset would be empty, which drops UTF-8 names (as on ABIv11 before
# compat.c). Ours answers UTF-8; libarchive's own UTF-8 code needs no iconv.
if [ "$ICONV" = OFF ]; then
  sed -i.orig -e 's|^/\* #undef HAVE_LOCALE_CHARSET \*/|#define HAVE_LOCALE_CHARSET 1|' \
    -e 's|^/\* #undef HAVE_LOCALCHARSET_H \*/|#define HAVE_LOCALCHARSET_H 1|' "$B/libarchive/config.h"
  grep -q '^#define HAVE_LOCALE_CHARSET 1' "$B/libarchive/config.h" &&
    grep -q '^#define HAVE_LOCALCHARSET_H 1' "$B/libarchive/config.h" ||
    { echo "libarchive config.h: locale_charset not enabled" >&2; exit 1; }
fi
cmake --build "$B/libarchive" --target archive_static -j8 >"$B/libarchive-build.log"
cp "$B/libarchive/libarchive/libarchive.a" "$UNARC_PREFIX/lib/"
cp "$R/deps/libarchive/libarchive/archive.h" "$R/deps/libarchive/libarchive/archive_entry.h" "$UNARC_PREFIX/include/"

# XAD: the files the contrib mmakefile builds into xadmaster.library, compiled as plain
# objects. HA and TR-DOS (GPL-only) are not built, as in the AROS library.
# Build from a generated copy of upstream/contrib with patches/contrib/series
# applied in order. The copy lives in build/ and is recreated every time: edit
# patches, never this copy (see docs/patching.md).
XS=$B/contrib-src
rm -rf "$XS"
mkdir -p "$XS/workbench/libs"
CONTRIB=contrib
[ "$UNARC_ABI" = abiv11 ] || CONTRIB=contrib-mainline
cp -R "$R/upstream/$CONTRIB/workbench/libs/xad" "$XS/workbench/libs/xad"
while read -r pt; do
  case $pt in ''|'#'*) continue ;; esac
  patch -s -p1 -d "$XS" < "$R/patches/$CONTRIB/$pt"
done < "$R/patches/$CONTRIB/series"
X=$XS/workbench/libs/xad/portable
O=$B/xad
mkdir -p "$O"
# The library's own headers (proto, defines, inline, clib) come from AROS's
# genmodule and differ between the ABIs. ABIv11's SDK has them; mainline's
# has no XAD, so they are generated here with that build's genmodule.
XINC=
if [ ! -f "$SDK/include/proto/xadmaster.h" ]; then
  : "${UNARC_V1_HOST_BUILD:?set in local.env: the host build tree with tools/genmodule}"
  XINC=$B/xad-include
  rm -rf "$XINC"; mkdir -p "$XINC/proto" "$XINC/defines" "$XINC/inline" "$XINC/clib"
  ( cd "$X" && "$UNARC_V1_HOST_BUILD/tools/genmodule" -c xadmaster.conf -d "$XINC" \
      writeincludes xadmaster library )
  mkdir -p "$XINC/libraries"
  cp "$XS/workbench/libs/xad/amiga/include/C/libraries/xadmaster.h" "$XINC/libraries/"
fi
XFLAGS="--sysroot=$SDK -O2 -DHAVE_STDINT_H -D__DOS_NOLIBBASE__ -D__UTILITY_NOLIBBASE__ \
  ${XINC:+-I$XINC} -I$SDK/include/SDI -I$X/include -I$X -I$X/amiga -Wno-uninitialized -Wno-parentheses -Werror=implicit-function-declaration"
[ "$UNARC_ABI" != raspi ] || XFLAGS="$XFLAGS -fsigned-char"
: >"$B/xad-build.log"
objs=()
for f in clientfunc copymem crc dates diskfile diskunarc error filename fileunarc hook \
         amiga/hook_disk hook_diskarc amiga/hook_fh hook_mem hook_splitted hook_stream info \
         objects protection include/ConvertE cs_atarist_to_unicode cs_c64_to_unicode \
         cs_ibmcp437_to_unicode cs_macroman_to_unicode cs_unicode_to_iso8859-1 \
         cs_windowscp1252_to_unicod clients; do
  o=$O/$(basename "$f").o
  "$TC/$CROSS_PREFIX-gcc" $XFLAGS -c "$X/$f.c" -o "$o" 2>>"$B/xad-build.log"
  objs+=("$o")
done
rm -f "$UNARC_PREFIX/lib/libxadstatic.a"
"$TC/$CROSS_PREFIX-ar" rcs "$UNARC_PREFIX/lib/libxadstatic.a" "${objs[@]}"
echo "deps built in $UNARC_PREFIX"
