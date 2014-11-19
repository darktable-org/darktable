#!/bin/bash
#
# Copyright (c) 2014 Pascal de Bruijn
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

AUTHOR="$(getent passwd $USER | awk -F ':' '{print $5}' | awk -F ',' '{print $1}') <$USER@$HOSTNAME>"

DNG=$1

MAKE=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.Make ' | sed 's#Exif.Image.Make *##g')
MODEL=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.Model ' | sed 's#Exif.Image.Model *##g')

MANGLED_MAKE_MODEL=$(echo $MAKE $MODEL | sed 's# CORPORATION##gi' | sed 's#Canon Canon#Canon#g' | sed 's#NIKON NIKON#NIKON#g' | sed 's#PENTAX PENTAX#PENTAX#g' | sed 's#OLYMPUS IMAGING CORP.#OLYMPUS#g' | sed 's#OLYMPUS OPTICAL CO.,LTD#OLYMPUS#g' | sed 's# FinePix##g')

SOFTWARE=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.Software ' | awk '{print $2 " " $3 " " $4 " " $5 " " $6}')

ILLUMINANT=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.CalibrationIlluminant2 ' | awk '{print $2}')
MATRIX_XR=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $2}')
MATRIX_XG=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $3}')
MATRIX_XB=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $4}')
MATRIX_YR=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $5}')
MATRIX_YG=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $6}')
MATRIX_YB=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $7}')
MATRIX_ZR=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $8}')
MATRIX_ZG=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $9}')
MATRIX_ZB=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.ColorMatrix2 ' | sed 's#/10000##g' | awk '{print $10}')

WHITE=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.WhiteLevel ' | awk '{print $2}' | bc)
BLACK=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.BlackLevel ' | awk '{print $2}' | bc)

WHITE_HEX="0x$(echo "ibase=10;obase=16;$WHITE" | bc | tr 'A-Z' 'a-z')"

CFA_PATTERN_WIDTH=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.CFARepeatPatternDim ' | awk '{print $2}')
CFA_PATTERN_HEIGHT=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.CFARepeatPatternDim ' | awk '{print $3}')

CFA_PATTERN=($(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.CFAPattern ' | awk '{$1=""; print $0}' | sed 's#0#RED#g; s#1#GREEN#g; s#2#BLUE#g'))

IMG_WIDTH=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.ImageWidth ' | awk '{print $2}')
IMG_LENGTH=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.SubImage1.ImageLength ' | awk '{print $2}')

if [[ $IMG_WIDTH -gt $IMG_LENGTH ]]; then
  IMG_LONG=$IMG_WIDTH
  IMG_SHORT=$IMG_LENGTH
else
  IMG_LONG=$IMG_LENGTH
  IMG_SHORT=$IMG_WIDTH
fi

if [[ $MAKE == Panasonic ]]; then
  if [[ $((100 * $IMG_LONG / $IMG_SHORT)) -gt 164 ]]; then
    MODE=" mode=\"16:9\""
  elif [[ $((100 * $IMG_LONG / $IMG_SHORT)) -gt 142 ]]; then
    MODE=" mode=\"3:2\""
  elif [[ $((100 * $IMG_LONG / $IMG_SHORT)) -gt 116 ]]; then
    MODE=" mode=\"4:3\""
  else
    MODE=" mode=\"1:1\""
  fi
else
  MODE=""
fi

echo "DNG created by : $SOFTWARE"
echo "DNG Illuminant : $ILLUMINANT (should be 21)"
echo ""

echo ""
echo "$ nano -w src/external/adobe_coeff.c"
echo ""
echo "{ \"$MANGLED_MAKE_MODEL\", $BLACK, $WHITE_HEX, { $MATRIX_XR,$MATRIX_XG,$MATRIX_XB,$MATRIX_YR,$MATRIX_YG,$MATRIX_YB,$MATRIX_ZR,$MATRIX_ZG,$MATRIX_ZB } },"
echo ""
echo "$ git commit -a -m \"adobe_coeff: $MANGLED_MAKE_MODEL support\" --author \"$AUTHOR\""
echo ""

echo ""
echo "$ nano -w src/external/rawspeed/data/cameras.xml (mind the tabs)"
echo ""
echo -e "\t<Camera make=\"$MAKE\" model=\"$MODEL\"$MODE>"

if [[ $MAKE == FUJIFILM && $CFA_PATTERN_WIDTH == 6 && $CFA_PATTERN_HEIGHT == 6 ]]; then
  echo -e "\t\t<CFA2 width=\"$CFA_PATTERN_WIDTH\" height=\"$CFA_PATTERN_HEIGHT\">"
  # The DNG's CFA pattern is mysteriously shifted horizontally for
  # 14-bit x-trans chips (despite it being stored unshifted in the raw
  # file). Identify 14-bit cips by their max white value.
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
echo -e "\t\t<Sensor black=\"$BLACK\" white=\"$WHITE\"/>"
echo -e "\t</Camera>"
echo ""

if [[ $MAKE == Panasonic ]]; then
  echo "NOTE: Panasonic RW2s are different dependant on aspect ratio, please run this tool on RW2 for each of the camera's ratios (4:3,3:2,16:9,1:1)"
  echo ""
fi
echo "NOTE: The default crop exposes the full sensor including garbage pixels, which need to be visually inspected. (negative width/height values are right/bottom crops, which are preferred)"
echo ""
echo "NOTE: Sensor black and white levels sometimes vary based on ISO, please run this tool on raws for each of the camera's supported ISOs"
echo ""
echo "$ git commit -a -m \"rawspeed: $MANGLED_MAKE_MODEL support\" --author \"$AUTHOR\""
echo ""
