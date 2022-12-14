#!/bin/bash

unset CDPATH

if [[ -z $1 ]]; then
    echo "usage: $0 <ref-previous-release"
    echo ""
    echo "   the ref is generally the branch point of the current release."
    echo "   for example release-4.1.0"
    exit 1
fi

REF=$1

#  For darktable

DT=$(git log --no-merges --oneline $REF..HEAD | wc -l)

echo darktable $DT

#  For rawspeed

#  get darktable submodule commits
DRSFIRST=$(git log --no-merges --oneline $REF..HEAD src/external/rawspeed |
               tail -1 | cut -d' ' -f1)
DRSLAST=$(git log --no-merges --oneline $REF..HEAD src/external/rawspeed |
              head -1 | cut -d' ' -f1)

#  get corresponding commits in rawspspeed
RSFIRST=$(git show $DRSFIRST -- src/external/rawspeed |
              grep '^-Subproject' | cut -d' ' -f3)

RSLAST=$(git show $DRSLAST -- src/external/rawspeed |
              grep '^+Subproject' | cut -d' ' -f3)

cd src/external/rawspeed
RS=$(git log --no-merges --oneline $RSFIRST..$RSLAST | wc -l)

echo rawspeed $RS

echo Total: $(expr $DT + $RS)
