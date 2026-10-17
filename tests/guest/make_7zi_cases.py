#!/usr/bin/env python3
"""unarc-cli with its engine choice for 7z, RAR and ZIP on AROS (libarchive
first where it reads the archive, the 7-Zip helper unarc-7z for encryption,
sets and methods libarchive lacks): add the cases to a GUI stage
(make_gui_stage.py), with the host's reference extraction of each.

Usage: make_7zi_cases.py <stage-dir> <run-id> <aros unarc-7z> <aros fake7z>
                         <test-archive-dir> <corpus-dir>
In the guest, after setup.script: Execute RAM:u/7zi.script
Each case extracts to RAM:o/<n> and writes RESULTS:<run>/7zi-<n>.sums (uatool
sum); the log of all runs is RESULTS:<run>/7zi-log.txt. References come from
7zz (7z, ZIP) and unar (RAR), not from our own code: <stage>/7zi/expected/<n>.sums.
cases.txt gives each case's expected return code and engine (the "Engine:"
line of the run; "-" when the run stops before one is chosen).
Compare with tests/host/verify_7zi.py.

Runs without the helper (RAM:nh, only unarc-cli) and with a helper of another
release (RAM:mm, fake7z as unarc-7z) check the messages and the fallback.
"""
import hashlib, os, random, shutil, subprocess, sys, tempfile, zipfile

stage, run, aros_bin, fake_bin, testdir, corpus = sys.argv[1:7]
SZ, UNAR = "/opt/homebrew/bin/7zz", "/opt/homebrew/bin/unar"
d = os.path.join(stage, "7zi")
exp = os.path.join(d, "expected")
os.makedirs(exp, exist_ok=True)
shutil.copy2(aros_bin, os.path.join(stage, "unarc-7z"))
shutil.copy2(fake_bin, os.path.join(stage, "fake7z"))


def put(src, name=None):
    shutil.copy2(src, os.path.join(d, name or os.path.basename(src)))


for f in ("tree.7z", "r4.part1.rar", "r4.part2.rar", "r4.part3.rar", "r4.part4.rar",
          "gap.part1.rar", "gap.part2.rar", "gap.part4.rar", "split.7z.001", "split.7z.002",
          "split.7z.003", "split.7z.004", "splitgap.7z.001", "splitgap.7z.003",
          "splitgap.7z.004") + tuple(f"r5.part0{i}.rar" for i in range(1, 9)):
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

with tempfile.TemporaryDirectory() as t:
    # A large solid 7z for timing both engines: 3000 small files and some big ones.
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
    # Methods libarchive lacks, and ZIP encryption.
    small = os.path.join(t, "small")
    os.makedirs(os.path.join(small, "sub"))
    open(os.path.join(small, "a.txt"), "w").write("hello zip\n" * 300)
    open(os.path.join(small, "sub", "b.bin"), "wb").write(bytes(rnd.getrandbits(8) for _ in range(90000)))
    for name, opts in (("deflate64.7z", ["-t7z", "-m0=Deflate64"]),
                       ("deflate64.zip", ["-tzip", "-mm=Deflate64"]),
                       ("zc.zip", ["-tzip", "-psecret", "-mem=ZipCrypto"]),
                       ("za.zip", ["-tzip", "-psecret", "-mem=AES256"]),
                       ("zp.zip", ["-tzip"])):
        subprocess.run([SZ, "a", *opts, os.path.join(d, name), "a.txt", "sub"], cwd=small,
                       check=True, stdout=subprocess.DEVNULL)
    # A ZIP name in CP437 without the UTF-8 flag (both engines must agree).
    zn = os.path.join(d, "cp437n.zip")
    with zipfile.ZipFile(zn, "w") as z:
        z.writestr(zipfile.ZipInfo("GrXn.txt", (2022, 3, 4, 5, 6, 8)), b"cp437 name\n")
        z.writestr(zipfile.ZipInfo("plain.txt", (2022, 3, 4, 5, 6, 8)), b"plain\n")
    raw = open(zn, "rb").read()
    open(zn, "wb").write(raw.replace(b"GrXn.txt", b"Gr\x81n.txt"))



