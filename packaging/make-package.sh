#!/bin/bash
# Make the package for AROS ABIv11 x86_64 from the current build:
#   packaging/make-package.sh <version>      e.g. 0.0.3-test1
# Writes dist/Unarc-<version>-x86_64-abiv11.zip and adds it to dist/SHA256SUMS.
# A file already in dist/ is never replaced: a new build needs a new version.
#
# The archive holds one drawer, Unarc, and its icon. Inside: Unarc (Zune),
# unarc-cli (Shell), unarc-7z (7-Zip's handlers), unarc-lha (LHa for UNIX,
# for making LhA archives), ReadMe, a Licences drawer with every licence
# text, THIRD_PARTY_NOTICES and the source revision, and a Source drawer
# with unarc-lha's source: its terms allow no binary-only distribution. Programs get the Unix mode 0755 in the ZIP, which AROS's UnZip
# turns into the e bit.
set -euo pipefail
. "$(dirname "$0")/../scripts/env.sh"
VER=${1:?usage: make-package.sh <version>}
R=$UNARC_ROOT
BIN=$R/build/abiv11-x86_64/bin
Z7=$R/build/7z-backend/aros/bin/unarc-7z
LHA=$BIN/unarc-lha
OUT=$R/dist/Unarc-$VER-x86_64-abiv11.zip
mkdir -p "$R/dist"
if [ -e "$OUT" ]; then echo "$OUT exists; delivered files are not replaced" >&2; exit 1; fi
for f in "$BIN/Unarc" "$BIN/unarc-cli" "$Z7" "$LHA"; do
  [ -f "$f" ] || { echo "missing $f: build first" >&2; exit 1; }
done
# The programs must have been built for this name (UNARC_RELEASE=<version>
# scripts/build-abiv11.sh): it is in their $VER and in what they show.
# Compared as text, not as a pattern: "0.0.4" must not match "0x0x4".
for f in "$BIN/Unarc" "$BIN/unarc-cli"; do
  got=$(strings -a "$f" | grep -a -m1 '^\$VER: ' | sed 's/^[^)]*) //')
  [ "$got" = "$VER" ] || {
    echo "$f was built as \"$got\", not $VER: run UNARC_RELEASE=$VER scripts/build-abiv11.sh" >&2
    exit 1; }
done
if [ -n "$(git -C "$R" status --porcelain -- src experiments scripts packaging patches upstreams.json deps.lock)" ]; then
  echo "the source tree has uncommitted changes; commit before packaging" >&2; exit 1
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 "$R/packaging/make-icons.py" "$T/icons" >/dev/null
# unarc-lha's source as built: LHa for UNIX at its pin, our patches beside
# it (they are the document of the changes its terms ask for), applied.
LHAPIN=$(python3 -c "import json;print(json.load(open('$R/upstreams.json'))['repositories']['lha']['commit'])")
mkdir -p "$T/lhasrc/unarc-lha-source/unarc-patches"
( cd "$R/upstream/lha" && git archive --format=tar "$LHAPIN" ) | tar -x -C "$T/lhasrc/unarc-lha-source"
cp "$R/patches/lha/"*.patch "$R/patches/lha/series" "$T/lhasrc/unarc-lha-source/unarc-patches/"
( cd "$T/lhasrc/unarc-lha-source" && while read -r p; do [ -n "$p" ] && patch -s -p1 < "unarc-patches/$p"; done < unarc-patches/series )
{
  echo "unarc-lha is LHa for UNIX (https://github.com/sodero/lha, commit $LHAPIN)"
  echo "with the changes in unarc-patches/, applied in the order of unarc-patches/series."
  echo "Each patch says what it changes and why. The terms of LHa for UNIX are in"
  echo "man/lha.man and, in English, in dist/LHa_4U4A.guide (\"License\"); they also"
  echo "apply to this modified program. Built for AROS ABIv11 x86_64 with"
  echo "scripts/build-lha-abiv11.sh of aros-unarc $(git -C "$R" rev-parse HEAD)."
} >"$T/lhasrc/unarc-lha-source/README.unarc"
( cd "$T/lhasrc" && zip -q -r -X ../unarc-lha-source.zip unarc-lha-source )
sed -n '/^@NODE "License"/,/^@ENDNODE/p' "$R/upstream/lha/dist/LHa_4U4A.guide" | grep -v '^@' \
  >"$T/LHa-for-UNIX-terms.txt"
{
  echo "Unarc $VER"
  echo "aros-unarc $(git -C "$R" rev-parse HEAD)"
  echo "unarc-7z: 7-Zip 26.04, experiments/7z-backend of the same revision"
  echo "unarc-lha: LHa for UNIX, sodero/lha $LHAPIN with patches/lha (source in Source/)"
  echo "built $(date -u +%Y-%m-%d) for AROS ABIv11 x86_64"
} >"$T/Revision"
python3 - "$OUT" "$T" "$R" "$BIN" "$Z7" "$LHA" <<'PY'
import os, sys, time, zipfile
out, t, r, bindir, z7, lha = sys.argv[1:7]
now = time.localtime()[:6]
def add(z, arc, src=None, data=None, mode=0o644, isdir=False):
    zi = zipfile.ZipInfo(arc + ("/" if isdir else ""), now)
    zi.create_system = 3
    zi.external_attr = ((0o40000 if isdir else 0o100000) | mode) << 16 | (0x10 if isdir else 0)
    zi.compress_type = zipfile.ZIP_DEFLATED
    z.writestr(zi, b"" if isdir else (data if data is not None else open(src, "rb").read()))
with zipfile.ZipFile(out, "w") as z:
    add(z, "Unarc.info", os.path.join(t, "icons", "drawer.info"))
    add(z, "Unarc", isdir=True, mode=0o755)
    add(z, "Unarc/Unarc", os.path.join(bindir, "Unarc"), mode=0o755)
    add(z, "Unarc/Unarc.info", os.path.join(t, "icons", "Unarc.info"))
    add(z, "Unarc/unarc-cli", os.path.join(bindir, "unarc-cli"), mode=0o755)
    add(z, "Unarc/unarc-7z", z7, mode=0o755)
    add(z, "Unarc/unarc-lha", lha, mode=0o755)
    add(z, "Unarc/ReadMe", os.path.join(r, "packaging", "ReadMe"))
    add(z, "Unarc/ReadMe.info", os.path.join(t, "icons", "ReadMe.info"))
    add(z, "Unarc/Licences", isdir=True, mode=0o755)
    add(z, "Unarc/Licences/Unarc-LICENSE.txt", os.path.join(r, "LICENSE"))
    add(z, "Unarc/Licences/THIRD_PARTY_NOTICES", os.path.join(r, "THIRD_PARTY_NOTICES"))
    add(z, "Unarc/Licences/Revision", os.path.join(t, "Revision"))
    for f in sorted(os.listdir(os.path.join(r, "licenses"))):
        add(z, f"Unarc/Licences/{f}", os.path.join(r, "licenses", f))
    add(z, "Unarc/Licences/LHa-for-UNIX-terms.txt", os.path.join(t, "LHa-for-UNIX-terms.txt"))
    add(z, "Unarc/Licences/LHa-for-UNIX-lha.man", os.path.join(r, "upstream", "lha", "man", "lha.man"))
    add(z, "Unarc/Source", isdir=True, mode=0o755)
    add(z, "Unarc/Source/unarc-lha-source.zip", os.path.join(t, "unarc-lha-source.zip"))
PY
( cd "$R/dist" && shasum -a 256 "$(basename "$OUT")" >>SHA256SUMS )
ls -l "$OUT"
tail -1 "$R/dist/SHA256SUMS"
