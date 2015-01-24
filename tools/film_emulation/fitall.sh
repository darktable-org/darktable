#!/bin/bash

# manual step:
# download all color luts from gmic/gimp plugin, there is a button
# for that in the ui (could also wget the list, but that doesn't auto-update
# without parsing the gmic update script etc)

# build programs
make
# create identity look up table
./identitylut
# create input for g'mic (srgb, low dynamic range 8-bit)
# xmp forces srgb as input and output profiles
rm -f identity_srgb.png
darktable-cli identity.pfm srgb_to_srgb.xmp identity_srgb.png
# create input for darktable (linear, floating point).
# xmp reads srgb and outputs as linear rgb
rm -f input.pfm
darktable-cli identity.pfm srgb_to_rec709.xmp input.pfm

# now fit all color luts!
for i in 14 #$(seq 0 1 20)
do
  # create reference image
  #  gmic unity_srgb.png "$i" -map_clut -o reference.png
  gmic unity_srgb.png -gimp_emulate_film_colorslide $i,1,0,1,0,0,0,0,0 -o reference.png
  # create pfm, in linear rec709 output
  rm -f reference.pfm
  darktable-cli reference.png srgb_to_rec709.xmp reference.pfm
  # fit:
  ./fit
  # rename output xmp to something sane:
  # mv input.xmp ${i%cimgz}xmp
  mv input.xmp preset_${i}.xmp
done

