#!/bin/bash
#
# This script is a quick hack to make it easier to build the darktable documetation
# in PDF, html-multi and html-single formats, for the user manual and LUA API.
# In reality, it would be better to fix the makefiles to make then more intuitive,
# rather than scripting this in shell. In particular, the error handling in this
# script is not so good if there are build failures.
#
# usage:
# ./build.sh       # must be run first
# ./build-doc.sh   # can use --help more info
#
# this script assumes the build.sh script was already run to generate the build
# directory and makefiles. It also assumes that the build dependencies have already
# been installed.
#
# For Manjaro Linux, this involved installing the following:
#
################
# sudo pacman -S base-devel cmake intltool lensfun curl exiv2 lcms2 librsvg libxslt sqlite pugixml
# sudo pacman -S openexr libwebp graphicsmagick libcups libsoup libgphoto2 sdl mesa-libgl dbus-glib osm-gps-map
# sudo pacman -S jdk10-openjdk gnome-doc-utils fop imagemagick extra/docbook-xml extra/docbook-xsl po4a
#
# cd ~/aur
# git clone https://aur.archlinux.org/saxon6.git
# cd saxon6/
# makepkg -sri
# cd /usr/share/java
# ln -s saxon6/saxon.jar
# echo '#!/bin/sh
#
# exec java -classpath /usr/share/java/saxon.jar com.icl.saxon.StyleSheet "@0"' | sudo tee /usr/local/bin/saxon-xslt
# cd ~/aur
# git clone https://aur.archlinux.org/saxon-he.git
# cd saxon-he
# makepkg -sri
# cd ~/aur
# git clone https://aur.archlinux.org/docbook-xsl-saxon.git
# cd docbook-xsl-saxon
# makepkg -sri
################
#

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR" && pwd -P)

BUILD_DIR_DEFAULT="$DT_SRC_DIR/build"
BUILD_DIR="$BUILD_DIR_DEFAULT"
MAKE_TASKS=-1

PRINT_HELP=0


parse_args()
{
        while [ "$#" -ge 1 ] ; do
                option="$1"
                case $option in
                --build-dir)
                        BUILD_DIR="$2"
                        shift
                        ;;
                -j|--jobs)
                        MAKE_TASKS=$(printf "%d" "$2" >/dev/null 2>&1 && printf "$2" || printf "$MAKE_TASKS")
                        shift
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

print_help()
{
        cat <<EOF
$(basename $0) [OPTIONS]

Options:
   --build-dir      <string>  Building directory
                              (default: $BUILD_DIR_DEFAULT)

-j --jobs <integer>           Number of tasks
                              (default: number of CPUs)

Extra:
-h --help                Print help message

EOF

}

print_no_build_dir()
{
	cat <<EOF
Error: build directory
	${BUILD_DIR}
doesn't exist. You need to run the 'build.sh' script first to
create the build directory and generate the makefiles. If you
used a non-default build directory when running build.sh, be
sure to specify the same build directory here with the
--build-dir option.

EOF

}

# ---------------------------------------------------------------------------
# utility functions
# ---------------------------------------------------------------------------

num_cpu()
{
        local ncpu
        local platform=$(uname -s)

        case "$platform" in
        SunOS)
                ncpu=$(/usr/sbin/psrinfo |wc -l)
                ;;
        Linux|MINGW64*)
                if [ -r /proc/cpuinfo ]; then
                        ncpu=$(grep -c "^processor" /proc/cpuinfo)
                elif [ -x /sbin/sysctl ]; then
                        ncpu=$(/sbin/sysctl -n hw.ncpu 2>/dev/null)
                        if [ $? -ne 0 ]; then
                                ncpu=-1
                        fi
                fi
                ;;
        Darwin)
                ncpu=$(/usr/sbin/sysctl -n machdep.cpu.core_count 2>/dev/null)
                ;;
        *)
                printf "warning: unable to determine number of CPUs on $platform\n"
                ncpu=-1
                ;;
        esac

        if [ $ncpu -lt 1 ] ; then
                ncpu=1
        fi
        if [ $ncpu -ge 8 ] ; then
                ncpu=8
        fi
        printf "$ncpu"
}

