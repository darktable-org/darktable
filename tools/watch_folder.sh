#!/bin/sh

# MIT License
#
# Copyright (c) 2016-2017 Tobias Ellinghaus
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

. "$(dirname "$0")/common.sh"

if [ $# -ne 1 ]; then
  echo "This script watches a folder for new images and imports them into a running instance of darktable"
  echo "Usage: $0 <folder>"
  exit 1
fi

BASE_FOLDER=$(ReadLink "$1")

if [ ! -d "${BASE_FOLDER}"  ]; then
  echo "error accessing directory '$BASE_FOLDER'"
  exit 1
fi

DBUS_SEND=$(which dbus-send)
if [ $? -ne 0 ]; then
  echo "can't find 'dbus-send' in PATH"
  exit 1
fi

INOTIFYWAIT=$(which inotifywait)
if [ $? -ne 0 ]; then
  echo "can't find 'inotifywait' in PATH"
  exit 1
fi

HAVE_LUA=$("${DBUS_SEND}" --print-reply --type=method_call --dest=org.darktable.service /darktable org.freedesktop.DBus.Properties.Get string:org.darktable.service.Remote string:LuaEnabled 2> /dev/null)
if [ $? -ne 0 ]; then
  echo "darktable isn't running or DBUS isn't working properly"
  exit 1
fi

echo "${HAVE_LUA}" | grep "true$" > /dev/null
HAVE_LUA=$?

cleanup()
{
  "${DBUS_SEND}" --type=method_call --dest=org.darktable.service /darktable org.darktable.service.Remote.Lua string:"require('darktable').print('stopping to watch \`${BASE_FOLDER}\'')"
}

if [ ${HAVE_LUA} -eq 0 ]; then
  echo "Using Lua to load images, no error handling but uninterrupted workflow"
  "${DBUS_SEND}" --type=method_call --dest=org.darktable.service /darktable org.darktable.service.Remote.Lua string:"require('darktable').print('watching \`${BASE_FOLDER}\'')"
  trap cleanup INT
  trap "echo; echo clean up done. bye" EXIT
else
  echo "darktable doesn't seem to support Lua, loading images directly. This results in better error handling but might interrupt the workflow"
fi



"${INOTIFYWAIT}" --monitor "${BASE_FOLDER}" --event close_write --excludei ".*\.xmp$" |
  while read -r path event file; do
    if [ ${HAVE_LUA} -eq 0 ]; then
      echo "'${file}' added"
      "${DBUS_SEND}" --type=method_call --dest=org.darktable.service /darktable org.darktable.service.Remote.Lua string:"local dt = require('darktable') dt.database.import('${path}/${file}') dt.print('a new image was added')"
    else
      ID=$("${DBUS_SEND}" --print-reply --type=method_call --dest=org.darktable.service /darktable org.darktable.service.Remote.Open string:"${path}/${file}" | tail --lines 1 | sed 's/.* //')
      if [ "${ID}" -eq 0 ]; then
        # TODO: maybe try to wait a few seconds and retry? Not sure if that is needed.
        echo "'${file}' couldn't be added"
      else
        echo "'${file}' added with id ${ID}"
      fi
    fi

  done
