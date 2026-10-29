#!/usr/bin/env python3
"""The same package as an AROS .pkg, for systems without UnZip.

    packaging/zip-to-pkg.py dist/Arca-<ver>-x86_64-v1.zip

Writes dist/Arca-<ver>-x86_64-v1.pkg next to the ZIP and adds it to
dist/SHA256SUMS. Mainline AROS ships no UnZip (and no lha); its C:Unpack
reads .pkg, the format of the AROS installer: "PKG", version 1, a LONG size
(of the whole uncompressed stream),
then per file a LONG path length, the path with its NUL, a LONG data length
and the data (big endian), the whole stream bzip2-compressed
(workbench/c/Unpack/package.c, tools/package/FORMAT). Every file is copied
byte for byte from the ZIP. Unpack makes drawers as it needs them and keeps
no protection bits or dates; a delivered file is never replaced.
"""
import bz2, hashlib, os, struct, sys, zipfile

src = sys.argv[1]
if not src.endswith(".zip"):
    sys.exit("give the package .zip")
out = src[:-4] + ".pkg"
if os.path.exists(out):
    sys.exit(f"{out} exists; delivered files are not replaced")
body = bytearray(b"PKG" + bytes([1]) + struct.pack(">I", 0))
n = 0
with zipfile.ZipFile(src) as z:
    for info in z.infolist():
        if info.is_dir():
            continue
        path = info.filename.encode("iso-8859-1")
        data = z.read(info)
        body += struct.pack(">I", len(path)) + path + b"\0" + struct.pack(">I", len(data)) + data
        n += 1
# The header's size is the whole uncompressed stream, header included
# (tools/package/FORMAT). Unpack checks it since mainline 5ac123c062
# (2026-09-19) and refuses 0 with "invalid package size".
body[4:8] = struct.pack(">I", len(body))
with open(out, "wb") as f:
    f.write(bz2.compress(bytes(body), 9))
digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
with open(os.path.join(os.path.dirname(out), "SHA256SUMS"), "a") as f:
    f.write(f"{digest}  {os.path.basename(out)}\n")
print(f"{out}: {n} files, {os.path.getsize(out)} bytes\n{digest}")
