#!/usr/bin/env python3
"""Build the test archives into build/corpus/.

Amiga archives are made by the original Amiga programs (LhA 2.15, LZX 1.21r1
from Aminet) running under amitools' vamos, so headers, comments and protection
bits are what real Amiga users have. vamos reports empty file comments, so the
venv's vamos gets tools/vamos-comment-hack.diff: comments and protection come
from $AMIMETA_DIR/<name>.json. The Amiga programs are downloaded and checked by
SHA-256; they are test tools only and never shipped.

Usage: make_corpus.py <vamos-binary> <outdir>
Needs on the host: lha or lhasa (to unpack the Aminet files), 7zz, bsdtar, zip.
"""
import hashlib, io, json, os, random, shutil, subprocess, sys, tarfile, urllib.request, zipfile

AMINET = {
    "lha.run": ("https://aminet.net/util/arc/lha.run",
                "76bae515264fcc3e1c69058ff03a4bcb096152a732cf19fdb03cceee18932497"),
    "lzx121r1.lha": ("https://aminet.net/util/arc/lzx121r1.lha",
                     "64d6bb56abea7fdec87dd20654a58fd1f04823bdc4339c5a9fa8edec80a67756"),
}
FAKE = "+".join(f"{l}=mode:fake,version:40" for l in
                ("graphics.library", "gadtools.library", "intuition.library", "asl.library",
                 "locale.library", "icon.library", "diskfont.library"))

