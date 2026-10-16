#!/usr/bin/env python3
"""Compare a 7zi run (tests/guest/make_7zi_cases.py) with the host references.

Usage: verify_7zi.py <stage-dir>/7zi/expected <collected RESULTS:<run> dir>
For each case: the return code from 7zi-log.txt against the one the case
name expects, and the guest's files (path and SHA-256) against the
reference. The guest extracts into a folder the plan chooses (the archive's
top folder or one named after it); when every guest path starts with one
folder the reference does not have, that folder is taken off first.
"""
import os, re, sys

exp, res = sys.argv[1:3]
log = open(os.path.join(res, "7zi-log.txt"), "rb").read().decode("latin-1")
rcs = {int(m.group(1)): int(m.group(2))
       for m in re.finditer(r"== (\d+) .*?\nRC=(\d+)", log, re.S)}
fails = 0
for line in open(os.path.join(exp, "cases.txt")):
    n, what = int(line[:2]), line[3:].strip()
    want_rc = int(re.search(r"RC (\d+)", what).group(1)) if re.search(r"RC (\d+)", what) else 0
    got = open(os.path.join(res, f"7zi-{n:02d}.sums"), "rb").read().decode("latin-1").split("\n")
    got = [g for g in got if g.strip()]
    ref_path = os.path.join(exp, f"{n:02d}.sums")
    if os.path.exists(ref_path):
        ref = [r for r in open(ref_path, "rb").read().decode("latin-1").split("\n") if r.strip()]
        g = [x.split("  ", 1) for x in got if x != "NOEXIST"]
        r = [x.split("  ", 1) for x in ref]
        firsts = {p.split("/", 1)[0] for _, p in g}
        if len(firsts) == 1 and all("/" in p for _, p in g) and not any(
                p.split("/", 1)[0] in firsts for _, p in r):
            g = [(h, p.split("/", 1)[1]) for h, p in g]
        ok_files = sorted(map(tuple, g)) == sorted(map(tuple, r)) and "NOEXIST" not in got
        detail = f"{len(g)} files, reference {len(r)}"
    else:
        ok_files = got == ["NOEXIST"]
        detail = "nothing written" if ok_files else f"{len(got)} lines written"
    ok = ok_files and rcs.get(n) == want_rc
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {n:02d} RC={rcs.get(n)} (want {want_rc}) {detail}: {what}")
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
