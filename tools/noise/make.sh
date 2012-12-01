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
# plot "${i%pfm}dat" u 1:2 w l lw 4 title "noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  plot "${i%pfm}dat" u 1:(log(\$5)) w l lw 4 title "histogram ${i%.pfm}", '' u 1:(log(\$6)) w l lw 4, '' u 1:(log(\$7)) w l lw 4

# f2(x) = a0 + a1*x + a2*x**2 + a3*x**3 +a4*x**4 + a5*x**5
# f1(x) = f2(b*x)
# f3(x) = f2(c*x)
# g2(x) = b0 + b1*x + b2*x**2 + b3*x**3 +b4*x**4 + b5*x**5

  # gamma part: photon noise; polynomial: chip (dark current/readout)
# f2(x) = a0*x**a1 + (1.-x)*(a2*x + a3*x**2 + a4*x**3 +a5*x**4)
# f1(x) = b*a0*x**a1 + (1.-x)*(a2*x + a3*x**2 + a4*x**3 +a5*x**4) 
# f3(x) = c*a0*x**a1 + (1.-x)*(a2*x + a3*x**2 + a4*x**3 +a5*x**4) 

#  g2(x) = b0*x**b1 + (1.-x)*(b2*x + b3*x**2 + b4*x**3 +b5*x**4)
#  g1(x) = d*b0*x**b1 + (1.-x)*(b2*x + b3*x**2 + b4*x**3 +b5*x**4)
#  g3(x) = e*b0*x**b1 + (1.-x)*(b2*x + b3*x**2 + b4*x**3 +b5*x**4)

# a0 = 1; a1 = 0.00015; a2 = 0.80; a3 =-1.35; a4 = 1.34; a5 =-0.48; b=1; c=1; d=1; e=1;
# a0 = 1; a1 = 0.1; a2 = 0.1; a3 = 0.1; a4 = 0.1; a5 = 0.1; b = 1; c = 1; d=1; e=1;
# b0 = 1; b1 = 0.1; b2 = 0.1; b3 = 0.1; b4 = 0.1; b5 = 0.1;

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

# fit f2(x) "${i%pfm}dat" u 1:9:(1.-\$1) via a0,a1,a2,a3,a4,a5
# set xrange [0:0.35]
# fit f1(x) "${i%pfm}dat" u 1:8:(1.-\$1)  via b
# fit f3(x) "${i%pfm}dat" u 1:10:(1.-\$1) via c
# print a0, a1, a2, a3, a4, a5, b, c
#  print a0, a1, a2, b, c
# set xrange [0:1]
# plot "${i%pfm}dat" u 1:8 w l lw 4 title "cdf ${i%.pfm}", '' u 1:9 w l lw 4, '' u 1:10 w l lw 4, f1(x) lt 1, f2(x) lt 2, f3(x) lt 3
# set xrange [0:0.3]
# plot "${i%pfm}dat" u 1:(\$8-f1(\$1)) w l lw 4 title "cdf ${i%.pfm}", '' u 1:(\$9-f2(\$1)) w l lw 4, '' u 1:(\$10-f3(\$1)) w l lw 4
# # invert f:
# set xrange [0:1]
# FIT_MAXITER=50
# FIT_LIMIT=1e-10
# fit g2(x) "${i%pfm}dat" u (f2(\$1)):1 via b0,b1,b2,b3,b4,b5
# fit g1(x) "${i%pfm}dat" u (f1(\$1)):1 via d
# fit g3(x) "${i%pfm}dat" u (f3(\$1)):1 via e
# plot g1(x) w l title 'conversion curve', g2(x), g3(x)
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
#  set xrange [0:0.5]
#  plot "${i%pfm}dat" u 1:(\$8-f1(\$1)) w l lw 4 title "cdf ${i%.pfm}", '' u 1:(\$9-f2(\$1)) w l lw 4, '' u 1:(\$10-f3(\$1)) w l lw 4
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
