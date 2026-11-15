# Arca icons

The application Workbench icon uses the user-supplied orca archive-box artwork.
The exact original is retained in `source/orca/original.png`; the cleaned master
is `source/orca/application.png`. The white orca markings and silver highlights
are preserved. Only exterior dust in the alpha mask is removed; the artwork is
not regenerated. `runtime/orca/application.png` is the 64x64 RGBA package asset;
48px and 128px exports are also provided. The source artwork was supplied for
this application; the Tabler licence below does not apply to it.

Run `python3 scripts/render-application-icon.py` with Pillow to reproduce the
cleaned master and exports. Normal packaging uses the checked-in PNG and needs
no image-processing dependency.

The application drawer and ReadMe retain the blue Tabler folder/file-text icons
in `runtime/tabler/`. Unmodified upstream SVGs and their revision/checksums are
in `source/tabler/`. Tabler Icons are copyright Paweł Kuna, MIT licensed; see
`../licenses/tabler-icons-MIT.txt`. The former Tabler archive asset is retained
as source history but is no longer used as the application icon.

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
