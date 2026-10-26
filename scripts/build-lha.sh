#!/bin/bash
# Build the LHA helper unarc-lha for one ABI (scripts/build-lha.sh abiv11|v1): LHa for UNIX (sodero/lha,
# pinned in upstreams.json) with patches/lha/series applied to a copy. It is a
# program of its own under the LHa for UNIX licence, run by Unarc as a child
# process, never linked into Unarc. Run fetch-deps.sh first.
set -euo pipefail
. "$(dirname "$0")/abi.sh" "${1:-abiv11}"
S=$B/lha-src
[ -d "$R/upstream/lha/src" ] || { echo "upstream/lha missing: run scripts/fetch-deps.sh" >&2; exit 1; }
rm -rf "$S"
mkdir -p "$B/bin"
# A copy without .git: the patches apply with patch(1), as for contrib.
(cd "$R/upstream/lha" && git archive --format=tar HEAD) | (mkdir -p "$S" && tar -x -C "$S")
while read -r p; do
  [ -n "$p" ] || continue
  case $p in '#'*) continue ;; esac
  patch -s -p1 -d "$S" < "$R/patches/lha/$p"
done < "$R/patches/lha/series"
cd "$S"
autoreconf -fi >/dev/null 2>&1 || { aclocal && autoheader && automake -a && autoconf; }
export PATH="$TC:$PATH"
# The settings of sodero's own AROS build (cicd/aros_x86_64.sh), with our
# version string and the SDK as sysroot.
export CC="x86_64-aros-gcc --sysroot=$SDK"
export CFLAGS="-O2 -DUNARC_LHA -DEXPAND_WILDCARDS -D__VER__=1.14i-unarc -D__DAY__=$(date +%d.%m.%Y)"
./configure --host=x86_64-aros --disable-iconv --disable-largefile --with-tmp-file=no \
  --with-additional-suffixes="lha," >/dev/null
# configure records its options, the compiler with this machine's SDK path
# among them, and lha --version prints them: replace them with a description.
sed -i.orig 's|^#define LHA_CONFIGURE_OPTIONS .*|#define LHA_CONFIGURE_OPTIONS "cross build for $ABI_TEXT (aros-unarc scripts/build-lha.sh)"|' config.h
make -C src >/dev/null
cp src/lha "$B/bin/unarc-lha"
undef=$("$TC/x86_64-aros-nm" -u "$B/bin/unarc-lha" | grep -v ' w ' || true)
if [ -n "$undef" ]; then echo "unresolved symbols in unarc-lha:"; echo "$undef"; exit 1; fi
ls -l "$B/bin/unarc-lha"
