#!/usr/bin/env python3
"""Stage for the GUI tests: Unarc, unarc-cli, the guest helper and a corpus.

Usage: make_gui_stage.py <bin-dir> <corpus-dir> <stage-dir> <run-id> [cli-stage-dir]
In the guest:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:u
               Execute RAM:u/setup.script
setup.script copies test archives to RAM:t, gives some of them project icons
whose default tool is RAM:u/Unarc (no system-wide association is installed),
and makes the "Unarc Browse" project icon (MODE=BROWSE). The GUI cases are then
driven from the host (keys, clicks, screenshots); every event Unarc logs goes to
RESULTS:<run>/unarc-debug.log. With a cli-stage-dir (made by make_stage.py) its
run.script is included as cli.script for the CLI regression.
"""
import os, shutil, sys

bindir, corpus, stage, run = sys.argv[1:5]
cli_stage = sys.argv[5] if len(sys.argv) > 5 and not sys.argv[5].startswith("--") else None
# --huge: a 200 MB stored ZIP (one file) for cancelling and closing during a
# long write; made here, not in the corpus, so other runs stay small.
huge = "--huge" in sys.argv
# --limpet <zip>: the Limpet terminal archive, for running the CLI in it.
limpet = sys.argv[sys.argv.index("--limpet") + 1] if "--limpet" in sys.argv else None
if os.path.exists(stage):
    sys.exit(f"{stage} exists; use a new directory")
os.makedirs(os.path.join(stage, "corpus"))
for b in ("Unarc", "unarc-cli", "uatool", "zune-control"):
    shutil.copy2(os.path.join(bindir, b), os.path.join(stage, b))
ARCHIVES = ("single.zip", "onetop.zip", "names.zip", "unicode.zip", "damaged.zip", "big.zip",
            "big2.zip", "many.zip", "many.tar.gz", "many.lha", "amiga.lha", "amiga.lzx",
            "tree.tar.gz", "tree.7z", "longnames.zip", "encrypted.zip", "cp437.zip",
            "many50k.zip", "r4.part1.rar", "r4.part2.rar", "r4.part3.rar", "r4.part4.rar",
            "gap.part1.rar", "gap.part2.rar", "gap.part4.rar")
for n in ARCHIVES:
    shutil.copy2(os.path.join(corpus, n), os.path.join(stage, "corpus", n))
if cli_stage:
    for n in os.listdir(os.path.join(cli_stage, "corpus")):
        dst = os.path.join(stage, "corpus", n)
        if not os.path.exists(dst):
            shutil.copy2(os.path.join(cli_stage, "corpus", n), dst)
    for n in ("run.script", "charset-probe", "longpath-probe", "ffs-probe", "probe.adf", "IDF0",
              "IDF1"):
        src = os.path.join(cli_stage, n)
        if os.path.exists(src):
            shutil.copy2(src, os.path.join(stage, "cli.script" if n == "run.script" else n))
open(os.path.join(stage, "Unarc Browse"), "w").write("Opens archives in the browser.\n")
if limpet:
    shutil.copy2(limpet, os.path.join(stage, "Limpet.zip"))
if huge:
    import random, zipfile
    rnd = random.Random(7)
    blk = bytes(rnd.getrandbits(8) for _ in range(1 << 20))
    with zipfile.ZipFile(os.path.join(stage, "corpus", "huge.zip"), "w", zipfile.ZIP_STORED) as z:
        with z.open(zipfile.ZipInfo("huge/one.bin", (2020, 1, 1, 0, 0, 0)), "w") as f:
            for _ in range(200):
                f.write(blk)
        z.writestr(zipfile.ZipInfo("huge/after.txt", (2020, 1, 1, 0, 0, 0)), "after\n")
# Dragging rows out to a Wanderer drawer: two files of one name in different
# folders (they meet once the folders are stripped), a folder with a subfolder,
# and top.txt, which the drawer RAM:dd already holds (setup makes it) so that
# a drop is a conflict.
import zipfile
with zipfile.ZipFile(os.path.join(stage, "corpus", "dragtest.zip"), "w") as z:
    for name, data in (("top.txt", "new top\n"), ("a/", None), ("a/x.txt", "from a\n"),
                       ("b/", None), ("b/x.txt", "from b\n"), ("docs/", None),
                       ("docs/one.txt", "one\n"), ("docs/sub/", None),
                       ("docs/sub/two.txt", "two\n")):
        zi = zipfile.ZipInfo(name, (2021, 6, 1, 12, 0, 0))
        if data is None:
            zi.external_attr = 0o40755 << 16 | 0x10
            z.writestr(zi, "")
        else:
            z.writestr(zi, data)

