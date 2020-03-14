#!/bin/bash

#
# Usage: purge_non_existing_images [-p]
#        -p  do the purge, otherwise only display non existing images
#

if ! which sqlite3 > /dev/null; then
    echo "error: please install sqlite3 binary".
    exit 1
fi

if pgrep -x "darktable" > /dev/null ; then
    echo "error: darktable is running, please exit first"
    exit 1
fi

configdir="$HOME/.config/darktable"
DBFILE="$configdir/library.db"
dryrun=1
library=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ] ; do
  option="$1"
  case ${option} in
  -h|--help)
    echo "Delete non existing images from darktable's database"
    echo "Usage:   $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c|--configdir <path>    path to the darktable config directory"
    echo "                           (default: '${configdir}')"
    echo "  -l|--library <path>      path to the library.db"
    echo "                           (default: '${DBFILE}')"
    echo "  -p|--purge               actually delete the tags instead of just finding them"
    exit 0
    ;;
  -l|--library)
    library="$2"
    shift
    ;;
  -c|--configdir)
    configdir="$2"
    shift
    ;;
  -p|--purge)
    dryrun=0
    ;;
  *)
    echo "warning: ignoring unknown option $option"
    ;;
  esac
    shift
done

DBFILE="$configdir/library.db"

if [ "$library" != "" ]; then
    DBFILE="$library"
fi

if [ ! -f "$DBFILE" ]; then
    echo "error: library db '${DBFILE}' doesn't exist"
    exit 1
fi

TMPFILE=$(mktemp -t tmp.XXXXXXXXXX)
QUERY="SELECT A.id,B.folder,A.filename FROM images AS A JOIN film_rolls AS B ON A.film_id = B.id"

sqlite3 $DBFILE "$QUERY" > "$TMPFILE"

echo "Removing the following non existent file(s):"

cat "$TMPFILE" | while read -r result
do
  ID=$(echo "$result" | cut -f1 -d"|")
  FD=$(echo "$result" | cut -f2 -d"|")
  FL=$(echo "$result" | cut -f3 -d"|")
  if ! [ -f "$FD/$FL" ];
  then
    echo "  $FD/$FL with ID = $ID"

    if [ $dryrun -eq 0 ]; then
        for table in images meta_data; do
            sqlite3 "$DBFILE" "DELETE FROM $table WHERE id=$ID"
        done

        for table in color_labels history masks_history selected_images tagged_images history_hash module_order; do
            sqlite3 "$DBFILE" "DELETE FROM $table WHERE imgid=$ID"
        done
    fi
  fi
done
rm "$TMPFILE"


if [ $dryrun -eq 0 ]; then
    # delete now-empty filmrolls
    sqlite3 "$DBFILE" "DELETE FROM film_rolls WHERE (SELECT COUNT(A.id) FROM images AS A WHERE A.film_id=film_rolls.id)=0"
    sqlite3 "$DBFILE" "VACUUM; ANALYZE"
else
    echo
    echo Remove following now-empty filmrolls:
    sqlite3 "$DBFILE" "SELECT folder FROM film_rolls WHERE (SELECT COUNT(A.id) FROM images AS A WHERE A.film_id=film_rolls.id)=0"

    echo
    echo to really remove non existing images from the database call:
    echo "${commandline} --purge"
fi
