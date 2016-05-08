#!/bin/sh

REF="reference.pfm"

# benchmark the performance of denoising algorithms, in terms of PSNR.
# 
# instructions:
# shoot a static scene with all interesting iso settings.
# shoot several at lowest iso (like 3x iso 100).
# combine the iso 100 shots as a hdr in lt mode (to average out noise),
# this will be reference.pfm
# export them all as pfm.
#

for i in *.pfm
do
  if [ "$i" == "$REF" ]
  then
    continue
  fi
  echo "$i : $(compare -metric PSNR $i $REF /dev/null 2>&1)"
done

