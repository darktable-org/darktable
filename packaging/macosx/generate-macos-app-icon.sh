#!/usr/bin/env bash

# This script generates the macOS app icons in all required resolutions
# and puts them into ./Icons.icns

# Go to directory of script
scriptDir=$(dirname "$0")
cd "$scriptDir"/
scriptDir=$(pwd)

# Set variables
buildDir="${scriptDir}/../../build"
iconsDir="${buildDir}/Icons.iconset"
iconFile="${scriptDir}/../../data/pixmaps/scalable/darktable_macos_icon.svg"

# ensure clean build
rm -rf "${iconsDir}"
mkdir "${iconsDir}"

rsvg-convert -h 16 ${iconFile} > ${iconsDir}/icon_16.png
rsvg-convert -h 32 ${iconFile} > ${iconsDir}/icon_16@2x.png
rsvg-convert -h 32 ${iconFile} > ${iconsDir}/icon_32.png
rsvg-convert -h 64 ${iconFile} > ${iconsDir}/icon_32@2x.png
rsvg-convert -h 128 ${iconFile} > ${iconsDir}/icon_128.png
rsvg-convert -h 256 ${iconFile} > ${iconsDir}/icon_128@2x.png
rsvg-convert -h 256 ${iconFile} > ${iconsDir}/icon_256.png
rsvg-convert -h 512 ${iconFile} > ${iconsDir}/icon_256@2x.png
rsvg-convert -h 512 ${iconFile} > ${iconsDir}/icon_512.png
rsvg-convert -h 1024 ${iconFile} > ${iconsDir}/icon_512@2x.png

iconutil -c icns ${iconsDir} -o ${scriptDir}/Icons.icns
