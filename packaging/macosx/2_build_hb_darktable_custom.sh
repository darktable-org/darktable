#!/bin/bash
#
# Script to build and install darktable with custom configuration
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/
scriptDir=$(pwd)

# Set variables
buildDir="${scriptDir}/../../build"
installDir="${buildDir}/macosx"

homebrewHome=$(brew --prefix)

# Build and install darktable here
# ../../build.sh --install --build-type Release --prefix ${PWD}

# Check for previous attempt and clean
if [[ -d "$buildDir" ]]; then
    echo "Deleting directory $buildDir ... "
    rm -R "$buildDir"
fi

# Create directory
mkdir "$buildDir"
cd "$buildDir"

# Configure build
cmake .. \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0.1 \
    -DCMAKE_CXX_FLAGS=-stdlib=libc++ \
    -DCMAKE_OBJCXX_FLAGS=-stdlib=libc++ \
    -DBINARY_PACKAGE_BUILD=ON \
    -DRAWSPEED_ENABLE_LTO=ON \
    -DBUILD_CURVE_TOOLS=ON \
    -DBUILD_NOISE_TOOLS=ON \
    -DDONT_USE_INTERNAL_LUA=OFF \
    -DBUILD_SSE2_CODEPATHS=OFF \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DUSE_COLORD=OFF \
    -DUSE_KWALLET=OFF \
    -DBUILD_CMSTEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$installDir"

# Build using all available cores
make -j"$(sysctl -n hw.ncpu)"

# Install
make install
