#!/bin/bash

#
# Usage: extract_wb_from_images
#

commandline="$0 $*"

# handle command line arguments
option="$1"
if [ "${option}" = "-h" ] || [ "${option}" = "--help" ]; then
  echo "Extract White Balance preset info from images"
  echo "Usage:   $0 <file1> [file2] ..."
  echo ""
  echo "This tool will generate archive with white balance"
  echo "presets extracted from provided image files"
  exit 0
fi

tmp_dir=$(mktemp -d -t dt-wb-XXXXXXXXXX)
cur_dir=$(pwd)

tarball="$cur_dir"/darktable-whitebalance-$(date +'%Y%m%d').tar.gz

echo "Extracting WB presets."
for image in "$@"
do
    echo -n "."
    exiftool -Make -Model "-WBType*" "-WB_*" "-ColorTemp*"                     \
      -WhiteBalance -WhiteBalance2 -WhitePoint -ColorCompensationFilter        \
      -WBShiftAB -WBShiftAB_GM -WBShiftAB_GM_Precise -WBShiftGM -WBScale       \
      -WhiteBalanceFineTune -WhiteBalanceComp -WhiteBalanceSetting             \
      -WhiteBalanceBracket -WhiteBalanceBias -WBMode -WhiteBalanceMode         \
      -WhiteBalanceTemperature -WhiteBalanceDetected -ColorTemperature         \
      -WBShiftIntelligentAuto -WBShiftCreativeControl -WhiteBalanceSetup       \
      -WBRedLevel -WBBlueLevel -WBGreenLevel -RedBalance -BlueBalance          \
      "${image}" > "${tmp_dir}/${image}.txt"
done

echo
echo "preparing tarball..."

tar -czf "${tarball}" -C ${tmp_dir} .

echo "cleaning up..."
rm -rf $tmp_dir

echo

echo "Extracting wb presets done, post the following file to us:"
echo $tarball
