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

# Dependencies that must be linked
hbMustLink="libomp"

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

# Fix for unlinked keg-only dependencies
echo
echo "Checking for unlinked keg-only dependencies..."
# This is a lot easier with jq...
#unlinked=$( brew info --json=v1 ${hbMustLink}| jq "map(select(.linked_keg == null) | .name)" | jq -r '.[]' )
mustlink=$(
    name=
    brew info --json=v1 $hbMustLink \
        | grep -Eo '"(full_name|linked_keg)":.*' \
        | sed -e 's/"//g;s/://g;s/,//g' \
        | while read key value;
        do
            if [ "${key}" == "full_name" ]; then
              name="${value}"
            fi
            if [ "${name}" -a "${key}" == "linked_keg" -a "${value}" == "null" ]; then
              echo "${name}"
            fi
        done
)
if [ "${mustlink}" ]; then
    echo
    echo "Unlinked dependencies found! Attempting to relink..."
    brew link --force ${mustlink}
else
    echo "None."
fi
