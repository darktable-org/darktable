#!/bin/sh

isolist="200 400 800 1600"

for iso in $isolist
do
  grep '"name"' data/noiseprofiles.json | tr -s " " "_" | grep -v '"skip"_*:_*true' | grep "\"iso\"_*:_*${iso}_*," | sed 's/.*"name"_*:_*"\([^"]*\)_iso_[0-9]*".*"a"_*:_*\[_*[0-9.eE+-]*_*,_\([0-9.eE+-]*\).*"b"_*:_*\[_*[0-9.eE+-]*_*,_\([0-9.eE+-]*\).*/\1 \2 \3/' | nl > noise_${iso}.dat

  gnuplot << EOF
  set term pdf fontscale 0.5 size 10, 10
  set output 'noise_iso_$iso.pdf'
#  for web:
#  set term png fontscale 0.8 size 700,700 
#  set output 'noise_iso_$iso.png'
  set format x "%g"
  set format y "%g"
  set xlabel 'poissonian'
  set ylabel 'gaussian'
  plot "noise_${iso}.dat" u 3:4 w p title "iso $iso", '' u 3:4:(sprintf("\n%s", stringcolumn(2))) w labels notitle
EOF
done

rm -f noise_*.dat


