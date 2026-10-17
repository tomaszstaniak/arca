#!/usr/bin/env python3
"""Compare a 7zi run (tests/guest/make_7zi_cases.py) with the host references.

Usage: verify_7zi.py <stage-dir>/7zi/expected <collected RESULTS:<run> dir>
For each case: the return code and the engine (the run's "Engine:" line)
against cases.txt, and the guest's files (path and SHA-256) against the
reference. The guest extracts into a folder the plan chooses (the archive's
top folder or one named after it); when every guest path starts with one
folder the reference does not have, that folder is taken off first.
Also: case 30 (a CP437 ZIP name through 7-Zip) must give exactly the files
of case 29 (libarchive); the Ctrl-C cases end stopped (31, while listing) or
cancelled (32, while decoding); no unarc-7z may be left running at the end.
"""
import os, re, sys

exp, res = sys.argv[1:3]
log = open(os.path.join(res, "7zi-log.txt"), "rb").read().decode("latin-1")
blocks = {int(m.group(1)): m.group(2) for m in re.finditer(r"== (\d+) [^\n]*\n(.*?)(?=\n== \d+ |\Z)", log, re.S)}
fails = 0


def got_sums(n):
    got = open(os.path.join(res, f"7zi-{n:02d}.sums"), "rb").read().decode("latin-1").split("\n")
    return [g for g in got if g.strip()]


def strip_top(g, r):
    firsts = {p.split("/", 1)[0] for _, p in g}
    if len(firsts) == 1 and all("/" in p for _, p in g) and not any(
            p.split("/", 1)[0] in firsts for _, p in r):
        return [(h, p.split("/", 1)[1]) for h, p in g]
    return g


def check(ok, line):
    global fails
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {line}")


for line in open(os.path.join(exp, "cases.txt"), encoding="latin-1"):
    n, want_rc, want_eng, what = line.rstrip("\n").split("|", 3)
    n, want_rc = int(n), int(want_rc)
    b = blocks.get(n, "")
    m = re.search(r"RC=(\d+)", b)
    rc = int(m.group(1)) if m else None
    m = re.search(r"^Engine:\s+(\S+)", b, re.M)
    eng = m.group(1) if m else "-"
    got = got_sums(n)
    ref_path = os.path.join(exp, f"{n:02d}.sums")
    if os.path.exists(ref_path):
        ref = [r for r in open(ref_path, "rb").read().decode("latin-1").split("\n") if r.strip()]
        g = [tuple(x.split("  ", 1)) for x in got if x != "NOEXIST"]
        r = [tuple(x.split("  ", 1)) for x in ref]
        g = strip_top(g, r)
        ok_files = sorted(g) == sorted(r) and "NOEXIST" not in got
        detail = f"{len(g)} files, reference {len(r)}"
    elif want_rc == 20:
        ok_files = got == ["NOEXIST"]
        detail = "nothing written" if ok_files else f"{len(got)} lines written"
    else:
        ok_files, detail = True, f"{len([x for x in got if x != 'NOEXIST'])} files (no reference)"
    ok = ok_files and rc == want_rc and eng == want_eng
    check(ok, f"{n:02d} RC={rc} (want {want_rc}) engine {eng} (want {want_eng}) {detail}: {what}")

check(got_sums(29) == got_sums(30) and len(got_sums(29)) == 2,
      "30 = 29: the CP437 name comes out the same through both engines")
check("Stopped while reading" in blocks.get(31, ""), "31: Ctrl-C while listing: stopped, nothing written")
check("CANCELLED" in blocks.get(32, ""), "32: Ctrl-C while decoding: cancelled")
for n, text in ((33, "unarc-7z is not next to Unarc"), (37, "another release")):
    check(text in blocks.get(n, ""), f"{n:02d}: says \"{text}\"")
st = open(os.path.join(res, "7zi-status.txt"), "rb").read().decode("latin-1")
check("unarc-7z" not in st, "no unarc-7z left running at the end")
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
