#!/bin/bash
#
# Script to install required homebrew packages
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Check if brew exists
if ! [ -x "$(command -v brew)" ]; then
    echo 'Homebrew not found. Follow instructions as provided by https://brew.sh/ to install it.' >&2
    exit 1
else
    echo "Found homebrew running in $(arch)-based environment."
fi

# Make sure that homebrew is up-to-date
brew update
brew upgrade

# Define homebrew dependencies
hbDependencies="adwaita-icon-theme \
    cmake \
    cmocka \
    curl \
    desktop-file-utils \
    exiv2 \
    gettext \
    git \
    glib \
    gmic \
    gphoto2 \
    graphicsmagick \
    gtk-mac-integration \
    gtk+3 \
    icu4c \
    intltool \
    iso-codes \
    jpeg \
    jpeg-xl \
    json-glib \
    lensfun \
    libavif \
    libheif \
    libomp \
    librsvg \
    libsecret \
    libsoup@2 \
    little-cms2 \
    llvm \
    lua \
    ninja \
    openexr \
    openjpeg \
    osm-gps-map \
    perl \
    po4a \
    portmidi \
    pugixml \
    sdl2 \
    webp"

# Categorize dependency list
standalone=
deps=
notfound=
hbInstalled=$( brew list --formula --quiet )
hbLeaves=$( brew leaves --installed-on-request )
for hbDependency in $hbDependencies; do
    if [[ " ${hbInstalled[*]} " == *"${hbDependency}"* ]];
    then
      if [[ " ${hbLeaves[*]} " == *"${hbDependency}"* ]]; then
        standalone="${hbDependency} ${standalone}"
      else
        deps="${hbDependency} ${deps}"
      fi
    else
      notfound="${hbDependency} ${notfound}"
    fi
done

# Show installed dependencies
if [ "${standalone}" -o "${deps}" ]; then
    echo
    echo "Installed Dependencies:"
    (
        brew list --formula --quiet --versions ${standalone}
        brew list --formula --quiet --versions ${deps} | sed -e 's/$/ (autoinstalled)/'
    ) | sort
fi

# Install missing dependencies
if [ "${notfound}" ]; then
    echo
    echo "Missing Dependencies:"
    echo "${notfound}"

    brew install ${notfound}
fi

# fix for keg-only libomp
brew link --force libomp
