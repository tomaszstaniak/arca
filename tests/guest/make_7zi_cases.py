#!/usr/bin/env python3
"""unarc-cli through the 7-Zip engine (unarc-7z) on AROS: add the cases to a
GUI stage (make_gui_stage.py), with the host's reference extraction of each.

Usage: make_7zi_cases.py <stage-dir> <run-id> <aros unarc-7z> <test-archive-dir> <corpus-dir>
In the guest, after setup.script: Execute RAM:u/7zi.script
Each case extracts to RAM:o/<n> and writes RESULTS:<run>/7zi-<n>.sums (uatool
sum); the log of all runs is RESULTS:<run>/7zi-log.txt. References come from
7zz (7z) and unar (RAR), not from our own code: <stage>/7zi/expected/<n>.sums.
Compare with tests/host/verify_7zi.py.
"""
import hashlib, os, random, shutil, subprocess, sys, tempfile

stage, run, aros_bin, testdir, corpus = sys.argv[1:6]
SZ, UNAR = "/opt/homebrew/bin/7zz", "/opt/homebrew/bin/unar"
d = os.path.join(stage, "7zi")
exp = os.path.join(d, "expected")
os.makedirs(exp, exist_ok=True)
shutil.copy2(aros_bin, os.path.join(stage, "unarc-7z"))


def put(src, name=None):
    shutil.copy2(src, os.path.join(d, name or os.path.basename(src)))


for f in ("tree.7z", "r4.part1.rar", "r4.part2.rar", "r4.part3.rar", "r4.part4.rar",
          "gap.part1.rar", "gap.part2.rar", "gap.part4.rar") + tuple(
              f"r5.part0{i}.rar" for i in range(1, 9)):
    put(os.path.join(corpus, f))
put(os.path.join(testdir, "enc.7z"))
put(os.path.join(testdir, "hdr.7z"))
for f in ("rar5_solid_encrypted.rar", "rar4_solid_encrypted.rar",
          "rar5_solid_encrypted_filenames.rar"):
    put(os.path.join(testdir, "rar-enc", f))
# The archive's own name in ISO-8859-1 with a space (made in the guest by the
# script, which is written in ISO-8859-1): the path goes to unarc-7z as hex
# and is opened byte for byte.
latin = "Gr\xfcn Archiv.7z"
# A large solid 7z for timing both engines: 3000 small files and some big ones.
with tempfile.TemporaryDirectory() as t:
    src = os.path.join(t, "solid")
    rnd = random.Random(5)
    for i in range(3000):
        p = os.path.join(src, f"d{i // 100:02d}")
        os.makedirs(p, exist_ok=True)
        open(os.path.join(p, f"f{i:04d}.txt"), "w").write(f"file {i}\n" * (i % 50 + 1))
    for i in range(4):
        open(os.path.join(src, f"big{i}.bin"), "wb").write(bytes(rnd.getrandbits(8) for _ in range(1 << 20)))
    subprocess.run([SZ, "a", "-mx5", "-ms=on", os.path.join(d, "solid.7z"), "solid"], cwd=t,
                   check=True, stdout=subprocess.DEVNULL)


def sums(root):
    out = []
    for base, _, files in os.walk(root):
        for fn in files:
            p = os.path.join(base, fn)
            if os.path.islink(p):
                continue
            rel = os.path.relpath(p, root)
            out.append((hashlib.sha256(open(p, "rb").read()).hexdigest(), rel))
    return sorted(out)


def reference(n, archive, password=None, only_plain=False):
    """The tree a correct extraction gives, as 'sha  path' lines (ISO-8859-1)."""
    with tempfile.TemporaryDirectory() as t:
        a = os.path.join(d, archive)
        if archive.endswith(".7z"):
            cmd = [SZ, "x", "-y", "-o" + t, a] + (["-p" + password] if password else ["-p"])
        else:
            cmd = [UNAR, "-q", "-D", "-o", t, a] + (["-p", password] if password else [])
        # Links are not extracted on AROS (not by any engine): leave them out.
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        lines = [] if only_plain else [f"{h}  {r}" for h, r in sums(t)]
    with open(os.path.join(exp, f"{n:02d}.sums"), "wb") as f:
        f.write("".join(l + "\n" for l in lines).encode("latin-1"))


