#!/usr/bin/env python3
"""Test archives for the core fixes of stage 4, added to an existing corpus.

Usage: make_stage4.py <corpus-dir>

  longroot-<38 chars>.zip  two top-level files: the folder named after the
                           archive is longer than FFS takes (30)
  longtop.zip              one top directory of 40 characters
  dirmerge.zip             two directories that differ only after 30
                           characters, and one named exactly the 30-character
                           prefix; each holds a file with its own content
  swap-a.zip, swap-b.zip   same names and sizes, other order (b replaces a
                           between listing and extracting)
  swap-more.zip            swap-a plus one more entry
  swap-short.zip           swap-a without its last entry
  bigzero.tar.xz           one 2.25 GB file of zeros (more than 2^31 bytes)
  many50k.zip              50 000 small files in 500 directories (list display)
"""
import io, lzma, os, sys, tarfile, zipfile

out = sys.argv[1]
D = (2026, 10, 16, 12, 0, 0)


def zwrite(name, entries):
    with zipfile.ZipFile(os.path.join(out, name), "w", zipfile.ZIP_DEFLATED) as z:
        for n, data in entries:
            z.writestr(zipfile.ZipInfo(n, D), data)


root = "longroot-" + "r" * 29          # 38 characters
zwrite(root + ".zip", [("one.txt", b"one\n"), ("two.txt", b"two\n")])
zwrite("longtop.zip", [("T" * 40 + "/", b""), ("T" * 40 + "/in.txt", b"in longtop\n")])
p30 = "d" * 30
zwrite("dirmerge.zip", [
    (p30 + "AAAA/a.txt", b"from AAAA\n"),
    (p30 + "BBBB/b.txt", b"from BBBB\n"),
    (p30 + "/c.txt", b"from the 30-character directory\n"),
    (p30 + "AAAA/sub/a2.txt", b"deeper in AAAA\n"),
])
a = [("x.txt", b"X" * 100), ("y.txt", b"Y" * 100), ("z.txt", b"Z" * 100)]
zwrite("swap-a.zip", a)
zwrite("swap-b.zip", [a[1], a[0], a[2]])
zwrite("swap-more.zip", a + [("w.txt", b"W" * 100)])
zwrite("swap-short.zip", a[:2])

with zipfile.ZipFile(os.path.join(out, "many50k.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for i in range(50000):
        z.writestr(zipfile.ZipInfo(f"big/d{i // 100:03d}/entry{i:05d}.txt", D), f"entry {i}\n")

# 2.25 GB of zeros, streamed through xz so that nothing large touches the disk.
size = 2_415_919_104
path = os.path.join(out, "bigzero.tar.xz")
if not os.path.exists(path):
    class Zeros(io.RawIOBase):
        def __init__(self, n):
            self.left = n
        def readable(self):
            return True
        def readinto(self, b):
            n = min(len(b), self.left)
            b[:n] = bytes(n)
            self.left -= n
            return n
    with lzma.open(path, "wb", preset=6) as xz, tarfile.open(fileobj=xz, mode="w|") as t:
        ti = tarfile.TarInfo("bigzero/zero.bin")
        ti.size = size
        ti.mtime = 1792152000
        t.addfile(ti, io.BufferedReader(Zeros(size), 1 << 20))
print("stage-4 corpus written to", out)
