# XAD LhA upstream fixes

These four independent patches target mainline contrib, based on 6bbefa406c281909294c4c4e8743f7f892caa1cf. They are preserved under the separate `contrib-mainline` manifest/series. The existing application `contrib` pin/series is unchanged; integration is a separate step. Do not apply both series on top of each other.

| Patch | Reproduction | Expected result |
|---|---|---|
| spaces | level1/2 member named `space name` | same name, not `space_name` |
| hold | Amiga level1/2 protection byte 0x80 | protection 0x80, not 0 |
| directories | named -lhd- at levels1/2; empty level1 basename before its comment | directory flag, correct path and comment |
| protection | explicit UNIX 0644 or DOSreadonly in an Amiga archive; both extension orders | UNIX 0x8802, DOS 0x15; last explicit type/value remains consistent |

## Reproduction data

Run `python3 tests/upstream/xad_lha/make_cases.py` to generate 18 small stored LHA fixtures and expected.json. Every fixture passed `7zz t`. The output data is synthetic; no third-party archive binary is required. `probe.c` prints each XAD entry's name, type, Amiga protection and comment. It uses the same static initialization adapter as the application, src/engines/xad_static.c. The fixes themselves do not depend on that adapter.

## Mainline verification

2026-10-18: mainline ABIv1 x86_64 under QEMU, GCC 10.5.0, mainline SDK. Built baseline and four isolated variants from the same contrib code and initialization adapter. All platform headers, compiler, startup and linked runtime libraries were mainline. Missing XAD API declaration headers were supplied through an isolated SDK-202609 XAD-only include overlay; no ABIv11 library/binary was linked.

Ran one fixture per command, 18 per variant (90 processes); every return code0. Spaces and hold each fix 2 target cases with 16 unchanged controls; directory fix covers 4 cases with 14 unchanged controls; protection fixes 8 cases with 10 unchanged controls. The directory-name-only level2 case was already correct and remains a control within the directory group. `check_results.py` checks both target metadata and byte-equivalent parsed control output. Raw receipts are retained in private run evidence. Generate fixtures, then pass the collected guest-result directory to the checker; no private paths are needed to build the application.

This checks parser metadata, not filesystem extraction, the installed shared library, or complete Unarc support on mainline. No system library was changed. An initial invalid run used an overlong Execute line and is excluded; the recorded receipts use short individual commands.

Independent code review found no blocker. The previous combined application's UNIX boolean is deliberately not reused: each extension updates its conversion discriminator, avoiding the 0x50/0x40 ordering defect.
