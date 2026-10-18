#!/usr/bin/env python3
"""Stage for the LhA packing tests: unarc-cli, unarc-lha, Unarc, uatool, a
source tree and an AROS Shell script that packs it with LhA, reads it back
with Unarc (XAD) and, where the system has it, C:lha, and compares names,
protection bits, comments, dates and contents.

Usage: make_lha_stage.py <bin-dir> <stage-dir> <run-id>
In the guest:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:u
               Execute RAM:u/run.script
Results go to RESULTS:<run-id>/; the archives are copied there too, for the
host checks (lzhdump.py, LHa for UNIX, 7-Zip, Amiga LhA 2.15 under vamos).
"""
import os, random, shutil, sys, zipfile

bindir, stage, run = sys.argv[1:4]
if os.path.exists(stage):
    sys.exit(f"{stage} exists; use a new directory")
os.makedirs(stage)
for n in ("unarc-cli", "unarc-lha", "Unarc", "uatool"):
    shutil.copy2(os.path.join(bindir, n), os.path.join(stage, n))

# The tree, as a ZIP that the system's UnZip unpacks into RAM:lt/src. Names
# with every character the Shell or a pattern would read, ISO-8859-1, a long
# one, and 1200 small files in 12 drawers.
rnd = random.Random(7)
names = {
    "src/ReadMe": b"Read me first.\n",
    "src/Script": b"x\n",
    "src/Tools/Tool": bytes(range(256)) * 20,
    "src/Tools/Big": bytes(rnd.getrandbits(8) for _ in range(300000)) + b"amiga " * 200000,
    'src/odd/quote"name': b"q\n",
    "src/odd/star*name": b"s\n",
    "src/odd/semi;colon": b"c\n",
    "src/odd/space name": b"sp\n",
    "src/odd/pct%name": b"p\n",
    "src/odd/#?pattern": b"h\n",
    "src/odd/" + "long" * 25: b"long name\n",
}
for d in range(12):
    for k in range(100):
        names[f"src/many/d{d:02d}/f{k:03d}.txt"] = f"{d} {k}\n".encode() * (k % 7 + 1)
