#!/bin/bash

# grab path to our binary (have to call it with full path in the directory where your pictures are):
path=${0%/*}
# find the compiled executable there (run ./build.sh in that directory if it's not there)
NP=$path/noiseprofile
database=/tmp/noiseprofiletest.db
copyfrom=~/.config/darktable/library.db
echo "copying library.db for testing purposes to ${database}"
if [ ! -e $copyfrom ]
then
  echo "*** please run darktable at least once to create an initial library"
  echo "*** or copy some valid library to ${database} manualy before running."
  echo "*** this is needed so we can setup a testing environment for your new presets."
  exit 1
fi
cp ${copyfrom} ${database}


if [ ! -x $NP -o ! -x ${path}/floatdump ]
then
  echo "*** couldn't find noise profiling binary, please do the following:"
  echo "cd $path"
  echo "./build.sh"
  exit 2
fi

# clear file
> presets.txt

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
 # output preset for dt:
 dir=$(pwd)
 cam=${dir##*/}
 iso=${i%.pfm}
 a=$(cat ${i%pfm}fit | cut -f2 -d' ')
 b=$(cat ${i%pfm}fit | cut -f5 -d' ')

 echo "{N_(\"${cam} ${iso}\"),       \"\",      \"\",              0, 0,         {1.0f, 1.0f, {$a, $a, $a}, {$b, $b, $b}}}," >> presets.txt
 # use $path/floatdump to instert test preset, like
 bin1=$(echo 1.0f | $path/floatdump)
 bina=$(echo $a | $path/floatdump)
 binb=$(echo $b | $path/floatdump)
 # schema for this table is:
 # CREATE TABLE presets (name varchar, description varchar, operation varchar, op_version integer, op_params blob, enabled integer, blendop_params blob, model varchar, maker varchar, lens varchar, iso_min real, iso_max real, exposure_min real, exposure_max real, aperture_min real, aperture_max real, focal_length_min real, focal_length_max real, writeprotect integer, autoapply integer, filter integer, def integer, isldr integer, blendop_version integer);
 echo "insert into presets values ('test ${cam} ${iso}', '', 'denoiseprofile', 1, X'${bin1}${bin1}${bina}${bina}${bina}${binb}${binb}${binb}', 1, X'00', '', '', '', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4);" | sqlite3 ${database}
done

echo ""
echo "* all done! i inserted your new presets for you to test into ${database}."
echo "* to test them locally, run:"
echo ""
echo "  darktable --library ${database}"
echo ""
echo "* if you're happy with the results, post presets.txt to us."
echo "* if not, probably something went wrong. it's probably a good idea to get in touch"
echo "* so we can help you sort it out."
