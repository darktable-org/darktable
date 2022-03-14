#!/bin/bash
#
# Copyright (c) 2014-2022 darktable developers.
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#

TOOLS_DIR=$(dirname "$0")
TOOLS_DIR=$(cd "$TOOLS_DIR" && pwd -P)

. "$TOOLS_DIR"/noise/subr.sh

AUTHOR="$(getent passwd "$USER" | awk -F ':' '{print $5}' | awk -F ',' '{print $1}') <$USER@$HOSTNAME>"

DNG="$1"

if [ ! -f "$DNG" ];
then
  echo "Error: file does not exist"
  exit
fi

MAKE=$(get_exif_key "$DNG" Exif.Image.Make)
MODEL=$(get_exif_key "$DNG" Exif.Image.Model)
UNIQUE_CAMERA_MODEL=$(get_exif_key "$DNG" Exif.Image.UniqueCameraModel)

ISO=$(get_image_iso "$DNG")

# This doesn't work with two name makes but there aren't any active ones
ID_MAKE=$(get_image_camera_maker "$DNG")
ID_MODEL=$(get_image_camera_model "$DNG")

MANGLED_MAKE_MODEL=$(echo "$ID_MAKE" "$ID_MODEL")

SOFTWARE=$(exiv2 -Pv -K Exif.Image.Software "$DNG" 2>/dev/null)

ILLUMINANT=$(exiv2 -Pv -K Exif.Image.CalibrationIlluminant2 "$DNG" 2>/dev/null)
MATRIX=($(exiv2 -Pv -K Exif.Image.ColorMatrix2 "$DNG" 2>/dev/null | sed 's#/10000##g'))

# FIXME: uses only the first value of possibly different multiple ones
WHITE=$(exiv2 -Pv -K Exif.SubImage1.WhiteLevel "$DNG" 2>/dev/null | awk '{print $1}' | bc)
BLACK=$(exiv2 -Pv -K Exif.SubImage1.BlackLevel "$DNG" 2>/dev/null | awk '{print $1}' | bc)

CFA_PATTERN_DIM=$(exiv2 -Pv -K Exif.SubImage1.CFARepeatPatternDim "$DNG" 2>/dev/null)
CFA_PATTERN_WIDTH=$(echo $CFA_PATTERN_DIM | awk '{print $1}')
CFA_PATTERN_HEIGHT=$(echo $CFA_PATTERN_DIM | awk '{print $2}')

CFA_PATTERN=($(exiv2 -Pv -K Exif.SubImage1.CFAPattern "$DNG" 2>/dev/null | sed 's#0#RED#g; s#1#GREEN#g; s#2#BLUE#g'))

IMG_WIDTH=$(exiv2 -Pv -K Exif.SubImage1.ImageWidth "$DNG" 2>/dev/null)
IMG_LENGTH=$(exiv2 -Pv -K Exif.SubImage1.ImageLength "$DNG" 2>/dev/null)

if [[ $IMG_WIDTH -gt $IMG_LENGTH ]]; then
  IMG_LONG=$IMG_WIDTH
  IMG_SHORT=$IMG_LENGTH
else
  IMG_LONG=$IMG_LENGTH
  IMG_SHORT=$IMG_WIDTH
fi

MODE=""
SENSOR_ISO=""

if [[ $MAKE == Panasonic ]]; then
  if [[ $((100 * IMG_LONG / IMG_SHORT)) -gt 164 ]]; then
    MODE=" mode=\"16:9\""
  elif [[ $((100 * IMG_LONG / IMG_SHORT)) -gt 142 ]]; then
    MODE=" mode=\"3:2\""
  elif [[ $((100 * IMG_LONG / IMG_SHORT)) -gt 116 ]]; then
    MODE=" mode=\"4:3\""
  else
    MODE=" mode=\"1:1\""
  fi
elif [[ $MAKE == "NIKON CORPORATION" ]]; then
# i'm not sure it can be detected automatically
# rawspeed code to detect it is big
  MODE=" mode=\"<FIXME (14 or 12)>bit-<FIXME (compressed or uncompressed)>\""
elif [[ $MAKE == "Canon" ]]; then
  SENSOR_ISO=" iso_list=\"$ISO\""
fi

echo "DNG created by : $SOFTWARE"
echo "DNG Illuminant : $ILLUMINANT (should be 21)"
echo ""

echo ""
echo "$ nano -w src/external/rawspeed/data/cameras.xml (mind the tabs)"
echo ""
echo -e "\t<Camera make=\"$MAKE\" model=\"$MODEL\"$MODE>"
echo -e "\t\t<ID make=\"$ID_MAKE\" model=\"$ID_MODEL\">$UNIQUE_CAMERA_MODEL</ID>"

