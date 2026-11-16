# Arca

Arca is an archive manager for AROS. Drop files or drawers onto its window to
pack them. Drop an archive to extract it, or open it to browse its contents.
The Zune interface shows system DefIcons for archive formats. The same archive
operations are available through `arca-cli` from the Shell.

[Website](https://tomaszstaniak.github.io/arca/) · [Downloads](https://github.com/tomaszstaniak/arca/releases/latest)

## Install

Install with apkg:

```text
apkg update
apkg install arca
```

You can also download and unpack the ZIP for your system:

- x86_64 ABIv1 PC for mainline AROS.
- x86_64 ABIv11 PC for systems using the 2026.09 ABIv11 base.
- AArch64 ABIv1 Raspberry Pi for AROS on Pi.

Keep the Arca drawer together and start `Arca` from its icon. It requires the
system Zune, NList, NListview and NFloattext classes. The Pi build was tested in
QEMU; it has not been tested on physical hardware.

## Working with archives

- Create LhA, ZIP, 7z and tar.gz archives.
- Browse and extract ZIP, LhA, LZX, 7z, RAR, tar and other libarchive/XAD formats.
- Open encrypted ZIP/7z and multi-volume archives with the included 7-Zip helper.
- Preserve Amiga comments and protection bits when creating LhA archives.
- Extract selected files or drag them to a Wanderer drawer. Drop several archives
  to extract them in a queue.

```text
arca-cli archive.zip LIST
arca-cli archive.zip TO Work:Extracted
arca-cli Work:backup.zip PACK FILES Work:Documents FORMAT ZIP
```

The [package guide](packaging/ReadMe) explains the settings and limitations.
Arca maps names outside ISO-8859-1 to the AROS character set and reports the
changes. It does not create links stored in an archive.

Older SFS versions cannot seek within archive files larger than 2 GB. Use smaller
archives or split volumes on those filesystems. Large RAR archives and long
listings can be slower on ABIv11 than on mainline AROS.

## Build and licences

[Development](docs/development.md) · [Relinking LGPL libraries](docs/relinking.md)
· [Patch provenance](docs/patching.md)

The application code uses the MIT licence. Bundled libraries and helper programs
have their own licences, including LGPL and the LHa for UNIX terms. Releases
include the helper and dependency sources, with materials for relinking the LGPL
libraries. See [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES) and the package's
Licences/Source drawers.
