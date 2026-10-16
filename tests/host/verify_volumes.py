#!/usr/bin/env python3
"""Compare files extracted on AROS from multi-volume sets with what The
Unarchiver's command-line tool (unar, an independent reader) extracts from the
same sets on the host.

Usage: verify_volumes.py <collected run dir> <corpus dir>
"""
import hashlib, os, subprocess, sys, tempfile

run, corpus = sys.argv[1], sys.argv[2]
# V4: the mixed.* set is the r4 set with its names shuffled against the volume
# numbers inside. unar follows the names and so gets another order; the
# reference is therefore the same set under its right names.
cases = {"V1-r4-from-part2": "r4.part1.rar", "V2-old-names": "old.rar",
         "V3-alt-names-gap": "alt.rar", "V4-mixed-names": "r4.part1.rar",
         "V5-rar5-from-part5": "r5.part01.rar", "V6-7z-split": "split.7z.001"}
bad = 0


def tree(root):
    out = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            if os.path.islink(p):
                continue
            out[os.path.relpath(p, root)] = hashlib.sha256(open(p, "rb").read()).hexdigest()
    return out


def sums_from_log(path):
    out, on = {}, False
    for line in open(path, encoding="latin-1"):
        if line.startswith("== sums"):
            on = True
            continue
        if on and len(line) > 66 and line[64:66] == "  ":
            out[line[66:].rstrip("\n")] = line[:64]
    return out


for case, first in cases.items():
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["unar", "-q", "-f", "-D", "-o", tmp, os.path.join(corpus, first)],
                       capture_output=True, text=True)
        want = tree(tmp)
    got = sums_from_log(os.path.join(run, case + ".txt"))
    # Either side may put everything under one folder (ours: the folder rule);
    # compare below it.
    def norm(d):
        firsts = {k.split("/")[0] for k in d}
        if len(firsts) == 1 and all("/" in k for k in d):
            return {k.split("/", 1)[1]: v for k, v in d.items()}
        return d
    want, g = norm({k.replace(os.sep, "/"): v for k, v in want.items()}), norm(got)
    missing = [k for k in want if k not in g]
    differ = [k for k in want if k in g and g[k] != want[k]]
    ok = bool(want) and not missing and not differ
    print(f"{case}: {'ok' if ok else 'PROBLEM'} ({len(want)} files from unar, {len(got)} sums "
          f"from AROS)" + (f" missing {missing[:3]}" if missing else "") +
          (f" differ {differ[:3]}" if differ else ""))
    bad += 0 if ok else 1
print("RESULT:", "ok" if not bad else f"{bad} problems")
sys.exit(1 if bad else 0)