def sums(root):
    out = []
    for base, _, fs in os.walk(root):
        for fn in fs:
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
        if archive.endswith(".rar"):
            cmd = [UNAR, "-q", "-D", "-o", t, a] + (["-p", password] if password else [])
        else:
            cmd = [SZ, "x", "-y", "-o" + t, a] + (["-p" + password] if password else ["-p"])
        # Links are not extracted on AROS (not by any engine): leave them out.
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        lines = [] if only_plain else [f"{h}  {r}" for h, r in sums(t)]
    with open(os.path.join(exp, f"{n:02d}.sums"), "wb") as f:
        f.write("".join(l + "\n" for l in lines).encode("latin-1"))


O = f"RESULTS:{run}"
LOG = f"{O}/7zi-log.txt"
L = ["FailAt 30", "Stack 40960", "Protect RAM:u/unarc-7z +e", f"MakeDir {O}", "MakeDir RAM:o",
     "CD RAM:u/7zi", f'Copy tree.7z "{latin}"',
     # Without the helper, and with a helper of another release.
     "MakeDir RAM:nh", "Copy RAM:u/unarc-cli RAM:nh/", "Protect RAM:nh/unarc-cli +e",
     "MakeDir RAM:mm", "Copy RAM:u/unarc-cli RAM:mm/", "Copy RAM:u/fake7z RAM:mm/unarc-7z",
     "Protect RAM:mm/unarc-cli +e", "Protect RAM:mm/unarc-7z +e"]
cases = []


def case(n, args, what, rc, engine, ref=None, cli="RAM:u/unarc-cli", pre=None, extra=""):
    L.append(f'Echo "== {n:02d} {what}" >>{LOG}')
    if pre:
        L.append(pre)
    L.append(f"{cli} <NIL: >>{LOG} {args} TO RAM:o/{n:02d} DIAGOUT STATS QUIET {extra}".rstrip())
    L.append(f'Echo "RC=$RC" >>{LOG}')
    L.append(f"If EXISTS RAM:o/{n:02d}")
    L.append(f"RAM:u/uatool sum RAM:o/{n:02d} >{O}/7zi-{n:02d}.sums")
    L.append("Else")
    L.append(f"Echo NOEXIST >{O}/7zi-{n:02d}.sums")
    L.append("EndIf")
    cases.append((n, rc, engine, what))
    if ref:
        reference(n, *ref)


case(1, "tree.7z", "7z, plain: libarchive", 0, "libarchive", ("tree.7z",))
case(2, "tree.7z ENGINE 7-zip", "7z, plain, 7-Zip (cross-check)", 0, "7-zip", ("tree.7z",))
case(3, "enc.7z", "7z, encrypted data, no password: 7-Zip, entries left out", 5, "7-zip",
     ("enc.7z", "secret", True))
case(4, "enc.7z PASSWORD secret", "7z, encrypted data, right password", 0, "7-zip", ("enc.7z", "secret"))
case(5, "enc.7z PASSWORD wrong", "7z, encrypted data, wrong password: errors", 10, "7-zip",
     ("enc.7z", "secret", True))
case(6, "hdr.7z", "7z, encrypted list, no password: nothing written", 20, "-")
case(7, "hdr.7z PASSWORD wrong", "7z, encrypted list, wrong password", 20, "-")
case(8, "hdr.7z PASSWORD secret", "7z, encrypted list, right password", 0, "7-zip", ("hdr.7z", "secret"))
case(9, "rar5_solid_encrypted.rar PASSWORD password", "RAR 5 solid encrypted", 0, "7-zip",
     ("rar5_solid_encrypted.rar", "password"))
case(10, "rar4_solid_encrypted.rar PASSWORD password", "RAR 4 solid encrypted", 0, "7-zip",
     ("rar4_solid_encrypted.rar", "password"))
case(11, "rar5_solid_encrypted_filenames.rar PASSWORD password", "RAR 5 encrypted list", 0, "7-zip",
     ("rar5_solid_encrypted_filenames.rar", "password"))
case(12, "r5.part01.rar", "RAR 5 set of 8 volumes", 0, "7-zip", ("r5.part01.rar",))
case(13, "r4.part2.rar", "RAR 4 set given by its second volume (its link is not extracted)", 5,
     "7-zip", ("r4.part1.rar",))
