#!/usr/bin/env python3
"""Assemble a pool staging directory: the CLI, the corpus and an AROS Shell script
that runs every non-interactive case and writes logs to RESULTS:<run>/.

Usage: make_stage.py <unarc-cli> <corpus-dir> <stage-dir> <run-id>
In the guest:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:u
               Execute RAM:u/run.script
"""
import os, shutil, sys

cli, corpus, stage, run = sys.argv[1:5]
opts = set(sys.argv[5:])   # nodiskfull: skip the RESULTS:-full case; aros-one: compare with C:xadList
                           # core2: replacement, ZIP names/recovery, scale and memory suite only
                           # nomem: core2 without the memory loops; regress: core2 plus the
                           # stage-2 extraction and conflict cases; ffsprobe: ffs_probe.c on
                           # its own fresh FFS image (needs probe.adf next to the CLI)
probe = os.path.join(os.path.dirname(cli), "charset-probe")
if os.path.exists(stage):
    sys.exit(f"{stage} exists; use a new directory")
os.makedirs(os.path.join(stage, "corpus"))
shutil.copy2(cli, os.path.join(stage, "unarc-cli"))
shutil.copy2(probe, os.path.join(stage, "charset-probe"))
shutil.copy2(os.path.join(os.path.dirname(cli), "longpath-probe"), os.path.join(stage, "longpath-probe"))
shutil.copy2(os.path.join(os.path.dirname(cli), "uatool"), os.path.join(stage, "uatool"))
if "ffsprobe" in opts:
    shutil.copy2(os.path.join(os.path.dirname(cli), "ffs-probe"), os.path.join(stage, "ffs-probe"))
    shutil.copy2(os.path.join(os.path.dirname(cli), "probe.adf"), os.path.join(stage, "probe.adf"))
    with open(os.path.join(stage, "IDF1"), "w") as f:
        f.write("FileSystem     = afs-handler\nDevice         = diskimage.device\nUnit           = 1\n"
                "BlockSize      = 512\nSurfaces       = 2\nBlocksPerTrack = 11\nLowCyl         = 0\n"
                "HighCyl        = 79\nReserved       = 2\nActivate       = 1\n")
with open(os.path.join(stage, "IDF0"), "w") as f:
    # DOSDriver file with the values of the base system's Storage/DOSDrivers/IDF0
    # (an ADF on diskimage.device unit 0); Mount names the device after the file.
    f.write("FileSystem     = afs-handler\nDevice         = diskimage.device\nUnit           = 0\n"
            "BlockSize      = 512\nSurfaces       = 2\nBlocksPerTrack = 11\nLowCyl         = 0\n"
            "HighCyl        = 79\nReserved       = 2\nActivate       = 1\n")
for n in os.listdir(corpus):
    if not n.startswith("."):
        shutil.copy2(os.path.join(corpus, n), os.path.join(stage, "corpus", n))

O = f"RESULTS:{run}"
L = ["FailAt 30", "Stack 40960"]
if "freeown" in opts:
    # Only this project's own, already collected results from earlier runs.
    for old in ("ua1015a", "ua1015a-c4", "ua1015b", "ua1015b-full", "ua1015d", "ua1015e"):
        L.append(f"Delete RESULTS:{old} ALL QUIET FORCE")
L += [f"MakeDir {O}", f"MakeDir {O}/out",
     "Protect RAM:u/unarc-cli +e", "Protect RAM:u/charset-probe +e", "Protect RAM:u/longpath-probe +e",
     "Protect RAM:u/uatool +e",
     f"Version >{O}/env.txt", f"Version exec.library FULL >>{O}/env.txt",
     f"Version dos.library FULL >>{O}/env.txt",
     f"Echo \"xadmaster in LIBS:\" >>{O}/env.txt",
     f"List LIBS:xadmaster.library LIBS:XAD >>{O}/env.txt",
     f"Echo \"stack\" >>{O}/env.txt", f"Stack >>{O}/env.txt",
     f"Avail >>{O}/env.txt",
     f"List RAM:u/unarc-cli >>{O}/env.txt",
     "CD RAM:u",
     f"charset-probe >{O}/charset-probe.txt",
     f"Info RESULTS: >>{O}/env.txt"]

