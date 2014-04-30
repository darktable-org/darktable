#!/bin/bash
#
# Copyright (c) 2014 Pascal de Bruijn
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
