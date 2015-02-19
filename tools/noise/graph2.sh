#!/bin/sh

# remove whitespace and braces,
# filter out panasonic and powershot etc because they usually blow up the plot ranges
# also only choose nikon d800 and canon 5d mark 2 for comparison
cat src/common/noiseprofiles.h | grep '^[ \t]*{"'  | sed 's/\s*,\s*/,/g' | tr " " "_" | tr -d "{}()\"" | tr "/" "_" | \
  grep -v "PowerShot" | \
  grep -v "Panasonic" | \
  grep -v "DYNAX" | \
  grep -v "NEX-C3" | \
  grep -v "pentax_k-x" | \
  grep -v "D5100" | \
  awk -F, "{if (\$3 == \"NIKON_D800\" || \$3 == \"Canon_EOS_5D_Mark_II\") { print \$0; }}" \
  > trim.txt

# get all:
filter="cat"
# only canon:
# filter="grep Canon"
# only canon mark 2+3
# filter="grep Mark"
# only Nikon
# filter="grep NIKON"

# get list of cameras (use exif model only):
cams=$(cat trim.txt | $filter | awk -F, "{ print \$3; }" | sort | uniq | tr "\n" " ") # could print \$2,\$3 to get maker and model
# cams=$(cat trim.txt | awk -F, "{ print \$1; }" | sed 's/_iso_.*$//' | sort | uniq | tr "/" "_" | tr "\n" " ") # use first field to get commented double measurements as separate data points
# filtered:
# cams=$(cat trim.txt | $filter | awk -F, "{ print \$1; }" | sed 's/_iso_.*$//' | sort | uniq | tr "/" "_" | tr "\n" " ")

# is actually num cams + 1, because the first column is iso
num_cams=$(( $(echo $cams | wc -w) + 1))

# get sorted list of iso values:
isos=$(cat trim.txt | $filter | awk -F, "{ print \$4; }" | sort -g | uniq)
# isos="200 400 800 1600"


# we want output files to look like:
# iso  cam1 cam2 cam3 cam4
# iso1 ..
# iso2   ...
#
# so we first output the header:
echo "iso ${cams}" > data.txt
echo "iso ${cams}" > data2.txt

for iso in $isos
do
  echo -n "$iso " >> data.txt
  echo -n "$iso " >> data2.txt
  echo "collecting iso $iso .."
  for cam in $cams
  do
    # echo "looking for cam $cam"
    # collect green poissonian value for this camera and iso ($6)
    a=$(cat trim.txt | awk -F, "{if ((\$3 == \"$cam\") && (\$4 == $iso)) { print \$6; } }" | tr -d "\n")
    # same for gaussian one
    b=$(cat trim.txt | awk -F, "{if ((\$3 == \"$cam\") && (\$4 == $iso)) { print \$9; } }" | tr -d "\n")
    if [ "$a" = "" ]
    then
      # echo "no value found for $cam iso $iso"
      a="?"
    fi
    echo -n "$a " >> data.txt
    if [ "$b" = "" ]
    then
      # echo "no value found for $cam iso $iso"
      b="?"
    fi
    echo -n "$b " >> data2.txt
  done
  echo "" >> data.txt
  echo "" >> data2.txt
done

# now for some plotting pleasure:
gnuplot << EOF
set term pdf fontscale 0.5 size 10, 10
set output 'poissonian.pdf'
set mxtics 10
set grid mxtics xtics ytics
set logscale xy
# set yrange [0:1e-5]
set datafile missing "?"
set key autotitle columnhead
set xlabel 'iso speed'
set ylabel 'photon noise'
plot for [i=2:${num_cams}] './data.txt' u 1:(column(i)) w lp title column(i)

set output 'gaussian.pdf'
unset logscale
set logscale x
set yrange [-1e-6:1e-5]
set ylabel 'sensor noise'
plot for [i=2:${num_cams}] './data2.txt' u 1:(column(i)) w lp title column(i)
EOF

rm -f trim.txt data.txt data2.txt
