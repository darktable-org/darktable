#!/bin/bash

DBFILE=~/.config/darktable/library.db
QUERY="select A.id,B.folder,A.filename from images as A join film_rolls as B on A.film_id = B.id"
sqlite3 $DBFILE "$QUERY"| while read result
do
  ID=$(echo "$result" | cut -f1 -d"|")
  FD=$(echo "$result" | cut -f2 -d"|")
  FL=$(echo "$result" | cut -f3 -d"|")
  if ! [ -f "$FD/$FL" ];
  then
    echo "removing non existent file $FD/$FL"
    # uncomment this dangerous line to actually do it:
    # sqlite3 $DBFILE "delete from images where id=$ID"
  fi
done
