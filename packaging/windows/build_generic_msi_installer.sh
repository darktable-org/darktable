#!/bin/bash

# a convenient build script which follows the post dependency installation steps in ./BUILD.md

mkdir build_msi_gen
cd build_msi_gen || exit

cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -DBINARY_PACKAGE_BUILD=ON ../.

cmake --build .
cmake --build . --target package