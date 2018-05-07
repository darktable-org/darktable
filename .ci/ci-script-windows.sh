#!/bin/bash
#    This file is part of darktable.
#
#    darktable is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    darktable is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

# Continuous Integration script for darktable
# Author: Peter Budai <peterbudai@hotmail.com>

# it is supposed to be run by appveyor-ci

# Enable colors
normal=$(tput sgr0)
red=$(tput setaf 1)
green=$(tput setaf 2)
cyan=$(tput setaf 6)

# Basic status function
_status() {
    local type="${1}"
    local status="${package:+${package}: }${2}"
    local items=("${@:3}")
    case "${type}" in
        failure) local -n nameref_color='red';   title='[DARKTABLE CI] FAILURE:' ;;
        success) local -n nameref_color='green'; title='[DARKTABLE CI] SUCCESS:' ;;
        message) local -n nameref_color='cyan';  title='[DARKTABLE CI]'
    esac
    printf "\n${nameref_color}${title}${normal} ${status}\n\n"
}

# Run command with status
execute(){
    local status="${1}"
    local command="${2}"
    local arguments=("${@:3}")
    cd "${package:-.}"
    message "${status}"
    if [[ "${command}" != *:* ]]
        then ${command} ${arguments[@]}
        else ${command%%:*} | ${command#*:} ${arguments[@]}
    fi || failure "${status} failed"
    cd - > /dev/null
}

# Build
build_darktable() {
    cd $(cygpath ${APPVEYOR_BUILD_FOLDER})

    mkdir build && cd build
    cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(cygpath ${APPVEYOR_BUILD_FOLDER})/build $(cygpath ${APPVEYOR_BUILD_FOLDER})
    cmake --build .
    cmake --build . --target package
}

# Status functions
failure() { local status="${1}"; local items=("${@:2}"); _status failure "${status}." "${items[@]}"; exit 1; }
success() { local status="${1}"; local items=("${@:2}"); _status success "${status}." "${items[@]}"; exit 0; }
message() { local status="${1}"; local items=("${@:2}"); _status message "${status}"  "${items[@]}"; }

# Install build environment and build
PATH=/c/msys64/mingw64/bin:$PATH

# reduce time required to install packages by disabling pacman's disk space checking
sed -i 's/^CheckSpace/#CheckSpace/g' /etc/pacman.conf

# write a custom fonts.conf to speed up fc-cache
export FONTCONFIG_FILE=$(cygpath -a fonts.conf)
cat > "$FONTCONFIG_FILE" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig><dir>$(cygpath -aw fonts)</dir></fontconfig>
EOF

execute 'Installing base-devel and toolchain'  pacman -S --needed --noconfirm mingw-w64-x86_64-{toolchain,clang,cmake}
execute 'Installing dependencies' pacman -S --needed --noconfirm  mingw-w64-x86_64-{exiv2,lcms2,lensfun,dbus-glib,openexr,sqlite3,libxslt,libsoup,libwebp,libsecret,lua,graphicsmagick,openjpeg2,gtk3,pugixml,libexif,osm-gps-map,libgphoto2,flickcurl,drmingw,gettext,python3,iso-codes}
execute 'Updating lensfun databse' lensfun-update-data
execute 'Building darktable' build_darktable
