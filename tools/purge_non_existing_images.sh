#!/bin/bash

#
# Usage: purge_non_existing_images [-p]
#        -p  do the purge, otherwise only display non existing images
#

if ! command -v sqlite3 >/dev/null
then
    echo "error: please install sqlite3 binary".
    exit 1
fi

if pgrep -x "darktable" >/dev/null
then
    echo "error: darktable is running, please exit first"
    exit 1
fi

configdir="${HOME}/.config/darktable"
DBFILE="${configdir}/library.db"
dryrun=1
library=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ]
do
    option="$1"
    case "$option" in
    -h | --help)
        echo "Delete non existing images from darktable's database"
        echo "Usage:   ${0} [options]"
        echo ""
        echo "Options:"
        echo "  -c|--configdir <path>    path to the darktable config directory"
        echo "                           (default: '${configdir}')"
        echo "  -l|--library <path>      path to the library.db"
        echo "                           (default: '${DBFILE}')"
        echo "  -p|--purge               actually delete the tags instead of just finding them"
        exit 0
        ;;
    -l | --library)
        library="$2"
        shift
        ;;
    -c | --configdir)
        configdir="$2"
        shift
        ;;
    -p | --purge)
        dryrun=0
        ;;
    *)
        echo "warning: ignoring unknown option ${option}"
        ;;
    esac
    shift
done

DBFILE="$configdir/library.db"

if [ "$library" != "" ]
then
    DBFILE="$library"
fi

if [ ! -f "$DBFILE" ]
then
    echo "error: library db '${DBFILE}' doesn't exist"
    exit 1
fi

QUERY="SELECT images.id, film_rolls.folder || '/' || images.filename FROM images JOIN film_rolls ON images.film_id = film_rolls.id"

echo "Removing the following non existent file(s):"

while read -r -u 9 id path
do
    if ! [ -f "$path" ]
    then
        echo "  ${path} with ID = ${id}"
        ids="${ids+${ids},}${id}"
    fi
done 9< <(sqlite3 -separator $'\t' "$DBFILE" "$QUERY")

if [ "$dryrun" -eq 0 ]
then
    for table in images meta_data
    do
        sqlite3 "$DBFILE" <<< "DELETE FROM ${table} WHERE id IN ($ids)"
    done

    for table in color_labels history masks_history selected_images tagged_images history_hash module_order
    do
        sqlite3 "$DBFILE" <<< "DELETE FROM ${table} WHERE imgid in ($ids)"
    done

    # delete now-empty film rolls
    sqlite3 "$DBFILE" "DELETE FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"
    sqlite3 "$DBFILE" "VACUUM; ANALYZE"
else
    echo
    echo Remove following now-empty filmrolls:
    sqlite3 "$DBFILE" "SELECT folder FROM film_rolls WHERE NOT EXISTS (SELECT 1 FROM images WHERE images.film_id = film_rolls.id)"

    echo
    echo to really remove non existing images from the database call:
    echo "${commandline} --purge"
fi
