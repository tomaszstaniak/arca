---
type: guide
updated: 2026-10-16
---

# Development

## Supported scope

Intended targets, in order: AROS ABIv11 on x86_64 first; then ABIv11 on the other
AROS architectures; then mainline AROS (ABIv1). Only ABIv11 x86_64 is built so far.
ABIv11 and mainline binaries are not interchangeable; each needs its own toolchain,
SDK, build output and test.

## Setup

Copy `local.env.example` to `local.env` and set:

- `UNARC_TOOLCHAIN_ABIV11`: directory with `x86_64-aros-gcc` (GCC 10.5.0 for ABIv11);
- `UNARC_SDK_ABIV11`: the ABIv11 SDK directory containing `include/` and `lib/`;
- optionally `UNARC_DEPS_MIRROR`: local clones to fetch dependencies from.

Environment variables already set take precedence over `local.env`. The scripts
need bash, CMake, git and patch.

## Build (ABIv11 x86_64)

    scripts/fetch-deps.sh          # pinned sources from deps.lock into deps/
    scripts/build-deps-abiv11.sh   # static liblzma, libarchive, XAD into build/abiv11-x86_64/prefix
    scripts/build-abiv11.sh        # build/abiv11-x86_64/bin/unarc-cli and Unarc

`fetch-deps.sh` puts unmodified dependencies (`deps.lock`) in `deps/` and the
repositories we patch (`upstreams.json`) as clean checkouts in `upstream/`.
`build-deps-abiv11.sh` applies `patches/<id>/series` to a copy in `build/`; neither
`deps/` nor `upstream/` is edited. See [patching](patching.md). `build-abiv11.sh`
stops with a non-zero exit code if a linked program has undefined symbols:
collect-aros itself can exit 0 with symbols left unresolved, which is also why
`build-deps-abiv11.sh` switches off functions CMake wrongly finds.

## Layout

- `src/core/`: the model, format recognition and engine choice (`registry.c`,
  `session.c`), name conversion (`names.c`), the extraction plan (`plan.c`), the
  only code that writes extracted files (`fsl.c`) and packing (`pack.c`).
- `src/engines/`: libarchive, a statically linked XAD (`xad_static.c`, LGPL), and
  `be_7z.c`, which runs the separate program `unarc-7z` (7-Zip's 7z and RAR
  handlers, `experiments/7z-backend/`) for 7z and RAR when it is installed next
  to Unarc and unarc-cli. It reads the program's line protocol (PROTOCOL.md
  there) from a `PIPE:` handle; the archive path goes as hex, the selection
  through a file in `T:`, the password through a second pipe. Without the
  program, libarchive reads 7z and RAR as before (no passwords).
- `src/cli/`: `unarc-cli`, the Shell program.
- `src/gui/`: `Unarc`, the Zune program; `job.c` runs each job in a worker process.

## Using the command-line program

    unarc-cli ARCHIVE,FILES/M,TO/K,LIST/S,PACK/S,FORMAT/K,LEVEL/K/N,RECOVER/S,ENGINE/K,
              OVERWRITE/S,SKIP/S,RENAME/S,NOWRAP/S,CANCELAFTER/K/N,STATS/S,QUIET/S,
              DIAGOUT/S,FORMATS/S,HELP/S,PASSWORD/K

- `unarc-cli HELP` prints usage with examples; `FORMATS` what each format supports.
- `LIST` shows the entries, name changes and where extraction would go; it writes
  nothing.
- Without `LIST` or `PACK` the archive is extracted next to itself, or into `TO`.
  A damaged archive is refused; `RECOVER` extracts what can be read into a new folder.
- `PACK` makes a new archive from `FILES`; the format comes from the name (`.zip`,
  `.tar.gz`/`.tgz`, `.7z`) or `FORMAT`. An existing archive is replaced only with
  `OVERWRITE`.
- Existing files: asked on an interactive console; otherwise skipped unless
  `OVERWRITE`, `SKIP` or `RENAME` is given.
- Results go to standard output; progress, messages and questions to the error
  stream, which the AROS Shell cannot redirect; `DIAGOUT` copies them to standard
  output for scripts.
