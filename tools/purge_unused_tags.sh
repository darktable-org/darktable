#!/bin/sh

#
# Usage: purge_unused_tags [-p]
#        -p  do the purge, otherwise only display unused tags
#

DBFILE=~/.config/darktable/library.db

# tags not used
Q1C="SELECT name FROM tags WHERE id NOT IN (SELECT tagid FROM tagged_images);"
Q1="DELETE FROM tags WHERE id NOT IN (SELECT tagid FROM tagged_images);"

if [ "$1" = "-p" ]; then
    echo Purging tags...
    echo "$Q1C" | sqlite3 $DBFILE
    echo "$Q1" | sqlite3 $DBFILE
else
    echo "$Q1C" | sqlite3 $DBFILE
    echo
    echo to really purge from the database call:
    echo $0 -p
fi
