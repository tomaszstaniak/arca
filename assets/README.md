# Arca icons

The packaged Workbench icons use the original Tabler outline icons, with a blue stroke (`#2878b8`):

| Use | Tabler icon | Runtime PNG |
|---|---|---|
| Application Workbench icon | archive | runtime/tabler/archive.png |
| Application drawer | folder | runtime/tabler/folder.png |
| ReadMe document | file-text | runtime/tabler/file-text.png |

Unmodified upstream SVGs and their revision/checksums are in `source/tabler/`.
Tabler Icons are copyright Paweł Kuna, MIT licensed; see
`../licenses/tabler-icons-MIT.txt`. Only colour and raster size are set during export;
paths, proportions and the standard two-unit stroke are unchanged.

## Packing preview

The start window loads system DefIcons through icon.library, independently of
Arca.info. It tries the selected format's conventional names (LHA, ZIP, 7z/7zip,
TGZ/GZIP), then Archive, then the system project default. Missing format artwork
therefore does not require bundled interface icons. A system with one Archive
icon may show the same image for several formats; the extension below it still
changes. Custom file-identification hooks can use different names.

The preview uses the original icon pixel dimensions, overriding desktop scaling
only for these in-memory images. Small DefIcons are never enlarged. The icon and
instructions share one drop target; there is no separate arrow. System settings
and files are unchanged;
no companion .info is created for a packed archive. Preview images are reloaded whenever the window is reopened.

## Export

Install CairoSVG 2.8.2 and Pillow 12.0.0 in a host Python environment, then run
`python3 scripts/render-tabler-icons.py`. CairoSVG requires the Cairo library.
The script renders at 4x resolution and downsamples to transparent 64x64 RGBA PNGs.
Review and retain the generated PNGs in Git alongside their SVG sources.

Normal builds/packages need neither CairoSVG nor Pillow: `packaging/make-icons.py`
uses the bundled PNGs and attaches the existing AROS `icOn` metadata. The application
stack size, drawer dimensions and ReadMe's MultiView default tool are preserved.
The Tabler licence is included automatically by the package's `licenses/` collection.
