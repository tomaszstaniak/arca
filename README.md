# Arca

Arca packs, browses and extracts archives on AROS. Drop files or drawers to create
an archive, drop an archive to extract it, or open it to inspect its contents.
The native Zune interface uses system DefIcons; `arca-cli` provides the same core
from the Shell.

[Website](https://tomaszstaniak.github.io/arca/) · [Downloads](https://github.com/tomaszstaniak/arca/releases/latest)

## Install

With apkg:

```text
apkg update
apkg install arca
```

Or download the ZIP for your system, unpack it and keep the Arca drawer together.
Choose the **x86_64 ABIv1 PC** package for mainline AROS, **x86_64 ABIv11 PC**
for AROS One / the 2026.09 base, or **AArch64 ABIv1 Raspberry Pi** for native
AROS on Pi. Pi checks use QEMU; real hardware has not been verified. Start
`Arca` from its icon. It uses the system Zune, NList, NListview and NFloattext classes.

## Formats

- Create LhA, ZIP, 7z and tar.gz archives.
- Browse and extract ZIP, LhA, LZX, 7z, RAR, tar and other libarchive/XAD formats.
- Read encrypted ZIP/7z and multi-volume archives through the included 7-Zip helper.
- Preserve Amiga comments and protection bits when creating LhA archives.
- Extract selected entries, drag them to a Wanderer drawer, or process a queue.

```text
arca-cli archive.zip LIST
arca-cli archive.zip TO Work:Extracted
arca-cli Work:backup.zip PACK FILES Work:Documents FORMAT ZIP
```

Read the [package guide](packaging/ReadMe) for settings and limits. Names outside
ISO-8859-1 are mapped to AROS's character set and reported. Stored links are not
created. Older SFS versions cannot seek within an archive file larger than 2 GB;
use smaller archives or split volumes there. ABIv11 can be slower on large RAR
archives and long listings than mainline AROS.

## Build and licences

[Development](docs/development.md) · [Relinking LGPL libraries](docs/relinking.md)
· [Patch provenance](docs/patching.md)

The application code is MIT licensed. Bundled libraries and helper programs keep
their own licences, including LGPL and the LHa for UNIX terms. The release includes
helper source and full dependency sources/relink materials; see
[THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and the package's Licences/Source drawers.
