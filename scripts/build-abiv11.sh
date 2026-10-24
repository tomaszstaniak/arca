#!/bin/bash
# Build the command-line program for ABIv11 x86_64. Run build-deps-abiv11.sh first.
set -euo pipefail
. "$(dirname "$0")/env.sh"
: "${UNARC_TOOLCHAIN_ABIV11:?set in local.env}" "${UNARC_SDK_ABIV11:?set in local.env}"
R=$UNARC_ROOT
B=$R/build/abiv11-x86_64
P=$B/prefix
SDK=$UNARC_SDK_ABIV11
TC=$UNARC_TOOLCHAIN_ABIV11
X=$B/contrib-src/workbench/libs/xad/portable
CC="$TC/x86_64-aros-gcc"
CFLAGS=(--sysroot="$SDK" -O2 -Wall -Wno-pointer-sign -I"$P/include" -I"$R/src/third_party/limpet/include")
# The release name for $VER and the program's own display (src/version.h);
# packaging/make-package.sh needs the build made for the name it packages.
# The $VER pair is derived from the name, major.(minor * 100 + patch), which
# stays in order and unique only while patch is below 100. The $VER date is
# the date of the commit built, so that a rebuild of one commit gives the same
# file and a new release cannot carry the previous one's date.
if [ -n "${UNARC_RELEASE:-}" ]; then
  if [[ ! $UNARC_RELEASE =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)(-[A-Za-z0-9.]+)?$ ]] ||
     [ "${BASH_REMATCH[3]}" -ge 100 ]; then
    echo "UNARC_RELEASE \"$UNARC_RELEASE\": expected major.minor.patch[-suffix], patch below 100" >&2
    exit 1
  fi
  UNARC_VR="$((10#${BASH_REMATCH[1]})).$((10#${BASH_REMATCH[2]} * 100 + 10#${BASH_REMATCH[3]}))"
  UNARC_DATE=$(git -C "$R" log -1 --format=%cd --date=format:%d.%m.%Y)
  CFLAGS+=(-DUNARC_RELEASE="\"$UNARC_RELEASE\"" -DUNARC_VR="\"$UNARC_VR\""
           -DUNARC_DATE="\"$UNARC_DATE\"")
fi
mkdir -p "$B/obj" "$B/bin"
objs=()
for f in core/compat core/session core/volumes core/bigfile core/methods core/times core/pack core/registry core/names core/plan core/fsl core/helper engines/be_libarchive engines/be_xad engines/be_7z third_party/limpet/limpet_query third_party/limpet/limpet_format cli/main; do
  o=$B/obj/$(basename "$f").o
  "$CC" "${CFLAGS[@]}" -c "$R/src/$f.c" -o "$o"
  objs+=("$o")
done
# The XAD glue needs XAD's private headers.
"$CC" --sysroot="$SDK" -O2 -Wall -DHAVE_STDINT_H -D__DOS_NOLIBBASE__ -D__UTILITY_NOLIBBASE__ \
  -I"$SDK/include/SDI" -I"$X/include" -I"$X" -I"$X/amiga" \
  -c "$R/src/engines/xad_static.c" -o "$B/obj/xad_static.o"
objs+=("$B/obj/xad_static.o")
"$CC" --sysroot="$SDK" -o "$B/bin/unarc-cli" "${objs[@]}" \
  "$P/lib/libarchive.a" "$P/lib/libxadstatic.a" "$P/lib/liblzma.a" \
  "$SDK/lib/libbz2_nostdio.a" "$SDK/lib/libz.static.a" -liconv -lpthread
# collect-aros can exit 0 with unresolved symbols left (a space in the output
# path is one way); check ourselves.
undef=$("$TC/x86_64-aros-nm" -u "$B/bin/unarc-cli" | grep -v ' w ' || true)
if [ -n "$undef" ]; then echo "unresolved symbols:"; echo "$undef"; exit 1; fi
ls -l "$B/bin/unarc-cli"

# The Zune program: the same core and engines, the GUI and the job worker.
gobjs=()
for o in "${objs[@]}"; do
  case $o in */main.o) ;; *) gobjs+=("$o") ;; esac
done
# The table, the step slider, the icon and the drop area come from zunekit
# at the revision pinned in deps.lock.
ZK=$R/deps/zunekit/src
[ -f "$ZK/zk_drop.c" ] || { echo "deps/zunekit missing or old: run scripts/fetch-deps.sh" >&2; exit 1; }
for f in gui/job gui/main; do
  o=$B/obj/gui_$(basename "$f").o
  "$CC" "${CFLAGS[@]}" -I"$ZK" -c "$R/src/$f.c" -o "$o"
  gobjs+=("$o")
done
for c in zk_table zk_steps zk_icon zk_drop; do
  "$CC" "${CFLAGS[@]}" -c "$ZK/$c.c" -o "$B/obj/$c.o"
  gobjs+=("$B/obj/$c.o")
done
"$CC" --sysroot="$SDK" -o "$B/bin/Unarc" "${gobjs[@]}" \
  "$P/lib/libarchive.a" "$P/lib/libxadstatic.a" "$P/lib/liblzma.a" \
  "$SDK/lib/libbz2_nostdio.a" "$SDK/lib/libz.static.a" -liconv -lpthread
undef=$("$TC/x86_64-aros-nm" -u "$B/bin/Unarc" | grep -v ' w ' || true)
if [ -n "$undef" ]; then echo "unresolved symbols in Unarc:"; echo "$undef"; exit 1; fi
ls -l "$B/bin/Unarc"

# Probe of the SDK's iconv (see tests/probes/charset_probe.c).
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/charset-probe" "$R/tests/probes/charset_probe.c" -liconv
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/longpath-probe" "$R/tests/probes/longpath_probe.c"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/reldel" "$R/tests/probes/reldel.c"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/ffs-probe" "$R/tests/probes/ffs_probe.c"
"$CC" --sysroot="$SDK" -O2 -I"$P/include" -o "$B/bin/uatool" "$R/tests/probes/uatool.c" \
  "$R/src/core/volumes.c" "$R/src/core/bigfile.c" "$SDK/lib/libz.static.a"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/zune-control" "$R/tests/probes/zune_control.c"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/listbench" "$R/tests/probes/listbench.c"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/whohas" "$R/tests/probes/whohas.c"
"$CC" --sysroot="$SDK" -O2 -o "$B/bin/fake7z" "$R/tests/probes/fake7z.c"
