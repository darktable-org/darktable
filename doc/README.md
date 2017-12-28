# darktable

This is darktable, a free raw photo viewer and organizer.

Build dependencies for many platforms are listed here:

https://redmine.darktable.org/projects/darktable/wiki/Building_darktable_20

### Requirements
In summary, you'll need:

 - `gcc`, `g++`, GNU `make` and `cmake`
 - `gtk+-3.0` (and dependencies, e.g. `cairo`, `gdkpixbuf`, etc.)
 - `xml`, `pugixml`, `xslt` and `json` libraries
 - `png`, `jpeg`, `tiff`, `rsvg2` libraries
 - `sqlite3`, `exiv2`, `lensfun`, `lcms2`, `curl`

Optionally, you might need for special features:

 - `libcups2` (for the print module)
 - `gphoto2` (for camera support, recommended)
 - `flickcurl` (for Flickr support)
 - SDL, SDL-image and Mesa OpenGL (to build darktable-viewer)
 - `osm-gps-map` and `libsoup` for geo tagging view
 - `lensfun` (lens distortion plugin)
 - OpenEXR for HDR export
 - `libsecret` for storing passwords
 - `libcolord-dev` `libcolord-gtk-dev` for colour profile support
 - `webp` and `openjpeg` libraries for WebP and JPEG 2000 support
 - GraphicsMagick library for TIFF-encoded EXIF thumbnails and LDR image format support

### Build

Then, type:

```
$ ./build.sh --prefix /usr --buildtype Release
$ cd build && make install (or sudo make install)
$ darktable
```

Optionally, to build the user manual:

 - Java JDK, `gnome-doc-utils`, Saxon 6.5.x, FOP and ImageMagick
 - `xsltproc` and the DocBook XML DTD and XSL stylesheets

Then, type:

```
$ cd build
$ make darktable-usermanual
$ evince doc/usermanual/darktable-usermanual.pdf
```

Optionally, to build translations of the manual pages:

 - PO for anything (`po4a`)

Other used packages (supplied in the source tree):

 - RawSpeed
 - Lua 5.2 and LuaAutoc (although the local system version can be used instead)

Darktable has OpenCL support for graphics cards with:

 - at least 1GB graphics RAM (more is better)
 - a modern AMD or nVidia chipset
 - proprietary drivers from the manufacturer loaded (currently no open opencl
   drivers support all of the features that darktable needs)

Enjoy!

Send any bug reports to the mailing list: darktable-dev@lists.darktable.org
Find more information on the web: https://www.darktable.org/