make_name()
{
        local make="make"
        local platform=$(uname -s)

        case "$platform" in
        SunOS)
                PATH="/usr/gnu/bin:$PATH"
                export PATH
                make="gmake"
                ;;
        esac
        printf "$make"
}

# ---------------------------------------------------------------------------
# Let's process the user's wishes
# ---------------------------------------------------------------------------

MAKE_TASKS=$(num_cpu)
MAKE=$(make_name)

parse_args "$@"

if [ $PRINT_HELP -ne 0 ] ; then
        print_help
        exit 1
fi

if [ ! -d $BUILD_DIR ]; then
	print_no_build_dir
	exit 1
fi

# ---------------------------------------------------------------------------
# Let's go
# ---------------------------------------------------------------------------

cat <<EOF
darktable build script

Building directory:  $BUILD_DIR
Build tasks:         $MAKE_TASKS

NOTE: dtorg languages are set in doc/usermanual/DTORG.LINGUAS

EOF

cmd_build_usermanual_pdfs="cmake --build "$BUILD_DIR" --target usermanual -- -j$MAKE_TASKS"
cmd_build_usermanual_dtorg="cmake --build "$BUILD_DIR" --target darktable-usermanual-dtorg -- -j$MAKE_TASKS"
cmd_build_usermanual_html="cmake --build "$BUILD_DIR" --target darktable-usermanual-html -- -j$MAKE_TASKS"
cmd_build_luaapi_pdfs="cmake --build "$BUILD_DIR" --target lua-api -- -j$MAKE_TASKS"
cmd_build_luaapi_dtorg="cmake --build "$BUILD_DIR" --target darktable-lua-api-dtorg -- -j$MAKE_TASKS"
cmd_build_luaapi_html="cmake --build "$BUILD_DIR" --target darktable-lua-api-html -- -j$MAKE_TASKS"

eval "$cmd_build_usermanual_pdfs"
eval "$cmd_build_usermanual_dtorg"
eval "$cmd_build_usermanual_html"
# makefile seems to put images in wrong place, this hack cleans it up
if [ ! -d ${BUILD_DIR}/doc/usermanual/dtorg/images ]; then
   mkdir -p ${BUILD_DIR}/doc/usermanual/dtorg/images
   mv ${BUILD_DIR}/doc/usermanual/dtorg/*.jpg \
      ${BUILD_DIR}/doc/usermanual/dtorg/darkroom \
      ${BUILD_DIR}/doc/usermanual/dtorg/lighttable \
      ${BUILD_DIR}/doc/usermanual/dtorg/map \
      ${BUILD_DIR}/doc/usermanual/dtorg/overview \
      ${BUILD_DIR}/doc/usermanual/dtorg/preferences \
      ${BUILD_DIR}/doc/usermanual/dtorg/print \
      ${BUILD_DIR}/doc/usermanual/dtorg/tethering \
      ${BUILD_DIR}/doc/usermanual/dtorg/images
fi

eval "$cmd_build_luaapi_pdfs"
eval "$cmd_build_luaapi_dtorg"
eval "$cmd_build_luaapi_html"

	cat <<EOF

darktable documentation finished building,
The documents are in the directories:

User Manual:
	PDFs:   ${BUILD_DIR}/doc/usermanual
     website:	${BUILD_DIR}/doc/usermanual/dtorg  (refer to index.html in language subdir)
 single HTML:	${BUILD_DIR}/doc/usermanual/html

LUA API:
        PDFs:	${BUILD_DIR}/doc/usermanual
     website:	${BUILD_DIR}/doc/usermanual/dtorg_lua_api
 single HTML:	${BUILD_DIR}/doc/usermanual/html_lua_api

EOF
