# PhotographyLAB (Qt 6 front end)

A Qt 6 / QML photo editor on darktable's engine. It links `lib_darktable`
headless, the way `darktable-cli` does, so every pixel still comes from
darktable's pixelpipe; only the window is new.

## Building

Needs everything darktable needs, plus Qt 6.4 or later (Core, Gui, Quick,
QuickControls2). On Ubuntu 24.04:

```sh
sudo apt install qt6-base-dev qt6-declarative-dev qml6-module-qtquick \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts \
  qml6-module-qtquick-dialogs qml6-module-qtquick-templates \
  qml6-module-qtqml-workerscript qml6-module-qtquick-window
cmake -B build -G Ninja -DUSE_PHOTOGRAPHYLAB=ON
cmake --build build
build/bin/photographylab [photo]
```

On Windows, build in the MSYS2 UCRT64 shell as for darktable, adding
`mingw-w64-ucrt-x86_64-qt6-base` and `mingw-w64-ucrt-x86_64-qt6-declarative`.

## Layout

| file | what it does |
|---|---|
| `plab_engine.c/.h` | the only code that calls darktable: import, history, render to memory, styles, export |
| `engineworker.*` | runs the engine on its own thread; turns slider values into module parameters |
| `labcontroller.*` | the state QML binds to; sends one render at a time, always the newest |
| `qml/` | the window: photo, and Basic / Looks / Detail / Optics tabs |
| `styles/` | the bundled `custom|...` styles, imported into the library on first start |

Edits are ordinary darktable history on top of what darktable applies by
itself, so an image edited here opens in darktable with the same look, and
the `.xmp` sidecar follows darktable's own preference.

Settings, library and LUTs live in their own `photographylab` folders, apart
from a darktable install.

## Checking a build without a screen

```sh
QT_QPA_PLATFORM=offscreen build/bin/photographylab --size 1440x900 \
  --set exposure=0.5 --set lut=/path/look.cube --screenshot shot.png photo.raw
build/bin/photographylab --export out.jpg photo.raw
```
