# XAD LhA metadata regressions

`make_cases.py` generates 18 synthetic, stored LHA archives (levels 1 and 2) and an expected-metadata manifest. It needs only Python 3. The fixtures contain no third-party data.

    python3 tests/upstream/xad_lha/make_cases.py

Build `probe.c` with XAD and the application's static initialization adapter (`src/engines/xad_static.c`), using the target compiler and SDK. Run each fixture separately with the baseline and each isolated fix. Keep each command short: mainline Execute misinterpreted the first test script's long argument list.

For variant `baseline`, `spaces`, `hold`, `directories` or `protection` and fixture stem `NAME`, write stdout to `lhax2-VARIANT-NAME.txt` and the immediate Shell return code to `lhax2-VARIANT-NAME.rc`. Then check the collected results:

    python3 tests/upstream/xad_lha/check_results.py /path/to/guest-results

The checker requires all 90 return codes to be 0, validates the target fields, compares unrelated output with the baseline, and requires each fix to change at least one reproduced failure. It writes a small summary.json beside the generator. Raw guest receipts belong in private run evidence, outside this repository.

Every generated archive also passed `7zz t` independently. See `docs/upstream-xad-lha.md` for the platform, build scope and results.
