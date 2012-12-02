#!/bin/bash
NP=~/vcs/darktable/tools/noise/noiseprofile

for i in *.pfm
do
  echo "profiling : $i"
  $NP $i > ${i%pfm}dat
  echo "plotting  : $i"
  gnuplot << EOF
  set term pdf
  set print "${i%pfm}fit"
  set output "${i%pfm}pdf"
  plot "${i%pfm}dat" u 1:(log(\$5)) w l lw 4 title "histogram ${i%.pfm}", '' u 1:(log(\$6)) w l lw 4, '' u 1:(log(\$7)) w l lw 4

  f1(x) = a1*x + b1
  f2(x) = a2*x + b2
  f3(x) = a3*x + b3
  a1=0.1;b1=0.01;
  a2=0.1;b2=0.01;
  a3=0.1;b3=0.01;
  set xrange [0:0.35]
  fit f1(x) "${i%pfm}dat" u 1:(\$2**2) via a1,b1
  set xrange [0:0.9]
  fit f2(x) "${i%pfm}dat" u 1:(\$3**2) via a2,b2
  set xrange [0:0.5]
  fit f3(x) "${i%pfm}dat" u 1:(\$4**2) via a3,b3

  set xrange [0:1]
  plot "${i%pfm}dat" u 1:2 w l lw 4 title "noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4, \
    '' u 1:(sqrt(f1(\$1))) w l lw 2 lt 1 title "fit",\
    '' u 1:(sqrt(f2(\$1))) w l lw 2 lt 2,\
    '' u 1:(sqrt(f3(\$1))) w l lw 2 lt 3

  print a1, a2, a3, b1, b2, b3
EOF
  # fitted parametric curves:
  $NP $i -c $(cat ${i%pfm}fit) > ${i%.pfm}_flat.dat 2> ${i%.pfm}_curves.dat
  # data based histogram inversion:
# $NP $i -h ${i%pfm}dat > ${i%.pfm}_flat.dat 2> ${i%.pfm}_curves.dat
  echo "flattened : $i"
  gnuplot << EOF
  set term pdf
  set output "${i%.pfm}_flat.pdf"
  plot "${i%.pfm}_flat.dat" u 1:2 w l lw 4 title "flat noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  plot "${i%pfm}dat" u 1:(log(\$5)) w l lw 4 title "flat histogram ${i%.pfm}", '' u 1:(log(\$6)) w l lw 4, '' u 1:(log(\$7)) w l lw 4
  plot "${i%.pfm}_curves.dat" u 0:1 w l lw 4 title "conversion curves", '' u 0:2 w l lw 4, '' u 0:3 w l lw 4
EOF
done

