#!/bin/bash
# Make the package for one ABI from the current build of that ABI:
#   packaging/make-package.sh <version> [abiv11|v1|raspi]      e.g. 0.1.0
# Writes the full-target ZIP name selected by scripts/abi.sh and a checksum file.
# A file already in dist/ is never replaced: a new build needs a new version.
#
# The archive holds one drawer, Arca, and its icon. Inside: Arca (Zune),
# arca-cli (Shell), unarc-7z (7-Zip's handlers), unarc-lha (LHa for UNIX,
# for making LhA archives), ReadMe, a Licences drawer with every licence
# text, THIRD_PARTY_NOTICES and the source revision, and a Source drawer
# with unarc-lha's source: its terms allow no binary-only distribution. Programs get the Unix mode 0755 in the ZIP, which AROS's UnZip
# turns into the e bit.
set -euo pipefail
VER=${1:?usage: make-package.sh <version> [abiv11|v1|raspi]}
. "$(dirname "$0")/../scripts/abi.sh" "${2:-abiv11}"
BIN=$B/bin
Z7=$R/build/7z-backend/aros/bin/unarc-7z
[ "$UNARC_ABI" = v1 ] && Z7=$R/build/7z-backend/aros-v1/bin/unarc-7z
[ "$UNARC_ABI" = raspi ] && Z7=$R/build/7z-backend/aros-raspi/bin/unarc-7z
LHA=$BIN/unarc-lha
PACKAGE_REVISION=$(python3 -c 'import sys,tomllib; print(tomllib.load(open(sys.argv[1], "rb"))["revision"])' "$R/packaging/arca.$UNARC_ABI.toml")
PACKAGE_VERSION=$VER
[ "$PACKAGE_REVISION" -eq 1 ] || PACKAGE_VERSION=$VER-r$PACKAGE_REVISION
OUT=$R/dist/Arca-$PACKAGE_VERSION-$PKG_SUFFIX.zip
mkdir -p "$R/dist"
if [ -e "$OUT" ]; then echo "$OUT exists; delivered files are not replaced" >&2; exit 1; fi
for f in "$BIN/Arca" "$BIN/arca-cli" "$Z7" "$LHA"; do
  [ -f "$f" ] || { echo "missing $f: build first" >&2; exit 1; }
done
# The programs must have been built for this name (UNARC_RELEASE=<version>
# scripts/build-abiv11.sh): it is in their $VER and in what they show.
# Compared as text, not as a pattern: "0.0.4" must not match "0x0x4".
for f in "$BIN/Arca" "$BIN/arca-cli"; do
  # grep -m1 stops reading; under pipefail the SIGPIPE to strings would end
  # the script, so not in this pipeline.
  got=$(set +o pipefail; strings -a "$f" | grep -a -m1 '^\$VER: ' | sed 's/^[^)]*) //')
  [ "$got" = "$VER" ] || {
    echo "$f was built as \"$got\", not $VER: run UNARC_RELEASE=$VER scripts/build.sh $UNARC_ABI" >&2
    exit 1; }
done
if [ -n "$(git -C "$R" status --porcelain -- src experiments scripts packaging patches upstreams.json deps.lock licenses THIRD_PARTY_NOTICES docs README.md)" ]; then
  echo "the source tree has uncommitted changes; commit before packaging" >&2; exit 1
fi
STRIP="$TC/$CROSS_PREFIX-strip"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
python3 "$R/packaging/make-icons.py" "$T/icons" >/dev/null
# Debug sections contain build-host paths. Preserve executable code/relocations.
mkdir -p "$T/bin"
for f in Arca arca-cli; do cp "$BIN/$f" "$T/bin/$f"; "$STRIP" --strip-debug "$T/bin/$f"; done
cp "$Z7" "$T/bin/unarc-7z"; "$STRIP" --strip-debug "$T/bin/unarc-7z"
cp "$LHA" "$T/bin/unarc-lha"; "$STRIP" --strip-debug "$T/bin/unarc-lha"
BIN="$T/bin"; Z7="$BIN/unarc-7z"; LHA="$BIN/unarc-lha"
# unarc-lha's source as built: LHa for UNIX at its pin, our patches beside
# it (they are the document of the changes its terms ask for), applied.
LHAPIN=$(python3 -c "import json;print(json.load(open('$R/upstreams.json'))['repositories']['lha']['commit'])")
mkdir -p "$T/lhasrc/unarc-lha-source/unarc-patches"
git -C "$R/upstream/lha" archive --format=tar --output="$T/lha.tar" "$LHAPIN"
python3 - "$T/lha.tar" "$T/lhasrc/unarc-lha-source" <<'PYLHA'
import pathlib, sys, tarfile
with tarfile.open(sys.argv[1]) as archive:
    members = [m for m in archive if pathlib.Path(m.name).name.lower()
               not in ('agents.md', 'claude.md', 'project.json')]
    archive.extractall(sys.argv[2], members=members, filter='data')
