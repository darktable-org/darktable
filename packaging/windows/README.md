#Developing darktable for Windows
This document serves as a guide for Windows users interested in contributing to darktable. 

##A Brief History Lesson
The case for a Windows darktable build complete with installers and automatic updates, etc. has long been contentious. This is due in part to concerns about the throngs of unsophisticated Windows users and lack of adequate Windows package managers. However, the largest roadblock was always people willing to develop/support a Windows build.

As mentioned in [darktable for Windows](https://www.darktable.org/2017/08/darktable-for-windows/), at some point in 2017, Peter Budai decided to help create and patch a official build snapshot for 64-bit Windows.

Because of his (and others') dev work, we have this helpful guide on building ***your very own*** darktable for Windows. This should facilitate development for darktable on Windows platforms.

If you have a strong interest in more details, take a look at:
- [That other OS](https://www.darktable.org/2011/07/that-other-os/)
- [Why don't you provide a Windows build?](https://www.darktable.org/2015/07/why-dont-you-provide-a-windows-build/)
- [darktable for Windows](https://www.darktable.org/2017/08/darktable-for-windows/)
- [the general FAQs](https://www.darktable.org/about/faq/)
 
##Getting Started
There are plenty of posts on hacking with darktable, but we'll try to limit assumptions here about Windows dev familiarity. 
 
The first thing we're going to attempt is to build 64-bit darktable from source. You have a multitude of options, but the instructions follow two methods:
1. Native compilation using MSYS
2. Cross compilation on Linux
 
##Native Compilation using MSYS
###Install Dependencies
1. Install [MSYS2](https://msys2.github.io/).
2. Update the base MSYS2 system until no further updates are available using:
    ```
    pacman -Syu
    ```
3. From MSYS2 terminal, install x64 developer tools and x86_64 toolchain and git from the MSYS2 prompt:
    ```
    pacman -S base-devel
    pacman -S mingw-w64-x86_64-{toolchain,cmake,nsis}
    pacman -S git
    ```
4. Install required libraries and dependencies:

    **Note:** you may need to restart MINGW64 or MSYS terminal after installing the required libraries and dependencies to ensure environment variables are properly set (i.e. `CAMLIBS` and `IOLIBS`)
    
    ```
    pacman -S mingw-w64-x86_64-{exiv2,lcms2,lensfun,dbus-glib,openexr,sqlite3,libxslt,libsoup,libwebp,libsecret,lua,graphicsmagick,openjpeg2,gtk3,pugixml,libexif,osm-gps-map,libgphoto2,flickcurl,drmingw,gettext,python3,iso-codes,python3-jsonschema,python3-setuptools}
    ```
5. Downgrade lensfun (optional)

    MSYS2's prepackaged version of lensfun (`0.3.95-1`) does not work with darktable. If you get errors while building, you should manually install an older version (`0.3.2-4`) of lensfun:
    1. download [lensfun 0.3.2-4](http://repo.msys2.org/mingw/x86_64/mingw-w64-x86_64-lensfun-0.3.2-4-any.pkg.tar.xz), save it to somewhere accessible from MSYS terminal (i.e. `~/lensfun_downgrade`)
    2. navigate to wherever you saved the download and run the following in the MSYS terminal
        ```
        pacman -U mingw-w64-x86_64-lensfun-0.3.2-4-any.pkg.tar.xz
        ```
    3. prevent pacman from upgrading lensfun until MSYS is packaged with a working version by editing the file `/etc/pacman.conf` and adding `mingw-w64-x86_64-lensfun` to the `IgnorePkg` list.
        ```
       #Pacman won’t upgrade packages listed in IgnorePkg and members of IgnoreGroup         
        IgnorePkg = mingw-w64-x86_64-lensfun
        ```
    4. Make sure that the MSYS python 3.7 environment has lensfun installed by copying lensfun data from python 3.6 folder - it should be compatible without any modification:
        ```
            cd /mingw64/lib
            cp -r python3.6/site-packages/lensfun python3.7/site-packages
            cp python3.6/site-packages/lensfun-0.3.2-py3.6.egg-info python3.7/site-packages
        ```
    5. Update your lensfun database
        ```
        lensfun-update-data
        ```
6. Install USB Drivers for your Camera (optional)
    
    If you want to use tethering, some cameras will require a driver update. You can find the appropriate drivers via [zadig](http://zadig.akeo.ie)
    
    When you run it, replace current Windows camera driver with WinUSB driver.
    
    **Note:** please evaluate the suitability of the replacement drivers for your systems. We make no assertions or guarantees that the zadig drivers will work.
    
7. Install User Manual dependencies (optional)
    
    1. Some of the dependencies are available via pacman (woohoo)
        ```
        pacman -S po4a
        pacman -S gnome-doc-utils
        pacman -S mingw-w64-x86_64-{imagemagick,docbook-xsl}
        pacman -S libxml2-python
        ```
    
    2. You'll also need a Java Runtime Environment (JRE) installed (see [Java downloads](https://www.java.com/en/download/))
    
    3. Manually install SAXON
        - Download from [Source Forge](https://sourceforge.net/projects/saxon/files/Saxon-HE/) 
        - Copy the contents into `~/java/saxon/`
   
    4. Manually install DocBook Extension to Saxon
        - Download from [Source Forge](https://sourceforge.net/projects/docbook/files/docbook-xsl-saxon/1.00/docbook-xsl-saxon-1.00.zip) into the respective folder.
        - Copy the contents of folder docbook-xsl-saxon-1.00 into `~/java/docbook-xsl-saxon`
   
    5. Manually install FOP
        - Download the latest binary zip file from [FOP](http://www.apache.org/dyn/closer.cgi/xmlgraphics/fop) 
        - Open the file
        - Copy files from `fop<x.x>/fop` into `~/java/fop`
        - Copy `fop.xconf` to `/etc/fop.xml`
        - change one line in fop shell script in fop folder from:
            
            `if [ "$OS" = "Windows_NT" ] ; then`
            
            to:
            
            `if [ "$OS" = "Windows_NT" ] && [ "$MSYSTEM" != "MINGW64" ] ; then`
   
    6. Modify your bash_profile file in your HOME directory and add the following lines:
        ```
            # Added as per http://wiki.gimp.org/wiki/Hacking:Building/Windows
            export PREFIX="/mingw64"
            export LD_LIBRARY_PATH="$PREFIX/lib:$LD_LIBRARY_PATH"
            
            # This can be different depending on your current JAVA version
            JAVA_HOME="/C/Program Files (x86)/Java/jre1.8.0_101"
            export PATH=$PATH:$JAVA_HOME/bin
            
            # Update your CLASSPATH
            CLASSPATH=$CLASSPATH:~/java/saxon/saxon9he.jar:~/java/docbook-xsl-saxon/saxon65.jar
            export CLASSPATH
            export SAXON_INSTALL_DIR=~
            
            # Update your PATH for fop
            PATH=$PATH:~/java/fop
        ```
    7. Refresh your bash_profile
       ```
       . .bash_profile
       ```

###Checkout Repository 
We'll clone the main git module into our local MSYS environment under `~/darktable`
```
cd ~
git clone git://github.com/darktable-org/darktable.git
cd darktable
git submodule init
git submodule update
```   

###Build and Install
Install `darktable.exe` into `/opt/darktable`
```
mkdir build
cd build
cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_USERMANUAL=OFF -DCMAKE_INSTALL_PREFIX=/opt/darktable ../.
cmake --build .
cmake --build . --target install
```
####Build User Manual
`-DBUILD_USERMANUAL` is an optional parameter which defaults to OFF... if you set the flag to ON, you'll build the user manual as well as the application.

To build just the user manual, run `cmake --build . --target usermanual`

####Build Installer 
For building the installer image, which will  create darktable-<VERSION>.exe installer in current build directory, use:
    `cmake --build . --target package`
This creates an installer which is optimized for the local host and is not expected to run on other hardware or software

For a generic installer, run
```
cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -DBINARY_PACKAGE_BUILD=ON ../.
```
##Cross compile on Linux
In order to build darktable for Windows there is a chance to cross compile it
on Linux. The result is currently not particularly usable, but still, if you
are curious you could give it a try.

The tests have been done cross compiling on openSUSE. Since it's using a
virtualized installation only used for this stuff you can safely do everything
as root. It's also enough to install a server setup without X.

1. Grab the [64-bit openSUSE install ISO](http://software.opensuse.org/122/en)
2. Install in Virtualbox
3. Prepare the system
    ```
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

4. Fetch darktable source
   ```
   git clone https://github.com/darktable-org/darktable.git
   ```
5. cd into the freshly cloned source tree and edit cmake/toolchain_mingw64.cmake if needed,
it should be ok if using the virtualized openSUSE environment as described here.
6. Build darktable using this script (save it as build-win.sh). 

    Make sure to edit it to suit your environment, too, if needed. Again, if following these directions it should be working out of the box. Run the script from the root directory of the git clone (i.e., put it next to build.sh):

    ```
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
    echo "Darktable finished building, to actually install darktable you need to type:"
    echo "# cd ${BUILD_DIR}; sudo make install"
    fi
    ```

7. Last but not least ```cd build-win; make install```
8. Now you have a Windows version of darktable in /opt/darktable-win/, which
should be suited for 64 bit Windows installations. 

    Before you can run it do ```cp /usr/x86_64-w64-mingw32/sys-root/mingw/bin/*.dll /opt/darktable-win/bin/```
9. Done. Copy /opt/darktable-win/ to a Windows box, go to the bin folder and
double click darktable.exe.

###Known Issues
- The current working directory has to be the bin folder when running the .exe.
Double clicking in Explorer does that for you.
- GTK engine is not copied over/found currently, probably just a matter of putting
the right DLLs into the correct places and/or setting some environment variables.
We also want "wimp" in gtkrc instead of clearlooks. You can find it in
/usr/x86_64-w64-mingw32/sys-root/mingw/lib/gtk-2.0/2.10.0/engines/
- A stupid error message about missing dbus symbols in a DLL. This is fixed in libglib
already but the version used by us doesn't have it. Using newer openSUSE releases
might fix this but I couldn't find lensfun and libexiv2 for them. Some day we might
add our own build service to OBS so that will go away eventually.
- Moving darktable's main window freezes the application. No idea what's going on
there. Since I have seen some Windows installations moving the window on startup
and subsequently freezing automatically it might be impossible to run darktable
for you.
- Building with openCL isn't completely done yet (links used for caching). This
might change soonish, no idea if it would work in the end though.
- Importing directories didn't work in my tests (freezes), single images imported
fine though, I could edit them and export them.
- Lua support doesn't compile right now since it uses select() on a file which is not
supported on Windows.

Have fun with this, report back your findings and understand that we don't intend
to officially support a Windows version, let alone provide binaries for download.