#!/usr/bin/env bash
# builds PhotographyLAB and packages it as a single runnable AppImage.
# run from the darktable source root; the AppImage lands in build-plab/
set -euo pipefail

ARCH=$(uname -m)
SRC=$(pwd)
BUILD=$SRC/build-plab
APPDIR=$BUILD/AppDir

# skip the build when SKIP_BUILD=1, to repackage an existing build-plab
if [ "${SKIP_BUILD:-0}" != 1 ]; then
  rm -rf "$BUILD"
  # BINARY_PACKAGE_BUILD drops -march=native, so the result runs on any x86-64
  cmake -S . -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DBINARY_PACKAGE_BUILD=1 -DUSE_PHOTOGRAPHYLAB=ON \
    -DUSE_LUA=OFF -DUSE_MAP=OFF -DUSE_GMIC=OFF -DUSE_AI=OFF -DUSE_MCP=OFF \
    -DBUILD_TESTING=OFF
  cmake --build "$BUILD"
fi
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD"

install -Dm644 packaging/photographylab/photographylab.desktop \
  "$APPDIR/usr/share/applications/photographylab.desktop"
install -Dm644 data/pixmaps/scalable/darktable.svg \
  "$APPDIR/usr/share/icons/hicolor/scalable/apps/photographylab.svg"

# newest lens profiles, when the build host fetched them (lensfun-update-data)
if [ -d /var/lib/lensfun-updates ]; then
  mkdir -p "$APPDIR/usr/share/lensfun"
  cp -a /var/lib/lensfun-updates/* "$APPDIR/usr/share/lensfun"
fi

cd "$BUILD"
wget -c --no-verbose "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage"
wget -c --no-verbose "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage"
chmod +x linuxdeploy-*.AppImage

export QMAKE=${QMAKE:-$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)}
export QML_SOURCES_PATHS=$SRC/src/plab/qml
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
export EXTRA_QT_MODULES="waylandclient"
export DISABLE_COPYRIGHT_FILES_DEPLOYMENT=1
export LINUXDEPLOY_OUTPUT_VERSION=$(git -C "$SRC" rev-parse --short HEAD)
export OUTPUT=PhotographyLAB-$LINUXDEPLOY_OUTPUT_VERSION-$ARCH.AppImage

PLUGINS=$APPDIR/usr/lib/$ARCH-linux-gnu/darktable
[ -d "$PLUGINS" ] || PLUGINS=$APPDIR/usr/lib/darktable

# darktable's modules are dlopen()ed, so their dependencies are listed by hand
./linuxdeploy-$ARCH.AppImage \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/photographylab" \
  --desktop-file "$APPDIR/usr/share/applications/photographylab.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/scalable/apps/photographylab.svg" \
  --deploy-deps-only "$PLUGINS/plugins" \
  --deploy-deps-only "$PLUGINS/plugins/imageio/format" \
  --deploy-deps-only "$PLUGINS/plugins/imageio/storage" \
  --plugin qt \
  --output appimage
