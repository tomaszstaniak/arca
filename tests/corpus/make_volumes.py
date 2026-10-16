#!/usr/bin/env python3
"""Multi-volume test sets, from libarchive's own test archives (independent of
our code) and 7-Zip.

Usage: make_volumes.py <libarchive-source-dir> <corpus-dir>

RAR 4, 4 volumes (libarchive test_read_format_rar_multivolume):
  r4.part1.rar .. r4.part4.rar       part names
  old.rar old.r00 old.r01 old.r02    old names
  alt.rar alt.r01 alt.r02 alt.r03    old names with a gap (no .r00); the
                                     content says the set is complete
  mixed.part1.rar .. part4.rar       names shuffled against the content
  gap.part1/2/4.rar                  volume 3 missing
  short.part1..3.rar                 the last volume missing
  nofirst.part2..4.rar               the first volume missing
RAR 5, 8 volumes (test_read_format_rar5_multiarchive):
  r5.part01.rar .. r5.part08.rar
7z split by 7-Zip (-v20k) from tree content:
  split.7z.001 ..                    complete
  splitgap.7z.001, .003 ..           .002 missing
"""
import binascii, os, shutil, subprocess, sys, tempfile

src, out = sys.argv[1], sys.argv[2]
T = os.path.join(src, "libarchive", "test")


def uudecode(name):
    data = bytearray()
    started = False
    for line in open(os.path.join(T, name), "rb"):
        if line.startswith(b"begin"):
            started = True
            continue
        if line.strip() == b"end":
            break
        if started and line.strip():
            data += binascii.a2b_uu(line)
    return bytes(data)


r4 = [uudecode(f"test_read_format_rar_multivolume.part000{i}.rar.uu") for i in range(1, 5)]
r5 = [uudecode(f"test_read_format_rar5_multiarchive.part0{i}.rar.uu") for i in range(1, 9)]


def put(name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)


for i, d in enumerate(r4):
    put(f"r4.part{i + 1}.rar", d)
for n, d in zip(("old.rar", "old.r00", "old.r01", "old.r02"), r4):
    put(n, d)
for n, d in zip(("alt.rar", "alt.r01", "alt.r02", "alt.r03"), r4):
    put(n, d)
for n, d in zip(("mixed.part3.rar", "mixed.part1.rar", "mixed.part4.rar", "mixed.part2.rar"), r4):
    put(n, d)
for i in (0, 1, 3):
    put(f"gap.part{i + 1}.rar", r4[i])
for i in (0, 1, 2):
    put(f"short.part{i + 1}.rar", r4[i])
for i in (1, 2, 3):
    put(f"nofirst.part{i + 1}.rar", r4[i])
for i, d in enumerate(r5):
    put(f"r5.part{i + 1:02d}.rar", d)

with tempfile.TemporaryDirectory() as tmp:
    src_tree = os.path.join(tmp, "splitsrc")
    os.makedirs(src_tree)
    import random
    rnd = random.Random(11)
    for i in range(3):
        with open(os.path.join(src_tree, f"part{i}.bin"), "wb") as f:
            f.write(bytes(rnd.getrandbits(8) for _ in range(25000)))
    subprocess.run(["7zz", "a", "-mx0", "-v20k", os.path.join(tmp, "split.7z"), "splitsrc"],
                   cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    parts = sorted(p for p in os.listdir(tmp) if p.startswith("split.7z."))
    for p in parts:
        shutil.copy(os.path.join(tmp, p), os.path.join(out, p))
        if not p.endswith(".002"):
            shutil.copy(os.path.join(tmp, p), os.path.join(out, "splitgap" + p[5:]))
print("volume sets written to", out)
