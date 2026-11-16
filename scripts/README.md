# Workflow scripts

Bash. Run from any directory; each script finds the project itself.

| Script | Does |
|---|---|
| `env.sh` | Sourced by the others. Loads `../local.env` without overriding variables already set; relative values resolve against the project root. |
| `fetch-deps.sh` | Clones every source in `../deps.lock` at its pinned revision into `../deps/`. |
| `abi.sh` | Sourced by the build scripts: toolchain, SDK and build directory for `abiv11`, `v1` or `raspi`. |
| `build-deps.sh <abi>` | Static liblzma, libarchive and XAD, XAD with the matching contrib patch series applied to a generated copy (ABIv11: contrib; ABIv1/Pi: contrib-mainline). |
| `build-lha.sh <abi>` | `unarc-lha`, the LhA helper: LHa for UNIX from `upstream/lha` with `../patches/lha/series` applied to a copy. |
| `build.sh <abi>` | `Arca`, `arca-cli` and the test probes; checks for undefined symbols. |
| `build-unwind-fix-v1.sh` | Mainline only: libgcc's unwinder rebuilt so that C++ throws are caught (for `unarc-7z`). |
| `build*-abiv11.sh` | The same scripts for `abiv11`, under their older names. |
| `cmake/x86_64-aros.cmake` | CMake toolchain file used by the dependency build. |
| `include-noiconv/` | `localcharset.h` for libarchive built without iconv (mainline). |

Machine paths belong in `../local.env` (see `../local.env.example`). Commands and
keys are documented in [development](../docs/development.md).
