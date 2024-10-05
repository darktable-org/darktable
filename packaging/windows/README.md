The steps to build darktable Windows executable and make installer (Windows 8.1 will need to have
[UCRT installed](https://support.microsoft.com/en-us/topic/update-for-universal-c-runtime-in-windows-c0514201-7fe6-95a3-b0a5-287930f3560c))
are as follows:

* Install MSYS2 (instructions and prerequisites can be found on the official website: https://www.msys2.org)

* Start the MSYS terminal and update the base system until no further updates are available by repeating:
    ```bash
    pacman -Syu
    ```

* From the MSYS terminal, install the toolchain (assuming x64), developer tools and git:
    ```bash
    pacman -S --needed base-devel git intltool po4a
    pacman -S --needed mingw-w64-ucrt-x86_64-{cc,cmake,gcc-libs,ninja,omp}
    ```

* Install required and recommended dependencies for darktable:
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-{libxslt,python-jsonschema,curl,drmingw,exiv2,gettext,gmic,graphicsmagick,gtk3,icu,imath,iso-codes,lcms2,lensfun,libavif,libgphoto2,libheif,libjpeg-turbo,libjxl,libpng,libraw,librsvg,libsecret,libtiff,libwebp,libxml2,lua,openexr,openjpeg2,osm-gps-map,portmidi,pugixml,SDL2,sqlite3,webp-pixbuf-loader,zlib}
    ```

* Install the optional tool for building an installer image (currently x64 only):
    ```bash
    pacman -S --needed mingw-w64-ucrt-x86_64-nsis
    ```

* Switch to the UCRT64 terminal and update your Lensfun database:
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
            export CAMLIBS="$MINGW_PREFIX/lib/libgphoto2/2.5.31/"
            export IOLIBS="$MINGW_PREFIX/lib/libgphoto2_port/0.12.2/"
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
    or by performing the steps manually:
    ```bash
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -S . -B build
    cmake --build build
    cmake --install build
    ```
    After this darktable will be installed in `/opt/darktable `directory and can be started by typing `/opt/darktable/bin/darktable.exe` from the UCRT64 terminal.

    *NOTE: If you are using the Lua scripts, build the installer and install darktable.
    The Lua scripts check the operating system and see Windows and expect a Windows shell when executing system commands.
    Running darktable from the UCRT64 terminal gives a bash shell and therefore the commands will not work.*

* For building the installer image (currently x64 only), which will create darktable-<VERSION>-win64.exe installer in the current build directory, use:
    ```bash
    cmake --build build --target package
    ```

    *NOTE: The package created will be optimized for the machine on which it has been built, but it could not run on other PCs with different hardware or different Windows version. If you want to create a "generic" package, change the first cmake command line as follows:*
    ```bash
    cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -DBINARY_PACKAGE_BUILD=ON -S . -B build
    ```

If you are in a hurry you can now run darktable by executing the `darktable.exe` found in the `build/bin` folder, install in `/opt/darktable` as described earlier, or create an installer image.

If you like experimenting you could also install `mingw-w64-ucrt-x86_64-{clang,llvm-openmp}` and use clang/clang++ instead of gcc/g++ by setting the `CC=clang` and `CXX=clang++` variables. Alternatively, you can use the CLANG64 environment instead of UCRT64 and try building darktable with its default toolchain (note that the prefix for installation of all the packages above then becomes `mingw-w64-clang-x86_64-`).

Windows on Arm (WoA) users should use the `mingw-w64-clang-aarch64-` prefix when installing packages above and build within the CLANGARM64 environment instead (note that it is not currently possible to build the installer image because of missing `nsis`).

Have fun with this, report back your findings.
