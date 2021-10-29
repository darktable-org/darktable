[![icon](/data/pixmaps/idbutton.png?raw=true)](https://www.darktable.org/) darktable [![GitHub Workflow Status (branch)](https://img.shields.io/github/workflow/status/darktable-org/darktable/CI/master)](https://github.com/darktable-org/darktable/actions/workflows/ci.yml?query=branch%3Amaster+is%3Acompleted+event%3Apush) [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/470/badge)](https://bestpractices.coreinfrastructure.org/projects/470)
=========

darktable is an open source photography workflow application and non-destructive raw developer. A virtual lighttable and darkroom for photographers. It manages your digital negatives in a database, lets you view them through a zoomable lighttable and enables you to develop raw images, enhance them and export them to local or remote storage.

darktable is **not** a free Adobe® Lightroom® replacement.

[https://www.darktable.org/](https://www.darktable.org/ "darktable homepage")

## Table of Contents

1. [Documentation](#documentation)
2. [Website](#website)
3. [Requirements](#requirements)
   - [Supported platforms](#supported-platforms)
   - [Hardware](#hardware)
4. [Installing](#installing)
   - [Latest release](#latest-release)
   - [Development snapshot](#development-snapshot)
5. [Updating from older versions](#updating-from-older-versions)
6. [Getting extensions](#getting-extensions)
7. [Building](#building)
   - [Dependencies](#dependencies)
   - [Get the source](#get-the-source)
   - [Get submodules](#get-submodules)
   - [Compile](#compile)
   - [Further reading](#further-reading)
8. [Using](#using)
   - [Test/unstable version](#testunstable-version)
   - [Regular/stable version](#regularstable-version)
9. [Contributing](#contributing)
10. [FAQ](#faq)
   - [Why is my camera not detected when plugged-in ?](#why-is-my-camera-not-detected-when-plugged-in-)
   - [Why is my lens not detected/corrected in darkroom ?](#why-is-my-lens-not-detectedcorrected-in-darkroom-)
   - [Why are the thumbnails in lighttable looking different than the preview in darkroom ?](#why-are-the-thumbnails-in-lighttable-looking-different-than-the-preview-in-darkroom-)
11. [Wiki](#wiki)
12. [Mailing lists](#mailing-lists)

Documentation
-------------

The darktable user manual is maintained in the [dtdocs](https://github.com/darktable-org/dtdocs) repository.

Lua API documentation is maintained in the [luadocs](https://github.com/darktable-org/luadocs) repository.

Website
-------

The website [https://www.darktable.org/](https://www.darktable.org/) is maintained in the [dtorg](https://github.com/darktable-org/dtorg) repository.

Requirements
------------

### Supported platforms

* Linux (64 bits)
* Free BSD (64 bits)
* Windows 8 (64 bits), Windows 10 (64 bits)
* macOS

*32 bits platforms are not officially supported, they might or might not work.*

*Windows support is still young and suffers from bugs that do not affect Linux. If possible,
prefer using darktable on Linux.*

### Hardware

(workable minimum / **recommended minimum**):
* RAM: 4 GB / **8 GB**
* CPU: Intel Pentium 4 / **Intel Core i5 4×2.4 GHz**
* GPU: none / **Nvidia with 1024 CUDA cores, 4 GB, OpenCL 1.2 compatible**
* free disk space: 250 MB / **1 GB**

*darktable can run on lightweight configurations (even Raspberry Pi), but expect modules like denoising, local contrast,
contrast equalizer, retouch or liquify to be slow beyond usable.*

*GPU is not mandatory but strongly recommended for a smoother experience.
Nvidia GPU are recommended for safety because some AMD drivers behave unreliably with some modules (local contrast).*

Installing
----------

If the latest release is still not available as a pre-built package for your distribution,
you can build the software yourself following the instructions [below](#building).

### Latest release

3.6.1 (stable)

* [Download executable for Windows](https://github.com/darktable-org/darktable/releases/download/release-3.6.1/darktable-3.6.1-win64.exe)
* [Download executable for mac OS](https://github.com/darktable-org/darktable/releases/download/release-3.6.1/darktable-3.6.1.6.dmg)
* [Install native packages and repositories for Linux](https://software.opensuse.org/download.html?project=graphics:darktable:stable&package=darktable)
* [Install Flatpak package for Linux](https://flathub.org/apps/details/org.darktable.Darktable)
* [More information about installing darktable on any system](https://www.darktable.org/install/)

*When using a pre-built package, ensure it has been built with Lua, OpenCL, OpenMP and Colord support.
These are optional and will not prevent darktable from running if missing,
but their absence will degrade user experience.
Noticeably, some Flatpak, Snap and Appimage packages lack OpenCL and Lua support.*

### Development snapshot

The development snapshot is the state of the master branch at current time. It is intended for testing and is generally not safe. See the notes [below](#get-the-source) for warnings and precautions about using the master branch.

* [Install native packages and repositories for Linux](https://software.opensuse.org/download.html?project=graphics:darktable:master&package=darktable) (one snapshot per day).
* No pre-compiled packages are provided for the master branch on macOS and Windows. See how to build it manually below.

Updating from older versions
----------------------------

When updating darktable from an older release, you need to install
the newest version. Files will be preserved.

However, sometimes newer releases need to change the structure of the library database
(containing the whole list of images known to darktable, with their editing history). You will then
be prompted with a request to either upgrade the database or close the software.

**The migration to a newer database structure/newer release means new and old edits
will not be compatible with older versions of darktable.** Upgrades are definitive.
Newer versions are always compatible with older edits, but newer edits are generally
not compatible with older versions.

darktable automatically backs up the library database when a new version upgrades it
(in `~/.config/darktable/library.db-pre-3.0.0` for example), so
you can revert to the previous release by restoring this backup if needed
(simply rename it `library.db`).

If you try to open a newer database with an older software version, the parts of the edits done with new features
will be discarded and you will lose them. This also applies to the sidecar XMP files.

If you plan to move regularly between 2 versions (new/unstable and old/stable) see [below](#testunstable-version)
how to do it safely.

Getting extensions
------------------

Extensions and plugins use the Lua scripting language and can be downloaded [here](https://github.com/darktable-org/lua-scripts). Lua support is optional in darktable, ensure you have the interpreter `lua` and its development files (package
`lua-dev` or `lua-devel`, depending on distributions) installed on your system
while building or ensure the package you are using has been built with this library.

Extensions allow exporting for various media and websites, merge/stack/blend HDR, panoramas or focus bracketing,
apply AI-based facial recognition, manage tags and GPS data, etc.

Building
--------

### Dependencies

Compatible compilers:
* Clang: 8, 9, 10
* GCC: 8, 9, 10
* Mingw64: 6, 7

Required dependencies minimal version:
* CMake 3.10
* Gtk 3.22
* Glib 2.40
* SQLite 3.15 (but 3.24 strongly recommended)

Optional dependencies minimal version:
* OpenMP 4.5 *(for CPU multi-threading and SIMD vectorization)*
* LLVM 3.9 *(for OpenCL checks at compilation time)*
* OpenCL 1.2 *(for GPU-accelerated computing)*
* Lua 5.3 *(for plugins and extensions scripting)*
* libavif 0.7.2 *(for AVIF import/export)*
* WebP 0.3.0 *(for WebP exports)*

Optional dependencies with no version requirement:
* Gphoto2 *(for camera tethering)*
* Lensfun *(for lens automatic correction)*
* OpenEXR *(for EXR import and export)*
* OpenJPEG *(for Jpeg2000 export)*
* Colord, Xatom *(for system display color profile fetching)*
* G'Mic *(for gmz compressed lut support)*
* PortMidi *(for MIDI input support)*
* SDL2 *(for gamepad input support)*

To install all the dependencies on Linux systems, you may use the source repositories of your distribution
(provided they are up-to-date):

#### Fedora and RHEL

```bash
sudo dnf builddep darktable
```

#### OpenSuse

```bash
sudo zypper si -d darktable
```

#### Ubuntu

```bash
sed -e '/^#\sdeb-src /s/^# *//;t;d' "/etc/apt/sources.list" \
  | sudo tee /etc/apt/sources.list.d/darktable-sources-tmp.list > /dev/null \
  && (
    sudo apt-get update
    sudo apt-get build-dep darktable
  )
sudo rm /etc/apt/sources.list.d/darktable-sources-tmp.list
```

#### Debian

```bash
sudo apt-get build-dep darktable
```

#### Install missing dependencies

If mandatory dependencies are missing on your system, building the software will fail with
errors like `Package XXX has not been found` or `Command YYY has no provider on this system`.
What you need to do, then, is to search which package provides the required missing package or command in your distribution,
then install it. This can usually be done in your package manager (not the applications manager
customarily provided by default in your distribution) or on the internet with a search engine.
You may need to install a package manager first (like Synaptic on Debian/Ubuntu, or DNF Dragora on Fedora/RHEL).

This process might be tedious but you only need to do it once. See this
[page on building darktable](https://github.com/darktable-org/darktable/wiki/Building-darktable)
for one-line commands that will install most dependencies on the most frequent Linux distributions.

### Get the source

#### Master branch (unstable)

The master branch contains the latest version of the source code and is intended:
* as a working base for developers,
* for beta-testers to chase bugs,
* for users willing to sacrifice stability for new features without waiting for the next release.

The master branch comes with no guarantee of stability, might corrupt your database and XMP files,
might result in loss of data and edits history or temporarily break compatibility with previous versions and commits.

How dangerous is it? Most of the time, it is fairly stable. As with any rolling-release kind of deployment, bugs appear more often
but are fixed faster too. But sometimes, they result in losses or inconsistencies in the editing history of your pictures,
which is fine if you don't need to open your edits again in the future, but maybe not if you manage an estate.

After backing up your `~/.config/darktable` directory as well as the sidecar .XMP files of the pictures you will open
with the master branch, you may get the source:

```bash
git clone --recurse-submodules --depth 1 https://github.com/darktable-org/darktable.git
cd darktable
```

See below (in "Using") how to start a test install of the unstable version without damaging your regular stable install and files.

#### Latest stable release

3.6.1

darktable project releases one major version every year, for Christmas, tagged with even numbers (e.g., 2.2, 2.4, 2.6, 3.0).
Minor revisions are tagged with a third digit (e.g., 3.0.1, 3.0.2) and mostly provide bug fixes and minor new features.
You may want to compile these stable releases yourself to get better performance for your particular computer:

```bash
git clone --recurse-submodules --depth 1 https://github.com/darktable-org/darktable.git
cd darktable
git fetch --tags
git checkout tags/release-3.6.1
```

### Get submodules

Note that [LibXCF](https://github.com/houz/libxcf.git), [OpenCL](https://github.com/KhronosGroup/OpenCL-Headers.git), [rawspeed](https://github.com/darktable-org/rawspeed), and [whereami](https://github.com/gpakosz/whereami) are tracked via a git submodule, so after checking-out the darktable, you need to update/checkout them:

```bash
git submodule update --init
```

### Compile

#### Easy way

WARNING: in case you have already built darktable in the past, don't forget to remove entirely (`rm -R`) the `build`
and `/opt/darktable` directories to avoid conflicting files from different versions. Many weird behaviours and transient
bugs have been reported that can be tracked down to the building cache not properly invalidating the changed dependencies, so
the safest way is to completely remove previously built binaries and restart from scratch.

darktable provides a shell script that automatically takes care of the building on Linux and macOS for classic cases in a single command.

```bash
./build.sh --prefix /opt/darktable --build-type Release --install --sudo
```

If you want to install a test version alongside your regular/stable version, change the install prefix:

```bash
./build.sh --prefix /opt/darktable-test --build-type Release --install --sudo
```
This builds the software for your architecture only, with:

* `-O3` optimization level,
* SSE/AVX support if detected,
* OpenMP support (multi-threading and vectorization) if detected,
* OpenCL support (GPU offloading) if detected,
* Lua scripting support if detected.

#### Manual way

You can alternatively use the manual building to pass on custom arguments.

##### Linux/macOS

```bash
mkdir build/
cd build/
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
make
sudo make install
```

##### Windows

See https://github.com/darktable-org/darktable/blob/master/packaging/windows/BUILD.md

### Using

#### Test/unstable version

To use a test version of darktable without damaging your regular/stable version files and database, start darktable in a terminal with:

```bash
/opt/darktable-test/bin/darktable --configdir "~/.config/darktable-test"
```

and ensure to disable the option "write sidecar file for each image" in preferences -> storage -> XMP. This way,
your regular/stable version will save its configuration files in `~/.config/darktable`, as usual, and
the test/unstable one in `~/.config/darktable-test`, so they will not produce database conflicts.

#### Regular/stable version

Simply launch it from your desktop application menu, or in terminal, run `darktable` or `/opt/darktable/bin/darktable`. If the installation did not create a launcher in your applications menu, run:

```bash
sudo ln -s /opt/darktable/share/applications/darktable.desktop /usr/share/applications/darktable.desktop
```

You may find darktable configuration files in `~/.config/darktable`.
If you are having crashes at startup, try launching darktable without OpenCL with `darktable --conf opencl=FALSE`.

### Further reading

There is a comprehensive list of [build instructions for Ubuntu/Debian related Linux distributions](https://github.com/darktable-org/darktable/wiki/Build-instructions-for-Ubuntu) or for [Fedora and related ones distributions](https://github.com/darktable-org/darktable/wiki/Build-Instructions-for-Fedora). These build instructions could easily be adapted to all other distributions


Contributing
------------

* Write a blog about darktable
* Create a tutorial for darktable
* Help expand the [user wiki](https://github.com/darktable-org/darktable/wiki)
* Answer questions on the [user mailing list](https://www.mail-archive.com/darktable-user@lists.darktable.org/)
* Share your ideas on the [developer mailing list](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)
* Test [releases](https://www.darktable.org/install/)
* Review [pull requests](https://github.com/darktable-org/darktable/pulls)
* Start [hacking on darktable](https://www.darktable.org/development/) and see [developer's guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide)


FAQ
---

### Why is my camera not detected when plugged-in ?

Check that you have the latest [gphoto2 library](http://www.gphoto.org/ "gphoto2 homepage") installed in order to support the newest cameras.

### Why is my lens not detected/corrected in darkroom ?

Lens correction profiles are provided by Lensfun, which has 2 parts: a program and a database.
Most Linux distributions provide a recent enough version of the program,
but provide an outdated version of the database. If
[Lensfun](https://lensfun.github.io/) is correctly installed, then update its database in a terminal by running:

```bash
lensfun-update-data
```

or alternatively

```bash
/usr/bin/g-lensfun-update-data
```

### Why are the thumbnails in lighttable looking different than the preview in darkroom ?

For RAW files never edited before in darktable (when you only imported them), the lighttable uses by default
the embedded JPEG thumbnail put in the RAW file by your camera. Loading this JPEG file is faster and makes the
lighttable more responsive when importing large collections of images.

However, this JPEG thumbnail is processed by the firmware of the camera, with proprietary algorithms,
and colors, sharpness and contrast might not look the same as
darktable processing, which is what you see when opening the darkroom.
Notice camera's manufacturers don't publish the pixel cuisine they perform in their firmware
so their look is not exactly nor easily reproducible externally.

However, once RAW images have been previously edited in darktable,
the lighttable thumbnail should match exactly the darkroom preview because they are processed the same.

To never see the embedded JPEG thumbnail in lighttable, for RAW files, you can check the option
"don't use embedded preview JPEG but half-size raw" in preferences -> lighttable.

Wiki
----

* [GitHub wiki](https://github.com/darktable-org/darktable/wiki "github wiki")
* [Developer wiki](https://github.com/darktable-org/darktable/wiki/Developer's-guide "darktable developer wiki")


Mailing lists
-------------

* Users [[subscribe](mailto:darktable-user+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-user@lists.darktable.org/)]
* Developer [[subscribe](mailto:darktable-dev+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)]
* CI (read-only, high traffic!) [[subscribe](mailto:darktable-ci+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-ci@lists.darktable.org/)]
