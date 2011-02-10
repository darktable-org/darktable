#!/bin/bash

# this uses astyle to standardize our indentation/braces etc

SOURCES=$(find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v LibRaw | grep -v src/rawspeed | grep -v gegl-operations)

for i in $SOURCES
do
  astyle --style=bsd --indent=spaces=2 --indent-switches < $i > dreggn.c
  mv dreggn.c $i
done

