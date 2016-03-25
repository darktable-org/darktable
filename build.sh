#!/bin/sh

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR"; pwd)

cd $DT_SRC_DIR;

# ---------------------------------------------------------------------------
# Set default values to option vars
# ---------------------------------------------------------------------------

INSTALL_PREFIX=""
BUILD_TYPE=""
MAKE_TASKS=-1
BUILD_DIR_DEFAULT="$DT_SRC_DIR/build"
BUILD_DIR="$BUILD_DIR_DEFAULT"
BUILD_GENERATOR="Unix Makefiles"
ADDRESS_SANITIZER=0

PRINT_HELP=0

OPT_FLICKR=-1
OPT_LIBSECRET=-1
OPT_KWALLET=-1
OPT_OPENMP=-1
OPT_OPENCL=-1
OPT_UNITY=-1
OPT_TETHERING=-1
OPT_LUA=-1
OPT_GEO=-1
OPT_OPENEXR=-1
OPT_WEBP=-1

# ---------------------------------------------------------------------------
# Parsing functions
# ---------------------------------------------------------------------------

parse_feature()
{
	feature=$1
	value=$2

	case $feature in
	flickr|libsecret|kwallet|openmp|opencl|unity|tethering|lua|geo|openexr|webp)
		eval "OPT_$(printf $feature|tr a-z A-Z)"=$value
		;;
	*)
		echo "warning: unknown feature '$feature'"
		;;
	esac
}

parse_args()
{
	while [ "$#" -ge 1 ] ; do
		option="$1"
		case $option in
		--prefix)
			INSTALL_PREFIX="$2"
			shift
			;;
		--build-type)
			BUILD_TYPE="$2"
			shift
			;;
		--build-dir)
			BUILD_DIR="$2"
			shift
			;;
		--build-generator)
			BUILD_GENERATOR="$2"
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
		--asan)
			ADDRESS_SANITIZER=1
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
}

# ---------------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------------

print_help()
{
	cat <<EOF
$(basename $0) [OPTIONS]

Options:
Installation:
   --prefix         <string>  Install directory prefix (default: /opt/darktable)

Build:
   --build-dir      <string>  Building directory
                              (default: $BUILD_DIR_DEFAULT)
   --build-type     <string>  Build type (Release, Debug, RelWithDebInfo)
                              (default: RelWithDebInfo)
   --build-generator <string> Build tool (default: Unix Makefiles)
-j --jobs <integer>           Number of tasks (default: number of CPUs)

Features:
By default cmake will enabel the features it autodetects on the build machine.
Specifying the option on the command line forces the feature on or off.
All these options have a --disable-* equivalent. 
   --enable-flickr
   --enable-libsecret
   --enable-kwallet
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

}

# ---------------------------------------------------------------------------
# Let's process the user's wishes
# ---------------------------------------------------------------------------

parse_args "$@"

if [ $PRINT_HELP -ne 0 ] ; then
	print_help
	exit 1
fi

if [ "$INSTALL_PREFIX" =  "" ]; then
	INSTALL_PREFIX=/opt/darktable/
fi

if [ "$BUILD_TYPE" =  "" ]; then
	BUILD_TYPE="RelWithDebInfo"
fi

KERNELNAME=$(uname -s)
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
Build generator:     $BUILD_GENERATOR
Build tasks:         $MAKE_TASKS


EOF

if [ ! -d "$BUILD_DIR" ]; then
	mkdir "$BUILD_DIR"
fi

PREPEND=""
if [ $ADDRESS_SANITIZER -ne 0 ] ; then
	PREPEND="CFLAGS=\"-fsanitize=address -fno-omit-frame-pointer\""
	PREPEND="$PREPEND CXXFLAGS=\"-fsanitize=address -fno-omit-frame-pointer\""
	PREPEND="$PREPEND LDFLAGS=\"-fsanitize=address\""
fi

set -e 

OLDPWD="$(pwd)"
cd "$BUILD_DIR"
eval $PREPEND \
cmake \
	-G \"$BUILD_GENERATOR\" \
	-DCMAKE_INSTALL_PREFIX=${INSTALL_PREFIX} \
	-DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
	${CMAKE_MORE_OPTIONS} \
	\"$DT_SRC_DIR\" 
cd "$OLDPWD"
cmake --build "$BUILD_DIR" -- -j$MAKE_TASKS

if [ $? = 0 ]; then
	cat <<EOF
Darktable finished building, to actually install darktable you need to type:
\$ cmake --build "$BUILD_DIR" --target install # optionnaly prefixed by sudo
EOF
else
   exit 1
fi
