#!/bin/bash

#
# Usage: extract_wb_from_images [-p]
#        -p  do the purge, otherwise only display unused tags
#

commandline="$0 $*"

# handle command line arguments
option="$1"
if [ "${option}" = "-h" ] || [ "${option}" = "--help" ]; then
  echo "Extract White Balance preset info from images"
  echo "Usage:   $0 <file1> [file2] ..."
  echo ""
  echo "This tool will generate archive with wite balance"
  echo "presets extracted from provided image files"
  exit 0
fi

tmp_dir=$(mktemp -d -t dt-wb-XXXXXXXXXX)
cur_dir=$(pwd)

tarball="$cur_dir"/darktable-whitebalance-$(date +'%Y%m%d').tar.bz2

echo "Extracting WB presets."
for image in "$@"
do
    echo -n "."
    exiftool -Make -Model "-WBType*" "-WB_*" \
      -WhiteBalance -WhiteBalance2 -WhitePoint \
      -WBShiftAB -WBShiftAB_GM -WBShiftGM -WhiteBalanceFineTune \
      -WhiteBalanceBracket -WBShiftIntelligentAuto -WBShiftCreativeControl \
      -WBRedLevel -WBBlueLevel -WBGreenLevel \
      "${image}" > "${tmp_dir}/${image}.txt"
done

echo
echo "preparing tarball..."

tar -cjf "${tarball}" -C ${tmp_dir} .

echo "cleaning up..."
rm -rf $tmp_dir

echo

echo "Extracting wb presets done, post the following file to us:"
echo $tarball