with zipfile.ZipFile(os.path.join(stage, "tree.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for n, data in names.items():
        zi = zipfile.ZipInfo(n, date_time=(2026, 10, 14, 9, 30, 41))
        zi.external_attr = 0o644 << 16
        z.writestr(zi, data)

O = f"RESULTS:{run}"
U = "RAM:u/unarc-cli DIAGOUT"
L = ["FailAt 30", "Stack 40960", f"MakeDir {O}", f"MakeDir {O}/out",
     "Protect RAM:u/unarc-cli +e", "Protect RAM:u/unarc-lha +e", "Protect RAM:u/Unarc +e",
     "Protect RAM:u/uatool +e",
     f"Version >{O}/env.txt", f"Version RAM:u/unarc-lha >>{O}/env.txt",
     "MakeDir RAM:lt", "C:UnZip >NIL: -q RAM:u/tree.zip -d RAM:lt",
     # Amiga metadata: comments (one ISO-8859-1), every protection bit, a
     # commented drawer, an empty drawer, a date with odd seconds.
     "MakeDir RAM:lt/src/Empty",
     # ISO-8859-1 name: made here, the script is written in that encoding.
     "Echo \"latin\" >\"RAM:lt/src/odd/caf\xe9\"",
     "FileNote RAM:lt/src/ReadMe \"Read me first, caf\xe9\"",
     "FileNote RAM:lt/src/Tools/Tool \"A tool\"",
     "FileNote RAM:lt/src/Tools \"The tools drawer\"",
     "Protect RAM:lt/src/ReadMe sprwd",
     "Protect RAM:lt/src/Script srwed",
     "Protect RAM:lt/src/Tools/Tool prwed",
     "Protect RAM:lt/src/Tools/Big are",
     "Protect RAM:lt/src/odd/caf\xe9 hsparwed",
     "SetDate RAM:lt/src/Script DATE 01-Jan-99 TIME 12:34:57",
     f"List RAM:lt/src ALL LFORMAT \"%A|%C|%D %T|%L|%P%N\" >{O}/src-list.txt",
     f"RAM:u/uatool dates RAM:lt/src >{O}/src-dates.txt",
     f"RAM:u/uatool sum RAM:lt/src >{O}/src-sums.txt"]


def case(name, cmd, after=()):
    log = f"{O}/{name}.txt"
    L.append(f"Echo \"== {cmd}\" >{log}")
    L.append(f"{cmd} <NIL: >>{log}")
    L.append(f"Echo \"RC=$RC\" >>{log}")
    for a in after:
        L.append(f"{a} >>{log}")


case("H1-pack", f"{U} RAM:lt/t.lha PACK RAM:lt/src",
     ["List RAM:lt PAT=#?.lha", "Echo \"leftover temporary files:\"", "List RAM:lt PAT=(.ua#?|lh??????)"])
case("H2-list-xad", f"{U} RAM:lt/t.lha LIST")
L.append("MakeDir RAM:lt/x")
case("H3-extract-xad", f"{U} RAM:lt/t.lha TO RAM:lt/x")
# Not as case() "after" lines: those get a second redirection to the log,
# and with two the Shell wrote neither.
L += [f"List RAM:lt/x/src ALL LFORMAT \"%A|%C|%D %T|%L|%P%N\" >{O}/x-list.txt",
      f"RAM:u/uatool dates RAM:lt/x/src >{O}/x-dates.txt",
      f"RAM:u/uatool sum RAM:lt/x/src >{O}/x-sums.txt"]
case("H4-store", f"{U} RAM:lt/t0.lha PACK RAM:lt/src/Tools LEVEL 0", [f"{U} RAM:lt/t0.lha LIST"])
# Names given on the command line, each with a character the Shell or a
# pattern reads (the CLI passes them on in the list file). In a quoted Shell
# argument "*" escapes: "**" is a star, "*\"" a quote.
case("H5-odd-names", f"{U} RAM:lt/odd.lha PACK \"RAM:lt/src/odd/semi;colon\" "
     f"\"RAM:lt/src/odd/star**name\" \"RAM:lt/src/odd/quote*\"name\" \"RAM:lt/src/odd/#?pattern\"",
     [f"{U} RAM:lt/odd.lha LIST"])
# Ctrl-C part way (CANCELAFTER): no archive, no temporary files.
case("H6-cancel", f"{U} RAM:lt/c.lha PACK RAM:lt/src CANCELAFTER 200000",
     ["List RAM:lt PAT=c.lha", "Echo \"leftover temporary files:\"", "List RAM:lt PAT=(.ua#?|lh??????)"])
# Refusals: a volume, the archive inside a drawer being packed, a link.
case("H7-volume", f"{U} RAM:lt/v.lha PACK RAM:")
case("H8-inside", f"{U} RAM:lt/src/in.lha PACK RAM:lt/src", ["List RAM:lt/src PAT=#?.lha"])
L += ["MakeDir SYS:uatest-lk", "Echo \"t\" >SYS:uatest-lk/target.txt",
      "MakeLink SYS:uatest-lk/link SYS:uatest-lk/target.txt"]
case("H9-link", f"{U} RAM:lt/l.lha PACK SYS:uatest-lk", ["List RAM:lt PAT=l.lha"])
L += ["Delete SYS:uatest-lk/link QUIET FORCE", "Delete SYS:uatest-lk ALL QUIET FORCE"]
# Replace: the old archive kept until the new one is complete.
case("H10-replace", f"{U} RAM:lt/t0.lha PACK RAM:lt/src/odd OVERWRITE",
     [f"{U} RAM:lt/t0.lha LIST", "Echo \"leftover temporary files:\"", "List RAM:lt PAT=(.ua#?|lh??????)"])
# The system's own LhA, where there is one (AROS One): list and extract.
L += ["If EXISTS C:lha",
      f"  C:lha v RAM:lt/t.lha >{O}/S1-clha-list.txt",
      "  MakeDir RAM:lt/y",
      f"  C:lha x RAM:lt/t.lha RAM:lt/y/ >{O}/S2-clha-extract.txt",
      f"  List RAM:lt/y/src ALL LFORMAT \"%A|%C|%D %T|%L|%P%N\" >{O}/y-list.txt",
      f"  RAM:u/uatool sum RAM:lt/y/src >{O}/y-sums.txt",
      "EndIf",
      f"Copy RAM:lt/t.lha RAM:lt/t0.lha RAM:lt/odd.lha {O}/out",
      f"Echo \"DONE\" >{O}/DONE.txt", "Echo \"lha run finished\""]
with open(os.path.join(stage, "run.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(L) + "\n")
print(f"{len(L)} script lines")
