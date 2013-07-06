#!/bin/sh

# TODO: extract basecurves, color matrices etc and create a complete html table for the web:
echo "cameras with profiled presets for denoising:"
cat src/common/noiseprofiles.h | grep -E '^[[:space:]]*{"' | grep "iso 400\"" | sed 's/^[ \t]*//' | tr -d "{}()\"" | cut -d '_' -f2 | cut -d ',' -f1 | tr " " "_" | sed -e "s/_iso_400//" | tr [:upper:] [:lower:] | tr "_" " " | sort | sed 's/$/<\/li>/' | sed 's/^/<li>/'
