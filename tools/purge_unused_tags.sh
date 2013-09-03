#!/bin/sh

#
# Usage: purge_unused_tags [-d]
#        -d  do the purge, otherwise only display unused tags
#

DBFILE=~/.config/darktable/library.db

# tags not used
Q1C="SELECT name FROM tags WHERE id NOT IN (SELECT tagid FROM tagged_images);"
Q1="DELETE FROM tags WHERE id NOT IN (SELECT tagid FROM tagged_images);"

# tagxtag for missing tags
Q2="DELETE FROM tagxtag WHERE id1 NOT IN (SELECT id FROM TAGS) OR id2 NOT IN (SELECT id FROM tags);"

if [ "$1" = "-p" ]; then
    echo Purging tags...
    echo "$Q1C" | sqlite3 $DBFILE
    echo "$Q1" | sqlite3 $DBFILE
    echo "$Q2" | sqlite3 $DBFILE
else
    echo "$Q1C" | sqlite3 $DBFILE
fi
