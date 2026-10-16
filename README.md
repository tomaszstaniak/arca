# aros-unarc

`aros-unarc` (working name) is an archiver for AROS: a Zune program (`Unarc`) and a
Shell program (`unarc-cli`) over one core. It lists and extracts ZIP, LhA, LZX,
7z, RAR, tar and compressed tar archives and more through libarchive and a
statically linked XAD, and makes ZIP, tar.gz and 7z archives. It is in
development: nothing has been released, and only AROS ABIv11 on x86_64 is built
and tested.

## Start here

- [Documentation](docs/README.md): installation, usage, and public guides.
- [Development](docs/development.md): contributor setup, build, and tests.

## Shared rules

`AGENTS.md` locates the applicable standard through workspace configuration.
`project.json` records profiles, structure version, review, and participation separately.
Local work notes, current-state references, backlog, and run evidence live in private
`<workspace>/docs/` outside repository Git; they are not required to read this published README or use the project.

## Project boundaries

Original code lives in `src/`, tests in `tests/`, build and test entry points in
`scripts/`. Dependencies are fetched at pinned revisions (`deps.lock`,
`upstreams.json`); changes to them are kept as patches in `patches/`. Intended
platform: AROS, ABIv11 first (x86_64 first, then other architectures), then
mainline ABIv1. No target is supported yet.

Our own code is under the MIT licence (`LICENSE`). The XAD glue
(`src/engines/xad_static.*`) is LGPL-2.1-or-later, and every dependency keeps its
own licence: see `THIRD_PARTY_NOTICES` and `licenses/`. A built program is
therefore not MIT-only; `docs/relinking.md` explains how to rebuild it with a
modified XAD or libiconv.
