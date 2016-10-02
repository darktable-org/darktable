#!/bin/sh

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
#   SRC_DIR - read-only directory with git checkout to compile
#   CC, CXX, CFLAGS, CXXFLAGS are not required, should make sense too

set -e

cd "$BUILD_DIR"
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo "$SRC_DIR"

# to get as much of the issues into the log as possible
cmake --build "$BUILD_DIR" -- -j3 || cmake --build "$BUILD_DIR" -- -j1 -k