def case(name, cmd, after=()):
    log = f"{O}/{name}.txt"
    L.append(f"Echo \"== {cmd}\" >{log}")
    L.append(f"{cmd} <NIL: >>{log}")
    L.append(f"Echo \"RC=$RC\" >>{log}")
    for a in after:
        L.append(f"{a} >>{log}")


def ex(name, archive, dest, extra="", listdir=None, mk=True):
    if mk:
        L.append(f"MakeDir {dest} ALL")
    # Last line: any temporary or backup file left behind must show up here.
    case(name, f"unarc-cli corpus/{archive} TO {dest} {extra}".strip(),
         [f"List {listdir or dest} ALL", "Echo \"leftover .ua files:\"",
          f"List {dest} ALL FILES PAT=.ua#? LFORMAT %p%n"])


def core2():
    """Stage 2b: write safety, ZIP names and recovery, scale, memory."""
    U = "unarc-cli"
    # A small FFS disk image for running out of space while replacing a file.
    L.append(f"Copy corpus/replace.adf RAM:r.adf")
    # Mount first, then insert: inserted before the handler runs, the image was
    # not seen ("Please insert volume UATest"), measured twice.
    # AROS One 1.3 has IDF0 and IDF1 mounted already; Mount then stops the
    # script with an error requester.
    L.extend(["Assign >NIL: IDF0: EXISTS", "If WARN", "Mount RAM:u/IDF0", "EndIf"])
    L.append("C:MountDiskImage UNIT 0 INSERT RAM:r.adf")
    L.append("Wait 3")
    case("R0-adf", "Info UATest:", ["List UATest: ALL"])
    case("R1-replace-diskfull", f"{U} corpus/replace-big.zip TO UATest: OVERWRITE",
         ["List UATest: ALL", "Info UATest:"])
    L.append(f"Copy UATest:victim.bin {O}/out/victim-after-diskfull.bin")
    case("R2-replace-fits", f"{U} corpus/replace-small.zip TO UATest: OVERWRITE",
         ["List UATest: ALL", "Type UATest:keep.txt"])
    case("R3-longnames-ffs", f"{U} corpus/longnames.zip TO UATest:ln", ["List UATest:ln ALL"])
    # Cancel while replacing: big2.zip has the same names, other data, 1999 dates.
    L.append("MakeDir RAM:rc")
    case("R4a-big-first", f"{U} corpus/big.zip TO RAM:rc NOWRAP QUIET", ["List RAM:rc/big"])
    case("R4b-replace-cancel-in-first", f"{U} corpus/big2.zip TO RAM:rc NOWRAP OVERWRITE CANCELAFTER 4000000",
         ["List RAM:rc ALL"])
    case("R4c-replace-cancel-in-second", f"{U} corpus/big2.zip TO RAM:rc NOWRAP OVERWRITE CANCELAFTER 12000000",
         ["List RAM:rc ALL"])
    # A file another program holds open cannot be replaced; the rest can.
    # ">>" opens the file for writing in the Run process and keeps it open.
    L.append("Run >>RAM:rc/big/part2.bin Wait 25")
    L.append("Wait 2")
    case("R5-replace-in-use", f"{U} corpus/big.zip TO RAM:rc NOWRAP OVERWRITE", ["List RAM:rc ALL"])
    L.append("Wait 25")
    L.append("Protect RAM:rc/big/part0.bin -wd")
    case("R6-replace-protected", f"{U} corpus/big2.zip TO RAM:rc NOWRAP OVERWRITE QUIET",
         ["List RAM:rc ALL"])
    # ZIP names and recovery
    case("Z1-unicode-list", f"{U} corpus/unicode.zip LIST")
    ex("Z1-unicode-extract", "unicode.zip", "RAM:z1")
    ex("Z2-damaged-normal", "damaged.zip", "RAM:z2")
    ex("Z3-damaged-recover", "damaged.zip", "RAM:z3", "RECOVER")
    ex("Z4-damaged-recover-xad", "damaged.zip", "RAM:z4", "RECOVER ENGINE xad")
    case("Z5-damaged-list", f"{U} corpus/damaged.zip LIST")
    # Paths
    ex("P1-deep", "deep.zip", "RAM:p1")
    ex("P2-toodeep", "toodeep.zip", "RAM:p2", listdir="RAM:p2 FILES")
    ex("P3-longnames-ram", "longnames.zip", "RAM:p3")
    case("P4-longnames-sys", f"{U} corpus/longnames.zip TO SYS:uatest-ln",
         ["List SYS:uatest-ln ALL", "Info SYS:"])
    L.append("Delete SYS:uatest-ln ALL QUIET FORCE")
    # Scale: 10 000 entries, timed in the program (guest clock)
    for n in ("many.zip", "many.tar.gz", "many.lha"):
        t = n.replace(".", "_")
        case(f"S-{t}-list", f"{U} corpus/{n} LIST QUIET STATS")
        case(f"S-{t}-extract", f"{U} corpus/{n} TO RAM:s QUIET STATS", ["Avail"])
        L.append("Delete RAM:s ALL QUIET")
    if "regress" in opts:
        # Stage-2 extraction and conflict cases, short form.
        ex("E01-names", "names.zip", "RAM:e1")
        ex("E03a-onetop", "onetop.zip", "RAM:e3")
        ex("E03b-onetop-again", "onetop.zip", "RAM:e3", mk=False)
        ex("E04a-single", "single.zip", "RAM:e4")
        ex("E04b-single-noswitch", "single.zip", "RAM:e4", mk=False)
        ex("E04c-single-skip", "single.zip", "RAM:e4", "SKIP", mk=False)
        ex("E04d-single-rename", "single.zip", "RAM:e4", "RENAME", mk=False)
        L.append("Protect RAM:e4/single.txt -d")
        ex("E04e-single-overwrite-protected", "single.zip", "RAM:e4", "OVERWRITE", mk=False)
        ex("E05-amiga-lha-auto", "amiga.lha", "RAM:e5")
        ex("E07-amiga-lzx", "amiga.lzx", "RAM:e7")
        ex("E08-tree-tgz", "tree.tar.gz", "RAM:e8")
        ex("E09-tree-7z", "tree.7z", "RAM:e9")
        ex("E12-cp437", "cp437.zip", "RAM:e12")
        case("E15-readonly-dest", "unarc-cli corpus/single.zip TO POOLINPUT:")
    if "nomem" in opts:
        return
    # Memory: ten runs each, Avail FLUSH around them; the probe is the control
    # (same C runtime libraries, no work).
    mem = f"{O}/M-memory.txt"
    def avail(label):
        L.append(f"Echo \"== {label}\" >>{mem}")
        L.append(f"Avail FLUSH >>{mem}")
    avail("start"); avail("start again")
    for _ in range(10):
        L.append("charset-probe >NIL:")
    avail("after 10 x probe (control)")
    for _ in range(10):
        L.append(f"{U} corpus/many.zip LIST QUIET >NIL:")
    avail("after 10 x list many.zip")
    for _ in range(10):
        L.append(f"{U} corpus/many.zip TO RAM:m QUIET >NIL:")
        L.append("Delete RAM:m ALL QUIET")
    avail("after 10 x extract many.zip + delete")
    for _ in range(10):
        L.append(f"{U} corpus/many.lha LIST QUIET >NIL:")
    avail("after 10 x list many.lha (xad)")
    for _ in range(10):
        L.append(f"{U} corpus/amiga.lzx TO RAM:m QUIET >NIL:")
        L.append("Delete RAM:m ALL QUIET")
    avail("after 10 x extract amiga.lzx (xad) + delete")

