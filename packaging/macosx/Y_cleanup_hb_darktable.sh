#!/bin/bash
#
# Script to forcefully remove any remainder of previous attemps
#

removableDirs="../../build bin lib libexec share package .config .cache"

for i in $removableDirs; do

    # Delete build directory
    if [[ -d "$i" ]]; then
        echo "Deleting directory $i ... "
        chown -R "$USER" "$i"
        rm -Rf "$i"
    fi

done