case(14, "gap.part1.rar", "RAR 4 set without volume 3: nothing written", 20, "-")
case(15, f'"{latin}"', "archive name in ISO-8859-1 with a space", 0, "libarchive", ("tree.7z",))
case(16, "solid.7z", "large solid 7z (3000 files): libarchive", 0, "libarchive", ("solid.7z",))
case(17, "solid.7z ENGINE 7-zip", "large solid 7z, 7-Zip", 0, "7-zip", ("solid.7z",))
case(18, "solid.7z ENGINE 7-zip CANCELAFTER 2000000", "7-Zip cancelled after 2 MB", 10, "7-zip")
case(19, "deflate64.7z", "7z in Deflate64 (not in libarchive): 7-Zip before writing", 0, "7-zip",
     ("deflate64.7z",))
case(20, "split.7z.002", "7z split into 4 files, given by the second: 7-Zip", 0, "7-zip",
     ("split.7z.001",))
case(21, "splitgap.7z.001", "7z split with part 2 missing: nothing written", 20, "-")
case(22, "zc.zip PASSWORD secret", "ZIP, ZipCrypto, right password: 7-Zip", 0, "7-zip",
     ("zc.zip", "secret"))
case(23, "za.zip PASSWORD secret", "ZIP, AES-256, right password: 7-Zip", 0, "7-zip", ("za.zip", "secret"))
case(24, "za.zip", "ZIP, AES-256, no password: entries left out", 5, "7-zip", ("za.zip", "secret", True))
case(25, "za.zip PASSWORD wrong", "ZIP, AES-256, wrong password: errors", 10, "7-zip",
     ("za.zip", "secret", True))
case(26, "zc.zip PASSWORD wrong", "ZIP, ZipCrypto, wrong password: errors", 10, "7-zip",
     ("zc.zip", "secret", True))
case(27, "zp.zip", "ZIP, plain: libarchive", 0, "libarchive", ("zp.zip",))
case(28, "deflate64.zip", "ZIP in Deflate64: 7-Zip before writing", 0, "7-zip", ("deflate64.zip",))
case(29, "cp437n.zip", "ZIP name in CP437, libarchive", 0, "libarchive")
case(30, "cp437n.zip ENGINE 7-zip", "ZIP name in CP437, 7-Zip: the same names as 29", 0, "7-zip")
# Real Ctrl-C (uatool break signals the running unarc-cli after <ticks>).
case(31, "solid.7z ENGINE 7-zip", "Ctrl-C while 7-Zip reads the entry list", 10, "-",
     pre="Run >NIL: RAM:u/uatool break unarc-cli 10")
case(32, "solid.7z ENGINE 7-zip", "Ctrl-C while 7-Zip decodes the solid block", 10, "7-zip",
     pre="Run >NIL: RAM:u/uatool break unarc-cli 100")
# Without the helper; with a helper of another release.
case(33, "hdr.7z PASSWORD secret", "no helper: encrypted list cannot be read here", 20, "-",
     cli="RAM:nh/unarc-cli")
case(34, "enc.7z", "no helper: libarchive's list, encrypted entries left out", 5, "libarchive",
     ("enc.7z", "secret", True), cli="RAM:nh/unarc-cli")
case(35, "za.zip PASSWORD secret", "no helper: AES ZIP, libarchive's list, entries left out", 5,
     "libarchive", ("za.zip", "secret", True), cli="RAM:nh/unarc-cli")
case(36, "r5.part01.rar", "no helper: RAR 5 set through libarchive", 0, "libarchive",
     ("r5.part01.rar",), cli="RAM:nh/unarc-cli")
case(37, "hdr.7z PASSWORD secret", "helper of another release: not used", 20, "-", cli="RAM:mm/unarc-cli")
case(38, "tree.7z", "helper of another release: plain 7z still read", 0, "libarchive", ("tree.7z",),
     cli="RAM:mm/unarc-cli")
L.append(f"List RAM:o/18 ALL FILES LFORMAT \"%l %p%n\" >{O}/7zi-18.list")
L.append(f"List RAM:o ALL LFORMAT \"%p%n %l\" >{O}/7zi-tree.txt")
L.append(f"Status >{O}/7zi-status.txt")
L.append("Delete RAM:o ALL QUIET")
L.append('Echo "7zi.script done"')
with open(os.path.join(stage, "7zi.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(L) + "\n")
with open(os.path.join(exp, "cases.txt"), "w") as f:
    f.write("".join(f"{n:02d}|{rc}|{eng}|{w}\n" for n, rc, eng, w in cases))
print(len(cases), "cases")
