#!/bin/sh

DT_SRC_DIR=`dirname "$0"`
DT_SRC_DIR=`cd "$DT_SRC_DIR"; pwd`

cd $DT_SRC_DIR;

INSTALL_PREFIX=$1
if [ "$INSTALL_PREFIX" =  "" ]; then
	INSTALL_PREFIX=/opt/darktable/
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
if [ `uname -s` = "SunOS" ]; then
	MAKE_TASKS=$( /usr/sbin/psrinfo |wc -l )
	MAKE=gmake
	PATH=/usr/gnu/bin:$PATH ; export PATH
else
	if [ -r /proc/cpuinfo ]; then
		MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)
	elif [ -x /sbin/sysctl ]; then
		TMP_CORES=$(/sbin/sysctl -n hw.ncpu 2>/dev/null)
		if [ "$?" = "0" ]; then
			MAKE_TASKS=$TMP_CORES
		fi
	fi
	MAKE=make
fi

if [ "$(($MAKE_TASKS < 1))" -eq 1 ]; then
	MAKE_TASKS=1
fi

cmake -DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
	-DINSTALL_IOP_LEGACY=Off .. && $MAKE -j $MAKE_TASKS 

if [ $? = 0 ]; then
	echo "Darktable finished building, to actually install darktable you need to type:"
	echo "# cd build; sudo make install"
fi