if "packonly" in opts:
  pass        # only the packing cases below
elif "core2" not in opts:
  for n in sorted(os.listdir(corpus)):
      if n.startswith(".") or n == "SHA256SUMS" or n.endswith(".log"):
          continue
      case(f"L-{n.replace('.', '_')}", f"unarc-cli corpus/{n} LIST")
  case("L-amiga_lha-libarchive", "unarc-cli corpus/amiga.lha LIST ENGINE libarchive")
  
  ex("E01-names", "names.zip", "RAM:e1")
  L.append("MakeDir RAM:e2 RAM:e2/in")
  ex("E02-unsafe", "unsafe.zip", "RAM:e2/in", listdir="RAM:e2", mk=False)
  L.append(f"List RAM:#?.txt >>{O}/E02-unsafe.txt")
  ex("E03a-onetop", "onetop.zip", "RAM:e3")
  ex("E03b-onetop-again", "onetop.zip", "RAM:e3", mk=False)
  ex("E04a-single", "single.zip", "RAM:e4")
  ex("E04b-single-noswitch", "single.zip", "RAM:e4", mk=False)
  ex("E04c-single-skip", "single.zip", "RAM:e4", "SKIP", mk=False)
  ex("E04d-single-rename", "single.zip", "RAM:e4", "RENAME", mk=False)
  L.append("Protect RAM:e4/single.txt -d")
  ex("E04e-single-overwrite-protected", "single.zip", "RAM:e4", "OVERWRITE", mk=False)
  ex("E05-amiga-lha-auto", "amiga.lha", "RAM:e5")
  ex("E05b-amiga-lha-libarchive", "amiga.lha", "RAM:e5b", "ENGINE libarchive")
  ex("E06-amiga-h0-lha", "amiga-h0.lha", "RAM:e6")
  ex("E07-amiga-lzx", "amiga.lzx", "RAM:e7")
  ex("E08-tree-tgz", "tree.tar.gz", "RAM:e8")
  ex("E09-tree-7z", "tree.7z", "RAM:e9")
  ex("E10-encrypted", "encrypted.zip", "RAM:e10")
  ex("E11-damaged", "damaged.zip", "RAM:e11")
  ex("E12-cp437", "cp437.zip", "RAM:e12")
  ex("E13-big-cancel", "big.zip", "RAM:e13", "CANCELAFTER 10000000")
  L.append(f"Date >{O}/E14-time.txt")
  ex("E14-big-full", "big.zip", "RAM:e14")
  L.append(f"Date >>{O}/E14-time.txt")
  case("E15-readonly-dest", "unarc-cli corpus/single.zip TO POOLINPUT:")
  case("E18-dest-missing", "unarc-cli corpus/onetop.zip TO RAM:e18/new/deeper", ["List RAM:e18 ALL"])
  # RESULTS: is a shared volume that other projects have nearly filled; a write
  # that does not fit must stop with the disk's own error and leave nothing behind.
  if "nodiskfull" not in opts:
      case("E19-disk-full", f"unarc-cli corpus/big.zip TO {O}-full", [f"List {O}-full ALL", "Info RESULTS:"])
  if "aros-one" in opts:
      # The system's own xadmaster.library on the same archives, for comparison.
      for n in ("amiga.lha", "amiga.lzx"):
          case(f"X-xadList-{n.replace('.', '_')}", f"C:xadList corpus/{n}")
      L.append(f"Version LIBS:xadmaster.library FULL >>{O}/env.txt")
  ex("E20-zeros", "zeros.tar.xz", "RAM:e20")
  L.append("Delete RAM:e20 ALL QUIET")
  case("E16-missing", "unarc-cli corpus/nothere.zip")
  case("E17-badargs", "unarc-cli")
