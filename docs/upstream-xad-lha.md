# XAD LhA upstream fixes

These four independent patches target mainline contrib, based on6bbefa406c281909294c4c4e8743f7f892caa1cf. They are preserved under the separate `contrib-mainline` manifest/series. The existing application `contrib` pin/series is unchanged; integration is a separate step. Do not apply both series on top of each other.

| Patch | Reproduction | Expected result |
|---|---|---|
| spaces | level1/2 member named `space name` | same name, not `space_name` |
| hold | Amiga level1/2 protection byte0x80 | protection0x80, not0 |
| directories | named -lhd- at levels1/2; empty level1 basename before its comment | directory flag, correct path and comment |
| protection | explicit UNIX0644 or DOSreadonly in an Amiga archive; both extension orders | UNIX0x8802, DOS0x15; last explicit type/value remains consistent |

## Reproduction data

Run `python3 tests/upstream/xad_lha/make_cases.py` to generate18 small stored LHA fixtures and expected.json. Every fixture passed7zz t. The output data is synthetic; no third-party archive binary is required. `probe.c` prints each XAD entry's name, type, Amiga protection and comment. It uses the same static initialization adapter as the application, src/engines/xad_static.c. The fixes themselves do not depend on that adapter.

## Mainline verification

2026-10-18: mainline ABIv1 x86_64 under QEMU, GCC10.5.0, mainline SDK. Built baseline and four isolated variants from the same contrib code and initialization adapter. All platform headers, compiler, startup and linked runtime libraries were mainline. Missing XAD API declaration headers were supplied through an isolated SDK-202609 XAD-only include overlay; no ABIv11 library/binary was linked.

Ran one fixture per command,18 per variant (90 processes); every return code0. Spaces and hold each fix2 target cases with16 unchanged controls; directory fix covers4 cases with14 unchanged controls; protection fixes8 cases with10 unchanged controls. The directory-name-only level2 case was already correct and remains a control within the directory group. `check_results.py` checks both target metadata and byte-equivalent parsed control output. Receipts are in tests/upstream/xad_lha/receipts-abiv1-20261018. Generate fixtures, then pass that directory to the checker.

This checks parser metadata, not filesystem extraction, the installed shared library, or complete Unarc support on mainline. No system library was changed. An initial invalid run used an overlong Execute line and is excluded; the recorded receipts use short individual commands.

Independent code review found no blocker. The previous combined application's UNIX boolean is deliberately not reused: each extension updates its conversion discriminator, avoiding the0x50/0x40 ordering defect.
