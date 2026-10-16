---
type: guide
updated: 2026-10-15
---

# Reproducing and saving upstream changes

## Sources

- `upstreams.json` lists the repositories we patch, with the full pinned commit.
  Today: `contrib` (AROS contrib, only `workbench/libs/xad` is checked out).
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
and builds from that copy. The copy is regenerated on every build; do not edit it.

## Making a new patch

There is no `work/` tooling yet. By hand:

1. Make a scratch copy outside the repository: `git clone upstream/contrib /tmp/contrib-work`,
   apply the existing series there with `git am patches/contrib/*.patch` in series order.
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
| contrib | `0001-xad-LZX-store-the-pure-bit-as-FIBF_PURE.patch` | candidate, not submitted; tested on ABIv11 x86_64 only |