elif "packonly" not in opts:
  core2()
if "pack" in opts:
    # Packing: one source tree with what Amiga files carry, packed in each
    # format, extracted again by unarc-cli, and the ZIP also tested by the
    # system's own UnZip (Info-ZIP, not libarchive). The archives are copied to
    # RESULTS: for 7-Zip and Python on the host.
    S = "RAM:pksrc"
    L += [f"MakeDir {S} {S}/sub {S}/sub/deep {S}/empty RAM:pk RAM:pkx",
          f"Echo \"hello\" >{S}/a.txt",
          f"Echo \"caf\xe9\" >\"{S}/Caf\xe9.txt\"",
          f"Echo \"x\" >{S}/sub/deep/s.script",
          f"Copy corpus/many.zip {S}/sub/many.bin",
          f"Protect {S}/sub/deep/s.script +s",
          f"Protect {S}/a.txt -w",
          f"Protect {S}/sub/many.bin +p",
          f"FileNote {S}/a.txt \"a comment\"",
          # SetDate's template is FILE/A,WEEKDAY,DATE,TIME: given without
          # keywords the date lands in WEEKDAY and nothing is set.
          f"SetDate {S}/a.txt DATE 01-Jan-99 TIME 12:00:00",
          # RAM: takes no soft links; make the links on SYS: (SFS). AROS
          # MakeLink makes a soft link unless HARD is given (it has no SOFT).
          "MakeDir SYS:uatest-lk",
          "Echo \"t\" >SYS:uatest-lk/target.txt",
          f"MakeLink SYS:uatest-lk/link SYS:uatest-lk/target.txt >{O}/K14-makelink.txt",
          f"MakeLink SYS:uatest-lk/loop SYS:uatest-lk >>{O}/K14-makelink.txt",
          f"List SYS:uatest-lk >>{O}/K14-makelink.txt",
          f"List {S} ALL >{O}/K0-source.txt"]
    for fmt, ext in (("zip", "zip"), ("tgz", "tar.gz"), ("7z", "7z")):
        case(f"K1-pack-{fmt}", f"unarc-cli RAM:pk/t.{ext} PACK {S}", [f"List RAM:pk/t.{ext}"])
        case(f"K2-unpack-{fmt}", f"unarc-cli RAM:pk/t.{ext} TO RAM:pkx/{fmt}",
             [f"List RAM:pkx/{fmt} ALL"])
        L.append(f"Copy RAM:pk/t.{ext} {O}/out/t.{ext}")
    case("K14-links", "unarc-cli RAM:pk/l.zip PACK SYS:uatest-lk",
         ["List SYS:uatest-lk", "unarc-cli RAM:pk/l.zip LIST"])
    # The links first, by name: Delete ALL must not walk into the loop.
    L.append("Delete SYS:uatest-lk/loop SYS:uatest-lk/link QUIET FORCE")
    L.append("Delete SYS:uatest-lk ALL QUIET FORCE")
    case("K3-infozip-test", "C:UnZip -t RAM:pk/t.zip")
    case("K3-infozip-list", "C:UnZip -Z -l RAM:pk/t.zip")
    case("K4-exists", f"unarc-cli RAM:pk/t.zip PACK {S}/a.txt", ["List RAM:pk"])
    case("K5-replace", f"unarc-cli RAM:pk/t.zip PACK {S}/a.txt OVERWRITE",
         ["List RAM:pk", "unarc-cli RAM:pk/t.zip LIST"])
    case("K6-self", f"unarc-cli {S}/self.zip PACK {S}",
         [f"unarc-cli {S}/self.zip LIST", f"List {S} PAT=#?.ua#?"])
    L.append(f"Delete {S}/self.zip QUIET")
    case("K7-cancel", f"unarc-cli RAM:pk/c.zip PACK {S} LEVEL 0 CANCELAFTER 200000",
         ["List RAM:pk ALL", "Echo \"leftover .ua files:\"", "List RAM:pk PAT=.ua#?"])
    # Replacing an archive with one packed from itself must be refused, the
    # original kept (review finding: an empty archive replaced it).
    case("K15-self-replace", "unarc-cli RAM:pk/t.zip PACK RAM:pk/t.zip OVERWRITE",
         ["List RAM:pk/t.zip", "unarc-cli RAM:pk/t.zip LIST"])
    case("K8-noext", f"unarc-cli RAM:pk/t.arc PACK {S}")
    case("K9-format", f"unarc-cli RAM:pk/t.arc PACK {S}/sub FORMAT 7Z", ["List RAM:pk"])
    # A drawer that cannot be read, at its start and part way through.
    L.append("SetEnv UnarcTestFailDir sub")
    case("K16-dir-unreadable", f"unarc-cli RAM:pk/f1.zip PACK {S}", ["unarc-cli RAM:pk/f1.zip LIST"])
    L.append("SetEnv UnarcTestFailDir sub/1")
    case("K17-dir-stops", f"unarc-cli RAM:pk/f2.zip PACK {S}", ["unarc-cli RAM:pk/f2.zip LIST"])
    L.append("UnSetEnv UnarcTestFailDir")
    case("K10-help", "unarc-cli HELP")
    case("K11-formats", "unarc-cli FORMATS")
    case("K12-missing-input", f"unarc-cli RAM:pk/m.zip PACK {S}/nothere {S}/a.txt",
         ["unarc-cli RAM:pk/m.zip LIST"])
    # Only names that do not exist: no archive at all (test3 made an empty
    # ZIP of "RAM", reported as made).
    case("K18-all-missing", f"unarc-cli RAM:pk/n.zip PACK nothere RAM:nothere/either",
         ["List RAM:pk PAT=n.zip", "Echo \"leftover .ua files:\"", "List RAM:pk PAT=.ua#?"])
    # The old archive cannot be deleted after a replace: kept under a name
    # next to the new one, said in the result, RC 5.
    case("K19a-keep-make", f"unarc-cli RAM:pk/k.zip PACK {S}/a.txt")
    L.append("SetEnv UnarcTestKeepOld 1")
    case("K19b-keep-old", f"unarc-cli RAM:pk/k.zip PACK {S}/sub OVERWRITE",
         ["List RAM:pk PAT=k.zip#?", "Echo \"leftover .ua files:\"", "List RAM:pk PAT=.ua#?",
          "unarc-cli RAM:pk/k.zip LIST", "unarc-cli RAM:pk/k.zip.old LIST"])
    L.append("UnSetEnv UnarcTestKeepOld")
    if "core2" in opts:
        # Disk full while packing onto the FFS test disk: no archive, no temporary.
        case("K13-diskfull", f"unarc-cli UATest:big.zip PACK corpus/big.zip LEVEL 0",
             ["List UATest: ALL", "Info UATest:"])
