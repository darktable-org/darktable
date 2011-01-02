#!/bin/sh

DT_SRC_DIR=`dirname "$0"`
DT_SRC_DIR=`cd "$DT_SRC_DIR"; pwd`

cd $DT_SRC_DIR;

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

MAKE_TASKS=1
if [ -r /proc/cpuinfo ]; then
	MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)
elif [ -x /sbin/sysctl ]; then
	TMP_CORES=$(/sbin/sysctl hw.ncpu 2>/dev/null)
	if [ "$?" = "0" ]; then
		MAKE_TASKS=$(echo $TMP_CORES | sed "s/.*\s//")
	fi
fi

cmake -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} .. && make && sudo make install
