#!/usr/bin/env python3
"""Add the unarc-7z probe and its cases to a GUI stage directory, and write the
host's answers for the same commands next to them for comparison.

Usage: make_7z_cases.py <stage-dir> <run-id> <aros unarc-7z> <host unarc-7z> <file>...
The files (archives and RAR/7z volume sets) are copied to <stage>/7z/. In the
guest: Execute RAM:u/7z.script (after setup.script). Each case writes
RESULTS:<run>/7z-<n>.out with the program's whole stdout (records and data);
expected/7z-<n>.out holds the host's output for the same command. Cases:
list without a password, with a wrong one and with the right one; extract of
entry 1 (or 0) with the right password. Passwords come from files on stdin.
"""
import os, shutil, subprocess, sys

stage, run, aros_bin, host_bin = sys.argv[1:5]
files = sys.argv[5:]
d = os.path.join(stage, "7z")
os.makedirs(os.path.join(d, "expected"), exist_ok=True)
shutil.copy2(aros_bin, os.path.join(stage, "unarc-7z"))
for f in files:
    shutil.copy2(f, os.path.join(d, os.path.basename(f)))
pw = {"enc.7z": "secret", "hdr.7z": "secret"}
for f in files:
    if os.path.basename(f).startswith(("rar4_", "rar5_")):
        pw[os.path.basename(f)] = "password"
open(os.path.join(d, "pw-none"), "w").write("")
open(os.path.join(d, "pw-wrong"), "w").write("notit\n")
for p in set(pw.values()):
    open(os.path.join(d, f"pw-{p}"), "w").write(p + "\n")

O = f"RESULTS:{run}"
L = ["FailAt 30", "Stack 40960", "Protect RAM:u/unarc-7z +e", f"MakeDir {O}", "CD RAM:u/7z"]
n = 0


def case(args, stdin, archive_dir):
    global n
    n += 1
    out = f"{O}/7z-{n:02d}.out"
    L.append(f"Echo \"{n:02d} unarc-7z {' '.join(args)} <{stdin}\" >>{O}/7z-cases.txt")
    L.append(f"RAM:u/unarc-7z {' '.join(args)} <{stdin} >{out}")
    L.append(f"Echo \"RC=$RC\" >>{O}/7z-cases.txt")
    # The host answer for the same command. In UTC: RAR 4 and ZIP store local
    # times, 7-Zip turns them into UTC with the process's zone, and the guest
    # has none (offset 0), so it reports the stored wall-clock time.
    with open(os.path.join(archive_dir, stdin), "rb") as si:
        r = subprocess.run([host_bin] + args, cwd=archive_dir, stdin=si, capture_output=True,
                           env=dict(os.environ, TZ="UTC"))
    open(os.path.join(d, "expected", f"7z-{n:02d}.out"), "wb").write(r.stdout)


for f in files:
    b = os.path.basename(f)
    if b.startswith(("r5.part0", "r4.part", "old.r")) and b not in ("r5.part01.rar",
                                                                   "r4.part1.rar", "old.rar"):
        continue                     # later volumes: opened through the first
    case(["list", b], "pw-none", d)
    if b in pw:
        case(["list", b], "pw-wrong", d)
        case(["list", b], f"pw-{pw[b]}", d)
        case(["extract", b, "1"], f"pw-{pw[b]}", d)
    else:
        case(["extract", b, "1"], "pw-none", d)
with open(os.path.join(stage, "7z.script"), "w") as fh:
    fh.write("\n".join(L) + "\nEcho \"7z.script done\"\n")
print(n, "cases")
