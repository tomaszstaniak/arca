#!/usr/bin/env python3
"""Export the user-supplied application artwork. Host-only: Pillow."""
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw

root = Path(__file__).resolve().parents[1]
source = root / "assets/source/orca"
runtime = root / "assets/runtime/orca"
runtime.mkdir(parents=True, exist_ok=True)
image = Image.open(source / "original.png").convert("RGBA")
alpha = image.getchannel("A")
# The supplied cutout has white dust with alpha 1/255, not an opaque backdrop.
# Keep the connected badge, including its white face and silver highlights.
component = alpha.point(lambda a: 255 if a > 16 else 0)
ImageDraw.floodfill(component, (image.width // 2, image.height // 2), 128)
component = component.point(lambda a: 255 if a == 128 else 0)
alpha = ImageChops.multiply(alpha, component)
clean = Image.new("RGBA", image.size)
clean.paste(image, (0, 0))
clean.putalpha(alpha)
# Remove invisible RGB matte data as well as the discarded exterior alpha.
clean.paste((0, 0, 0, 0), (0, 0), alpha.point(lambda a: 255 if a == 0 else 0))
clean.save(source / "application.png", optimize=True)
for size in (48, 64, 128):
    exported = clean.resize((size, size), Image.Resampling.LANCZOS)
    a = exported.getchannel("A").point(lambda v: 0 if v <= 8 else v)
    exported.putalpha(a)
    exported.paste((0, 0, 0, 0), (0, 0), a.point(lambda v: 255 if v == 0 else 0))
    name = "application.png" if size == 64 else f"application-{size}.png"
    exported.save(runtime / name, optimize=True)
    print(runtime / name)
