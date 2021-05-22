#!/bin/bash

echo "THIS SCRIPT IS DEPRECATED"
echo
echo "It must be updated to ensure all strings are keep as-is"
echo "and especially the SQL statements"
exit 1

# this uses clang-format to standardize our indentation/braces etc

# change if your executable is named different
CLANG_FORMAT=clang-format-9


# add all the files and directories that may not be reformatted, relative to src/
IGNORE_SET=(
    external
    common/noiseprofiles.h
    common/colormatrices.c
    common/nvidia_gpus.h
)

####################################################################################

function join { local IFS="$1"; shift; echo "$*"; }

IGNORE_SET=(${IGNORE_SET[@]/#/^src/})
IGNORE_STRING=$(join \| "${IGNORE_SET[@]}")

SOURCES=$(find src | egrep -v ${IGNORE_STRING} | egrep "\.h$|\.hh$|\.c$|\.cc$")

for FILE in $SOURCES
do
  ${CLANG_FORMAT} -i $FILE
done
