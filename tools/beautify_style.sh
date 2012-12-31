#!/bin/sh

# this uses astyle to standardize our indentation/braces etc

# Sources have changed since this was written. LibRaw and rawspeed are now in src/external, so here is the updated SOURCES line
SOURCES=$(find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v src/external | grep -v gegl-operations)

for i in $SOURCES
do
  astyle --style=bsd --indent=spaces=2 --indent-switches < $i > dreggn.c
  mv dreggn.c $i
done

