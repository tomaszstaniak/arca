#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare files a core2 guest run copied to RESULTS:<run>/out with the corpus.

Usage: verify_core2.py <collected-run-dir> <corpus-dir>
Names on the FAT results disk come back from macOS as Unicode (NFC or NFD);
both spellings are tried.
"""
import hashlib, os, sys, unicodedata, zipfile

run, corpus = sys.argv[1], sys.argv[2]
out = os.path.join(run, "out")
h = lambda b: hashlib.sha256(b).hexdigest()
ok = bad = 0

def find(path):
    for form in ("NFC", "NFD"):
        p = unicodedata.normalize(form, path)
        if os.path.exists(p):
            return p
    return None

def check(rel, data, label):
    global ok, bad
    p = find(os.path.join(out, rel))
    if p and h(open(p, "rb").read()) == h(data):
        ok += 1
    else:
        bad += 1
        print("MISMATCH" if p else "MISSING", label, rel)

z = zipfile.ZipFile(os.path.join(corpus, "unicode.zip"))
members = z.infolist()   # in archive order, as make_corpus.py writes them
expected = ["Grün.txt", "Grün2.txt", "Grün3.txt", "Café nfd.txt", "Crème.txt", "Crème 2.txt",
            "a_.txt", "a_ 2.txt", "Été.txt", "été 2.txt"]
for disk, m in zip(expected, members):
    check(os.path.join("z1", "unicode", disk), z.read(m), "unicode")

z = zipfile.ZipFile(os.path.join(corpus, "names.zip"))
for d in ("z3", "z4"):
    for disk, member in [("time 12_30.txt", "time 12:30.txt"), ("Readme", "Readme"),
                         ("README 2", "README"), ("dir/sub/deep.txt", "dir/sub/deep.txt")]:
        check(os.path.join(d, "damaged", disk), z.read(member), "recover " + d)
z = zipfile.ZipFile(os.path.join(corpus, "longnames.zip"))
for i in z.infolist():
    # Since 2026-10-16 names over 107 characters are shortened to 107 (the
    # longest every AROS handler takes); runs before that left them out.
    check(os.path.join("p3", "longnames", i.filename[:107]), z.read(i), "longnames")
print("ok", ok, "bad", bad)
