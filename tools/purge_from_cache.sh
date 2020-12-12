#!/bin/bash

# MIT License
#
# Copyright (c) 2017 Tobias Ellinghaus
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

# make sure parsed alias will be expanded
shopt -s expand_aliases

. "$(dirname "$0")/common.sh"

if pgrep -x "darktable" > /dev/null ; then
    echo "error: darktable is running, please exit first"
    exit 1
fi

# default values
configdir="$HOME/.config/darktable"
cache_base="${HOME}/.cache/darktable"
library="$configdir/library.db"
dryrun=1
LIBDB=""

# remember the command line to show it in the end when not purging
commandline="$0 $*"

# handle command line arguments
while [ "$#" -ge 1 ] ; do
  option="$1"
  case ${option} in
  -h|--help)
    echo "Delete thumbnails of images that are no longer in darktable's library"
    echo "Usage:   $0 [options]"
    echo ""
    echo "Options:"
    echo "  -c|--cachedir <path>   path to the place where darktable's thumbnail caches are stored"
    echo "                           (default: '${cache_base}')"
    echo "  -d|--configdir <path>    path to the darktable config directory"
    echo "                           (default: '${configdir}')"
    echo "  -l|--library <path>      path to the library.db"
    echo "                           (default: '${library}')"
    echo "  -p|--purge               actually delete the files instead of just finding them"
    exit 0
    ;;
  -l|--library)
    LIBDB="$2"
    shift
    ;;
  -c|--cachedir|--cache_base)
    cache_base="$2"
    shift
    ;;
  -d|--configdir)
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

library="$configdir/library.db"

if [ "$LIBDB" != "" ]; then
    library="$LIBDB"
fi

# set the command to run for each stale file
action="echo found stale mipmap in "
if [ ${dryrun} -eq 0 ]; then
  action="rm --force"
fi

# get absolute canonical path to library. needed for cache dir
library=$(ReadLink "${library}")

if [ ! -f "${library}" ]; then
  echo "error: library db '${library}' doesn't exist"
  exit 1
fi

# the mipmap directory matching the selected library
cache_dir="${cache_base}/mipmaps-$(printf "%s" "${library}" | sha1sum | cut --delimiter=" " --fields=1).d"

if [ ! -d "${cache_dir}" ]; then
  echo "error: cache directory '${cache_dir}' doesn't exist"
  exit 1
fi

# get a list of all image ids from the library
id_list=$(mktemp -t darktable-tmp.XXXXXX)
sqlite3 "${library}" "select id from images order by id" > "${id_list}"

# iterate over cached mipmaps and check for each if the image is in the db
find "${cache_dir}" -type f | while read -r mipmap; do
  # get the image id from the filename
  id=$(echo "${mipmap}" | sed 's,.*/\([0-9]*\).*,\1,')
  # ... and delete it if it's not in the library
  grep "^${id}\$" "${id_list}" > /dev/null || ${action} "${mipmap}"
done

rm --force "${id_list}"

if [ $dryrun -eq 1 ]; then
    echo
    echo to really remove stale thumbnails from the cache call:
    echo "${commandline} --purge"
fi
