# XAD LhA upstream fixes

Four independent fixes target mainline contrib at `6bbefa406c281909294c4c4e8743f7f892caa1cf`:

| Change | Pull request | Reproduction |
|---|---|---|
| Preserve spaces | https://github.com/aros-development-team/contrib/pull/78 | Level 1/2 member `space name` was reported as `space_name`. |
| Unsigned protection byte | https://github.com/aros-development-team/contrib/pull/79 | Amiga attribute `0x80` was promoted as negative and its hold flag was lost. |
| Directory recognition | https://github.com/aros-development-team/contrib/pull/80 | Named `-lhd-` entries and an empty basename before an Amiga comment were reported as files. |
| Protection extension types | https://github.com/aros-development-team/contrib/pull/81 | An explicit UNIX or MS-DOS value was interpreted using the archive OS identifier. |

The saved `contrib-mainline` manifest and series are separate from the application's legacy `contrib` pin/series. Do not apply both series on top of each other. Application integration is a separate step.

## Verification

Tested 2026-10-18 on mainline ABIv1 x86_64 under QEMU, GCC 10.5.0 and mainline SDK. A baseline and four isolated variants used the same XAD built-in clients and static initialization adapter. The mainline SDK lacks XAD API declarations; an isolated XAD-only declaration overlay came from SDK-202609. All ABI macros, startup code and linked runtime libraries were mainline. No ABIv11 library or executable was linked, and no system library was replaced.

Each variant read 18 synthetic level 1/2 archives, one per command. All 90 exit codes were 0. Each fixture also passed `7zz t` independently.

- Spaces: 2 target cases corrected; 16 controls unchanged.
- Hold: 2 target cases corrected; 16 controls unchanged.
- Directories: 4 cases passed, including the already-correct directory-name-only level 2 control; 14 other outputs unchanged.
- Protection: 8 cases corrected; 10 controls unchanged. UNIX mode 0644 maps to `0x8802`, MS-DOS read-only to `0x15`; both mixed extension orders retain the last explicit value and its type.

This verifies metadata decoding, not filesystem extraction or complete Unarc support on mainline. An initial run with an overlong Execute line was invalid and excluded. The recorded comparison uses short commands with individual result and return-code files.

## Reproduction

`tests/upstream/xad_lha/` contains the generator, metadata probe and receipt checker. Run the generator, then pass the collected guest-result directory to the checker. Raw receipts remain in private run evidence. The PR bodies also include small self-contained base64 fixtures, so reproduction does not depend on this application repository.

Independent code review found no blocker. The previous combined application's sticky UNIX flag is not reused: each extension updates the conversion discriminator along with its value.