O = f"RESULTS:{run}"
L = ["FailAt 30", "Stack 40960", "Protect RAM:u/unarc-7z +e", f"MakeDir {O}", "MakeDir RAM:o",
     "CD RAM:u/7zi", f'Copy tree.7z "{latin}"']
cases = []


def case(n, args, what, ref=None, stdin_nil=True):
    log = f"{O}/7zi-log.txt"
    L.append(f'Echo "== {n:02d} {what}" >>{log}')
    L.append(f"RAM:u/unarc-cli{' <NIL:' if stdin_nil else ''} >>{log} {args} TO RAM:o/{n:02d} "
             "DIAGOUT STATS QUIET")
    L.append(f'Echo "RC=$RC" >>{log}')
    L.append(f"If EXISTS RAM:o/{n:02d}")
    L.append(f"RAM:u/uatool sum RAM:o/{n:02d} >{O}/7zi-{n:02d}.sums")
    L.append("Else")
    L.append(f"Echo NOEXIST >{O}/7zi-{n:02d}.sums")
    L.append("EndIf")
    cases.append((n, what))
    if ref:
        reference(n, *ref)


case(1, "tree.7z", "7z, plain", ("tree.7z",))
case(2, "tree.7z ENGINE libarchive", "7z, plain, libarchive (cross-check)", ("tree.7z",))
case(3, "enc.7z", "encrypted data, no password: entries left out, RC 5", ("enc.7z", "secret", True))
case(4, "enc.7z PASSWORD secret", "encrypted data, right password", ("enc.7z", "secret"))
case(5, "enc.7z PASSWORD wrong", "encrypted data, wrong password: errors, RC 10", ("enc.7z", "secret", True))
case(6, "hdr.7z", "encrypted list, no password: RC 20, nothing written")
case(7, "hdr.7z PASSWORD wrong", "encrypted list, wrong password: RC 20")
case(8, "hdr.7z PASSWORD secret", "encrypted list, right password", ("hdr.7z", "secret"))
case(9, "rar5_solid_encrypted.rar PASSWORD password", "RAR 5 solid encrypted",
     ("rar5_solid_encrypted.rar", "password"))
case(10, "rar4_solid_encrypted.rar PASSWORD password", "RAR 4 solid encrypted",
     ("rar4_solid_encrypted.rar", "password"))
case(11, "rar5_solid_encrypted_filenames.rar PASSWORD password", "RAR 5 encrypted list",
     ("rar5_solid_encrypted_filenames.rar", "password"))
case(12, "r5.part01.rar", "RAR 5 set of 8 volumes", ("r5.part01.rar",))
case(13, "r4.part2.rar", "RAR 4 set given by its second volume (its link is not extracted: RC 5)", ("r4.part1.rar",))
case(14, "gap.part1.rar", "RAR 4 set without volume 3: RC 20")
case(15, f'"{latin}"', "archive name in ISO-8859-1 with a space", ("tree.7z",))
case(16, "solid.7z", "large solid 7z (3000 files), 7-Zip engine", ("solid.7z",))
case(17, "solid.7z ENGINE libarchive", "large solid 7z, libarchive", ("solid.7z",))
# Ctrl-C during the large solid archive: CANCELAFTER is the CLI's own test
# aid (stops after N bytes, as Ctrl-C does), so the run needs no key timing.
L.append(f'Echo "== 18 solid.7z cancelled after 2 MB: RC 10" >>{O}/7zi-log.txt')
L.append(f"RAM:u/unarc-cli <NIL: >>{O}/7zi-log.txt solid.7z TO RAM:o/18 CANCELAFTER 2000000 DIAGOUT QUIET")
L.append(f'Echo "RC=$RC" >>{O}/7zi-log.txt')
L.append(f"List RAM:o/18 ALL FILES LFORMAT \"%l %p%n\" >{O}/7zi-18.list")
L.append(f"List RAM:o ALL LFORMAT \"%p%n %l\" >{O}/7zi-tree.txt")
L.append("Delete RAM:o ALL QUIET")
L.append('Echo "7zi.script done"')
with open(os.path.join(stage, "7zi.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(L) + "\n")
with open(os.path.join(exp, "cases.txt"), "w") as f:
    f.write("".join(f"{n:02d} {w}\n" for n, w in cases))
print(len(cases), "cases")