if [[ $MAKE == FUJIFILM && $CFA_PATTERN_WIDTH == 6 && $CFA_PATTERN_HEIGHT == 6 ]]; then
  echo -e "\t\t<CFA2 width=\"$CFA_PATTERN_WIDTH\" height=\"$CFA_PATTERN_HEIGHT\">"
  # The DNG's CFA pattern is mysteriously shifted horizontally for
  # 14-bit x-trans chips (despite it being stored unshifted in the raw
  # file). Identify 14-bit chips by their max white value.

  # FIXME: this is definitively wrong. from rawspeed, DngDecoder::parseCFA():
  # the cfa is specified relative to the ActiveArea. we want it relative (0,0)
  # Since in handleMetadata(), in subFrame() we unconditionally shift CFA by
  # activearea+DefaultCropOrigin; here we need to undo the 'ACTIVEAREA' part.

  if [[ $WHITE -gt 16000 ]]; then
    COL_OFFSET=2
  else
    COL_OFFSET=0
  fi
  for ROW in {0..5}; do
    COLORS=""
    for COL in {0..5}; do
      COLORS+=${CFA_PATTERN[$((ROW*6+(COL+COL_OFFSET)%6))]:0:1}
    done
    echo -e "\t\t\t<ColorRow y=\"$ROW\">$COLORS</ColorRow>"
  done
  echo -e "\t\t</CFA2>"
else
  # CFA2 is a superset of CFA, but keep with tradition and use CFA for Bayer matrices
  echo -e "\t\t<CFA width=\"$CFA_PATTERN_WIDTH\" height=\"$CFA_PATTERN_HEIGHT\">"
  echo -e "\t\t\t<Color x=\"0\" y=\"0\">${CFA_PATTERN[0]}</Color>"
  echo -e "\t\t\t<Color x=\"1\" y=\"0\">${CFA_PATTERN[1]}</Color>"
  echo -e "\t\t\t<Color x=\"0\" y=\"1\">${CFA_PATTERN[2]}</Color>"
  echo -e "\t\t\t<Color x=\"1\" y=\"1\">${CFA_PATTERN[3]}</Color>"
  echo -e "\t\t</CFA>"
fi

echo -e "\t\t<Crop x=\"0\" y=\"0\" width=\"0\" height=\"0\"/>"
echo -e "\t\t<Sensor black=\"$BLACK\" white=\"$WHITE\"$SENSOR_ISO/>"
echo -e "\t\t<ColorMatrices>"
echo -e "\t\t\t<ColorMatrix planes=\"3\">"
echo -e "\t\t\t\t<ColorMatrixRow plane=\"0\">${MATRIX[0]} ${MATRIX[1]} ${MATRIX[2]}</ColorMatrixRow>"
echo -e "\t\t\t\t<ColorMatrixRow plane=\"1\">${MATRIX[3]} ${MATRIX[4]} ${MATRIX[5]}</ColorMatrixRow>"
echo -e "\t\t\t\t<ColorMatrixRow plane=\"2\">${MATRIX[6]} ${MATRIX[7]} ${MATRIX[8]}</ColorMatrixRow>"
echo -e "\t\t\t</ColorMatrix>"
echo -e "\t\t</ColorMatrices>"
echo -e "\t</Camera>"
echo ""

if [[ $MAKE == Panasonic ]]; then
  echo "NOTE: Panasonic RW2s are different dependent on aspect ratio, please run this tool on RW2 for each of the camera's ratios (4:3,3:2,16:9,1:1)"
  echo ""
elif [[ $MAKE == "NIKON CORPORATION" ]]; then
  echo "NOTE: NIKON NEFs are different dependent on mode, please run this tool on NEF for each of the camera's mode (14-bit, 12-bit; compressed, uncompressed)"
  echo ""
elif [[ $MAKE == "Canon" ]]; then
  echo "NOTE: CANON CR2 have different black/white levels per ISO, please run this tool on CR2 for each of the camera's ISO (including all the sub-iso 1/2 and 1/3)"
  echo "NOTE: please see dngmeta.rb"
  echo ""
fi
echo "NOTE: The default crop exposes the full sensor including garbage pixels, which need to be visually inspected. (negative width/height values are right/bottom crops, which are preferred)"
echo ""
echo "NOTE: Sensor black and white levels sometimes vary based on ISO, please run this tool on raws for each of the camera's supported ISOs"
echo ""
echo "$ git commit -a -m \"rawspeed: $MANGLED_MAKE_MODEL support\" --author \"$AUTHOR\""
echo ""
