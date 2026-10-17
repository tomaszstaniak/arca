#!/usr/bin/env python3
"""Regression test of the volume finder (src/core/volumes.c) on the host.

Usage: test_volumes.py [<corpus dir>]   (default build/corpus)

Builds tests/host/volumes_host.c with AddressSanitizer and UBSan and checks:
- a named volume that does not exist (name.7z.001 with nothing beside it);
- a set whose first part is missing, given by the missing name;
- a directory read that fails part way, and every allocation failing in turn:
  no crash, and either the right answer or a refusal with a reason;
- RAR 4 and RAR 5 volumes larger than 2 GB and 4 GB (sparse files) whose end
  headers lie past those offsets: ABIv11's Seek takes a 32-bit position, so
  the finder must reach them by relative moves;
- a header with a wrong CRC is not believed;
- the corpus sets (make_volumes.py) give the answers recorded here.
"""
import os, struct, subprocess, sys, tempfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
corpus = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "corpus")
fails = 0


def check(what, ok, detail=""):
    global fails
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {what}{': ' + detail if detail and not ok else ''}")


tmp = tempfile.mkdtemp(prefix="vh-")
exe = os.path.join(tmp, "vh")
subprocess.run(["cc", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                "-Wno-deprecated-declarations", "-I", os.path.join(HERE, "shim"), "-o", exe,
                os.path.join(HERE, "volumes_host.c"), "-lz"], check=True)