O = f"RESULTS:{run}"
L = ["FailAt 30",
     f"MakeDir {O}",
     "Protect RAM:u/Unarc +e", "Protect RAM:u/unarc-cli +e", "Protect RAM:u/uatool +e",
     "Protect RAM:u/zune-control +e",
     f"SetEnv UnarcDebug {O}/unarc-debug.log",
     "MakeDir RAM:t",
     "Copy RAM:u/corpus/#? RAM:t QUIET",
     # Test associations: project icons that name the program, nothing global.
     "RAM:u/uatool icon RAM:t/single.zip RAM:u/Unarc",
     "RAM:u/uatool icon RAM:t/onetop.zip RAM:u/Unarc",
     "RAM:u/uatool icon RAM:t/many.zip RAM:u/Unarc",
     "RAM:u/uatool icon RAM:t/damaged.zip RAM:u/Unarc",
     "RAM:u/uatool icon RAM:t/big.zip RAM:u/Unarc",
     "RAM:u/uatool icon RAM:t/amiga.lha RAM:u/Unarc",
     "If EXISTS RAM:t/huge.zip", "RAM:u/uatool icon RAM:t/huge.zip RAM:u/Unarc", "EndIf",
     "RAM:u/uatool icon RAM:t/names.zip RAM:u/Unarc MODE=BROWSE",
     "RAM:u/uatool icon RAM:t/dragtest.zip RAM:u/Unarc MODE=BROWSE",
     "MakeDir RAM:dd",
     "Echo \"old top\" >RAM:dd/top.txt",
     "Copy \"RAM:u/Unarc Browse\" RAM:t/",
     "RAM:u/uatool icon \"RAM:t/Unarc Browse\" RAM:u/Unarc MODE=BROWSE",
     "If EXISTS RAM:u/Limpet.zip",
     "C:UnZip >NIL: RAM:u/Limpet.zip -d RAM:L",
     "MakeDir \"RAM:sp ace\"",
     "Copy RAM:t/single.zip \"RAM:sp ace/My Docs.zip\"",
     "EndIf",
     f"List RAM:t >{O}/setup-list.txt",
     f"Version RAM:u/Unarc >{O}/env.txt",
     f"Avail >>{O}/env.txt",
     "Echo \"setup done\""]
with open(os.path.join(stage, "setup.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(L) + "\n")

# Memory, signals and locks over repeated starts. TESTQUIT ends the program
# when its job has ended, so no clicks are needed; the debug log records the
# task's allocated signals at start and exit. The probe is the control (same
# C runtime, no work). Run with: Execute RAM:u/mem.script
M = f"{O}/G-memory.txt"
# No debug log while measuring: it grows a file on the FAT results disk.
G = ["FailAt 30", "UnSetEnv UnarcDebug"]
def avail(label):
    G.append(f"Echo \"== {label}\" >>{M}")
    G.append(f"Avail FLUSH >>{M}")
def loop(label, cmds, steps=(1, 2, 7, 12)):
    done = 0
    for upto in steps:
        while done < upto:
            G.extend(cmds)
            done += 1
        avail(f"after {done} x {label}")
avail("start"); avail("start again")
for _ in range(10):
    G.append("RAM:u/charset-probe >NIL:")
avail("after 10 x probe (control, C runtime only)")
loop("zune-control (control: Zune window, worker process)", ["RAM:u/zune-control"])
loop("zune-control 10000 rows (control)", ["RAM:u/zune-control 10000"])
loop("Unarc extract onetop.zip + delete",
     ["RAM:u/Unarc RAM:t/onetop.zip EXTRACT TESTQUIT", "Delete RAM:t/Project ALL QUIET"])
loop("Unarc browse many.zip (10 000 entries)", ["RAM:u/Unarc RAM:t/many.zip BROWSE TESTQUIT"])
G.append(f"SetEnv UnarcDebug {O}/unarc-debug.log")
# Locks: the archive's drawer and the output can be deleted right after.
G += ["MakeDir RAM:lk", "Copy RAM:t/onetop.zip RAM:lk/",
      "RAM:u/Unarc RAM:lk/onetop.zip EXTRACT TESTQUIT",
      f"List RAM:lk ALL >{O}/G-locks.txt",
      f"Delete RAM:lk ALL >>{O}/G-locks.txt",
      f"Echo \"delete RC=$RC\" >>{O}/G-locks.txt",
      "Echo \"mem.script done\""]
with open(os.path.join(stage, "mem.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(G) + "\n")
print("staged", stage)
