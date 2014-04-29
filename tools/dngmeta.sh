#!/bin/bash
#
# Copyright (c) 2014 Pascal de Bruijn
#

AUTHOR="$(getent passwd $USER | awk -F ':' '{print $5}' | awk -F ',' '{print $1}') <$USER@$HOSTNAME>"

DNG=$1

MAKE=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.Make ' | awk '{print $2}')
MODEL=$(exiv2 -Pkt $DNG 2>/dev/null | grep 'Exif.Image.Model ' | awk '{print $2}')

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

CFA_UPPER_LEFT=$(exiv2 -Pkt *.dng 2>/dev/null | grep 'Exif.SubImage1.CFAPattern' | awk '{print $2}' | sed 's#0#RED#' | sed 's#1#GREEN#' | sed 's#2#BLUE#')
CFA_UPPER_RIGHT=$(exiv2 -Pkt *.dng 2>/dev/null | grep 'Exif.SubImage1.CFAPattern' | awk '{print $3}' | sed 's#0#RED#' | sed 's#1#GREEN#' | sed 's#2#BLUE#')
CFA_LOWER_LEFT=$(exiv2 -Pkt *.dng 2>/dev/null | grep 'Exif.SubImage1.CFAPattern' | awk '{print $4}' | sed 's#0#RED#' | sed 's#1#GREEN#' | sed 's#2#BLUE#')
CFA_LOWER_RIGHT=$(exiv2 -Pkt *.dng 2>/dev/null | grep 'Exif.SubImage1.CFAPattern' | awk '{print $5}' | sed 's#0#RED#' | sed 's#1#GREEN#' | sed 's#2#BLUE#')


echo "DNG created by : $SOFTWARE"
echo "DNG Illuminant : $ILLUMINANT (should be 21)"
echo ""

echo ""
echo "$ nano -w src/external/adobe_coeff.c"
echo ""
echo "{ \"$MAKE $MODEL\", $BLACK, $WHITE_HEX,"
echo "{ $MATRIX_XR,$MATRIX_XG,$MATRIX_XB,$MATRIX_YR,$MATRIX_YG,$MATRIX_YB,$MATRIX_ZR,$MATRIX_ZG,$MATRIX_ZB } },"
echo ""
echo "$ git commit -a -m \"adobe_coeff: $MAKE $MODEL support\" --author \"$AUTHOR\""
echo ""

echo ""
echo "$ nano -w src/external/rawspeed/data/cameras.xml (mind the tabs)"
echo ""
echo -e "\t<Camera make=\"$MAKE\" model=\"$MODEL\" supported=\"yes\">"
echo -e "\t\t<CFA width=\"$CFA_PATTERN_WIDTH\" height=\"$CFA_PATTERN_HEIGHT\">"
echo -e "\t\t\t<Color x=\"0\" y=\"0\">$CFA_UPPER_LEFT</Color>"
echo -e "\t\t\t<Color x=\"1\" y=\"0\">$CFA_UPPER_RIGHT</Color>"
echo -e "\t\t\t<Color x=\"0\" y=\"1\">$CFA_LOWER_LEFT</Color>"
echo -e "\t\t\t<Color x=\"1\" y=\"1\">$CFA_LOWER_RIGHT</Color>"
echo -e "\t\t</CFA>"
echo -e "\t\t<Crop x=\"0\" y=\"0\" width=\"0\" height=\"0\"/>"
echo -e "\t\t<Sensor black=\"$BLACK\" white=\"$WHITE\"/>"
echo -e "\t</Camera>"
echo ""
echo "$ git commit -a -m \"rawspeed: $MAKE $MODEL support\" --author \"$AUTHOR\""
echo ""