def run(*args, cwd=None):
    p = subprocess.run([exe, *args], cwd=cwd, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


# ---- RAR writers (just the headers the finder reads; data is a sparse gap)

def vint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def r5_block(body, bad_crc=False):
    size = vint(len(body))
    crc = zlib.crc32(size + body) ^ (1 if bad_crc else 0)
    return struct.pack("<I", crc) + size + body


def rar5_volume(path, volnum, cont, datasize, bad_end_crc=False):
    """volnum None: the first volume."""
    aflags = 0x0001 | (0x0002 if volnum else 0)
    main = vint(1) + vint(0) + vint(aflags) + (vint(volnum) if volnum else b"")
    name = b"big.bin"
    fhead = (vint(2) + vint(0x0002) + vint(datasize) + vint(0) + vint(datasize) + vint(0x20) +
             vint(0) + vint(0) + vint(len(name)) + name)
    end = vint(5) + vint(0) + vint(1 if cont else 0)
    with open(path, "wb") as f:
        f.write(b"Rar!\x1a\x07\x01\x00" + r5_block(main) + r5_block(fhead))
        f.seek(datasize, 1)
        f.write(r5_block(end, bad_end_crc))


def r4_block(htype, flags, rest):
    size = 7 + len(rest)
    hdr = struct.pack("<BHH", htype, flags, size) + rest
    return struct.pack("<H", zlib.crc32(hdr) & 0xFFFF) + hdr


def rar4_volume(path, first, volnum, cont, datasize):
    main = r4_block(0x73, 0x0001 | 0x0010 | (0x0100 if first else 0), b"\0" * 6)
    name = b"big.bin"
    large = datasize >= 1 << 32
    rest = struct.pack("<IIBIIBBHI", datasize & 0xFFFFFFFF, datasize & 0xFFFFFFFF, 2, 0, 0, 29,
                       0x30, len(name), 0x20)
    if large:
        rest += struct.pack("<II", datasize >> 32, datasize >> 32)
    fhead = r4_block(0x74, 0x8000 | 0x0002 | (0x0100 if large else 0), rest + name)
    end = r4_block(0x7B, (0x0001 if cont else 0) | 0x0008, struct.pack("<H", volnum))
    with open(path, "wb") as f:
        f.write(b"Rar!\x1a\x07\x00" + main + fhead)
        f.seek(datasize, 1)
        f.write(end)


G = 1 << 30

# ---- 1. names that lead nowhere
d = os.path.join(tmp, "empty")
os.makedirs(d)
rc, out, err = run(os.path.join(d, "x.7z.001"))
check("missing x.7z.001, nothing beside it", rc == 0 and "single archive" in out, err[-300:])
open(os.path.join(d, "y.7z.002"), "wb").write(b"x")
rc, out, err = run(os.path.join(d, "y.7z.001"))
check("missing y.7z.001 with y.7z.002 present", rc == 0 and "part 1 (y.7z.001) is missing" in out,
      out + err[-300:])

# ---- 2. directory read failing, allocations failing
d = os.path.join(corpus)
rc, out, err = run("-exnextfail", "2", "r5.part03.rar", cwd=d)
check("directory read fails after 2 names", rc == 0 and "could not be read completely" in out,
      out + err[-300:])
rc, out, err = run("r5.part03.rar", cwd=d)
n_alloc = int(out.strip().splitlines()[-1].split()[-1])
good = out.splitlines()[0]
bad_runs = []
for k in range(1, n_alloc + 1):
    rc, o, e = run("-oom", str(k), "r5.part03.rar", cwd=d)
    first = o.splitlines()[0] if o else ""
    if rc != 0 or not (first == good or "out of memory" in first or "could not be read" in first):
        bad_runs.append((k, rc, first, e[-200:]))
check(f"each of the {n_alloc} allocations failing in turn", not bad_runs, repr(bad_runs[:3]))

# ---- 3. volumes past 2 GB and 4 GB
d = os.path.join(tmp, "big5")
os.makedirs(d)
rar5_volume(os.path.join(d, "big5.part1.rar"), None, True, 2 * G + 12345)
rar5_volume(os.path.join(d, "big5.part2.rar"), 1, True, 4 * G + 777)
rar5_volume(os.path.join(d, "big5.part3.rar"), 2, False, 1000)
rc, out, err = run("big5.part2.rar", cwd=d)
check("RAR 5 set, end headers past 2 GB and 4 GB", rc == 0 and 'set of 3, first given 0, problem ""' in out,
      out + err[-300:])
rc, out, err = run("-sfs", "big5.part2.rar", cwd=d)
check("RAR 5 set over 2 GB where no seek works (SFS): read forward", rc == 0 and
      'set of 3, first given 0, problem ""' in out, out + err[-300:])
os.remove(os.path.join(d, "big5.part3.rar"))
rc, out, err = run("big5.part1.rar", cwd=d)
check("RAR 5 set, last volume missing after a 4 GB volume",
      'volume 3 (big5.part3.rar) is missing: the set continues after "big5.part2.rar"' in out,
      out + err[-300:])
rar5_volume(os.path.join(d, "big5.part2.rar"), 1, True, 4 * G + 777, bad_end_crc=True)
rar5_volume(os.path.join(d, "big5.part3.rar"), 2, False, 1000)
rc, out, err = run("big5.part1.rar", cwd=d)
check("RAR 5 end header with a wrong CRC is not believed",
      '"big5.part2.rar" ends the set, but "big5.part3.rar" follows' in out, out + err[-300:])

# RAR 4: the order comes from the volume numbers in the end blocks, and the
# names are shuffled against them, so a finder that cannot reach the end
# blocks gets the order wrong.
d = os.path.join(tmp, "big4")
os.makedirs(d)
rar4_volume(os.path.join(d, "big4.rar"), True, 0, True, 2 * G + 99)
rar4_volume(os.path.join(d, "big4.r01"), False, 1, True, 4 * G + 5)    # LHD_LARGE
rar4_volume(os.path.join(d, "big4.r00"), False, 2, False, 300)
rc, out, err = run("big4.r00", cwd=d)
want = ['set of 3, first given 0, problem ""', "1 big4.rar", "2 big4.r01", "3 big4.r00"]
check("RAR 4 set ordered by end blocks past 2 GB and 4 GB", rc == 0 and all(w in out for w in want),
      out + err[-300:])
rc, out, err = run("-sfs", "big4.r00", cwd=d)
check("RAR 4 set over 2 GB where no seek works (SFS): read forward", rc == 0 and
      all(w in out for w in want), out + err[-300:])

# ---- encrypted headers (rar -hp): after RAR 5's encryption header, or a RAR 4
# main header with MHD_PASSWORD, nothing more can be read; the set is checked
# by name, not refused.
d = os.path.join(tmp, "hp")
os.makedirs(d)
for i in range(1, 4):
    with open(os.path.join(d, f"hp5.part{i}.rar"), "wb") as f:
        enc = vint(4) + vint(0) + vint(0) + vint(0) + vint(0) + b"\0" * 16 + vint(0)
        f.write(b"Rar!\x1a\x07\x01\x00" + r5_block(enc) + os.urandom(200))
    with open(os.path.join(d, f"hp4.part{i}.rar"), "wb") as f:
        f.write(b"Rar!\x1a\x07\x00" + r4_block(0x73, 0x0001 | 0x0080 | 0x0010 | (0x0100 if i == 1 else 0),
                                                 b"\0" * 6) + os.urandom(200))
for name in ("hp5.part2.rar", "hp4.part3.rar"):
    rc, out, err = run(name, cwd=d)
    check(f"{name[:3]} set with encrypted headers: complete by name", rc == 0 and
          'set of 3, first given 0, problem ""' in out, out + err[-300:])
os.remove(os.path.join(d, "hp5.part2.rar"))
rc, out, err = run("hp5.part1.rar", cwd=d)
check("hp5 set with encrypted headers, part 2 missing: named", "part 2 (hp5.part2.rar) is missing" in out
      or "volume 2" in out, out + err[-300:])

# ---- 4. the corpus, as recorded before the change to 64-bit positions
EXPECT = """\
r4.part2.rar: set of 4, first given 0, problem ""|r4.part1.rar r4.part2.rar r4.part3.rar r4.part4.rar
old.r01: set of 4, first given 0, problem ""|old.rar old.r00 old.r01 old.r02
alt.rar: set of 4, first given 1, problem ""|alt.rar alt.r01 alt.r02 alt.r03
mixed.part1.rar: set of 4, first given 0, problem ""|mixed.part3.rar mixed.part1.rar mixed.part4.rar mixed.part2.rar
mixed.part3.rar: set of 4, first given 1, problem ""|mixed.part3.rar mixed.part1.rar mixed.part4.rar mixed.part2.rar
gap.part1.rar: set of 3, first given 1, problem "volume 3 (gap.part3.rar) is missing; the next one here is volume 4"|gap.part1.rar gap.part2.rar gap.part4.rar
short.part2.rar: set of 3, first given 0, problem "volume 4 (short.part4.rar) is missing: the set continues after "short.part3.rar""|short.part1.rar short.part2.rar short.part3.rar
nofirst.part3.rar: set of 3, first given 0, problem "the first volume (nofirst.part1.rar) is missing"|nofirst.part2.rar nofirst.part3.rar nofirst.part4.rar
r5.part05.rar: set of 8, first given 0, problem ""|r5.part01.rar r5.part02.rar r5.part03.rar r5.part04.rar r5.part05.rar r5.part06.rar r5.part07.rar r5.part08.rar
split.7z.002: set of 4, first given 0, problem ""|split.7z.001 split.7z.002 split.7z.003 split.7z.004
splitgap.7z.003: set of 3, first given 0, problem "part 2 (splitgap.7z.002) is missing"|splitgap.7z.001 splitgap.7z.003 splitgap.7z.004
"""
for line in EXPECT.splitlines():
    head, files = line.split("|")
    name = head.split(":")[0]
    rc, out, err = run(name, cwd=corpus)
    got = out.splitlines()
    ok = rc == 0 and got[0] == head and [g.split()[1] for g in got[1:-1]] == files.split()
    check(f"corpus {name}", ok, out + err[-300:])

subprocess.run(["rm", "-rf", tmp])
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
