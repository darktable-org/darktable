#!/usr/bin/env bash

# This script builds darktable and then package it as an AppImage.
# You are expected to call this script from the darktable root folder.

# desktop-file-validate is an optional dependency for darktable build, but is required by linuxdeploy.
if ! [ -x "$(command -v desktop-file-validate)" ]; then
  echo 'Error: desktop-file-validate is not installed.' >&2
  exit 1
fi

# Ensure clean builds
rm -rf {build,AppDir}
mkdir {build,AppDir}

# For AppImage we have to install app in /usr subfolder of the AppDir
export DESTDIR=../AppDir
# The CLI parameters of this script will be passed verbatim to build.sh.
# This allows you to conveniently manage the enabling/disabling of various features.
# For example, you can easily build darktable with support for ImageMagick instead of GraphicsMagick
# by running this script with the parameters `--disable-graphicsmagick --enable-imagemagick`
./build.sh --build-dir ./build/ --prefix /usr --build-type Release $@ --install -- "-DBINARY_PACKAGE_BUILD=1 -DDONT_USE_INTERNAL_LUA=Off -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON"

# Sanitize path to executable in org.darktable.darktable.desktop (it will be handled by AppImage).
# In fact, most desktop files do not include the full path to the program in the Exec field
# (relying on the OS's path lookup functionality). When we'll do the same, this hack can be removed.
cd build
sed -i 's/\/usr\/bin\///' ../AppDir/usr/share/applications/org.darktable.darktable.desktop

# Since linuxdeploy is itself an AppImage, we don't rely on it being installed on the build system, but download it every time
# we run this script. If that doesn't suit you (for example, you want to build an AppImage without an Internet connection),
# you can edit this script accordingly, and call linuxdeploy and its plugin from where you put them.
wget -c --no-verbose "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -c --no-verbose "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"

chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION=3
export VERSION=$(sh ../tools/get_git_version_string.sh)
export DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1

./linuxdeploy-x86_64.AppImage --appdir ../AppDir --plugin gtk --output appimage
