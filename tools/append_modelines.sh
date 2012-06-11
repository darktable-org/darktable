#!/bin/bash

# this appends modelines to all source and header files to make sure kate and vim know how to format their stuff.

SOURCES=$(find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v LibRaw | grep -v src/rawspeed | grep -v gegl-operations)

MODELINES=`cat ./editor_modelines.txt`
for i in $SOURCES
do
  if [ grep "$MODELINES" "$i" ] ; then
    echo "$i already contains current modelines"
    exit 
    # WARNING: This only works as expected with exactly TWO modelines. Change this whenevery you add (or remove) a modeline
  elif [ tail -2 "$i" | grep vim ] ; then
    TEMPFILE=`tempfile`
    head -2 
    

    
  

  astyle --style=bsd --indent=spaces=2 --indent-switches < $i > dreggn.c
  mv dreggn.c $i
done

# vim: et,sw=2,ts=2
