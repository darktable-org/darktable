#!/bin/sh

#
# Usage: purge_unused_tags [-p]
#        -p  do the purge, otherwise only display unused tags
#

LIBDB=$HOME/.config/darktable/library.db
DATADB=$HOME/.config/darktable/data.db

# tags not used
Q1C="
ATTACH DATABASE \"$LIBDB\" as lib;
ATTACH DATABASE \"$DATADB\" as data;
SELECT name FROM data.tags WHERE id NOT IN (SELECT tagid FROM tagged_images);
"

Q1="
ATTACH DATABASE \"$LIBDB\" as lib;
ATTACH DATABASE \"$DATADB\" as data;
DELETE FROM data.tags WHERE id NOT IN (SELECT tagid FROM tagged_images);
"

if [ ! -f "$LIBDB" ]; then
    echo missing \""$LIBDB"\" file
    exit 1
fi

if [ ! -f "$DATADB" ]; then
    echo missing \""$DATADB"\" file
    exit 1
fi

if [ "$1" = "-p" ]; then
    echo Purging tags...
    echo "$Q1C" | sqlite3
    echo "$Q1" | sqlite3
else
    echo The following tags are not used:
    echo "$Q1C" | sqlite3
    echo
    echo to really purge from the database call:
    echo "$0" -p
fi
