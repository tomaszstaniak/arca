---
type: guide
updated: 2026-10-17
---

# Development

## Supported scope

Intended targets, in order: AROS ABIv11 on x86_64 first; then ABIv11 on the other
AROS architectures; then mainline AROS (ABIv1). Built: ABIv11 x86_64 and, from
0.0.4-test12, mainline ABIv1 x86_64 (`v1`). ABIv11 and mainline binaries are not
interchangeable; each has its own toolchain, SDK, build directory
(`build/abiv11-x86_64`, `build/v1-x86_64`), package and test. Passing on one
says nothing about the other.

## Setup

Copy `local.env.example` to `local.env` and set:

- `UNARC_TOOLCHAIN_ABIV11`: directory with `x86_64-aros-gcc` (GCC 10.5.0 for ABIv11);
- `UNARC_SDK_ABIV11`: the ABIv11 SDK directory containing `include/` and `lib/`;
- for mainline, `UNARC_TOOLCHAIN_V1` and `UNARC_SDK_V1` (the mainline GCC 10.5
  toolchain and the `Developer` directory of a mainline build), and
  `UNARC_V1_HOST_BUILD`, that build's host tree (its `genmodule` writes XAD's
  headers, which the mainline SDK lacks, and its GCC tree rebuilds libgcc's
  unwinder, see below);
- optionally `UNARC_DEPS_MIRROR`: local clones to fetch dependencies from.

Environment variables already set take precedence over `local.env`. The scripts
need bash, CMake, git and patch.

## Build

Each script takes the ABI, `abiv11` (the default) or `v1`:

    scripts/fetch-deps.sh                 # pinned sources from deps.lock into deps/
    scripts/build-deps.sh <abi>           # static liblzma, libarchive, XAD into build/<abi>-x86_64/prefix
    scripts/build.sh <abi>                # build/<abi>-x86_64/bin/arca-cli and Arca
    scripts/build-lha.sh <abi>            # build/<abi>-x86_64/bin/unarc-lha (LHa for UNIX + patches/lha)
    experiments/7z-backend/build.sh aros  # unarc-7z for ABIv11 (aros-v1 for mainline)

