#!/usr/bin/env python3
"""Check archives made by unarc-cli PACK with tools that are not libarchive.

Usage: verify_pack.py <dir with t.zip, t.tar.gz, t.7z> <corpus-dir>

The guest packed RAM:pksrc (see make_stage.py, option pack):
  a.txt         "hello\\n", write-protected, comment, dated 01-Jan-99 12:00:00
  Café.txt      "café\\n" (the name and text are ISO-8859-1 on AROS)
  sub/many.bin  a copy of corpus/many.zip, pure bit set
  sub/deep/s.script "x\\n", script bit set
  empty/        an empty drawer
  link          a soft link to a.txt (must not be in the archive)
Each archive is tested with 7-Zip (7zz t) and read with Python's zipfile or
tarfile; contents are compared byte for byte, and the metadata each format
kept is printed, so that what the formats keep is measured, not assumed.
"""
import hashlib, io, os, subprocess, sys, tarfile, zipfile

d, corpus = sys.argv[1], sys.argv[2]
many = open(os.path.join(corpus, "many.zip"), "rb").read()
want = {
    "pksrc/a.txt": b"hello\n",
    "pksrc/Café.txt": "café\n".encode("latin-1"),
    "pksrc/sub/many.bin": many,
    "pksrc/sub/deep/s.script": b"x\n",
}
want_dirs = {"pksrc", "pksrc/sub", "pksrc/sub/deep", "pksrc/empty"}
bad = 0


def date_check(fmt, text):
    """a.txt was dated 01-Jan-99 12:00 in the guest; the archive must carry a
    1999-01-01 date (a day either side for time zones), not the packing time."""
    global bad
    ok = any(x in text for x in ("1999, 1, 1", "1998, 12, 31", "1999-01-01", "1998-12-31",
                                 "mtime 9151", "mtime 9150", "mtime 9152"))
    print(f"  {fmt}: a.txt date {'kept' if ok else 'NOT KEPT'}")
    if not ok:
        bad += 1


def report(fmt, files, dirs, meta):
    global bad
    for name, data in want.items():
        got = files.get(name)
        if got is None:
            print(f"  {fmt}: MISSING {name}")
            bad += 1
        elif got != data:
            print(f"  {fmt}: DIFFERENT {name} ({len(got)} bytes, want {len(data)})")
            bad += 1
    extra = set(files) - set(want)
    for e in sorted(extra):
        print(f"  {fmt}: UNEXPECTED {e}")
        bad += 1
    for m in sorted(want_dirs - dirs):
        print(f"  {fmt}: missing directory {m}/")
    for name in sorted(meta):
        print(f"  {fmt}: {name}: {meta[name]}")
    date_check(fmt, meta.get("pksrc/a.txt", ""))
    print(f"  {fmt}: {len(files)} files, {len(dirs)} directories checked")


def sevenzip(path):
    global bad
    r = subprocess.run(["7zz", "t", path], capture_output=True, text=True)
    ok = r.returncode == 0 and "Everything is Ok" in r.stdout
    print(f"7zz t {os.path.basename(path)}: {'Everything is Ok' if ok else 'FAILED'}")
    if not ok:
        print(r.stdout[-800:], r.stderr[-400:])
        bad += 1
    r = subprocess.run(["7zz", "l", "-slt", path], capture_output=True, text=True)
    return r.stdout


z = os.path.join(d, "t.zip")
if os.path.exists(z):
    sevenzip(z)
    files, dirs, meta = {}, set(), {}
    with zipfile.ZipFile(z) as zf:
        if zf.testzip() is not None:
            print("zipfile: CRC error")
            bad += 1
        for i in zf.infolist():
            n = i.filename.rstrip("/")
            utf8 = bool(i.flag_bits & 0x800)
            if i.is_dir():
                dirs.add(n)
            else:
                files[n] = zf.read(i)
            meta[n] = (f"date {i.date_time}, unix mode {oct(i.external_attr >> 16)}, "
                       f"utf8 flag {utf8}, comment {i.comment!r}")
    print("zipfile:")
    report("zip", files, dirs, meta)

t = os.path.join(d, "t.tar.gz")
if os.path.exists(t):
    sevenzip(t)
    files, dirs, meta = {}, set(), {}
    with tarfile.open(t, "r:gz") as tf:
        for m in tf.getmembers():
            n = m.name.rstrip("/")
            if m.isdir():
                dirs.add(n)
            elif m.isfile():
                files[n] = tf.extractfile(m).read()
            meta[n] = (f"mtime {m.mtime}, mode {oct(m.mode)}, type {m.type!r}, "
                       f"pax {dict((k, v) for k, v in m.pax_headers.items())}")
    print("tarfile:")
    report("tar.gz", files, dirs, meta)

s = os.path.join(d, "t.7z")
if os.path.exists(s):
    slt = sevenzip(s)
    # 7-Zip extracts to a temporary directory: content and names, its own way.
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["7zz", "x", "-y", f"-o{tmp}", s], capture_output=True)
        files, dirs = {}, set()
        for root, ds, fs in os.walk(tmp):
            for x in ds:
                dirs.add(os.path.relpath(os.path.join(root, x), tmp))
            for x in fs:
                p = os.path.join(root, x)
                files[os.path.relpath(p, tmp)] = open(p, "rb").read()
    meta = {}
    cur = None
    for line in slt.splitlines():
        if line.startswith("Path = "):
            cur = line[7:]
        elif cur and line.startswith(("Modified = ", "Attributes = ")):
            meta[cur] = (meta.get(cur, "") + " " + line).strip()
    meta.pop(os.path.basename(s), None)
    print("7z (7zz x):")
    report("7z", files, dirs, meta)

print("RESULT:", "ok" if not bad else f"{bad} problems")
sys.exit(1 if bad else 0)
