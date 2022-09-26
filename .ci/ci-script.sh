#!/bin/bash

#    This file is part of darktable.
#    copyright (c) 2016 Roman Lebedev.
#
#    darktable is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    darktable is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

# it is supposed to be run by travis-ci
# expects a few env variables to be set:
#   BUILD_DIR - the working directory, where to build
#   INSTALL_DIR - the installation prefix.
#   SRC_DIR - read-only directory with git checkout to compile
#   CC, CXX, CFLAGS, CXXFLAGS are not required, should make sense too
#   TARGET - either build, skiptest, nofeatures or usermanual
#   ECO - some other flags for cmake

set -ex

VERBOSE="-v"
KEEPGOING="-k0"

if [ "$GENERATOR" = "Unix Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
fi;

if [ "$GENERATOR" = "MSYS Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
fi;

if [ -z "${MAKEFLAGS+x}" ];
then
  MAKEFLAGS="-j2 $VERBOSE"
fi

target_build()
{
  # to get as much of the issues into the log as possible
  cmake --build "$BUILD_DIR" -- $MAKEFLAGS || cmake --build "$BUILD_DIR" -- -j1 "$VERBOSE" "$KEEPGOING"

  ctest --output-on-failure || ctest --rerun-failed -V -VV

  # and now check that it installs where told and only there.
  cmake --build "$BUILD_DIR" --target install -- $MAKEFLAGS || cmake --build "$BUILD_DIR" --target install -- -j1 "$VERBOSE" "$KEEPGOING"
}

target_notest()
{
  # to get as much of the issues into the log as possible
  cmake --build "$BUILD_DIR" -- $MAKEFLAGS || cmake --build "$BUILD_DIR" -- -j1 "$VERBOSE" "$KEEPGOING"

  # and now check that it installs where told and only there.
  cmake --build "$BUILD_DIR" --target install -- $MAKEFLAGS || cmake --build "$BUILD_DIR" --target install -- -j1 "$VERBOSE" "$KEEPGOING"
}

target_usermanual()
{
  cmake --build "$BUILD_DIR" -- -j1 -v -k0 validate_usermanual_xml

  # # to get as much of the issues into the log as possible
  # cmake --build "$BUILD_DIR" -- $PARALLEL -v darktable-usermanual || cmake --build "$BUILD_DIR" -- -j1 -v -k0 darktable-usermanual
  # test -r doc/usermanual/darktable-usermanual.pdf
  # ls -lah doc/usermanual/darktable-usermanual.pdf
}

diskspace()
{
  df
  du -hcs "$SRC_DIR"
  du -hcs "$BUILD_DIR"
  du -hcs "$INSTALL_PREFIX"
}

diskspace

cd "$BUILD_DIR"

case "$TARGET" in
  "build")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -G"$GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" "$ECO" -DVALIDATE_APPDATA_FILE=ON -DBUILD_TESTING=ON -DTESTBUILD_OPENCL_PROGRAMS=ON "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_build
    ;;
  "skiptest")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" -G"$GENERATOR" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  "nofeatures")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DUSE_OPENMP=OFF \
      -DUSE_OPENCL=OFF \
      -DUSE_LUA=OFF \
      -DUSE_GAME=OFF \
      -DUSE_CAMERA_SUPPORT=OFF \
      -DUSE_NLS=OFF \
      -DUSE_GRAPHICSMAGICK=OFF \
      -DUSE_OPENJPEG=OFF \
      -DUSE_JXL=OFF \
      -DUSE_WEBP=OFF \
      -DUSE_AVIF=OFF \
      -DUSE_HEIF=OFF \
      -DUSE_XCF=OFF \
      -DBUILD_CMSTEST=OFF \
      -DUSE_OPENEXR=OFF \
      -DBUILD_PRINT=OFF \
      -DBUILD_RS_IDENTIFY=OFF \
      -DUSE_LENSFUN=OFF \
      -DUSE_GMIC=OFF \
      -DUSE_LIBSECRET=OFF \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  "nofeatures_nosse")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DUSE_OPENMP=OFF \
      -DUSE_OPENCL=OFF \
      -DUSE_LUA=OFF \
      -DUSE_GAME=OFF \
      -DUSE_CAMERA_SUPPORT=OFF \
      -DUSE_NLS=OFF \
      -DUSE_GRAPHICSMAGICK=OFF \
      -DUSE_OPENJPEG=OFF \
      -DUSE_JXL=OFF \
      -DUSE_WEBP=OFF \
      -DUSE_AVIF=OFF \
      -DUSE_HEIF=OFF \
      -DUSE_XCF=OFF \
      -DBUILD_CMSTEST=OFF \
      -DUSE_OPENEXR=OFF \
      -DBUILD_PRINT=OFF \
      -DBUILD_RS_IDENTIFY=OFF \
      -DUSE_LENSFUN=OFF \
      -DUSE_GMIC=OFF \
      -DUSE_LIBSECRET=OFF \
      -DBUILD_SSE2_CODEPATHS=OFF \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  *)
    exit 1
    ;;
esac

diskspace
