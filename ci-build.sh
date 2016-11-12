#!/bin/bash

# Continuous Integration Library for darktable
# Author: Peter Budai <peterbudai@hotmail.com>

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
    cd "$(dirname "$0")"
    mkdir artifacts
    mkdir build && cd build
    cmake -G "MSYS Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/c/projects/darktable/artifacts ../.
    cmake --build . --target install
}

# Status functions
failure() { local status="${1}"; local items=("${@:2}"); _status failure "${status}." "${items[@]}"; exit 1; }
success() { local status="${1}"; local items=("${@:2}"); _status success "${status}." "${items[@]}"; exit 0; }
message() { local status="${1}"; local items=("${@:2}"); _status message "${status}"  "${items[@]}"; }

# Install build environment and build
PATH=/c/msys64/mingw64/bin:$PATH
execute 'Installing base-devel and toolchain'  pacman -S --noconfirm mingw-w64-x86_64-{toolchain,cmake}
execute 'Installing dependencies' pacman -S --noconfirm  mingw-w64-x86_64-{exiv2,lcms2,lensfun,libglade,dbus-glib,openexr,sqlite3,libxslt,libsoup,libwebp,libsecret,lua,graphicsmagick,openjpeg2,gtk-engines,gtk3,pugixml,libexif,SDL}
execute 'Building darktable' build_darktable