if "core4" in opts:
    # Stage 4 core fixes (needs the core2 FFS image UATest: mounted, so run
    # with core2 or after it in the same boot).
    U = "unarc-cli"
    root = "longroot-" + "r" * 29
    case("C1a-longroot-ffs", f"{U} corpus/{root}.zip TO UATest:c1", ["List UATest:c1 ALL"])
    case("C1b-longroot-ffs-again", f"{U} corpus/{root}.zip TO UATest:c1", ["List UATest:c1 ALL"])
    case("C2-longtop-ffs", f"{U} corpus/longtop.zip TO UATest:c2", ["List UATest:c2 ALL"])
    case("C3a-dirmerge-ffs", f"{U} corpus/dirmerge.zip TO UATest:c3", ["List UATest:c3 ALL"])
    case("C3b-dirmerge-ram", f"{U} corpus/dirmerge.zip TO RAM:c3", ["List RAM:c3 ALL"])
    L.append(f"Copy UATest:c3 {O}/out/c3-ffs ALL QUIET")
    L.append(f"Copy RAM:c3 {O}/out/c3-ram ALL QUIET")
    L.append("Delete UATest:c1 UATest:c2 UATest:c3 ALL QUIET")
    # The archive replaced between listing and extracting: first as it is (the
    # stamp taken at listing refuses every replacement before anything is
    # written; C4a copies the same bytes again, which changes only the date),
    # then with UnarcTestNoStamp, so that the engines' header comparison is
    # tested on its own as before (C4*-nostamp).
    L.append("MakeDir RAM:sw")
    for suffix, env in (("", None), ("-nostamp", "SetEnv UnarcTestNoStamp 1")):
        if env:
            L.append(env)
        for name, other, eng in (("C4a-swap-same", "swap-a", ""), ("C4b-swap-order", "swap-b", ""),
                                 ("C4c-swap-more", "swap-more", ""),
                                 ("C4d-swap-short", "swap-short", ""),
                                 ("C4e-swap-order-xad", "swap-b", "ENGINE xad")):
            n = name + suffix
            L.append("Copy corpus/swap-a.zip RAM:sw/a.zip")
            case(n, f"{U} RAM:sw/a.zip TO RAM:sw/{n} {eng} TESTSWAP corpus/{other}.zip".replace("  ", " "),
                 [f"List RAM:sw/{n} ALL"])
    L.append("UnSetEnv UnarcTestNoStamp")
    # More than 2^31 bytes: written and checked, then replaced and checked.
    case("C6a-big-extract", f"{U} corpus/bigzero.tar.xz TO SYS:uatest-big STATS",
         ["List SYS:uatest-big ALL", "Info SYS:"])
    # NOWRAP: straight into the drawer that has bigzero/ already, so the file
    # is replaced (with the folder rule it went to a new "bigzero 2").
    case("C6b-big-replace", f"{U} corpus/bigzero.tar.xz TO SYS:uatest-big NOWRAP OVERWRITE STATS",
         ["List SYS:uatest-big ALL", "Echo \"leftover .ua files:\"",
          "List SYS:uatest-big ALL FILES PAT=.ua#?", "Echo \"== sums\"",
          "RAM:u/uatool sum SYS:uatest-big"])
    L.append("Delete SYS:uatest-big ALL QUIET FORCE")
