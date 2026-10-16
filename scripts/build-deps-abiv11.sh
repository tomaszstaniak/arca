#!/bin/bash
# Build the static dependencies for ABIv11 x86_64 into build/abiv11-x86_64/prefix.
set -euo pipefail
. "$(dirname "$0")/env.sh"
: "${UNARC_TOOLCHAIN_ABIV11:?set in local.env}" "${UNARC_SDK_ABIV11:?set in local.env}"
R=$UNARC_ROOT
B=$R/build/abiv11-x86_64
export UNARC_PREFIX=$B/prefix
TCF=$R/scripts/cmake/abiv11-x86_64.cmake
mkdir -p "$UNARC_PREFIX/lib" "$UNARC_PREFIX/include"
SDK=$UNARC_SDK_ABIV11
TC=$UNARC_TOOLCHAIN_ABIV11

# xz/liblzma: the ABIv11 SDK ships only stubs for lzma.library. AROS has no CPU-count
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
cmake -S "$R/deps/libarchive" -B "$B/libarchive" -DCMAKE_TOOLCHAIN_FILE="$TCF" \
  -DCMAKE_INSTALL_PREFIX="$UNARC_PREFIX" -DBUILD_SHARED_LIBS=OFF -DENABLE_WERROR=OFF \
  -DENABLE_TAR=OFF -DENABLE_CPIO=OFF -DENABLE_CAT=OFF -DENABLE_UNZIP=OFF -DENABLE_TEST=OFF \
  -DHAVE_OPENAT=0 -DHAVE_FSTATAT=0 -DHAVE_FUTIMENS=0 -DHAVE_TIMEGM=0 -DHAVE_STATVFS=0 \
  -DHAVE_FSTATVFS=0 -DHAVE_LINKAT=0 -DHAVE_FORK=0 -DHAVE_NL_LANGINFO=0 -DHAVE_GETPWNAM_R=0 \
  -DHAVE_GETGRNAM_R=0 -DHAVE_STATFS=0 -DHAVE_FSTATFS=0 \
  -DHAVE_ARC4RANDOM_BUF=0 -DHAVE_TZSET=0 -DHAVE_LZMA_STREAM_ENCODER_MT=0 \
  -DENABLE_OPENSSL=OFF -DENABLE_MBEDTLS=OFF -DENABLE_NETTLE=OFF -DENABLE_LIBB2=OFF \
  -DENABLE_LZ4=OFF -DENABLE_EXPAT=OFF -DENABLE_LIBXML2=OFF -DENABLE_PCREPOSIX=OFF \
  -DENABLE_PCRE2POSIX=OFF -DENABLE_ACL=OFF -DENABLE_XATTR=OFF -DENABLE_LZO=OFF -DENABLE_CNG=OFF \
  -DENABLE_ZSTD=OFF -DENABLE_ICONV=ON \
  -DENABLE_ZLIB=ON -DZLIB_INCLUDE_DIR="$SDK/include" -DZLIB_LIBRARY="$SDK/lib/libz.static.a" \
  -DENABLE_BZip2=ON -DBZIP2_INCLUDE_DIR="$SDK/include" -DBZIP2_LIBRARIES="$SDK/lib/libbz2_nostdio.a" \
  -DENABLE_LZMA=ON -DLIBLZMA_INCLUDE_DIR="$UNARC_PREFIX/include" -DLIBLZMA_LIBRARY="$UNARC_PREFIX/lib/liblzma.a" \
  >"$B/libarchive-configure.log"
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
cp -R "$R/upstream/contrib/workbench/libs/xad" "$XS/workbench/libs/xad"
while read -r pt; do
  case $pt in ''|'#'*) continue ;; esac
  patch -s -p1 -d "$XS" < "$R/patches/contrib/$pt"
done < "$R/patches/contrib/series"
X=$XS/workbench/libs/xad/portable
O=$B/xad
mkdir -p "$O"
XFLAGS="--sysroot=$SDK -O2 -DHAVE_STDINT_H -D__DOS_NOLIBBASE__ -D__UTILITY_NOLIBBASE__ \
  -I$SDK/include/SDI -I$X/include -I$X -I$X/amiga -Wno-uninitialized -Wno-parentheses"
objs=()
for f in clientfunc copymem crc dates diskfile diskunarc error filename fileunarc hook \
         amiga/hook_disk hook_diskarc amiga/hook_fh hook_mem hook_splitted hook_stream info \
         objects protection include/ConvertE cs_atarist_to_unicode cs_c64_to_unicode \
         cs_ibmcp437_to_unicode cs_macroman_to_unicode cs_unicode_to_iso8859-1 \
         cs_windowscp1252_to_unicod clients; do
  o=$O/$(basename "$f").o
  "$TC/x86_64-aros-gcc" $XFLAGS -c "$X/$f.c" -o "$o" 2>>"$B/xad-build.log"
  objs+=("$o")
done
rm -f "$UNARC_PREFIX/lib/libxadstatic.a"
"$TC/x86_64-aros-ar" rcs "$UNARC_PREFIX/lib/libxadstatic.a" "${objs[@]}"
echo "deps built in $UNARC_PREFIX"
