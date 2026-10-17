#!/usr/bin/env python3
"""Regression test of src/core/methods.c on the host: 7z and ZIP archives made
by 7-Zip (7zz) and Python with each method and kind of encryption, checked for
what the libarchive engine has to pass on to 7-Zip.

Usage: test_methods.py [--big]   (--big adds a 7z over 2 GB, ~2.2 GB of disk)
"""
import os, subprocess, sys, tempfile, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SZ = "/opt/homebrew/bin/7zz"
big = "--big" in sys.argv
fails = 0
tmp = tempfile.mkdtemp(prefix="mh-")
exe = os.path.join(tmp, "mh")
subprocess.run(["cc", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g",
                "-Wno-deprecated-declarations", "-I", os.path.join(HERE, "shim"),
                "-I/opt/homebrew/include", "-o", exe, os.path.join(HERE, "methods_host.c"),
                "-L/opt/homebrew/lib", "-llzma", "-lz"], check=True)
src = os.path.join(tmp, "src")
os.makedirs(os.path.join(src, "d"))
open(os.path.join(src, "a.txt"), "w").write("hello\n" * 2000)
open(os.path.join(src, "d", "b.bin"), "wb").write(os.urandom(70000))


def run(kind, path):
    p = subprocess.run([exe, kind, path], capture_output=True, text=True)
    if p.returncode or p.stderr:
        return None, p.stderr[-400:]
    import re
    f = dict(re.findall(r"(\w+)=('[^']*'|\S+)", p.stdout.split(": ", 1)[1]))
    return f, p.stdout


def check(what, got, **want):
    global fails
    f, raw = got
    ok = f is not None and all(f.get(k) == str(v) if not isinstance(v, str) else f.get(k) == v
                               for k, v in want.items())
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} {what}" + ("" if ok else f": {raw.strip()}"))


def mk7z(name, *opts, files=("a.txt", "d")):
    p = os.path.join(tmp, name)
    subprocess.run([SZ, "a", "-t7z", *opts, p, *files], cwd=src, check=True, stdout=subprocess.DEVNULL)
    return p


def mkzip(name, *opts):
    p = os.path.join(tmp, name)
    subprocess.run([SZ, "a", "-tzip", *opts, p, "a.txt", "d"], cwd=src, check=True,
                   stdout=subprocess.DEVNULL)
    return p


S = "''"
for name, opts, want in (
        ("lzma2.7z", [], dict(ok=1, unsupported=S, data_enc=0)),
        ("lzma.7z", ["-m0=LZMA"], dict(ok=1, unsupported=S)),
        ("ppmd.7z", ["-m0=PPMd"], dict(ok=1, unsupported=S)),
        ("bzip2.7z", ["-m0=BZip2"], dict(ok=1, unsupported=S)),
        ("deflate.7z", ["-m0=Deflate"], dict(ok=1, unsupported=S)),
        ("copy.7z", ["-mx0"], dict(ok=1, unsupported=S)),
        ("bcj2.7z", ["-mf=BCJ2"], dict(ok=1, unsupported=S)),
        ("arm64.7z", ["-mf=ARM64"], dict(ok=1, unsupported=S)),
        ("delta.7z", ["-mf=Delta:4"], dict(ok=1, unsupported=S)),
        ("rawhdr.7z", ["-mhc=off"], dict(ok=1, unsupported=S)),
        ("deflate64.7z", ["-m0=Deflate64"], dict(ok=1, unsupported="'the Deflate64 method'")),
        ("swap4.7z", ["-mf=SWAP4"], dict(ok=1, unsupported="'the Swap4 method'")),
        ("enc.7z", ["-psecret"], dict(ok=1, data_enc=1, list_enc=0)),
        ("hdrenc.7z", ["-psecret", "-mhe=on"], dict(ok=1, list_enc=1)),
        ("solid.7z", ["-ms=on", "-mf=off"], dict(ok=1, unsupported=S))):
    check(f"7z {name}", run("7z", mk7z(name, *opts)), **want)
empty = os.path.join(src, "empty")
os.makedirs(empty)
check("7z with only a folder (no streams)", run("7z", mk7z("dir.7z", files=("empty",))), ok=1, unsupported=S)

