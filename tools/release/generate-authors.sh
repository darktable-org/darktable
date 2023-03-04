#!/bin/bash
#
#   This file is part of darktable,
#   Copyright (C) 2019-2020 darktable developers.
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

CDPATH=
BASE=$1
HEAD=${2:-HEAD}

# minimal number of commits an author should have, to be listed at all
SHORTLOG_THRESHOLD=1

# minimal number of commits translator should have, to be listed
TRANSLATOR_THRESHOLD=1

# minimal number of commits regular contributor should have, to be listed
CONTRIBUTOR_THRESHOLD=4

# these will not be shown in contributors section.
ALL_DEVELOPERS=("Aldric Renaudin"
                "Alexandre Prokoudine"
                "Christian Tellefsen"
                "Edouard Gomez"
                "Henrik Andersson"
                "James C. McPherson"
                "José Carlos García Sogo"
                "Jérémy Rosen"
                "Pascal Obry"
                "Pascal de Bruijn"
                "Pedro Côrte-Real"
                "Peter Budai"
                "Roman Lebedev"
                "Simon Spannagel"
                "Stefan Schöfegger"
                "Tobias Ellinghaus"
                "Ulrich Pegelow"
                "johannes hanika"
                "parafin")

function short-log()
{
    local RANGE="$1"
    local MIN=$2
    local PATHS="$3"

    git shortlog -ns $RANGE -- $PATHS |
        while read num auth; do
            if [ $num -ge $MIN ]; then
                echo $auth;
            fi;
        done
}

function for-submodule()
{
    local SUBPATH=$1
    local MIN=$2

    local SHEAD=$(git log --patch -1 $HEAD -- $SUBPATH | grep "Subproject commit" | tail -1 | cut -d' ' -f3)

    SRANGE=""

    if [ -z $BASE ]; then
        SRANGE=$SHEAD
    else
        local SBASE=$(git log --patch -1 $BASE -- $SUBPATH | grep "Subproject commit" | tail -1 | cut -d' ' -f3)

        if [[ -z $SBASE ]]; then
            SRANGE="$SHEAD"
        else
            SRANGE="$SBASE..$SHEAD"
        fi
    fi

    (
        cd $SUBPATH
        short-log "$SRANGE" $MIN
    )
}

function is-developer()
{
    local AUTH="$1"

    for i in "${ALL_DEVELOPERS[@]}"; do
        if [ "$i" == "$AUTH" ]; then
            return 1
        fi
    done

    return 0
}

RANGE=$HEAD

if [ ! -z $BASE ]; then
    RANGE=$BASE..$HEAD
fi

echo "* developers:"
short-log $RANGE $SHORTLOG_THRESHOLD |
    while read name; do
        is-developer "$name"
        if [ $? == 1 ]; then
            echo $name
        fi
    done

echo
echo "* translators:"
short-log $RANGE $TRANSLATOR_THRESHOLD "./po/*.po ./doc/man/po/*.po ./doc/usermanual/po/*.po"

echo
echo "* contributors (at least $CONTRIBUTOR_THRESHOLD commits):"
short-log $RANGE $CONTRIBUTOR_THRESHOLD |
    while read name; do
        is-developer "$name"
        if [ $? == 0 ]; then
            echo $name
        fi
    done

# handle sub-modules if any

if [ -f .gitmodules ]; then
    cat .gitmodules | grep path |
        while read x c module; do
            MODULE_NAME=$(basename $module)
            echo
            echo "* Sub-module $MODULE_NAME contributors (at least 1 commit):"
            for-submodule $module $SHORTLOG_THRESHOLD
        done
fi

echo
echo "And all those of you that made previous releases possible"
