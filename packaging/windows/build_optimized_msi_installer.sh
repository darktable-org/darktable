#!/bin/bash

# a convenient build script which follows the post dependency installation steps in ./BUILD.md

mkdir build_msi_opt
cd build_msi_opt || exit

cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ../.

cmake --build .
cmake --build . --target package