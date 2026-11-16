#!/usr/bin/env python3
"""Update package documentation and metadata while preserving all other files."""
import argparse
import json
import tomllib
import zipfile
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('archive', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('abi', choices=['v1', 'abiv11', 'raspi'])
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
if args.output.exists():
    parser.error('output exists; published files must not be replaced')
with zipfile.ZipFile(args.archive) as old:
    previous = tomllib.loads(old.read('.arospkg/manifest.toml').decode())
    manifest = (root / 'packaging' / ('arca.' + args.abi + '.toml')).read_text()
    manifest = manifest.replace('@VERSION@', previous['version'])
    current = tomllib.loads(manifest)
    assert current['revision'] > previous['revision']
    assert (current['arch'], current['abi']) == (previous['arch'], previous['abi'])
    import subprocess
    target = subprocess.check_output(
        ['bash', str(root / 'packaging' / 'target-description.sh'), args.abi], text=True)
    readme = (root / 'packaging' / 'ReadMe').read_text().replace('@TARGET@\n', target)
    assert '@TARGET@' not in readme
    application = json.loads(old.read('Arca/application.json'))
    application['revision'] = current['revision']
    changes = {'Arca/ReadMe': readme.encode(), '.arospkg/manifest.toml': manifest.encode(),
               'Arca/application.json': (json.dumps(application, indent=2) + '\n').encode()}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, 'x') as new:
        for entry in old.infolist():
            new.writestr(entry, changes.get(entry.filename, old.read(entry)))
with zipfile.ZipFile(args.archive) as old, zipfile.ZipFile(args.output) as new:
    assert old.namelist() == new.namelist()
    assert {n for n in old.namelist() if old.read(n) != new.read(n)} == set(changes)
print(args.output)
