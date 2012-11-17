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
  plot "${i%pfm}dat" u 1:2 w l lw 4 title "noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  set output "${i%.pfm}_histogram.pdf"
  plot "${i%pfm}dat" u 1:(log(\$5)) w l lw 4 title "histogram ${i%.pfm}", '' u 1:(log(\$6)) w l lw 4, '' u 1:(log(\$7)) w l lw 4

  f2(x) = a0 + a1*x + a2*x**2 + a3*x**3 +a4*x**4 + a5*x**5
  f1(x) = f2(b*x)
  f3(x) = f2(c*x)
  a0 = 1; a2 = 1; a3 = 0; a4 = 0; a5 = 0; b = 1; c = 1;
  fit f2(x) "${i%pfm}dat" u 1:9  via a0,a1,a2
  set xrange [0:120]
  fit f1(x) "${i%pfm}dat" u 1:8  via b
  fit f3(x) "${i%pfm}dat" u 1:10 via c
# print a0, a1, a2, a3, a4, a5, b, c
  print a0, a1, a2, b, c
  set xrange [0:300]
  set output "${i%.pfm}_cdf.pdf"
  plot "${i%pfm}dat" u 1:8 w l lw 4 title "cdf ${i%.pfm}", '' u 1:9 w l lw 4, '' u 1:10 w l lw 4, f1(x) lt 1, f2(x) lt 2, f3(x) lt 3
  set xrange [0:150]
  plot "${i%pfm}dat" u 1:(\$8-f1(\$1)) w l lw 4 title "cdf ${i%.pfm}", '' u 1:(\$9-f2(\$1)) w l lw 4, '' u 1:(\$10-f3(\$1)) w l lw 4
#   f1(x) = (a1*x)**b
#   f2(x) = (a2*x)**b
#   f3(x) = (a3*x)**b
#   a1=0.005;a2=0.005;a3=0.005;b=1.4;
#   fit f2(x) "${i%pfm}dat" u 1:9  via a2, b
#   set xrange [50:200]
#   fit f1(x) "${i%pfm}dat" u 1:8  via a1
#   fit f3(x) "${i%pfm}dat" u 1:10 via a3
#   print a1, a2, a3, b
#   g1(x) = 1/a1 * x**(1/b)
#   g2(x) = 1/a2 * x**(1/b)
#   g3(x) = 1/a3 * x**(1/b)
#  set xrange [0:300]
#  set output "${i%.pfm}_cdf.pdf"
#  plot "${i%pfm}dat" u 1:8 w l lw 4 title "cdf ${i%.pfm}", '' u 1:9 w l lw 4, '' u 1:10 w l lw 4, f1(x) lt 1, f2(x) lt 2, f3(x) lt 3
#  set xrange [0:150]
#  plot "${i%pfm}dat" u 1:(\$8-f1(\$1)) w l lw 4 title "cdf ${i%.pfm}", '' u 1:(\$9-f2(\$1)) w l lw 4, '' u 1:(\$10-f3(\$1)) w l lw 4
EOF
  $NP $i -c $(cat ${i%pfm}fit) > ${i%.pfm}_flat.dat 2> ${i%.pfm}_curves.dat
  echo "flattened : $i"
  gnuplot << EOF
  set term pdf
  set output "${i%.pfm}_flat.pdf"
  plot "${i%.pfm}_flat.dat" u 1:2 w l lw 4 title "flat noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  plot "${i%pfm}dat" u 1:(log(\$5)) w l lw 4 title "flat histogram ${i%.pfm}", '' u 1:(log(\$6)) w l lw 4, '' u 1:(log(\$7)) w l lw 4
  plot "${i%.pfm}_curves.dat" u 0:1 w l lw 4 title "conversion curves", '' u 0:2 w l lw 4, '' u 0:3 w l lw 4
EOF
done
