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

cat src/external/rawspeed/data/cameras.xml | grep '<Camera ' \
                                           | sed 's#.*make *= *"##g' \
                                           | sed 's#" model *= *"# #g' \
                                           | sed 's#".*##g' \
                                           | sed 's# CORPORATION##gi' \
                                           | sed 's#Canon Canon#Canon#g' \
                                           | sed 's#NIKON NIKON#NIKON#g' \
                                           | sed 's#PENTAX PENTAX#PENTAX#g' \
                                           | sed 's#OLYMPUS IMAGING CORP.#OLYMPUS#g' \
                                           | sed 's#OLYMPUS OPTICAL CO.,LTD#OLYMPUS#g' \
                                           | sort | uniq | while read CAM
do
  if [[ ! `grep -i "$CAM" src/external/adobe_coeff.c 2>/dev/null` ]]; then
    echo "missing : $CAM"
  fi
done