if "vol" in opts:
    # Multi-volume sets, opened from different parts; incomplete sets refused.
    U = "unarc-cli"
    for name, part in (("V1-r4-from-part2", "r4.part2.rar"), ("V2-old-names", "old.r01"),
                       ("V3-alt-names-gap", "alt.rar"), ("V4-mixed-names", "mixed.part1.rar"),
                       ("V5-rar5-from-part5", "r5.part05.rar"), ("V6-7z-split", "split.7z.002")):
        # Sums, not copies: one set holds a 241 MB file and RESULTS: is small.
        case(name, f"{U} corpus/{part} TO RAM:v/{name} QUIET",
             [f"List RAM:v/{name} ALL", "Echo \"== sums\"", f"RAM:u/uatool sum RAM:v/{name}"])
        L.append(f"Delete RAM:v/{name} ALL QUIET")
    for name, part in (("V7-gap", "gap.part1.rar"), ("V8-short", "short.part2.rar"),
                       ("V9-nofirst", "nofirst.part3.rar"), ("V10-splitgap", "splitgap.7z.001")):
        case(name, f"{U} corpus/{part} TO RAM:v/{name}", [f"List RAM:v/{name} ALL"])
    case("V11-list-from-part3", f"{U} corpus/r4.part3.rar LIST")