The `*-abiv11.sh` names still work and build ABIv11. Mainline differs in three
places, each handled in the scripts: its SDK has no iconv (libarchive is built
without it and uses Arca's `locale_charset()`); it has no XAD headers
(generated with that build's genmodule); and its libgcc catches no C++ throw,
so `scripts/build-unwind-fix-v1.sh` must run before `unarc-7z` is built for
`aros-v1`, which links the rebuilt `unwind-dw2.o`.

`fetch-deps.sh` puts unmodified dependencies (`deps.lock`) in `deps/` and the
repositories we patch (`upstreams.json`) as clean checkouts in `upstream/`.
`build-deps.sh` applies `patches/<id>/series` to a copy in `build/`; neither
`deps/` nor `upstream/` is edited. See [patching](patching.md). `build.sh`
stops with a non-zero exit code if a linked program has undefined symbols:
collect-aros itself can exit 0 with symbols left unresolved, which is also why
`build-deps.sh` switches off functions CMake wrongly finds.

## Layout

- `src/core/`: the model, format recognition and engine choice (`registry.c`,
  `session.c`), name conversion (`names.c`), the extraction plan (`plan.c`), the
  only code that writes extracted files (`fsl.c`) and packing (`pack.c`);
  `methods.c` reads what a 7z or ZIP archive needs from its own index
  (encryption, methods); `bigfile.c` reads at 64-bit positions (ABIv11's
  `Seek` is 32-bit) and stamps archive files; `times.c` turns archive times
  into AROS dates with the locale's offset for every engine.
- `src/engines/`: libarchive, a statically linked XAD (`xad_static.c`, LGPL), and
  `be_7z.c`, which runs the separate program `unarc-7z` (7-Zip's 7z, RAR and
  ZIP handlers, `experiments/7z-backend/`) when it is installed next to Arca
  and arca-cli. It reads the program's line protocol (PROTOCOL.md there)
  from a `PIPE:` handle; the archive path goes as hex, the selection through a
  file in `T:`, the password through a second pipe. The program's protocol
  number is asked once per run; one of another release is not used.
  Starting a helper and reading its output is `src/core/helper.c`, shared
  with packing.
- Packing (`pack.c`): ZIP, tar.gz and 7z with libarchive's writers; LhA with
  the separate program `unarc-lha` (LHa for UNIX, `upstreams.json` "lha" with
  `patches/lha/`), given its names in a list file in `T:` and run in the
  items' drawer. Every format writes under a temporary name and gets its name
  at the end; an archive it replaces is deleted only then.
  Packing first walks every input once ("Preparing"): the list it builds,
  with each file's size, is what libarchive packs, and its total is the
  100 % of the progress, counted in input bytes the engine has taken. A file
  that grew after the walk is stored as far as its size then and reported; a
  drawer gone since is reported with everything below it. Cancel is honoured
  while the writer closes the archive and once more just before the
  temporary archive takes its name.
  LhA is different: its helper walks the drawers again itself and the list
  is dropped once it has given the totals and the link check, so files that
  change in between are packed as the helper finds them. Its progress is an
  estimate: each file counts in steps of LHa's running indicator (`q0`, up
  to 58 marks per file) times the file's size, and in full when LHa reports
  it done. The helper's output goes to a file in `T:` read as it grows,
  because PIPE: answers a read only once it can fill the whole buffer; that
  output handle is closed by us after the helper has ended, so its end is
  in the file before we read it.

Engine choice (`registry.c`): RAR goes to 7-Zip first. Plain 7z and ZIP go to
libarchive first, as it is 2-3 times faster with many small entries; before
it lists them, `methods.c` reads their index, and the engine answers
`UA_LIST_UNSUPPORTED` for an encrypted entry list, encrypted entries or a
method it lacks (Deflate64, Zstandard, ...). The session then tries the next
engine (7-Zip, then XAD for ZIP); if none reads the archive, the first list
is used as far as it goes (encrypted entries left out), or the archive is
refused as unsupported, not as damaged. A later engine that calls such an
archive damaged is not believed. 7z sets (`.7z.001`) go to 7-Zip first.
Damage is reported, and recovery stays a separate, explicit mode.

Times: AROS keeps local time; `times.c` shifts UTC times by the locale's
offset. 7-Zip marks each entry's time as stored local time or UTC
(protocol 3); for libarchive, RAR 4 and CAB are local, and a ZIP entry is
local when the central directory gives it no UTC extra field (`methods.c`).
ABIv11's `mktime()` ignores the time zone while `localtime()` applies it, so
libarchive's RAR 4 extended times come out shifted by the zone; RAR goes to
7-Zip first, which is right.

- `src/cli/`: `arca-cli`, the Shell program.
- `src/gui/`: `Arca`, the Zune program; `job.c` runs each job in a worker process.

## Using the command-line program

    arca-cli ARCHIVE,FILES/M,TO/K,LIST/S,PACK/S,FORMAT/K,LEVEL/K/N,RECOVER/S,ENGINE/K,
              OVERWRITE/S,SKIP/S,RENAME/S,NOWRAP/S,CANCELAFTER/K/N,STATS/S,QUIET/S,
              DIAGOUT/S,FORMATS/S,HELP/S,PASSWORD/K

- `arca-cli HELP` prints usage with examples; `FORMATS` what each format supports.
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
- Passwords (7z, RAR and ZIP with ZipCrypto or AES, through unarc-7z): an encrypted entry list is asked for
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
  volumes' own headers (read at 64-bit positions and checked against their
  CRC, so volumes over 2 GB work); an incomplete set is refused with the
  missing volume's name.
- The archive's files are stamped when listed (size, date, CRC of the first
  and last 64 KB) and checked again before extracting; a file that changed is
  refused before anything is written. Each engine also compares every header
  with the listed entry while extracting, which catches a change the stamp
  cannot see (the middle of a file rewritten in place, same size and date),
  but only when that entry is reached.
- `ENGINE` (`libarchive`, `xad`, `7-zip`, `libarchive-stream`), `CANCELAFTER`
  (bytes) and `TESTSWAP` (a file copied over the archive between listing and
  extracting) are for testing; so is the variable `UnarcTestNoStamp`, which
  skips the stamp check so that the header comparison can be tested alone.

## The Zune program

`Arca` with no arguments opens a start window: drop an archive to extract it,
files and drawers to pack them, or use the buttons, which do the same through
file requesters. Started with an archive (a project icon whose default tool is
`Arca`, or `Arca <archive>` in the Shell) it extracts next to the archive and
shows the result; the start window's "Double-clicked archives" setting, or the
tool type `MODE=BROWSE` (`BROWSE` in the Shell), shows the contents first instead.
`MODE=EXTRACT` / `EXTRACT` force extraction. The setting is stored in
`ENVARC:Arca.prefs`.

The centered packing preview follows the format selector using system DefIcons.
It falls back from format artwork to Archive and then the system project icon.
Arca's own Workbench icon is independent of the preview. See
[icon assets](../assets/README.md) for lookup and scaling details.