def sh(*a, cwd=None, env=None):
    r = subprocess.run(a, cwd=cwd, env=env, capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit(f"failed: {a}\n{r.stdout}\n{r.stderr}")
    return r.stdout

def fetch(tools):
    os.makedirs(tools, exist_ok=True)
    for name, (url, sha) in AMINET.items():
        p = os.path.join(tools, name)
        if not os.path.exists(p):
            urllib.request.urlretrieve(url, p)
        got = hashlib.sha256(open(p, "rb").read()).hexdigest()
        if got != sha:
            sys.exit(f"{name}: sha256 {got}, expected {sha}")
    sh("lha", "xqf", "lha.run", cwd=tools)
    sh("lha", "xqf", "lzx121r1.lha", cwd=tools)

def amiga_tree(root, meta):
    """Files with Amiga comments and protection bits (FIBF layout: RWED active low)."""
    os.makedirs(os.path.join(root, "Amiga", "Tools"))
    os.makedirs(os.path.join(root, "Amiga", "Data"))
    os.makedirs(meta)
    def put(rel, data, comment=None, prot=None):
        with open(os.path.join(root, rel), "wb") as f:
            f.write(data)
        m = {}
        if comment is not None: m["comment"] = comment
        if prot is not None: m["prot"] = prot
        if m:
            json.dump(m, open(os.path.join(meta, os.path.basename(rel) + ".json"), "w"))
    put("Amiga/ReadMe", b"Read this first.\n" * 20, "Read me first", 0x40)      # script
    put("Amiga/Tools/Tool", bytes(range(256)) * 64, "Werkzeug für Ärger", 0x20)  # pure
    rnd = random.Random(1)
    words = [b"amiga", b"aros", b"zune", b"archive", b"lha", b"lzx", b"\n"]
    put("Amiga/Data/table.bin", b" ".join(rnd.choice(words) for _ in range(60000)),
        "read only", 0x05)                                                          # no w, no d
    # 1995 dates, so the archives hold dates every tool can store. LhA 2.15
    # under vamos writes them correctly; LZX 1.21 under vamos still writes an
    # invalid date (it lists its own entries as "06-ARJ-95"), so LZX date
    # restoration needs an archive made on a real Amiga or in UAE.
    t = 803219400  # 1995-06-15 12:30:00 UTC
    for d, _, files in os.walk(os.path.join(root, "Amiga")):
        for n in files:
            os.utime(os.path.join(d, n), (t, t))

def vamos(vbin, tool, args, cwd, meta):
    env = dict(os.environ, AMIMETA_DIR=meta)
    # 68020 (32-bit bus) with 32 MiB of guest RAM; the default is too small for 10 000 entries; the default is too small for 10 000 entries.
    return sh(vbin, "-q", "-C", "68020", "-H", "disable", "-m", "32768", "-O", FAKE, "--", tool, *args, cwd=cwd, env=env)

def zip_modern(out):
    with zipfile.ZipFile(os.path.join(out, "names.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Café.txt", "Latin-1 representable\n")
        z.writestr("Zażółć.txt", "Polish: not all in Latin-1\n")
        z.writestr("日本語.txt", "Japanese\n")
        z.writestr("time 12:30.txt", "colon in name\n")
        z.writestr("Readme", "first\n")
        z.writestr("README", "second, same name without case\n")
        z.writestr("dir/sub/deep.txt", "nested\n")
    with zipfile.ZipFile(os.path.join(out, "unsafe.zip"), "w") as z:
        for n in ("ok.txt", "../escape.txt", "a/../../escape2.txt", "/abs.txt", "Work:vol.txt"):
            zi = zipfile.ZipInfo("x")
            zi.filename = n          # bypass ZipInfo's normalisation on purpose
            z.writestr(zi, f"payload of {n}\n")
    with zipfile.ZipFile(os.path.join(out, "onetop.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("Project/", "")
        z.writestr("Project/a.txt", "a\n")
        z.writestr("Project/docs/b.txt", "b\n")
    with zipfile.ZipFile(os.path.join(out, "single.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("single.txt", "just one file\n")
    # CP437 name without the UTF-8 flag, as old DOS/Windows zippers wrote them:
    # write an ASCII placeholder of the same length, then put the CP437 byte in.
    p = os.path.join(out, "cp437.zip")
    with zipfile.ZipFile(p, "w") as z:
        z.writestr("GrXn.txt", "cp437 name\n")
    raw = open(p, "rb").read()
    assert raw.count(b"GrXn.txt") == 2
    open(p, "wb").write(raw.replace(b"GrXn.txt", "Gr\u00fcn.txt".encode("cp437")))

def big(out):
    # Three 8 MiB members so a cancel lands in the middle of one.
    rnd = random.Random(2)
    with zipfile.ZipFile(os.path.join(out, "big.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for i in range(3):
            block = bytes(rnd.getrandbits(8) for _ in range(65536)) + b"\0" * 65536
            z.writestr(f"big/part{i}.bin", block * 64)


def zip_unicode(out):
    """Name metadata precedence and collisions that appear only after conversion."""
    import struct, zlib
    def unicode_extra(main_bytes, utf8_name, good=True):
        crc = zlib.crc32(main_bytes) & 0xffffffff
        if not good:
            crc ^= 0xdeadbeef
        data = bytes([1]) + struct.pack("<I", crc) + utf8_name.encode("utf-8")
        return struct.pack("<HH", 0x7075, len(data)) + data
    p = os.path.join(out, "unicode.zip")
    placeholders = []
    with zipfile.ZipFile(p, "w") as z:
        # ASCII main name, UTF-8 name in the Info-ZIP Unicode Path field (wins).
        zi = zipfile.ZipInfo("GRUEN.TXT"); zi.extra = unicode_extra(b"GRUEN.TXT", "Gr\u00fcn.txt")
        z.writestr(zi, "unicode path field\n")
        # CP437 main name with a matching Unicode field (both say the same).
        zi = zipfile.ZipInfo("Gr#n2.txt"); zi.extra = unicode_extra("Gr\u00fcn2.txt".encode("cp437"), "Gr\u00fcn2.txt")
        z.writestr(zi, "cp437 + unicode field\n"); placeholders.append((b"Gr#n2.txt", "Gr\u00fcn2.txt".encode("cp437")))
        # Unicode field whose CRC does not match: ignored, CP437 main name used.
        zi = zipfile.ZipInfo("Gr#n3.txt"); zi.extra = unicode_extra("Gr\u00fcn3.txt".encode("cp437"), "WRONG.txt", good=False)
        z.writestr(zi, "stale unicode field\n"); placeholders.append((b"Gr#n3.txt", "Gr\u00fcn3.txt".encode("cp437")))
        # macOS-style decomposed name (NFD) with the UTF-8 flag.
        z.writestr("Cafe\u0301 nfd.txt", "decomposed\n")
        # NFC and NFD spelling of one name: the same file once composed.
        z.writestr("Cr\u00e8me.txt", "composed\n")
        z.writestr("Cre\u0300me.txt", "decomposed twin\n")
        # Different names that become the same after conversion.
        z.writestr("a\u0142.txt", "l with stroke\n")
        z.writestr("a\u0144.txt", "n with acute\n")
        z.writestr("\u00c9t\u00e9.txt", "upper E acute\n")
        z.writestr("\u00e9t\u00e9.txt", "lower e acute\n")
    raw = open(p, "rb").read()
    for ph, real in placeholders:
        assert raw.count(ph) == 2 and len(ph) == len(real)
        raw = raw.replace(ph, real)
    open(p, "wb").write(raw)

def scale(out, work, vbin, meta, tools):
    """10 000 entries in 100 folders, as zip, tar.gz and Amiga LhA."""
    root = os.path.join(work, "many-src")
    for d in range(100):
        os.makedirs(os.path.join(root, "many", f"d{d:02d}"))
        for f in range(100):
            with open(os.path.join(root, "many", f"d{d:02d}", f"file{f:04d}.txt"), "w") as fh:
                fh.write(f"entry {d} {f}\n" * 3)
    with zipfile.ZipFile(os.path.join(out, "many.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for d in range(100):
            for f in range(100):
                n = f"many/d{d:02d}/file{f:04d}.txt"
                z.write(os.path.join(root, n), n)
    sh("bsdtar", "-czf", os.path.join(out, "many.tar.gz"), "many", cwd=root,
       env=dict(os.environ, COPYFILE_DISABLE="1"))
    shutil.copy(os.path.join(tools, "lha_68k"), os.path.join(root, "lha"))
    # LhA under vamos runs out of guest memory on 10 000 files in one call, so
    # add one folder per call (LhA appends to an existing archive). -h keeps
    # the "many/dNN/" part of each path, which LhA otherwise strips.
    for d in range(100):
        vamos(vbin, "lha", ["a", "-r", "-x", "-h", "-q", "many.lha", f"many/d{d:02d}/"], root, meta)
    shutil.move(os.path.join(root, "many.lha"), os.path.join(out, "many.lha"))

def paths(out):
    """Deep and long paths."""
    with zipfile.ZipFile(os.path.join(out, "deep.zip"), "w") as z:
        # 80 levels of 19 characters: about 1 600 characters, more than the 1 024
        # the first version allowed, less than the 4 096 limit now.
        p = "/".join(f"level-{i:02d}-abcdefghij"[:19] for i in range(80))
        z.writestr(p + "/bottom.txt", "deep\n")
    with zipfile.ZipFile(os.path.join(out, "toodeep.zip"), "w") as z:
        p = "/".join(f"level-{i:03d}-abcdefghi"[:19] for i in range(230))  # ~4 400
        z.writestr(p + "/bottom.txt", "too deep\n")
        z.writestr("shallow.txt", "still extracted\n")
    with zipfile.ZipFile(os.path.join(out, "longnames.zip"), "w") as z:
        for n in (30, 31, 60, 107, 108, 200, 255):
            z.writestr(("L%03d-" % n + "x" * n)[:n] + "", f"name of {n} characters\n")

def replacement(out, work):
    """big2.zip: same names as big.zip, other data and date; a small FFS disk."""
    rnd = random.Random(3)
    with zipfile.ZipFile(os.path.join(out, "big2.zip"), "w", zipfile.ZIP_DEFLATED) as z:
        for i in range(3):
            zi = zipfile.ZipInfo(f"big/part{i}.bin", date_time=(1999, 12, 31, 23, 59, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            block = bytes(rnd.getrandbits(8) for _ in range(65536)) + b"\1" * 65536
            z.writestr(zi, block * 64)
    # replace-big.zip carries a 200 KiB incompressible victim.bin, which does not
    # fit in the free space of replace.adf; replace-small.zip carries keep.txt.
    with zipfile.ZipFile(os.path.join(out, "replace-big.zip"), "w") as z:
        z.writestr("victim.bin", bytes(rnd.getrandbits(8) for _ in range(200 * 1024)))
    with zipfile.ZipFile(os.path.join(out, "replace-small.zip"), "w") as z:
        z.writestr("keep.txt", "new keep.txt from replace-small.zip\n")
    xdf = os.path.join(os.path.dirname(sys.argv[1]), "xdftool")
    d = os.path.join(work, "adf"); os.makedirs(d)
    open(os.path.join(d, "victim.bin"), "wb").write(b"OLD VICTIM " * 9300)       # ~100 KiB
    open(os.path.join(d, "keep.txt"), "wb").write(b"old keep.txt\n")
    open(os.path.join(d, "filler.bin"), "wb").write(bytes(rnd.getrandbits(8) for _ in range(600 * 1024)))
    adf = os.path.join(out, "replace.adf")
    sh(xdf, adf, "create", "+", "format", "UATest", "ffs",
       "+", "write", os.path.join(d, "victim.bin"),
       "+", "write", os.path.join(d, "keep.txt"),
       "+", "write", os.path.join(d, "filler.bin"))
    print(sh(xdf, adf, "info"))

def main():
    vbin, out = sys.argv[1], os.path.abspath(sys.argv[2])
    if os.path.exists(out):
        shutil.rmtree(out)
    work = os.path.join(out, ".work")
    tools = os.path.join(work, "tools")
    fetch(tools)
    src, meta = os.path.join(work, "src"), os.path.join(work, "meta")
    amiga_tree(src, meta)

    shutil.copy(os.path.join(tools, "lha_68k"), os.path.join(src, "lha"))
    shutil.copy(os.path.join(tools, "LZX_68000EC-r"), os.path.join(src, "lzx"))
    # LZX looks for its keyfile in L:; vamos maps L: to a directory we choose.
    os.makedirs(os.path.join(work, "L"))
    shutil.copy(os.path.join(tools, "LZX.Keyfile"), os.path.join(work, "L", "LZX.Keyfile"))
    log = []
    log.append(vamos(vbin, "lha", ["a", "-r", "-x", "amiga.lha", "Amiga/"], src, meta))
    log.append(vamos(vbin, "lha", ["vv", "amiga.lha"], src, meta))
    log.append(vamos(vbin, "lha", ["a", "-r", "-x", "-H0", "amiga-h0.lha", "Amiga/"], src, meta))
    # vamos assigns take Amiga paths; its root: volume is the host's /.
    env_l = ["-a", "L:root:" + os.path.join(work, "L").lstrip("/")]
    env = dict(os.environ, AMIMETA_DIR=meta)
    log.append(sh(vbin, "-q", "-O", FAKE, *env_l, "--", "lzx", "-r", "-x", "a", "amiga.lzx", "Amiga/",
                  cwd=src, env=env))
    log.append(sh(vbin, "-q", "-O", FAKE, *env_l, "--", "lzx", "v", "amiga.lzx", cwd=src, env=env))
    for n in ("amiga.lha", "amiga-h0.lha", "amiga.lzx"):
        shutil.move(os.path.join(src, n), os.path.join(out, n))
    open(os.path.join(out, "amiga-tools.log"), "w").write("\n".join(log))

    # Same tree, made by host tools (no Amiga metadata).
    plain = os.path.join(work, "plain")
    shutil.copytree(os.path.join(src, "Amiga"), os.path.join(plain, "Amiga"))
    # COPYFILE_DISABLE keeps macOS from adding AppleDouble "._*" members.
    tar_env = dict(os.environ, COPYFILE_DISABLE="1")
    sh("bsdtar", "-czf", os.path.join(out, "tree.tar.gz"), "Amiga", cwd=plain, env=tar_env)
    sh("7zz", "a", "-bd", "-mx=5", os.path.join(out, "tree.7z"), "Amiga", cwd=plain)
    sh("zip", "-qr", "-P", "secret", os.path.join(out, "encrypted.zip"), "Amiga", cwd=plain)
    zip_modern(out)
    zip_unicode(out)
    big(out)
    scale(out, work, vbin, meta, tools)
    paths(out)
    replacement(out, work)
    # Slow to unpack under TCG, small to stage: 3 x 96 MiB of zeros in tar.xz,
    # so a Ctrl-C typed after the command lands mid-file.
    zdir = os.path.join(work, "zeros", "zeros")
    os.makedirs(zdir)
    for i in range(3):
        with open(os.path.join(zdir, f"z{i}.bin"), "wb") as f:
            f.truncate(96 << 20)
    sh("bsdtar", "-cJf", os.path.join(out, "zeros.tar.xz"), "zeros",
       cwd=os.path.join(work, "zeros"), env=tar_env)
    raw = open(os.path.join(out, "names.zip"), "rb").read()
    open(os.path.join(out, "damaged.zip"), "wb").write(raw[: len(raw) // 2])
    shutil.rmtree(work)
    with open(os.path.join(out, "SHA256SUMS"), "w") as f:
        for n in sorted(os.listdir(out)):
            if n != "SHA256SUMS" and not n.endswith(".log"):
                f.write(f"{hashlib.sha256(open(os.path.join(out, n), 'rb').read()).hexdigest()}  {n}\n")
    print(open(os.path.join(out, "SHA256SUMS")).read())

main()
