#!/usr/bin/env python3
"""Stage for RAR volumes over 2 GB: the volume finder must read their end
headers, which lie past 2 GB, through ABIv11's 32-bit Seek (bigfile.c moves
there relatively). The volumes are made here like those of
tests/host/test_volumes.py (headers only, the data a gap of zeros) and zipped;
in the guest each set is unpacked to SYS:uatest-bigvol, checked with
uatool vols (Unarc's own volume finder) and deleted before the next.

Usage: make_bigvol_stage.py <bin-dir> <stage-dir> <run-id>
In the guest:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:u
               Execute RAM:u/bigvol.script
Results: RESULTS:<run>/bigvol.txt. Needs about 4.6 GB free on SYS:.
"""
import os, shutil, struct, sys, tempfile, zipfile, zlib

bindir, stage, run = sys.argv[1:4]
if os.path.exists(stage):
    sys.exit(f"{stage} exists; use a new directory")
os.makedirs(stage)
shutil.copy2(os.path.join(bindir, "uatool"), os.path.join(stage, "uatool"))
G = 1 << 30


def vint(v):
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def r5_block(body):
    size = vint(len(body))
    return struct.pack("<I", zlib.crc32(size + body)) + size + body


def rar5(f, volnum, cont, datasize):
    aflags = 0x0001 | (0x0002 if volnum else 0)
    main = vint(1) + vint(0) + vint(aflags) + (vint(volnum) if volnum else b"")
    name = b"big.bin"
    fh = (vint(2) + vint(0x0002) + vint(datasize) + vint(0) + vint(datasize) + vint(0x20) + vint(0) +
          vint(0) + vint(len(name)) + name)
    f.write(b"Rar!\x1a\x07\x01\x00" + r5_block(main) + r5_block(fh))
    zero = bytes(1 << 20)
    left = datasize
    while left:
        n = min(left, len(zero))
        f.write(zero[:n])
        left -= n
    f.write(r5_block(vint(5) + vint(0) + vint(1 if cont else 0)))


def r4_block(htype, flags, rest):
    hdr = struct.pack("<BHH", htype, flags, 7 + len(rest)) + rest
    return struct.pack("<H", zlib.crc32(hdr) & 0xFFFF) + hdr


def rar4(f, first, volnum, cont, datasize):
    f.write(b"Rar!\x1a\x07\x00" + r4_block(0x73, 0x0001 | 0x0010 | (0x0100 if first else 0), b"\0" * 6))
    name = b"big.bin"
    rest = struct.pack("<IIBIIBBHI", datasize, datasize, 2, 0, 0, 29, 0x30, len(name), 0x20) + name
    f.write(r4_block(0x74, 0x8000 | 0x0002, rest))
    zero = bytes(1 << 20)
    left = datasize
    while left:
        n = min(left, len(zero))
        f.write(zero[:n])
        left -= n
    f.write(r4_block(0x7B, (0x0001 if cont else 0) | 0x0008, struct.pack("<H", volnum)))


def member(z, name, writer):
    zi = zipfile.ZipInfo(name, (2026, 10, 17, 12, 0, 0))
    zi.compress_type = zipfile.ZIP_DEFLATED
    with z.open(zi, "w", force_zip64=True) as f:
        writer(f)


big = 2 * G + 4096 * 3 + 7
with zipfile.ZipFile(os.path.join(stage, "bigv5.zip"), "w", allowZip64=True) as z:
    member(z, "big5.part1.rar", lambda f: rar5(f, None, True, big))
    member(z, "big5.part2.rar", lambda f: rar5(f, 1, True, 1000))
    member(z, "big5.part3.rar", lambda f: rar5(f, 2, False, 1000))
# RAR 4: the names are shuffled against the volume numbers in the end
# blocks; the big volume's end block, past 2 GB, decides its place.
with zipfile.ZipFile(os.path.join(stage, "bigv4.zip"), "w", allowZip64=True) as z:
    member(z, "big4.rar", lambda f: rar4(f, True, 0, True, 300))
    member(z, "big4.r01", lambda f: rar4(f, False, 1, True, big))
    member(z, "big4.r00", lambda f: rar4(f, False, 2, False, 300))

O = f"RESULTS:{run}"
D = "SYS:uatest-bigvol"
L = ["FailAt 30", f"MakeDir {O}", f"Info SYS: >{O}/bigvol.txt"]
for zipname, ask, want in (("bigv5.zip", "big5.part2.rar", "set of 3, problem \"\": big5.part1, part2, part3"),
                           ("bigv4.zip", "big4.r00", "set of 3, problem \"\": big4.rar, big4.r01, big4.r00")):
    L += [f"MakeDir {D}",
          f"C:UnZip >>{O}/bigvol-unzip.txt RAM:u/{zipname} -d {D}",
          f"List {D} >>{O}/bigvol.txt",
          f'Echo "== {ask}: want {want}" >>{O}/bigvol.txt',
          f"RAM:u/uatool vols {D}/{ask} >>{O}/bigvol.txt",
          f"Delete {D} ALL QUIET FORCE"]
L.append('Echo "bigvol.script done"')
open(os.path.join(stage, "bigvol.script"), "w").write("\n".join(L) + "\n")
print("bigvol stage:", sorted(os.listdir(stage)))
