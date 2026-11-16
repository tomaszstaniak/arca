#!/usr/bin/env python3
"""Add author metadata using apkg-pack; the caller owns the final ZIP path."""
import os, shutil, subprocess, sys, tempfile, zipfile
from pathlib import Path
archive=Path(sys.argv[1]).resolve(); version,abi=sys.argv[2:4]
root=Path(__file__).resolve().parents[1]
tool=os.environ.get("APKG_PACK", "apkg-pack.pyz")
with tempfile.TemporaryDirectory() as temporary:
    stage=Path(temporary)
    with zipfile.ZipFile(archive) as z: z.extractall(stage)
    manifest=(root/"packaging"/("arca."+abi+".toml")).read_text().replace("@VERSION@",version)
    (stage/"Arca.arospkg.toml").write_text(manifest)
    candidate=stage/"package.zip"
    subprocess.run([sys.executable,tool,"check",str(stage/"Arca")],check=True)
    subprocess.run([sys.executable,tool,"build",str(stage/"Arca"),"--output",str(candidate)],check=True)
    subprocess.run([sys.executable,tool,"check",str(candidate)],check=True)
    shutil.copyfile(candidate,archive)
