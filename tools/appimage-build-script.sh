#!/usr/bin/env bash

# This script builds darktable and then packages it as an AppImage.
# You are expected to call it from the darktable sources root folder.

# This script contains the launch of linuxdeply, which itself is an AppImage.
# By default, FUSE, which is required for the standard technology of
# launching programs in AppImage format, does not work in Docker containers.
# So if this script is running in a Docker container, the environment
# variable APPIMAGE_EXTRACT_AND_RUN=1 must be set before calling it.

# desktop-file-validate is an optional dependency for darktable build,
# but is required by linuxdeploy.
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
# This allows you to conveniently manage the enabling/disabling of various
# features. For example, you can easily build darktable with support for
# ImageMagick instead of GraphicsMagick by running this script with the
# parameters `--disable-graphicsmagick --enable-imagemagick`
./build.sh --build-dir ./build/ --prefix /usr --build-type Release $@ --install -- "-DBINARY_PACKAGE_BUILD=1 -DBUILD_CURVE_TOOLS=ON -DBUILD_NOISE_TOOLS=ON -DDONT_USE_INTERNAL_LUA=Off -DCMAKE_VERBOSE_MAKEFILE:BOOL=ON"

# Sanitize path to executable in the .desktop (it will be handled by AppImage).
# In reality, most .desktop files do not include the full path to the program
# in the Exec field (relying on the OS's path lookup functionality).
# When we'll do the same, this hack can be removed.
cd build
sed -i 's/\/usr\/bin\///' ../AppDir/usr/share/applications/org.darktable.darktable.desktop

# The caller of the script should run `sudo lensfun-update-data` before making
# AppImage, for the nightly builds we did this in the GitHub Action.
# Ideally, we should include the latest lens data at the time of the build.
# So we assume that before calling this script, there was a call to
# lensfun-update-data, which downloaded the updates to /var/lib/lensfun-updates.
# It might be worth adding more complex logic here to find where the latest
# data is located on the build host, but for now we'll rely on the user of this
# script to read this comment and act accordingly.
mkdir -p ../AppDir/usr/share/lensfun
cp -a /var/lib/lensfun-updates/* ../AppDir/usr/share/lensfun

# Since linuxdeploy is itself an AppImage, we don't rely on it being installed
# on the build system, but download it every time we run this script. If that
# doesn't suit you (for example, you want to build an AppImage without an
# Internet connection), you can edit this script accordingly, and call
# linuxdeploy and its plugin from where you put them.
wget -c --no-verbose "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
wget -c --no-verbose "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"

chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION=3
export VERSION=$(sh ../tools/get_git_version_string.sh)
export DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1

# Instruct LDAI (LinuxDeploy AppImage plugin) to embed AppImage update information
if [ "$DARKTABLE_APPIMAGE_UPDATE" == "release" ]; then
  export LDAI_UPDATE_INFORMATION="gh-releases-zsync|darktable-org|darktable|latest|Darktable-*-x86_64.AppImage.zsync"
elif [ "$DARKTABLE_APPIMAGE_UPDATE" != "no" ]; then
  export LDAI_UPDATE_INFORMATION="gh-releases-zsync|darktable-org|darktable|nightly|Darktable-*-x86_64.AppImage.zsync"
fi

# '--deploy-deps-only' are needed to tell linuxdeploy where to collect deps of
# modules that are loaded via dlopen (therefore cannot be found automatically)
./linuxdeploy-x86_64.AppImage \
  --appdir ../AppDir \
  --plugin gtk \
  --deploy-deps-only ../AppDir/usr/lib/x86_64-linux-gnu/darktable/plugins \
  --deploy-deps-only ../AppDir/usr/lib/x86_64-linux-gnu/darktable/plugins/imageio/format \
  --deploy-deps-only ../AppDir/usr/lib/x86_64-linux-gnu/darktable/plugins/imageio/storage \
  --deploy-deps-only ../AppDir/usr/lib/x86_64-linux-gnu/darktable/plugins/lighttable \
  --deploy-deps-only ../AppDir/usr/lib/x86_64-linux-gnu/darktable/views \
  --custom-apprun ../packaging/AppImage/AppRun \
  --output appimage
