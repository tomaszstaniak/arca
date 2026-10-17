#!/usr/bin/env python3
"""Stage for screenshots and a short recording of a released test package.

The package is installed as a user would (UnZip into RAM:Unarc), and RAM:Demo
gets ordinary-looking content: a drawer to pack, archives to open and
extract, an encrypted one (password window), an LhA archive with Amiga
comments and protection bits, and a large one (the progress window has time to
show). Nothing here is a test variable; the program runs as shipped.

Usage: make_showcase_stage.py <package.zip> <uatool> <corpus-dir> <stage-dir>
In the guest:  C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:u
               Execute RAM:u/show.script
"""
import os, random, shutil, subprocess, sys, tempfile, zipfile, zlib, struct

pkg, uatool, corpus, stage = sys.argv[1:5]
SZ = "/opt/homebrew/bin/7zz"
if os.path.exists(stage):
    sys.exit(f"{stage} exists; use a new directory")
os.makedirs(stage)
shutil.copy2(pkg, os.path.join(stage, "package.zip"))
shutil.copy2(uatool, os.path.join(stage, "uatool"))
rnd = random.Random(2026)


def png(w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


words = ("archive garden summer letter river window market station music picture "
         "holiday mountain coffee evening journey family library project notes").split()


def text(n):
    return "\n".join(" ".join(rnd.choice(words) for _ in range(12)) for _ in range(n)) + "\n"


with tempfile.TemporaryDirectory() as t:
    demo = os.path.join(t, "Demo")
    web = os.path.join(demo, "Website")
    for d in ("css", "images", "scripts", "pages"):
        os.makedirs(os.path.join(web, d))
    open(os.path.join(web, "index.html"), "w").write("<html><body>" + text(40) + "</body></html>\n")
    open(os.path.join(web, "ReadMe.txt"), "w").write(text(10))
    open(os.path.join(web, "css", "style.css"), "w").write("body { margin: 0 }\n" * 60)
    open(os.path.join(web, "scripts", "menu.js"), "w").write("// menu\n" + text(30))
    for i, name in enumerate(("about", "contact", "gallery", "news", "links", "archive")):
        open(os.path.join(web, "pages", f"{name}.html"), "w").write(text(20 + 7 * i))
    for i, name in enumerate(("logo", "header", "photo-river", "photo-garden", "photo-market",
                              "icon-home", "icon-mail")):
        open(os.path.join(web, "images", f"{name}.png"), "wb").write(
            png(64 + 16 * i, 48 + 8 * i, (40 * i % 256, 120, 200)))
    os.makedirs(os.path.join(web, "drafts"))  # an empty drawer is part of the content

    # Archives in RAM:Demo. Fixed dates, so the browser shows sensible ones.
    def zipdir(path, root, out):
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for base, dirs, files in sorted(os.walk(path)):
                rel = os.path.relpath(base, root)
                zi = zipfile.ZipInfo(rel + "/", (2026, 9, 14, 10, 30, 0))
                zi.external_attr = 0o40755 << 16 | 0x10
                z.writestr(zi, "")
                for f in sorted(files):
                    p = os.path.join(base, f)
                    zi = zipfile.ZipInfo(os.path.join(rel, f), (2026, 9, 14, 10, 30, 0))
                    zi.compress_type = zipfile.ZIP_DEFLATED
                    z.writestr(zi, open(p, "rb").read())

    zipdir(web, demo, os.path.join(stage, "Website.zip"))

    # Holiday photos: incompressible, so extracting takes a while on the guest.
    photos = os.path.join(t, "Holiday 2026")
    os.makedirs(photos)
    for i in range(1, 25):
        open(os.path.join(photos, f"IMG_{4100 + i:04d}.jpg"), "wb").write(
            bytes(rnd.getrandbits(8) for _ in range(900_000 + 20_000 * i)))
    zipdir(photos, t, os.path.join(stage, "Holiday 2026.zip"))

    # Letters, encrypted data (the list is readable): the password is asked
    # before anything is written. Password: secret
    letters = os.path.join(t, "Letters")
    os.makedirs(letters)
    for n in ("To Anna.txt", "To the bank.txt", "Invitation.txt", "Thank you.txt"):
        open(os.path.join(letters, n), "w").write(text(15))
    subprocess.run([SZ, "a", "-mx5", "-psecret", os.path.join(stage, "Letters.7z"), "Letters"],
                   cwd=t, check=True, stdout=subprocess.DEVNULL)

    # Recipes as tar.gz, for a queue of mixed formats.
    rec = os.path.join(t, "Recipes")
    os.makedirs(rec)
    for n in ("bread", "soup", "apple cake", "pierogi", "salad"):
        open(os.path.join(rec, f"{n}.txt"), "w").write(text(12))
    subprocess.run(["tar", "--format=ustar", "-czf", os.path.join(stage, "Recipes.tar.gz"), "Recipes"],
                   cwd=t, check=True)

    shutil.copytree(web, os.path.join(stage, "Website"))

# LhA with Amiga comments and protection bits (made by Amiga LhA, corpus).
shutil.copy2(os.path.join(corpus, "amiga.lha"), os.path.join(stage, "Amiga Tools.lha"))

S = ["FailAt 30", "Stack 40960",
     "Protect RAM:u/uatool +e",
     'MakeDir "RAM:Unarc"',
     'C:UnZip >RAM:pkg-unzip.txt RAM:u/package.zip -d RAM:',
     "MakeDir RAM:Demo",
     "Copy RAM:u/Website RAM:Demo/Website ALL QUIET",
     'Copy "RAM:u/Holiday 2026.zip" RAM:Demo/',
     "Copy RAM:u/Website.zip RAM:Demo/",
     "Copy RAM:u/Letters.7z RAM:Demo/",
     "Copy RAM:u/Recipes.tar.gz RAM:Demo/",
     'Copy "RAM:u/Amiga Tools.lha" RAM:Demo/',
     # Project icons naming the program, as a user would set them up.
     'RAM:u/uatool icon "RAM:Demo/Holiday 2026.zip" RAM:Unarc/Unarc',
     "RAM:u/uatool icon RAM:Demo/Website.zip RAM:Unarc/Unarc",
     "RAM:u/uatool icon RAM:Demo/Letters.7z RAM:Unarc/Unarc",
     "RAM:u/uatool icon RAM:Demo/Recipes.tar.gz RAM:Unarc/Unarc",
     'RAM:u/uatool icon "RAM:Demo/Amiga Tools.lha" RAM:Unarc/Unarc',
     "Echo \"show setup done\""]
open(os.path.join(stage, "show.script"), "w", encoding="latin-1").write("\n".join(S) + "\n")

# The system's own LhA writer (AROS One 1.3 ships LHa for UNIX 1.14i with an
# Amiga patch as C:lha): a tree with protection bits, comments, an empty
# drawer and a 2 MB file, packed with header levels 1 and 2, read back by
# Unarc's engine (XAD) and listed with List, which shows bits and comments.
O = "RESULTS:lha1017"
L = ["FailAt 30", "Stack 40960", f"MakeDir {O}",
     "If NOT EXISTS C:lha", f'Echo "no C:lha on this system" >{O}/lha.txt', "Quit 5", "EndIf",
     f"Version C:lha FULL >{O}/lha.txt",
     "MakeDir RAM:lt", "MakeDir RAM:lt/Amiga", "MakeDir RAM:lt/Amiga/Empty", "MakeDir RAM:lt/Amiga/Tools",
     'Echo "Read me first" >RAM:lt/Amiga/ReadMe',
     "Copy RAM:Unarc/unarc-7z RAM:lt/Amiga/Tools/Big",
     'Echo "tool" >RAM:lt/Amiga/Tools/Tool',
     'Echo "" >RAM:lt/Amiga/Script',
     "Protect RAM:lt/Amiga/ReadMe hsprwd",
     "Protect RAM:lt/Amiga/Tools/Tool prwed",
     "Protect RAM:lt/Amiga/Script srwed",
     "Protect RAM:lt/Amiga/Tools/Big are",
     'FileNote RAM:lt/Amiga/ReadMe "Read me first, café"',
     'FileNote RAM:lt/Amiga/Tools/Tool "A tool"',
     'FileNote RAM:lt/Amiga/Tools "The tools drawer"',
     f"List RAM:lt/Amiga ALL >>{O}/lha.txt",
     "CD RAM:lt",
     f'Echo "== lha a1 (level 1)" >>{O}/lha.txt',
     f"C:lha a1 RAM:lt/l1.lha Amiga >>{O}/lha.txt",
     f'Echo "RC=$RC" >>{O}/lha.txt',
     f'Echo "== lha a (default level)" >>{O}/lha.txt',
     f"C:lha a RAM:lt/l2.lha Amiga >>{O}/lha.txt",
     f'Echo "RC=$RC" >>{O}/lha.txt',
     f"C:lha v RAM:lt/l1.lha >>{O}/lha.txt",
     f"C:lha v RAM:lt/l2.lha >>{O}/lha.txt",
     "CD RAM:"]
for n in ("l1", "l2"):
    L += [f'Echo "== {n} through Unarc (XAD)" >>{O}/lha.txt',
          f"RAM:Unarc/unarc-cli RAM:lt/{n}.lha LIST >>{O}/lha.txt",
          f"RAM:Unarc/unarc-cli <NIL: RAM:lt/{n}.lha TO RAM:lt/x{n} QUIET >>{O}/lha.txt",
          f'Echo "RC=$RC" >>{O}/lha.txt',
          f"List RAM:lt/x{n} ALL >>{O}/lha.txt",
          f"Copy RAM:lt/{n}.lha {O}/"]
L.append('Echo "lhaprobe done"')
open(os.path.join(stage, "lhaprobe.script"), "w", encoding="latin-1").write("\n".join(L) + "\n")
print("showcase stage:", sorted(os.listdir(stage)))
