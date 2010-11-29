#!/bin/sh

INSTALL_PREFIX=$1
if [ "$INSTALL_PREFIX" =  "" ]; then
	INSTALL_PREFIX=/usr/
fi

BUILD_TYPE=$2
if [ "$BUILD_TYPE" =  "" ]; then
        BUILD_TYPE=Release
fi

echo Installing to $INSTALL_PREFIX for $BUILD_TYPE

if [ ! -d build/ ]; then
	mkdir build/
fi

cd build/

MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)

# Should probably be fixed in cmake
RSVG_DIR=$(find /usr/include/ | grep "librsvg/rsvg.h" | sed "s,librsvg/rsvg.h$,,")
if [ "$RSVG_DIR" != "" ]; then
	RSVG_DIR="-DRSVG2_INCLUDE_DIR=$RSVG_DIR"
fi

cmake -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} $RSVG_DIR .. && make && sudo make install
