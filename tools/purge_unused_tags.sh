#!/bin/bash

#
# Usage: purge_unused_tags [-p]
#        -p  do the purge, otherwise only display unused tags
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
LIBDB="$configdir/library.db"
dryrun=1
library=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ] ; do
  option="$1"
  case ${option} in
  -h|--help)
    echo "Delete unused tags from darktable's database"
    echo "Usage:   $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c|--configdir <path>    path to the darktable config directory"
    echo "                           (default: '${configdir}')"
    echo "  -l|--library <path>      path to the library.db"
    echo "                           (default: '${LIBDB}')"
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

LIBDB="$configdir/library.db"
DATADB="$configdir/data.db"

if [ "$library" != "" ]; then
    LIBDB="$library"
fi

if [ ! -f "$LIBDB" ]; then
    echo "error: library db '${LIBDB}' doesn't exist"
    exit 1
fi

if [ ! -f "$DATADB" ]; then
    echo "error: data db '${DATADB}' doesn't exist"
    exit 1
fi

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

if [ ${dryrun} -eq 0 ]; then
    echo Purging tags...
    echo "$Q1C" | sqlite3
    echo "$Q1" | sqlite3

# since sqlite3 up until version 3.15 didn't support vacuuming
# attached databases we'll do them separately.

   sqlite3 "$LIBDB" "VACUUM; ANALYZE;"
   sqlite3 "$DATADB" "VACUUM; ANALYZE"

else
    echo The following tags are not used:
    echo "$Q1C" | sqlite3
    echo
    echo to really purge from the database call:
    echo "${commandline} --purge"
fi
