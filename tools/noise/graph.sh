#!/bin/bash

isolist="200 400 800 1600"

rm -f poissonian_all.txt gaussian_all.txt

for iso in $isolist
do
  cat src/iop/denoiseprofile.c | grep -E '{"' | grep "iso ${iso}\"" | tr -d "{}()\"" | cut -d '_' -f2 | cut -d ',' -f1 | tr " " "_" | sed -e "s/_iso_${iso}//" | nl > cams.txt 
  cat src/iop/denoiseprofile.c | grep -E '{"' | grep "iso ${iso}\"" | tr -d "{}()\"" | cut -d '_' -f2 | cut -d ',' -f8 | nl > poissonian.txt
  cat src/iop/denoiseprofile.c | grep -E '{"' | grep "iso ${iso}\"" | tr -d "{}()\"" | cut -d '_' -f2 | cut -d ',' -f11 | nl > gaussian.txt

  join cams.txt poissonian.txt > tmp.txt
  join tmp.txt gaussian.txt > noise_${iso}.dat

  echo -n "${iso} " >> poissonian_all.txt
  cat poissonian.txt | awk '{print $2}' | tr "\n" " " >> poissonian_all.txt
  echo  "" >> poissonian_all.txt

  echo -n "${iso} " >> gaussian_all.txt
  cat gaussian.txt | awk '{print $2}' | tr "\n" " " >> gaussian_all.txt
  echo  "" >> gaussian_all.txt

  rm -f cams.txt poissonian.txt gaussian.txt tmp.txt

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

gnuplot << EOF
set term pdf fontscale 0.5 size 10, 10
set output 'poissonian.pdf'
title(n) = sprintf("column %d", n)
plot for [i=2:37] './poissonian_all.txt' u 1:(column(i)) w lp t title(i)

set output 'gaussian.pdf'
plot for [i=2:37] './gaussian_all.txt' u 1:(column(i)) w lp t title(i)
EOF

rm -f noise_*.dat poissonian_all.txt gauissan_all.txt


