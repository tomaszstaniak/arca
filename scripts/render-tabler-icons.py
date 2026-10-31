#!/usr/bin/env python3
"""Export the pinned Tabler SVGs. Host-only: CairoSVG 2.8.2, Pillow 12.0.0."""
import io
import json
from pathlib import Path
import cairosvg
from PIL import Image

root = Path(__file__).resolve().parents[1]
source = root / "assets/source/tabler"
target = root / "assets/runtime/tabler"
config = json.loads((source / "origin.json").read_text())
target.mkdir(parents=True, exist_ok=True)
for name in config["icons"]:
    svg = (source / (name + ".svg")).read_text().replace("currentColor", config["colour"])
    size = config["size"]
    rendered = cairosvg.svg2png(bytestring=svg.encode(), output_width=size * 4, output_height=size * 4)
    with Image.open(io.BytesIO(rendered)) as image:
        image.convert("RGBA").resize((size, size), Image.Resampling.LANCZOS).save(target / (name + ".png"))