if "ffsprobe" in opts:
    # A second, fresh FFS image on unit 1 with its own volume name.
    L.append("Protect RAM:u/ffs-probe +e")
    L.append("Copy RAM:u/probe.adf RAM:p.adf")
    L += ["Assign >NIL: IDF1: EXISTS", "If WARN", "Mount RAM:u/IDF1", "EndIf"]
    L.append("C:MountDiskImage UNIT 1 INSERT RAM:p.adf")
    L.append("Wait 3")
    case("F0-probe-info", "Info UAProbe:")
    case("F1-ffs-probe", "ffs-probe UAProbe:", ["List UAProbe: ALL", "Info UAProbe:"])
if "lpprobe" in opts:
    # Long absolute paths given straight to Lock and CreateDir (no unarc, no Shell).
    L.append("MakeDir RAM:lp")
    case("D-longpath-ram", "longpath-probe RAM:lp")
    L.append("MakeDir SYS:uatest-lp")
    case("D-longpath-sys", "longpath-probe SYS:uatest-lp")
    L.append("Delete SYS:uatest-lp ALL QUIET FORCE")
L.append(f"Avail >{O}/avail-end.txt")
for d in (("e1", "e2", "e3", "e4", "e5", "e5b", "e6", "e7", "e8", "e9", "e11", "e12", "e18")
          if "core2" not in opts and "packonly" not in opts else
          () if "packonly" in opts else ("z1", "z3", "z4", "p3") +
          (("e1", "e3", "e4", "e5", "e7", "e8", "e9", "e12") if "regress" in opts else ())):
    L.append(f"Copy RAM:{d} {O}/out/{d} ALL QUIET")
L.append(f"Echo \"DONE\" >{O}/DONE.txt")
L.append("Echo \"unarc run finished\"")
# Since 0.0.2 the CLI writes progress and messages to the error stream, which
# the Shell cannot redirect; DIAGOUT copies them into the logs.
L = [("unarc-cli DIAGOUT " + l[len("unarc-cli "):]) if l.startswith("unarc-cli ") else l for l in L]
with open(os.path.join(stage, "run.script"), "w", encoding="latin-1") as f:
    f.write("\n".join(L) + "\n")
print(f"{len(L)} script lines")
