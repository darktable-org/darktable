#!/bin/bash
#
#   This file is part of darktable,
#   copyright (c) 2009--2010 johannes hanika.
#
#   darktable is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   darktable is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
#

header=$(cat << EOF
<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<!-- created with darktable utility scritps, http://darktable.sf.net/ -->

<svg
   xmlns:svg="http://www.w3.org/2000/svg"
   xmlns="http://www.w3.org/2000/svg"
   version="1.1"
   width="1000"
   height="500"
   id="svg2">
  <defs
     id="defs4" />
  <g
     id="layer1">
EOF
)

rect=$(cat << EOF
    <rect
       width="REP_WIDTH"
       height="REP_HEIGHT"
       x="REP_X"
       y="REP_Y"
       id="rectREP_ID"
       style="fill:#cacac6;fill-opacity:1;fill-rule:evenodd;stroke:#aca1ef;stroke-width:1;stroke-linecap:round;stroke-linejoin:round;stroke-miterlimit:4;stroke-opacity:1;stroke-dasharray:none;stroke-dashoffset:0" />
EOF
)

text=$(cat << EOF
    <text
       x="REP_X"
       y="REP_Y"
       id="textREP_ID"
       xml:space="preserve"
       style="font-size:8px;font-style:normal;font-weight:normal;fill:#000000;fill-opacity:1;stroke:none;font-family:Bitstream Vera Sans"><tspan
         x="REP_X"
         y="REP_Y"
         id="tspanREPD_ID">REP_TEXT</tspan></text>
EOF
)

footer=$(cat << EOF
  </g>
</svg>
EOF
)



# input file
log=$1

# output file
output="control.svg"

# output file header
echo "$header" > $output

# collect thread ids:
ids=$(grep -E "^\[run_job" $log | cut -f 2 -d ' ' | sort | uniq)

# start time:
start_time=$(grep -E "^\[run_job" $log | cut -f 3 -d ' ' | head -1)

# make a new bar for every thread
offset=0
for id in $ids
do
  # get index to start sort (begin of time)
  len=${#id}
  start=$(awk -v a="$(head -1 $log)" -v b="$id" 'BEGIN{print index(a,b)}')
  offs=$[$len + $start]

  # get sorted outputs by time (should already be sorted, in fact)
  numlines=$(cat $log | grep -E '^\[run_job.\] '$id | sort -n -k $offs | wc -l)
  for i in $(seq 0 2 $[$numlines-1])
  do
    line1=$(cat $log | grep -E '^\[run_job.\] '$id | sort -n -k $offs | tail -$[$numlines - $i] | head -1)
    line2=$(cat $log | grep -E '^\[run_job.\] '$id | sort -n -k $offs | tail -$[$numlines - $i - 1] | head -1)

    # get two lines, assert +- and job description string
    descr=$(echo $line1 | cut -f 4- -d" ")
    on=$(echo $line1 | cut -f 3 -d" ")
    off=$(echo $line2 | cut -f 3 -d" ")
    x_on=$(echo "100 * ($on - $start_time)" | bc -l)
    x_wd=$(echo "100 * ($off - $start_time) - $x_on" | bc -l)
    y=$[$offset * 30]
    ht=20
    yt=$[$y+10]

    # run rect and text through sed
    echo "$rect" | sed -e "s/REP_ID/$offset/g" -e "s/REP_X/$x_on/g" -e "s/REP_WIDTH/$x_wd/g" -e "s/REP_Y/$y/g" -e "s/REP_HEIGHT/$ht/g" >> $output
    echo "$text" | sed -e "s/REP_ID/$offset/g" -e "s/REP_X/$x_on/g" -e "s/REP_Y/$yt/g" -e "s/REP_TEXT/$descr/g" >> $output

  done
  offset=$[offset + 1]
done

# output file footer 
echo "$footer" >> $output
