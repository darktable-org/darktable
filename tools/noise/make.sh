NP=~/vcs/darktable/tools/noise/noiseprofile
#for i in *.pfm
for i in iso6400_wb.pfm
do
  echo "profiling : $i"
  $NP $i > ${i%pfm}dat
  echo "plotting  : $i"
  gnuplot << EOF
  set term pdf
  set print "${i%pfm}fit"
  set output "${i%pfm}pdf"
  plot "${i%pfm}dat" u 1:2 w l lw 4 title "noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  set output "${i%.pfm}_histogram.pdf"
  plot "${i%pfm}dat" u 1:5 w l lw 4 title "histogram ${i%.pfm}", '' u 1:6 w l lw 4, '' u 1:7 w l lw 4
  f1(x) = (a1*x)**b
  f2(x) = (a2*x)**b
  f3(x) = (a3*x)**b
  a1=0.005;a2=0.005;a3=0.005;b=1.4;
  fit f2(x) "${i%pfm}dat" u 1:9  via a2, b
  fit f1(x) "${i%pfm}dat" u 1:8  via a1
  fit f3(x) "${i%pfm}dat" u 1:10 via a3
  print a1, a2, a3, b
  g1(x) = 1/a1 * x**(1/b)
  g2(x) = 1/a2 * x**(1/b)
  g3(x) = 1/a3 * x**(1/b)
  set output "${i%.pfm}_cdf.pdf"
  plot "${i%pfm}dat" u 1:8 w l lw 4 title "cdf ${i%.pfm}", '' u 1:9 w l lw 4, '' u 1:10 w l lw 4, f1(x) lt 1, f2(x) lt 2, f3(x) lt 3
EOF
  $NP $i -c $(cat ${i%pfm}fit) > ${i%.pfm}_flat.dat
  echo "flattened : $i"
  gnuplot << EOF
  set term pdf
  set output "${i%.pfm}_flat.pdf"
  plot "${i%.pfm}_flat.dat" u 1:2 w l lw 4 title "flat noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
EOF
done
