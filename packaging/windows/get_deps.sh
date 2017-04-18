#!/bin/sh

# in theory a single call to ldd should be all that is needed, at least on the systems I have seen that
# seemed to also include all dependencies of the dependencies. However, it doesn't hurt to recurse through
# the files.

INITIAL_FILENAME=$1
OUTFILE=$2
BASE=$3

function process_file()
{
  ldd "${1}" | grep "=> /mingw64/" | sed "s,.* => \(.*\) ([0-9a-fx]*)$,\1," | while read f; do
    # if the file is in the final list already we have to ignore it
    grep "^\"${BASE}/${f}\"$" "${OUTFILE}" > /dev/null && continue

    echo "\"${BASE}/${f}\"" >> "${OUTFILE}"
    process_file "${f}"
  done
}

process_file ${INITIAL_FILENAME}
