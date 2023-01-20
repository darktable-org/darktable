To build darktable for the Windows operating system you have two basic options.

## Table of Contents

1. [Native build using MSYS2](#native-build-using-msys2)
2. [Cross-platform compile on Linux](#cross-platform-compile-on-linux)

Native build using MSYS2
------------------------

How to make a darktable Windows installer (x64 only; Windows 8.1 will need to have [UCRT installed](https://support.microsoft.com/en-us/topic/update-for-universal-c-runtime-in-windows-c0514201-7fe6-95a3-b0a5-287930f3560c)):

* Install MSYS2 (instructions and prerequisites can be found on the official website: https://www.msys2.org)

* Start the MSYS terminal and update the base system until no further updates are available by repeating:
    ```bash
    pacman -Syu
    ```

* From the MSYS terminal, install x64 developer tools, x86_64 toolchain and git:
    ```bash
    pacman -S --needed base-devel intltool git
    pacman -S --needed mingw-w64-ucrt-x86_64-{cc,cmake,gcc-libs,ninja,nsis,omp}
    ```

* Install required libraries and dependencies for darktable:
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-{exiv2,lcms2,lensfun,dbus-glib,openexr,sqlite3,libxslt,libsoup,libavif,libheif,libjxl,libwebp,libsecret,lua,graphicsmagick,openjpeg2,gtk3,pugixml,libexif,osm-gps-map,libgphoto2,drmingw,gettext,python3,iso-codes,python3-jsonschema,python3-setuptools}
    ```

* Install optional libraries and dependencies:

    for cLUT
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-gmic
    ```
    for NG input with midi or gamepad devices
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-{portmidi,SDL2}
    ```

* Install optional libraries required for [testing](../../src/tests/unittests):
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-cmocka
    ```

* Switch to the UCRT64 terminal and update your lensfun database:
    ```bash
    lensfun-update-data
    ```

* For libgphoto2 tethering:
    * You might need to restart the UCRT64 terminal to have CAMLIBS and IOLIBS environment variables properly set.
    Make sure they aren't pointing into your normal Windows installation in case you already have darktable installed.
    You can check them with:
        ```bash
        echo $CAMLIBS
        echo $IOLIBS
        ```
        * If you have to set them manually you can do so by setting the variables in your `~/.bash_profile`. For example (check your version numbers first):
            ```
            export CAMLIBS="$MINGW_PREFIX/lib/libgphoto2/2.5.30/"
            export IOLIBS="$MINGW_PREFIX/lib/libgphoto2_port/0.12.1/"
            ```
        * If you do so, execute the following command to activate those profile changes:
            ```bash
            . .bash_profile
            ```

    * Also use this program to install the USB driver on Windows for your camera (it will replace the current Windows camera driver with the WinUSB driver):
    https://zadig.akeo.ie

* From the UCRT64 terminal, clone the darktable git repository (in this example into `~/darktable`):
    ```bash
    cd ~
    git clone https://github.com/darktable-org/darktable.git
    cd darktable
    git submodule init
    git submodule update
    ```

* Finally build and install darktable, either the easy way by using the provided script:
    ```bash
    ./build.sh --prefix /opt/darktable --build-type Release --build-generator Ninja --install
    ```
    or performing the steps manually:
    ```bash
    mkdir build
    cd build
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ..
    cmake --build .
    cmake --install .
    ```
    After this darktable will be installed in `/opt/darktable `directory and can be started by typing `/opt/darktable/bin/darktable.exe` from the UCRT64 terminal.

    *NOTE: If you are using the Lua scripts, build the installer and install darktable.
    The Lua scripts check the operating system and see Windows and expect a Windows shell when executing system commands.
    Running darktable from the UCRT64 terminal gives a bash shell and therefore the commands will not work.*

* For building the installer image, which will create darktable-<VERSION>.exe installer in the current build directory, use:
    ```bash
    cmake --build . --target package
    ```

    *NOTE: The package created will be optimized for the machine on which it has been built, but it could not run on other PCs with different hardware or different Windows version. If you want to create a "generic" package, change the first cmake command line as follows:*
    ```bash
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -DBINARY_PACKAGE_BUILD=ON ..
    ```

While Ninja offers advantages of default parallel builds and reduced build times for incremental builds (builds on Windows are significantly slower than with Linux based systems), you can also fall back to more traditional Makefiles should the need arise. You'll need to install Autotools from an MSYS terminal with:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-autotools
```

Now return to the UCRT64 terminal and use this sequence instead:

```bash
mkdir build
cd build
cmake -G 'MSYS Makefiles' --parallel 6 -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ..
cmake --build .
```

Feel free to adjust the number of parallel jobs according to your needs: Ninja will use all available CPU cores by default, while Makefiles will assume no parallel jobs if not explicitly specified.

If you are in a hurry you can now run darktable by executing the `darktable.exe` found in the `build/bin` folder, install in `/opt/darktable` as described earlier, or create an install image.

If you like experimenting you could also install `mingw-w64-ucrt-x86_64-{clang,openmp}` and use that compiler instead of gcc/g++ by setting the `CC=clang` and `CXX=clang++` variables. Alternatively, you can use the CLANG64 environment instead of UCRT64 and try building darktable with its default toolchain (note that the prefix for installation of all the packages above then becomes `mingw-w64-clang-x86_64-`).


Cross-platform compile on Linux
-------------------------------

*NOTE:  These instructions have not been updated for some time and are likely to need revision.*

In order to build darktable for Windows there is a chance to cross compile it
on Linux. The result is currently not particularly usable, but still, if you
are curious you could give it a try.

The tests have been done cross compiling on openSUSE. Since it's using a
virtualized installation only used for this stuff you can safely do everything
as root. It's also enough to install a server setup without X.

- Grab the openSUSE install ISO from http://software.opensuse.org/122/en
(I used the 64 Bit version).
- Install in Virtualbox.
- Prepare the system:
    ```sh
    #!/bin/sh

    # add the cross compiling repositoryies
    # regular stuff
    zypper ar http://download.opensuse.org/repositories/windows:/mingw:/win64/openSUSE_12.2/windows:mingw:win64.repo
    # extra repository for exiv2 and lensfun
    zypper ar http://download.opensuse.org/repositories/home:/sergeyopensuse:/branches:/windows:/mingw:/win64/openSUSE_12.2/home:sergeyopensuse:branches:windows:mingw:win64.repo

    # install the needed tools like cross compilers
    zypper install bash-completion mingw64-cross-gcc mingw64-cross-gcc-c++ cmake binutils git libxslt

    # install the cross compiled libraries
    zypper install mingw64-win_iconv mingw64-win_iconv-devel mingw64-gtk2 \
                mingw64-gtk2-devel mingw64-libxml2 mingw64-libxml2-devel \
                mingw64-libgomp mingw64-lensfun mingw64-lensfun-devel \
                mingw64-pthreads mingw64-pthreads-devel mingw64-librsvg \
                mingw64-librsvg-devel mingw64-libsqlite mingw64-libsqlite-devel \
                mingw64-libexiv2 mingw64-libexiv2-devel mingw64-libcurl \
                mingw64-libcurl-devel mingw64-libjpeg mingw64-libjpeg-devel \
                mingw64-libtiff mingw64-libtiff-devel mingw64-liblcms2 \
                mingw64-liblcms2-devel mingw64-libopenjpeg1 mingw64-libopenjpeg-devel \
                mingw64-GraphicsMagick mingw64-GraphicsMagick-devel mingw64-libexpat \
                mingw64-libexpat-devel mingw64-libsoup mingw64-libsoup-devel

    # the jpeg headers installed by openSUSE contain some definitions that hurt us so we have to patch these.
    # this is super ugly and needs to be redone whenever the library is updated.
    sed -e 's:^#define PACKAGE:// #define PACKAGE:g' -i /usr/x86_64-w64-mingw32/sys-root/mingw/include/jconfig.h
    ```

- Put the sources somewhere, probably by grabbing them from git: `git clone https://github.com/darktable-org/darktable.git`
- cd into the freshly cloned source tree and edit `cmake/toolchain_mingw64.cmake` if needed, it should be ok if using the virtualized openSUSE environment as described here.
- Build darktable using this script (save it as `build-win.sh`). Make sure to edit it to suit your environment, too, if needed. Again, if following these directions it should be working out of the box. Run the script from the root directory of the git clone (i.e., put it next to `build.sh`):

    ```sh
    #!/bin/sh

    # Change this to reflect your setup
    # Also edit cmake/toolchain_mingw64.cmake
    MINGW="/usr/x86_64-w64-mingw32"
    RUNTIME_PREFIX=".."


    export PATH=${MINGW}/bin:$PATH
    export CPATH=${MINGW}/include:${MINGW}/include/OpenEXR/
    export LD_LIBRARY_PATH=${MINGW}/lib
    export LD_RUN_PATH=${MINGW}/lib
    export PKG_CONFIG_LIBDIR=${MINGW}/lib/pkgconfig

    DT_SRC_DIR=`dirname "$0"`
    DT_SRC_DIR=`cd "$DT_SRC_DIR"; pwd`
    BUILD_DIR="build-win"

    cd $DT_SRC_DIR;

    INSTALL_PREFIX=$1
    if [ "$INSTALL_PREFIX" =  "" ]; then
    INSTALL_PREFIX=/opt/darktable-win/
    fi

    BUILD_TYPE=$2
    if [ "$BUILD_TYPE" =  "" ]; then
            BUILD_TYPE=RelWithDebInfo
    fi

    echo "Installing to $INSTALL_PREFIX for $BUILD_TYPE"
    echo "WARNING:"
    echo "     This is a highly experimental try to cross compile darktable for Windows!"
    echo "     It is not guaranteed to compile or do anything useful when executed!"
    echo ""

    if [ ! -d ${BUILD_DIR} ]; then
    mkdir ${BUILD_DIR}
    fi

    cd ${BUILD_DIR}

    MAKE_TASKS=1
    if [ -r /proc/cpuinfo ]; then
    MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)
    elif [ -x /sbin/sysctl ]; then
    TMP_CORES=$(/sbin/sysctl -n hw.ncpu 2>/dev/null)
    if [ "$?" = "0" ]; then
        MAKE_TASKS=$TMP_CORES
    fi
    fi

    if [ "$(($MAKE_TASKS < 1))" -eq 1 ]; then
    MAKE_TASKS=1
    fi

    cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_mingw64.cmake \
        -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
        -DCMAKE_INSTALL_LOCALEDIR=${RUNTIME_PREFIX}/share/locale \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DUSE_OPENCL=Off \
        -DDONT_USE_INTERNAL_LUA=On \
        .. \
    && make -j $MAKE_TASKS

    if [ $? = 0 ]; then
    echo "darktable finished building, to actually install darktable you need to type:"
    echo "# cd ${BUILD_DIR}; sudo make install"
    fi
    ```

- Last but not least: `cd build-win; make install`
- Now you have a Windows version of darktable in `/opt/darktable-win/`, which should be suited for 64 bit Windows installations. Before you can run it do `cp /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll /opt/darktable-win/bin/`
- Done. Copy `/opt/darktable-win/` to a Windows box, go to the bin folder and double click `darktable.exe`.

Have fun with this, report back your findings.
