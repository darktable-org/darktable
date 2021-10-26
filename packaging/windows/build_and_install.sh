#!/bin/bash

# a convenient build script which follows the post dependency installation steps in ./BUILD.md

mkdir build
cd build || exit

cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ../.

cmake --build .
cmake --build . --target install