No file type association is installed by the build. To try it, give an archive a
project icon with `Arca` as its default tool, or set `Arca` as the default tool
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
packing; `UnarcTestGoneDir` (`name`) makes packing unable to open that drawer
again after preparing, as if deleted; `UnarcTestCancelAtFinish` sends Ctrl-C
when the writer starts closing the archive; `UnarcTestHelperNoProc` makes
starting a helper fail after its output file is open.

## Tests

- `tests/corpus/make_corpus.py` builds the test archives (Amiga archives are made
  by Aminet's LhA and LZX under amitools' vamos).
- `tests/guest/make_stage.py` packs the command-line program, the corpus and an
  AROS Shell script for a test machine (options select the suites: `core2`,
  `regress`, `pack`, `vol`, `ffsprobe`, ...); results go to `RESULTS:<run-id>/`.
- `tests/guest/make_gui_stage.py` adds the Zune program, test project icons and
  the memory/lock loop for the GUI; `make_7zi_cases.py` the engine-choice matrix
  (7z, RAR, ZIP, passwords, sets, Ctrl-C, no helper, a helper of another
  release); `make_review_cases.py` the time zone check, the package as a user
  installs it and an archive for cancelling in the GUI.
- `tests/host/verify_core2.py`, `verify_pack.py`, `verify_volumes.py` and
  `verify_7zi.py` check collected results on the host; references come from
  7-Zip, unar and Python, not from our own code.
- Host unit tests, built with AddressSanitizer and UBSan over a DOS shim with
  AROS's 32-bit `LONG` (`tests/host/shim`): `test_volumes.py` (the volume
  finder, including volumes over 2 GB and failing allocations),
  `test_methods.py` (what 7z and ZIP archives need; `--big` adds a 7z over
  2 GB), `be7z_host.c` (the 7-Zip engine's handling of the protocol, with the
  program replaced by a given output), `plan_host.c`. `experiments/7z-backend/
  host_test.py` checks the helper itself.
- `tests/host/screen/`: screen recording over QMP and a blank-frame count, for
  flicker measurements.
- `tests/probes/pack_icons.c` builds as `pack-icons-test` for each ABI. It checks
  named DefIcon lookup/fallback and repeated preview switches over window
  setup/cleanup. It removes its uniquely named temporary ENV icon.
- `tests/probes/`: small programs that isolate platform behaviour (iconv, long
  paths, FFS file creation), `fake7z` (a helper of another release) and
  `uatool`, which makes project icons, opens objects as Workbench does, sends
  AppWindow drops, sends Ctrl-C to a running command, prints file dates and the
  locale's offset (and sets it), and opens, closes or moves windows for the
  drag tests.

## Package

Build for a release name first: `UNARC_RELEASE=0.0.4-test7 scripts/build.sh <abi>`.
The name goes into both programs' `$VER` after the date and into what they
show (the settings window, `arca-cli HELP`); without it they are
`0.0.4-dev`. The `$VER` version.revision is major.(minor * 100 + patch), since
`Version` and installers compare two numbers: 0.0.4 is 0.4. The build derives
it from the name and refuses a patch number of 100 or more, where the pairs
would stop being unique (`src/version.h`).

`packaging/make-package.sh <version> [abiv11|v1]` writes
`dist/Arca-<version>-x86_64-abiv11.zip` or `dist/Arca-<version>-x86_64-v1.zip`
(one drawer: the three programs, ReadMe, icons from `packaging/make-icons.py`,
and a Licences drawer with every licence text, THIRD_PARTY_NOTICES and the
source revision) and adds it to `dist/SHA256SUMS`. It refuses to replace a
file in `dist/`, to package uncommitted sources, and to package programs built
for another release name.

Mainline AROS ships no UnZip (and no lha). `packaging/zip-to-pkg.py
dist/Arca-<version>-x86_64-v1.zip` writes the same files, byte for byte, as an
AROS `.pkg` next to it, which the system's own `C:Unpack` installs
(`Unpack Arca-<version>-x86_64-v1.pkg TO SYS:Utilities`). Unpack keeps no
protection bits or dates; the files get the volume's defaults (`rwed`).

## Provisional application name

The application is now Arca and the command is arca-cli. The repository,
UNARC_* build variables, test variables and helper filenames remain unchanged.
The build emits Arca and arca-cli plus build-only Unarc/unarc-cli aliases for
existing staging scripts; packages contain only the new application names.
The MUI application base stays UNARC to preserve MUI preferences. Application
settings load Arca.prefs first, fall back to Unarc.prefs, and save Arca.prefs.
Existing Workbench default-tool associations must be updated to Arca manually.