PYLHA
cp "$R/patches/lha/"*.patch "$R/patches/lha/series" "$T/lhasrc/unarc-lha-source/unarc-patches/"
( cd "$T/lhasrc/unarc-lha-source" && while read -r p; do [ -n "$p" ] && patch -s -p1 < "unarc-patches/$p"; done < unarc-patches/series )
{
  echo "unarc-lha is LHa for UNIX (https://github.com/sodero/lha, commit $LHAPIN)"
  echo "with the changes in unarc-patches/, applied in the order of unarc-patches/series."
  echo "Each patch says what it changes and why. The terms of LHa for UNIX are in"
  echo "man/lha.man and, in English, in dist/LHa_4U4A.guide (\"License\"); they also"
  echo "apply to this modified program. Built for $ABI_TEXT with"
  echo "scripts/build-lha.sh of aros-unarc $(git -C "$R" rev-parse HEAD)."
} >"$T/lhasrc/unarc-lha-source/README.unarc"
( cd "$T/lhasrc" && zip -q -r -X ../unarc-lha-source.zip unarc-lha-source )
sed -n '/^@NODE "License"/,/^@ENDNODE/p' "$R/upstream/lha/dist/LHa_4U4A.guide" | grep -v '^@' \
  >"$T/LHa-for-UNIX-terms.txt"
{
  echo "Arca $VER"
  echo "aros-unarc $(git -C "$R" rev-parse HEAD)"
  echo "unarc-7z: 7-Zip 26.04, experiments/7z-backend of the same revision"
  echo "unarc-lha: LHa for UNIX, sodero/lha $LHAPIN with patches/lha (source in Source/)"
  echo "built $(date -u +%Y-%m-%d) for $ABI_TEXT"
} >"$T/Revision"
# The ReadMe names the system this package is for.
bash "$R/packaging/target-description.sh" "$UNARC_ABI" >"$T/target"
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
    add(z, "Arca.info", os.path.join(t, "icons", "drawer.info"))
    add(z, "Arca", isdir=True, mode=0o755)
    add(z, "Arca/Arca", os.path.join(bindir, "Arca"), mode=0o755)
    add(z, "Arca/Arca.info", os.path.join(t, "icons", "Arca.info"))
    add(z, "Arca/arca-cli", os.path.join(bindir, "arca-cli"), mode=0o755)
    add(z, "Arca/unarc-7z", z7, mode=0o755)
    add(z, "Arca/unarc-lha", lha, mode=0o755)
    readme = open(os.path.join(r, "packaging", "ReadMe")).read()
    assert "@TARGET@" in readme
    add(z, "Arca/ReadMe", data=readme.replace("@TARGET@\n", open(os.path.join(t, "target")).read()).encode())
    add(z, "Arca/ReadMe.info", os.path.join(t, "icons", "ReadMe.info"))
    add(z, "Arca/Licences", isdir=True, mode=0o755)
    add(z, "Arca/Licences/Arca-LICENSE.txt", os.path.join(r, "LICENSE"))
    add(z, "Arca/Licences/THIRD_PARTY_NOTICES", os.path.join(r, "THIRD_PARTY_NOTICES"))
    add(z, "Arca/Licences/Revision", os.path.join(t, "Revision"))
    for f in sorted(os.listdir(os.path.join(r, "licenses"))):
        add(z, f"Arca/Licences/{f}", os.path.join(r, "licenses", f))
    add(z, "Arca/Licences/LHa-for-UNIX-terms.txt", os.path.join(t, "LHa-for-UNIX-terms.txt"))
    add(z, "Arca/Licences/LHa-for-UNIX-lha.man", os.path.join(r, "upstream", "lha", "man", "lha.man"))
    add(z, "Arca/Source", isdir=True, mode=0o755)
    add(z, "Arca/Source/unarc-lha-source.zip", os.path.join(t, "unarc-lha-source.zip"))
PY

ls -l "$OUT"


# Embed the per-ABI apkg manifest and application.json in the final ZIP.
python3 "$R/packaging/embed-apkg.py" "$OUT" "$VER" "$UNARC_ABI"
( cd "$R/dist" && shasum -a 256 "$(basename "$OUT")" >SHA256SUMS."$UNARC_ABI" )
