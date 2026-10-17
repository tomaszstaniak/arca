#!/usr/bin/env python3
"""Guest scripts for the checks asked for in the stage 4 review, added to a
GUI stage that already has the 7zi cases (make_7zi_cases.py):

- tz.script <tag>: the same archives through libarchive and 7-Zip, file
  dates as AROS keeps them (uatool dates) and the locale's offset, to
  RESULTS:<run>/tz-<tag>.txt. Run it, change the offset (uatool setgmt),
  run it again: 7z, RAR 5 and ZIP times stored in UTC must move with the
  offset, RAR 4 and DOS-only ZIP times must not, and both engines must agree.
- pkg.script: the package as a user gets it, unpacked into a drawer whose
  name has a space and run from there (no test binaries, no test variables);
  then with unarc-7z removed and with one of another release; at the end
  the drawer must delete (no lock left by the helper).
- encsolid.7z: solid.7z with encrypted data, for cancelling in the GUI before
  the first header (only the last entry selected) and at the password window.

Usage: make_review_cases.py <stage-dir> <run-id> <package.zip> <corpus-dir>
"""
import os, shutil, subprocess, sys, tempfile

stage, run, pkg, corpus = sys.argv[1:5]
SZ = "/opt/homebrew/bin/7zz"
d = os.path.join(stage, "7zi")
O = f"RESULTS:{run}"
shutil.copy2(pkg, os.path.join(stage, "package.zip"))
for f in ("old.rar", "old.r00", "old.r01", "old.r02"):
    shutil.copy2(os.path.join(corpus, f), os.path.join(d, f))
with tempfile.TemporaryDirectory() as t:
    subprocess.run([SZ, "x", "-o" + t, os.path.join(d, "solid.7z")], check=True,
                   stdout=subprocess.DEVNULL)
    subprocess.run([SZ, "a", "-mx5", "-ms=on", "-psecret", os.path.join(d, "encsolid.7z"), "solid"],
                   cwd=t, check=True, stdout=subprocess.DEVNULL)

# ---- time zone
ARCH = (("tree.7z", "7z, UTC"), ("r5.part01.rar", "RAR 5, UTC"), ("old.rar", "RAR 4, local"),
        ("zp.zip", "ZIP by 7-Zip, UTC extra field"), ("cp437n.zip", "ZIP by Python, DOS time only"))
L = [".KEY tag/A", ".BRA {", ".KET }", "FailAt 30", "Stack 40960", f"MakeDir {O}",
     "MakeDir RAM:tz", "MakeDir RAM:tz/{tag}",
     f"RAM:u/uatool gmt >{O}/tz-{{tag}}.txt"]
for a, what in ARCH:
    for eng in ("libarchive", "7-zip"):
        dest = f"RAM:tz/{{tag}}/{a.split('.')[0]}-{eng}"
        L.append(f'Echo "== {a} {eng} ({what})" >>{O}/tz-{{tag}}.txt')
        L.append(f"RAM:u/unarc-cli <NIL: >NIL: RAM:u/7zi/{a} TO {dest} ENGINE {eng} QUIET")
        L.append(f'Echo "RC=$RC" >>{O}/tz-{{tag}}.txt')
        L.append(f"RAM:u/uatool dates {dest} >>{O}/tz-{{tag}}.txt")
L.append('Echo "tz.script {tag} done"')
open(os.path.join(stage, "tz.script"), "w").write("\n".join(L) + "\n")

# ---- the package
P = '"RAM:Unarc Test/Unarc'
LOG = f"{O}/pkg-log.txt"
L = ["FailAt 30", "Stack 40960", f"MakeDir {O}",
     'MakeDir "RAM:Unarc Test"',
     f'C:UnZip >{O}/pkg-unzip.txt RAM:u/package.zip -d "RAM:Unarc Test"',
     f'Echo "unzip RC=$RC" >>{LOG}',
     f'List "RAM:Unarc Test" ALL >{O}/pkg-list.txt',
     f'Version {P}/unarc-cli" FULL >>{LOG}',
     f'Version {P}/Unarc" FULL >>{LOG}',
     "MakeDir RAM:pk"]


def pcase(n, args, what):
    L.append(f'Echo "== {n} {what}" >>{LOG}')
    L.append(f'{P}/unarc-cli" <NIL: >>{LOG} {args} TO RAM:pk/{n} DIAGOUT QUIET')
    L.append(f'Echo "RC=$RC" >>{LOG}')
    L.append(f"If EXISTS RAM:pk/{n}")
    L.append(f"RAM:u/uatool sum RAM:pk/{n} >{O}/pkg-{n}.sums")
    L.append("EndIf")


pcase("p1", "RAM:u/7zi/za.zip PASSWORD secret", "AES ZIP through the packaged unarc-7z")
pcase("p2", "RAM:u/7zi/hdr.7z PASSWORD secret", "7z with an encrypted list")
pcase("p3", "RAM:u/7zi/r5.part03.rar", "RAR 5 set given by its third volume")
pcase("p4", "RAM:u/7zi/zp.zip", "plain ZIP")
pcase("p5", "RAM:u/7zi/tree.7z", "plain 7z")
L.append(f'Rename {P}/unarc-7z" {P}/unarc-7z.off"')
pcase("p6", "RAM:u/7zi/hdr.7z PASSWORD secret", "unarc-7z removed: encrypted list")
pcase("p7", "RAM:u/7zi/tree.7z", "unarc-7z removed: plain 7z")
L.append(f'Copy RAM:u/fake7z {P}/unarc-7z"')
L.append(f'Protect {P}/unarc-7z" +e')
pcase("p8", "RAM:u/7zi/hdr.7z PASSWORD secret", "unarc-7z of another release: encrypted list")
pcase("p9", "RAM:u/7zi/za.zip PASSWORD secret", "unarc-7z of another release: AES ZIP")
L.append(f'Delete {P}/unarc-7z" QUIET')
L.append(f'Rename {P}/unarc-7z.off" {P}/unarc-7z"')
pcase("p10", "RAM:u/7zi/za.zip PASSWORD secret", "unarc-7z back: AES ZIP")
L.append(f"Status >{O}/pkg-status.txt")
L.append('Echo "pkg.script done; delete the drawer with pkgdel.script after the GUI checks"')
open(os.path.join(stage, "pkg.script"), "w").write("\n".join(L) + "\n")
open(os.path.join(stage, "pkgdel.script"), "w").write("\n".join([
    "FailAt 30",
    f'Delete "RAM:Unarc Test" ALL QUIET >{O}/pkg-delete.txt',
    f'Echo "delete RC=$RC" >>{O}/pkg-delete.txt',
    f'If EXISTS "RAM:Unarc Test"',
    f'Echo "the drawer is still there" >>{O}/pkg-delete.txt',
    "EndIf"]) + "\n")
print("tz.script, pkg.script, pkgdel.script, encsolid.7z")
