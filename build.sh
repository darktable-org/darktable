#!/bin/sh

DT_SRC_DIR=`dirname "$0"`
DT_SRC_DIR=`cd "$DT_SRC_DIR"; pwd`

cd $DT_SRC_DIR;

# ---------------------------------------------------------------------------
# Set default values to option vars
# ---------------------------------------------------------------------------

INSTALL_PREFIX=""
BUILD_TYPE=""
MAKE_TASKS=-1
BUILD_DIR="./build"

PRINT_HELP=0

OPT_FLICKR=-1
OPT_KWALLET=-1
OPT_GNOME_KEYRING=-1
OPT_OPENMP=-1
OPT_OPENCL=-1
OPT_UNITY=-1
OPT_TETHERING=-1
OPT_GEO=-1
OPT_LUA=-1
OPT_OPENEXR=-1
OPT_WEBP=-1

# ---------------------------------------------------------------------------
# Parse options
# ---------------------------------------------------------------------------

parse_feature()
{
	feature=$1
	value=$2
	
	case $feature in
	flickr)
		OPT_FLICKR=$value
		;;
	libsecret)
		OPT_LIBSECRET=$value
		;;
	kwallet)
		OPT_KWALLET=$value
		;;
	gnome-keyring)
		OPT_GNOME_KEYRING=$value
		;;
	openmp)
		OPT_OPENMP=$value
		;;
	opencl)
		OPT_OPENCL=$value
		;;
	unity)
		OPT_UNITY=$value
		;;
	tethering)
		OPT_TETHERING=$value
		;;
	lua)
		OPT_LUA=$value
		;;
	geo)
		OPT_GEO=$value
		;;
	openexr)
		OPT_OPENEXR=$value
		;;
	webp)
		OPT_WEBP=$value
		;;
	*)
		echo "warning: unknown feature '$feature'"
		;;
	esac
}

while [ $# -ge 1 ] ; do
	option="$1"
	case $option in
	--prefix)
		INSTALL_PREFIX="$2"
		shift
		;;
	--buildtype)
		BUILD_TYPE="$2"
		shift
		;;
	--builddir)
		BUILD_DIR="$2"
		shift
		;;
	-j|--jobs)
		MAKE_TASKS="$2"
		shift
		;;
	--enable-*)
		feature=${option#--enable-}
		parse_feature "$feature" 1
		;;
	--disable-*)
		feature=${option#--disable-}
		parse_feature "$feature" 0
		;;
	-h|--help)
		PRINT_HELP=1
		;;
	*)
		echo "warning: ignoring unknown option $option"
		;;
	esac
	shift
done

# ---------------------------------------------------------------------------
# Process user wishes
# ---------------------------------------------------------------------------

if [ $PRINT_HELP -ne 0 ] ; then
		cat <<EOF
build.sh [OPTIONS]

Options:
Installation:
   --prefix <string>     Install directory prefix (default: /opt/darktable)

Build:
   --builddir <string>   Building directory (default: $DT_SRC_DIR/build)
   --buildtype <string>  Build type (Release, Debug, default: Release)
-j --jobs <integer>      Number of tasks (default: number of CPUs)

Features:
All these options have a --disable-* equivalent. By default they are set
so that the cmake script autodetects features.
   --enable-flickr
   --enable-kwallet
   --enable-gnome-keyring
   --enable-openmp
   --enable-opencl
   --enable-unity
   --enable-tethering
   --enable-geo
   --enable-lua
   --enable-openexr
   --enable-webp

Extra:
-h --help                Print help message
EOF
	exit 1
fi

if [ "$INSTALL_PREFIX" =  "" ]; then
	INSTALL_PREFIX=/opt/darktable/
fi

if [ "$BUILD_TYPE" =  "" ]; then
	BUILD_TYPE=Release
fi

KERNELNAME=`uname -s`
# If no Make tasks given, try to be smart
if [ "$(($MAKE_TASKS < 1))" -eq 1 ]; then
	if [ "$KERNELNAME" = "SunOS" ]; then
		MAKE_TASKS=$( /usr/sbin/psrinfo |wc -l )
	else
		if [ -r /proc/cpuinfo ]; then
			MAKE_TASKS=$(grep -c "^processor" /proc/cpuinfo)
		elif [ -x /sbin/sysctl ]; then
			TMP_CORES=$(/sbin/sysctl -n hw.ncpu 2>/dev/null)
			if [ "$?" = "0" ]; then
				MAKE_TASKS=$TMP_CORES
			fi
		fi
	fi
fi

if [ "$KERNELNAME" = "SunOS" ]; then
	MAKE=gmake
	PATH=/usr/gnu/bin:$PATH ; export PATH
else
	MAKE=make
fi

# Being smart may fail :D
if [ "$(($MAKE_TASKS < 1))" -eq 1 ]; then
	MAKE_TASKS=1
fi

cmake_boolean_option()
{
	name=$1
	value=$2
	case $value in
	-1)
		# Do nothing
		;;
	0)
		CMAKE_MORE_OPTIONS="$CMAKE_MORE_OPTIONS -D${name}=Off"
		;;
	1)
		CMAKE_MORE_OPTIONS="$CMAKE_MORE_OPTIONS -D${name}=On"
		;;
	esac
}

CMAKE_MORE_OPTIONS=""
cmake_boolean_option USE_FLICKR $OPT_FLICKR
cmake_boolean_option USE_LIBSECRET $OPT_LIBSECRET
cmake_boolean_option USE_KWALLET $OPT_KWALLET
cmake_boolean_option USE_GNOME_KEYRING $OPT_GNOME_KEYRING
cmake_boolean_option USE_OPENMP $OPT_OPENMP
cmake_boolean_option USE_OPENCL $OPT_OPENCL
cmake_boolean_option USE_UNITY $OPT_UNITY
cmake_boolean_option USE_CAMERA_SUPPORT $OPT_TETHERING
cmake_boolean_option USE_GEO $OPT_GEO
cmake_boolean_option USE_LUA $OPT_LUA
cmake_boolean_option USE_OPENEXR $OPT_OPENEXR
cmake_boolean_option USE_WEBP $OPT_WEBP

# Some people might need this, but ignore if unset in environment
CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH:-}
CMAKE_MORE_OPTIONS="${CMAKE_MORE_OPTIONS} ${CMAKE_PREFIX_PATH}"

# ---------------------------------------------------------------------------
# Let's go
# ---------------------------------------------------------------------------

cat <<EOF
Darktable build script

Building directory:  $BUILD_DIR
Installation prefix: $INSTALL_PREFIX
Build type:          $BUILD_TYPE
Make program:        $MAKE
Make tasks:          $MAKE_TASKS


EOF

if [ ! -d "$BUILD_DIR" ]; then
	mkdir "$BUILD_DIR"
fi

cd "$BUILD_DIR"

cmake \
	-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
	-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
	${CMAKE_MORE_OPTIONS} \
	"$DT_SRC_DIR" \
&& $MAKE -j $MAKE_TASKS

if [ $? = 0 ]; then
	cat <<EOF
Darktable finished building, to actually install darktable you need to type:
# cd "$BUILD_DIR"; sudo make install
EOF
else
   exit 1
fi