- Text is ISO-8859-1, the console's character set, in `CON:` and in files. In a
  Limpet window (asked once through the Limpet SDK's presentation query, which
  `CON:` and file handlers refuse at once) the same text is written as UTF-8,
  since Limpet shows UTF-8. Limpet sends typed commands to the Shell as UTF-8
  (its README), so a non-ASCII name typed there will not match the ISO-8859-1
  name on disk; not handled here and not tested.
- Passwords (7z, RAR, through unarc-7z): an encrypted entry list is asked for
  at once; encrypted entries before anything is written. On an interactive
  console the password is read without echo (Limpet's UTF-8 as it is,
  ISO-8859-1 converted to UTF-8); in scripts `PASSWORD` gives it. Without one,
  encrypted entries are left out and reported (RC 5), and an encrypted entry
  list ends with RC 20. A wrong password for data shows as an error on each
  entry it affects (RC 10). The GUI asks in a window of its own, keeps the
  password for that archive while it runs, and never saves or logs it.
- Return codes: 0 done, 5 done with notes (skipped, renamed, recovered), 10 partly
  done, cancelled or stopped, 20 nothing done.
- A multi-volume archive can be named by any of its volumes. The set is found
  in the same drawer by name (`name.partN.rar`, `name.rar` + `name.rNN`,
  `name.7z.001`), but the order and completeness of a RAR set come from the
  volumes' own headers; an incomplete set is refused with the missing volume's
  name.
- `ENGINE` (`libarchive`, `xad`, `libarchive-stream`), `CANCELAFTER` (bytes) and
  `TESTSWAP` (a file copied over the archive between listing and extracting)
  are for testing.

## The Zune program

`Unarc` with no arguments opens a start window: drop an archive to extract it,
files and drawers to pack them, or use the buttons, which do the same through
file requesters. Started with an archive (a project icon whose default tool is
`Unarc`, or `Unarc <archive>` in the Shell) it extracts next to the archive and
shows the result; the start window's "Double-clicked archives" setting, or the
tool type `MODE=BROWSE` (`BROWSE` in the Shell), shows the contents first instead.
`MODE=EXTRACT` / `EXTRACT` force extraction. The setting is stored in
`ENVARC:Unarc.prefs`.

No file type association is installed by the build. To try it, give an archive a
project icon with `Unarc` as its default tool, or set `Unarc` as the default tool
of the archive types in Wanderer's preferences.

Several archives given or dropped at once are extracted one after another;
the progress window lists them with their status and ends with one summary.
Volumes of one multi-volume set count once. Waiting archives can be removed,
the running one cancelled, or everything ("Cancel all").

Entries can be dragged out of the browser onto an open Wanderer drawer
window: select them, then press on one of the selected rows and drag. They
are extracted into that drawer without the folders above them in the
archive; a selected folder brings its contents under its own name. Two
entries that end up with one name (`a/x.txt` and `b/x.txt`) are numbered
(`x 2.txt`), as within an archive. A name already in the drawer is a conflict
like any other. Releasing anywhere else extracts nothing and says why.
Amiga+A selects every entry. The drawer is found from the window under the
pointer: it must belong to Wanderer and carry a drawer path as its title
(workbench.library on AROS cannot list open drawers).

Test aids: `TESTQUIT` ends the program when its first job has ended; the variable
`UnarcDebug` names a file that receives a log of jobs, results and allocated
signals; `UnarcTestDelay` (ticks) slows the delivery of list entries;
`UnarcTestSlowList` (ticks per 100 entries) slows reading the archive itself;
`UnarcTestFailDir` (`name` or `name/k`) makes reading that drawer fail while
packing.

## Tests

- `tests/corpus/make_corpus.py` builds the test archives (Amiga archives are made
  by Aminet's LhA and LZX under amitools' vamos).
- `tests/guest/make_stage.py` packs the command-line program, the corpus and an
  AROS Shell script for a test machine (options select the suites: `core2`,
  `regress`, `pack`, `ffsprobe`, ...); results go to `RESULTS:<run-id>/`.
- `tests/guest/make_gui_stage.py` adds the Zune program, test project icons and
  the memory/lock loop for the GUI.
- `tests/host/verify_core2.py` and `tests/host/verify_pack.py` check collected
  results on the host; the packing check uses 7-Zip and Python, not libarchive.
- `tests/probes/`: small programs that isolate platform behaviour (iconv, long
  paths, FFS file creation) and `uatool`, which makes project icons, opens objects
  as Workbench does and sends AppWindow drops.
