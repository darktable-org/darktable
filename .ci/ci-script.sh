#!/bin/bash

#    This file is part of darktable.
#    Copyright (C) 2016-2024 darktable developers.
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

# This script is supposed to be run by Travis CI or GitHub workflow.
# It expects a few env variables to be set:
#   BUILD_DIR - the working directory where the program will be built
#   INSTALL_DIR - the installation prefix
#   SRC_DIR - directory with the source code to be compiled
#   CC, CXX, CFLAGS, CXXFLAGS are optional, but make sense for build
#   TARGET - either build or skiptest
#   ECO - some other flags for cmake

set -ex

if [ "$GENERATOR" = "Ninja" ];
then
  VERBOSE="-v"
  KEEPGOING="-k0"
  JOBS=""
fi;

if [ "$GENERATOR" = "Unix Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
  JOBS="-j2"
fi;

if [ "$GENERATOR" = "MSYS Makefiles" ];
then
  VERBOSE="VERBOSE=1";
  KEEPGOING="-k"
  JOBS="-j2"
fi;

target_build()
{
  cmake --build "$BUILD_DIR" -- $JOBS "$VERBOSE" "$KEEPGOING"

  ctest --output-on-failure || ctest --rerun-failed -V -VV

  cmake --build "$BUILD_DIR" --target install -- $JOBS "$VERBOSE" "$KEEPGOING"
}

target_notest()
{
  cmake --build "$BUILD_DIR" -- $JOBS "$VERBOSE" "$KEEPGOING"

  cmake --build "$BUILD_DIR" --target install -- $JOBS "$VERBOSE" "$KEEPGOING"
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
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DVALIDATE_APPDATA_FILE=ON \
      -DBUILD_TESTING=ON \
      -DTESTBUILD_OPENCL_PROGRAMS=ON \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_build
    ;;
  "skiptest")
    cmake -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
      -G"$GENERATOR" \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      $ECO "$SRC_DIR" || (cat "$BUILD_DIR"/CMakeFiles/CMakeOutput.log; cat "$BUILD_DIR"/CMakeFiles/CMakeError.log)
    target_notest
    ;;
  *)
    exit 1
    ;;
esac

diskspace
