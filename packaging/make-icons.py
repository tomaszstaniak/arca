#!/usr/bin/env python3
"""Attach AROS DiskObject metadata to the bundled application and document icons.

usage: make-icons.py OUTPUT_DIR
No renderer is needed for packaging. To regenerate the PNG assets, see
assets/README.md. SVG sources and the Tabler MIT notice are retained.
"""
import struct
import sys
import zlib
from pathlib import Path

SIZE = 64
RUNTIME = Path(__file__).resolve().parents[1] / "assets/runtime"
ASSETS = RUNTIME / "tabler"

ATTR_DRAWERX, ATTR_DRAWERY = 0x80001003, 0x80001004
ATTR_DRAWERWIDTH, ATTR_DRAWERHEIGHT = 0x80001005, 0x80001006
ATTR_STACKSIZE, ATTR_DEFAULTTOOL = 0x80001009, 0x8000100A
ATTR_TOOLTYPE = 0x8000100B
ATTR_TYPE, ATTR_FRAMELESS = 0x8000100F, 0x80001010
WBDRAWER, WBTOOL, WBPROJECT = 2, 3, 4


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def read_icon(name):
    path = RUNTIME / "orca/application.png" if name == "application" else ASSETS / (name + ".png")
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ValueError(f"Invalid PNG: {name}")
    if struct.unpack(">II", data[16:24]) != (SIZE, SIZE):
        raise ValueError(f"Expected {SIZE}x{SIZE}: {name}")
    return data


def icon_png(png, attrs):
    body = b""
    for tag, value in attrs:
        body += struct.pack(">I", tag)
        body += value.encode("latin-1") + b"\0" if isinstance(value, str) else struct.pack(">I", value)
    # Keep the rendered PNG untouched; metadata follows its 13-byte IHDR.
    return png[:33] + chunk(b"icOn", body) + png[33:]


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    icons = {
        "Arca.info": (read_icon("application"), [(ATTR_TYPE, WBTOOL), (ATTR_STACKSIZE, 262144), (ATTR_FRAMELESS, 1)]),
        "drawer.info": (read_icon("folder"), [(ATTR_TYPE, WBDRAWER), (ATTR_FRAMELESS, 1),
                                        (ATTR_DRAWERX, 80), (ATTR_DRAWERY, 60),
                                        (ATTR_DRAWERWIDTH, 420), (ATTR_DRAWERHEIGHT, 200)]),
        "ReadMe.info": (read_icon("file-text"), [(ATTR_TYPE, WBPROJECT), (ATTR_FRAMELESS, 1),
                                        (ATTR_DEFAULTTOOL, "SYS:Utilities/MultiView")]),
    }
    icons["Unarc.info"] = icons["Arca.info"]  # Existing test staging scripts.
    for name, (png, attrs) in icons.items():
        (out / name).write_bytes(icon_png(png, attrs))
        print(out / name)


if __name__ == "__main__":
    main()
