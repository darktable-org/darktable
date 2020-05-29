[![icon](/data/pixmaps/idbutton.png?raw=true)](https://www.darktable.org/) darktable [![build status](https://travis-ci.org/darktable-org/darktable.svg?branch=master)](https://travis-ci.org/darktable-org/darktable) [![CII Best Practices](https://bestpractices.coreinfrastructure.org/projects/470/badge)](https://bestpractices.coreinfrastructure.org/projects/470)
=========

darktable is an open source photography workflow application and raw developer. A virtual lighttable and darkroom for photographers. It manages your digital negatives in a database, lets you view them through a zoomable lighttable and enables you to develop raw images and enhance them.

[https://www.darktable.org/](https://www.darktable.org/ "darktable homepage")

Requirements
------------

### Supported platforms

* Linux (64 bits), 
* Free BSD (64 bits),
* Windows 8 (64 bits), Windows 10 (64 bits), 
* MacOS X.

*32 bits platforms are not officially supported, they might or might not work.* 

*Windows support is still young and suffers from bugs that do not affect Linux. If possible, 
prefer using darktable on Linux.*

### Hardware 

(minimal / **recommended**):
* RAM: 4 GB / **8 GB**
* CPU: Intel Pentium 4 / **Intel Core i5 4×2.4 GHz** 
* GPU: none / **Nvidia 1024 cores, 4 GB, OpenCL 1.2 compatible**
* free disk space: 250 MB / **1 GB**

*darktable can run on lightweight configurations (even Raspberry Pi), but expect modules like denoising, local contrast, 
contrast equalizer, retouch or liquify to be slow beyond usable.*

*GPU is not mandatory but strongly recommended for a smoother experience. 
Nvidia GPU are recommended for safety because some AMD drivers behave unreliably with some modules (local contrast).*

Installing
----------

### Latest release : 3.0.2 (stable)

* [Download executable for Windows](https://github.com/darktable-org/darktable/releases/download/release-3.0.2/darktable-3.0.2-win64.exe)
* [Download executable for Mac OS](https://github.com/darktable-org/darktable/releases/download/release-3.0.2/darktable-3.0.2.dmg)
* [Install native packages and repositories for Linux](https://software.opensuse.org/download.html?project=graphics:darktable&package=darktable)
* [Install Flatpack package for Linux](https://flathub.org/apps/details/org.darktable.Darktable)

### Master branch (unstable)

The master branch is for beta testing and is generaly not safe. See the notes below (in "Building" -> "Get the source") for warnings and precautions about using the master branch.

* [Install native packages and repositories for Linux](https://software.opensuse.org/download.html?project=graphics:darktable:master&package=darktable) (one snapshot per day).
* No precompiled packages are provided for the master branch on MacOS and Windows. See how to build it manually below.


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
* Sqlite 3.15 (but 3.24 strongly recommended)

Optional dependencies minimal version:
* OpenMP 4.5 *(for CPU multi-threading and SIMD vectorization)*
* LLVM 3.9 *(for OpenCL checks at compilation time)*
* OpenCL 1.2 *(for GPU-accelerated computing)*
* Lua 5.3 *(for plugins and extensions scripting)*
* libavif 0.6.0 *(for HEIC/HEIF/AVIF exports)*
* WebP 0.3.0 *(for WebP exports)*

Optional dependencies with no version requirement:
* Gphoto2 *(for camera tethering)*
* Lensfun *(for lens automatic correction)*
* OpenEXR *(for EXR import and export)*
* OpenJPEG *(for Jpeg2000 export)*
* Colord, Xatom *(for system display color profile fetching)* 
* G'Mic *(for HaldcLUT support)*


### Get the source

#### Master branch (unstable)

The master branch contains the latest version of the source code and is intended:
* as a working base for developers,
* for beta-testers to chase bugs, 
* for users willing to sacrifice stability for new features without waiting for next release.

The master branch comes with no garanty of stability, might corrupt your database and XMP files, 
might result in loss of data and edits history, and temporarily break compatibility with previous versions and commits.

How dangerous is it ? Most of the time, it is fairly stable. As any rolling-release kind of deployment, bugs appear more often
but are fixed faster too. But sometimes, they result in losses or inconsistencies in the editing history of your pictures,
which is fine if you don't need to open your edits again in the future, but maybe not if you manage an estate.

After backing up your `~/.config/darktable` directory as well as the sidecar .XMP files of the pictures you will open
with the master branch, you may get the source:
```bash
git clone https://github.com/darktable-org/darktable.git
cd darktable
```

See below (in "Using") how to start a test install of the unstable version without damaging your regular stable install and files.

#### Latest release : 3.0.2 (stable)

darktable project releases one major version every year, for Christmas, tagged with even numbers, (like 2.2, 2.4, 2.6, 3.0). 
Minor revisions are tagged with a third digit (like 3.0.1, 3.0.2) and mostly provide bug fixes and minor new features.
You may want to compile these stable releases yourself in order to get better performance for your particular computer:

```bash
git clone https://github.com/darktable-org/darktable.git
cd darktable
git fetch --tags
git checkout tags/release-3.0.2
```

### Get submodules

Note that [rawspeed](https://github.com/darktable-org/rawspeed) is tracked via a git submodule, as well as OpenCL and LibXCF modules, so after checking-out the darktable, you need to update/checkout them,

```bash
git submodule init
git submodule update
```

### Compile

#### Easy way

darktable provides a shell script that automaticaly takes care of the building on Linux and MacOS for classic cases in a single command. 


```bash
./build.sh --prefix /opt/darktable --build-type Release --install --sudo
```

If you want to install a test version alongside your regular/stable version, change the install prefix:

```bash
./build.sh --prefix /opt/darktable-test --build-type Release --install --sudo
```
This builds the software for your own architecture only, with:

* `-O3` optimization level, 
* SSE/AVX support if detected, 
* OpenMP support (multi-threading and vectorization) if detected,
* OpenCL support (GPU offloading) if detected,
* Lua scripting support if detected.

#### Manual way

You can alternatively use the manual building to pass on custom arguments. 

##### Linux/MacOS

```bash
mkdir build/
cd build/
cmake -DCMAKE_INSTALL_PREFIX=/opt/darktable/ ..
make
sudo make install
```

##### Windows

See https://github.com/darktable-org/darktable/blob/master/packaging/windows/BUILD.txt

### Using

#### Test/unstable version

To use a test version of darktable without damaging your regular/stable version files and database, start darktable in a terminal with:

```
/opt/darktable-test/bin/darktable --configdir "~/.config/darktable-test"
```

and ensure to disable the option "write sidecar file for each image" in preferences -> storage -> XMP.

#### Regular/stable version

Simply lauch it from your desktop application menu, or in terminal, run `darktable` or `/opt/darktable/bin/darktable`. If the installation did not create a launcher in your applications menu, run:

```
sudo ln -s /opt/darktable/share/applications/darktable.desktop /usr/share/applications/darktable.desktop
```

In case you are having crashes at startup, try lauching darktable without OpenCL with `darktable --conf opencl=FALSE`.

### Further reading

There is a comprehensive list of [build instructions for Ubuntu/Debian related Linux distributions](https://github.com/darktable-org/darktable/wiki/Build-instructions-for-Ubuntu-18.04-to-20.04) or for [Fedora and related ones distributions](https://github.com/darktable-org/darktable/wiki/Build-Instructions-for-Fedora). These build instructions could easily be adapted to all others distributions


Contributing
------------

* Write a blog about darktable
* Create a tutorial for darktable
* Help expand the [user wiki](https://github.com/darktable-org/darktable/wiki)
* Answer questions on the [user mailing list](https://www.mail-archive.com/darktable-user@lists.darktable.org/)
* Share your ideas on the [developer mailing list](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)
* Test [releases](https://www.darktable.org/install/)
* Review [pull requests](https://github.com/darktable-org/darktable/pulls)
* Start [hacking on darktable](https://www.darktable.org/redmine/projects/darktable/wiki/Contributing_code) and see [developer's guide](https://github.com/darktable-org/darktable/wiki/Developer's-guide)


FAQ
---

### Why is my camera not detected when plugged-in ?

Check that you have the latest [gphoto2 library](http://www.gphoto.org/ "gphoto2 homepage") installed in order to support the newest cameras.

### Why is my lens not detected/correctedal in darkroom ?

Lens correction profiles are provided by Lensfun, which has 2 parts: a program and a database. 
Most Linux distributions provide a recent-enough version of the program, 
but the majority provide an outdated version of the database. If 
[Lensfun](https://lensfun.github.io/) is correctly installed, then update its database in a terminal by running:

```
lensfun-update-data
```

or alternatively

```
/usr/bin/g-lensfun-update-data 
```

Wiki
----

* [GitHub wiki](https://github.com/darktable-org/darktable/wiki "github wiki")
* [User wiki](https://www.darktable.org/redmine/projects/users/wiki "darktable user wiki")
* [Developer wiki](https://github.com/darktable-org/darktable/wiki/Developer's-guide "darktable developer wiki")


Mailing lists
-------------

* Users [[subscribe](mailto:darktable-user+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-user@lists.darktable.org/)]
* Developer [[subscribe](mailto:darktable-dev+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-dev@lists.darktable.org/)]
* CI (read-only, high traffic!) [[subscribe](mailto:darktable-ci+subscribe@lists.darktable.org) | [archive](https://www.mail-archive.com/darktable-ci@lists.darktable.org/)]
