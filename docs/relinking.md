---
type: guide
updated: 2026-10-15
---

# Rebuilding with a modified XAD or libiconv

The program links two libraries under the GNU LGPL 2.1 statically: XAD and GNU
libiconv. The LGPL lets you change those libraries and use your changed version
with this program. Everything needed is public: the program's own source (MIT),
the build scripts, the pinned dependency revisions and our patches.

## XAD

1. Follow [development](development.md) to set up `local.env` and run
   `scripts/fetch-deps.sh`. XAD comes from `upstreams.json` into `upstream/contrib/`.
2. Put your change in a patch against that checkout (see [patching](patching.md))
   and add its file name to `patches/contrib/series`. Or, for a one-off build, use
   another contrib revision by changing the `commit` in `upstreams.json`.
3. Run `scripts/build-deps-abiv11.sh` and `scripts/build-abiv11.sh`. The new
   `build/abiv11-x86_64/bin/arca-cli` contains your XAD.

## libiconv

The build links the SDK's `lib/libiconv.a` (GNU libiconv 1.14 as built by the AROS
project for ABIv11). To use your own:

1. Build GNU libiconv for AROS ABIv11 x86_64 as a static library (the AROS
   project's build recipe and patches are in its contrib tree).
2. Point the link at it: in `scripts/build-abiv11.sh` replace `-liconv` with the
   path to your `libiconv.a`, keep its `iconv.h` first on the include path, and
   rebuild with `scripts/build-abiv11.sh`.

Note that the program defines `locale_charset()` itself (`src/core/compat.c`),
which replaces libiconv's own definition of that function.

## Source for a binary you received

A binary release names the program's source revision. With that revision, the
files listed above reproduce the build; the XAD and libiconv sources are the
pinned upstream revisions plus the patches in `patches/`.
