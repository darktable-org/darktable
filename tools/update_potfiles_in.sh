#!/bin/bash

SOURCES=$(
    (
	find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v src/external | grep -v gegl-operations
	find build/src/ -name '*.c' -o -name '*.cc' -o -name '*.h'
    ) | fgrep -v -f po/POTFILES.skip | sort
)

egrep -l '[^a-z_]_\(' $SOURCES > po/POTFILES.in.2
if diff po/POTFILES.in po/POTFILES.in.2 >/dev/null 2>&1 ; then
    rm -f po/POTFILES.in.2
else
    mv po/POTFILES.in.2 po/POTFILES.in
fi
