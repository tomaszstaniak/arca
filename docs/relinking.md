# Rebuilding with modified LGPL libraries

Arca statically links XAD on both ABIs and GNU libiconv on ABIv11. The separate
unarc-7z program links 7-Zip's decoder code. These components retain their LGPL
licences; the RAR decoder also retains its unRAR restriction.

The GitHub release provides the application source, full pinned dependency
sources and a relink kit for each ABI. Keep the selected SDK/compiler matched to
the ABI. No private workspace repository is needed: deps.lock and upstreams.json
use public sources, and the release source bundle contains their pinned contents.

## Rebuild the application

1. Follow development.md and create local.env with the SDK/toolchain locations.
2. Run scripts/fetch-deps.sh or unpack the complete release source bundle.
3. For ABIv11, XAD uses patches/contrib/; mainline uses patches/contrib-mainline/.
   Add a new source patch to the appropriate ordered series.
4. Run scripts/build-deps.sh <abi>, then scripts/build.sh <abi>.

To use a modified libiconv, build GNU libiconv 1.14 from the source included in
the release, then replace ICONVLIB in scripts/build.sh with your library path and
put its headers first. build-abiv11.sh is a wrapper for build.sh. The exact
unpatched GNU source and AROS recipe provenance are recorded under
third_party/libiconv/abiv11/. libiconv is not linked into the v1 build.

## Relink without rebuilding Arca's own source

Each relink kit provides the application object files, the static dependency
archives and relink.sh. Point ARCA_SDK and ARCA_TOOLCHAIN at the matching SDK and
compiler. Replace lib/libxadstatic.a or (ABIv11 only) lib/libiconv.a with your
modified library, then run relink.sh. The script links Arca and arca-cli; it never
installs either into the system. 7-Zip can be rebuilt with
experiments/7z-backend/build.sh using the complete release source bundle.

The SDK startup/link libraries retain the AROS Public License. Full library and
helper notices are in THIRD_PARTY_NOTICES and licenses/.
