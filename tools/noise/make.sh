NP=~/vcs/darktable/tools/noise/noiseprofile
for i in *.pfm
do
  echo "profiling : $i"
  $NP $i > ${i%pfm}dat
  echo "plotting  : $i"
  gnuplot << EOF
  set term pdf
  set output "${i%pfm}pdf"
  plot "${i%pfm}dat" u 1:2 w l lw 4 title "noise levels ${i%.pfm}", '' u 1:3 w l lw 4, '' u 1:4 w l lw 4
  set output "${i%.pfm}_histogram.pdf"
  plot "${i%pfm}dat" u 1:5 w l lw 4 title "histogram ${i%pfm}", '' u 1:6 w l lw 4, '' u 1:7 w l lw 4
EOF
done
