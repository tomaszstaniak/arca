# Workflow scripts

Bash. Run from any directory; each script finds the project itself.

| Script | Does |
|---|---|
| `env.sh` | Sourced by the others. Loads `../local.env` without overriding variables already set; relative values resolve against the project root. |
| `fetch-deps.sh` | Clones every source in `../deps.lock` at its pinned revision into `../deps/`. |
| `build-deps-abiv11.sh` | Static liblzma, libarchive and XAD for ABIv11 x86_64, XAD with `../patches/contrib/series` applied to a copy of `../upstream/contrib`. |
| `build-lha-abiv11.sh` | `unarc-lha`, the LhA helper: LHa for UNIX from `upstream/lha` with `../patches/lha/series` applied to a copy. |
| `build-abiv11.sh` | `unarc-cli` and the charset probe; checks for undefined symbols. |
| `cmake/abiv11-x86_64.cmake` | CMake toolchain file used by the dependency build. |

Machine paths belong in `../local.env` (see `../local.env.example`). Commands and
keys are documented in [development](../docs/development.md).
