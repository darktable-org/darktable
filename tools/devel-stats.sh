#!/bin/bash
#
#   This file is part of darktable,
#   copyright (c) 2019 pascal obry
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
# For example to prepare RELEASE_NOTES.md commit section:
#
# ./tools/devel-stats.sh release-2.6.0 master

CDPATH=
BASE=$1
HEAD=${2:-HEAD}

function short-log()
{
    local RANGE="$1"

    git log --oneline $RANGE | wc -l
}

function for-submodule()
{
    local SUBPATH=$1

    local SHEAD=$(git log --patch -1 $HEAD -- $SUBPATH | grep "Subproject commit" | tail -1 | cut -d' ' -f3)

    SRANGE=""

    if [ -z $BASE ]; then
        SRANGE=$SHEAD
    else
        local SBASE=$(git log --patch -1 $BASE -- $SUBPATH | grep "Subproject commit" | tail -1 | cut -d' ' -f3)
        SRANGE="$SBASE..$SHEAD"
    fi

    (
        cd $SUBPATH
        short-log "$SRANGE"
    )
}

RANGE=$HEAD

if [ ! -z $BASE ]; then
    RANGE=$BASE..$HEAD
fi

echo "* Nb commits:"
short-log $RANGE

# handle sub-modules if any

if [ -f .gitmodules ]; then
    cat .gitmodules | grep path |
        while read x c module; do
            MODULE_NAME=$(basename $module)
            echo
            echo "* Sub-module $MODULE_NAME nb commits:"
            for-submodule $module
        done
fi