for name, opts, want in (
        ("deflate.zip", [], dict(ok=1, unsupported=S, data_enc=0)),
        ("store.zip", ["-mx0"], dict(ok=1, unsupported=S)),
        ("bzip2.zip", ["-mm=BZip2"], dict(ok=1, unsupported=S)),
        ("lzma.zip", ["-mm=LZMA"], dict(ok=1, unsupported=S)),
        ("ppmd.zip", ["-mm=PPMd"], dict(ok=1, unsupported=S)),
        ("deflate64.zip", ["-mm=Deflate64"], dict(ok=1, unsupported="'the Deflate64 method'")),
        ("zipcrypto.zip", ["-psecret", "-mem=ZipCrypto"], dict(ok=1, data_enc=1, unsupported=S)),
        ("aes.zip", ["-psecret", "-mem=AES256"], dict(ok=1, data_enc=1, unsupported=S)),
        ("aes64.zip", ["-psecret", "-mem=AES256", "-mm=Deflate64"],
         dict(ok=1, data_enc=1, unsupported="'the Deflate64 method'"))):
    check(f"zip {name}", run("zip", mkzip(name, *opts)), **want)

# Times: 7-Zip writes NTFS (UTC) times, so no entry has only a DOS time;
# Python writes DOS times only.
check("zip by 7-Zip: no entry with only a DOS time", run("zip", os.path.join(tmp, "deflate.zip")), dos=0)
pz = os.path.join(tmp, "py.zip")
with zipfile.ZipFile(pz, "w") as z:
    z.writestr("x.txt", b"x")
    z.writestr("y.txt", b"y")
check("zip by Python: every entry has only a DOS time, x.txt found", run("zip", pz), dos=2, dos_x=1)

# Zip64: more entries than the 16-bit count holds.
z64 = os.path.join(tmp, "many.zip")
with zipfile.ZipFile(z64, "w") as z:
    for i in range(66000):
        z.writestr(f"f{i}", b"")
check("zip with 66000 entries (Zip64 end record)", run("zip", z64), ok=1, unsupported=S)
# A prefix before the archive, as in a self-extractor.
sfx = os.path.join(tmp, "sfx.zip")
open(sfx, "wb").write(b"\0" * 1000 + open(os.path.join(tmp, "deflate.zip"), "rb").read())
check("zip with a 1000-byte prefix", run("zip", sfx), ok=1, unsupported=S)
check("zip, file too short", run("zip", os.path.join(src, "a.txt")), ok=0)
check("7z, not a 7z", run("7z", os.path.join(src, "a.txt")), ok=0)
cut = os.path.join(tmp, "cut.7z")
open(cut, "wb").write(open(os.path.join(tmp, "lzma2.7z"), "rb").read()[:-10])
check("7z with its end cut off", run("7z", cut), ok=0)

# The stamp taken at listing (bigfile.c): a change at either end, in the size
# or in the date is seen; one in the middle that keeps all of those is not
# (documented; the engines' own checks remain for it).
def stamp(path):
    return subprocess.run([exe, "stamp", path], capture_output=True, text=True).stdout


sp = os.path.join(tmp, "stamp.bin")
blob = bytearray(os.urandom(300000))
open(sp, "wb").write(blob)
os.utime(sp, (1_000_000_000, 1_000_000_000))
s0 = stamp(sp)
for what, off in (("first byte", 0), ("last byte", len(blob) - 1), ("middle byte", 150000)):
    b2 = bytearray(blob)
    b2[off] ^= 1
    open(sp, "wb").write(b2)
    os.utime(sp, (1_000_000_000, 1_000_000_000))
    changed = stamp(sp) != s0
    ok = changed if what != "middle byte" else not changed
    fails += not ok
    print(f"{'ok  ' if ok else 'FAIL'} stamp: {what} changed, same size and date -> "
          f"{'seen' if changed else 'not seen'}{' (expected: not seen)' if what == 'middle byte' else ''}")
open(sp, "wb").write(blob)
os.utime(sp, (1_000_000_060, 1_000_000_060))
ok = stamp(sp) != s0 and s0.startswith("ok=1")
fails += not ok
print(f"{'ok  ' if ok else 'FAIL'} stamp: same bytes, date a minute later -> seen")

if big:
    # Header past 2 GB: the reader must get there by relative moves (the shim's
    # Seek, like ABIv11's, takes a 32-bit position).
    bd = os.path.join(tmp, "bigsrc")
    os.makedirs(bd)
    with open(os.path.join(bd, "zero.bin"), "wb") as f:
        f.truncate((2 << 30) + 12345)
    p = os.path.join(tmp, "big.7z")
    subprocess.run([SZ, "a", "-mx0", p, "zero.bin"], cwd=bd, check=True, stdout=subprocess.DEVNULL)
    os.remove(os.path.join(bd, "zero.bin"))
    check(f"7z of {os.path.getsize(p)} bytes, header past 2 GB", run("7z", p), ok=1, unsupported=S)

subprocess.run(["rm", "-rf", tmp])
print(f"{fails} failure(s)")
sys.exit(1 if fails else 0)
