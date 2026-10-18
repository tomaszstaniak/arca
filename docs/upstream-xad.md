# XAD upstream fixes

Submission target: mainline contrib. The application does not depend on acceptance.

## LZX pure flag

Merged 2026-10-18: https://github.com/aros-development-team/contrib/pull/77
Branch commit: 655518fc (based on current contrib d482c090).

`patches/contrib/0001-xad-LZX-store-the-pure-bit-as-FIBF_PURE.patch` changes the decoded Amiga pure flag from bit 4 (archive) to bit 5 (pure).

Verified 2026-10-18 against current contrib d482c0907fe312bcebef38d3c94ea4818f9f7a30 on mainline ABIv1 x86_64 under QEMU, GCC 10.5.0 and a mainline SDK. Two executables built the same XAD built-in clients and static initialization adapter, before and after the one-line change. The fixture was generated with Amiga LZX 1.21 and contains script, pure and read-only entries with comments.

Reproduction: mark a file pure, archive it with Amiga LZX, and inspect xadmaster's `xfi_Protection`. Upstream returns 0x10; corrected code returns 0x20. In the comparison both programs returned 0. Other LZX entries and LHA level 0/1 control archives were unchanged. This validates metadata decoding; it is not a claim that Unarc is supported on mainline or that the shared xadmaster.library was rebuilt and installed.

## LhA

Four isolated fixes have been tested on mainline ABIv1 and submitted as contrib PRs #78–81. See [the LhA verification and submission record](upstream-xad-lha.md).

The application's legacy combined patch remains separate. Its protection-extension ordering problem is corrected in the submitted series; do not confuse that upstream series with the current application integration.
