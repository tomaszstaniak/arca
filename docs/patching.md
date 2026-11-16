---
type: guide
updated: 2026-11-01
---

# Reproducing and saving upstream changes

## Sources

- `upstreams.json` lists the repositories we patch, with the full pinned commit.
  Today: `contrib` (AROS contrib, only `workbench/libs/xad` is checked out) and
  `lha` (sodero/lha, LHa for UNIX with Amiga support, built as the helper
  `unarc-lha`).
- `scripts/fetch-deps.sh` puts a clean checkout of each in `upstream/<id>/`
  (ignored by Git). It refuses to touch a checkout that has local changes or sits
  at another revision.
- `patches/<id>/series` lists that repository's patches in application order.
  Every listed repository must have a `series` file; an empty one means the pin
  is used unchanged.
- Unmodified dependencies (libarchive, xz) are pinned in `deps.lock` and fetched
  into `deps/`; they have no patches.

## How the build uses them

`scripts/build-deps-abiv11.sh` copies `upstream/contrib/workbench/libs/xad` to
`build/abiv11-x86_64/contrib-src/`, applies the series in order with `patch -p1`,
and builds from that copy. `scripts/build-lha-abiv11.sh` does the same for
`upstream/lha` in `build/abiv11-x86_64/lha-src/`. The copies are regenerated on
every build; do not edit them.

## Making a new patch

There is no `work/` tooling yet. By hand:

1. Make an editable copy in `work/<id>/` (ignored by Git): `git clone upstream/<id>
   work/<id>`, and apply the existing series there with `git am` in series order.
2. Change, build and test.
3. `git format-patch -1` in the scratch copy, name the result with the next unused
   number, and add it to `series`.
4. Fill the message: problem, how to reproduce it, cause, change, how it was tested
   (target, system, results), and the trailer lines `Repository:`, `Base:`,
   `Requires:`, `Scope:`, `Upstream-Status:` (`candidate`, `local-only`, `submitted`
   with a link, `accepted` with the commit) and `Removal:`.

A patch whose test has not been run says so. A patch is not described as tested on a
target where it was not run.

## Current series

| Repository | Patch | Status |
|---|---|---|
| contrib | `0001-xad-LZX-store-the-pure-bit-as-FIBF_PURE.patch` | accepted upstream in [contrib #77](https://github.com/aros-development-team/contrib/pull/77); retained for the pinned base; tested on ABIv11 x86_64 only |
| contrib | `0002-xad-LhA-directories-protection-bits-and-names.patch` | submission superseded by accepted [contrib #78](https://github.com/aros-development-team/contrib/pull/78), [#79](https://github.com/aros-development-team/contrib/pull/79), [#80](https://github.com/aros-development-team/contrib/pull/80) and [#81](https://github.com/aros-development-team/contrib/pull/81); retained for the old pin |
| lha | `0001`-`0005` (no UNIX permission headers in Amiga archives; no NUL without a comment; `--list=FILE`; Ctrl-C between blocks; options not expanded as patterns) | candidate, not submitted; tested on AROS One 1.3 (ABIv11 x86_64) only |

Upstream status checked on 2026-11-01. Accepted upstream does not establish inclusion in our pinned dependencies. The separate sodero/lha helper patches are not covered by the contrib merges and remain candidates; no new runtime validation was performed.
