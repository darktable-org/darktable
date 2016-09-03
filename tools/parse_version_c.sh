#!/bin/sh

set -e

H_FILE=$1

# version.c exists => check if it containts the up-to-date version
if [ -f "$H_FILE" ]; then
  OLD_VERSION=`cat "$H_FILE" | tr '\n' ' ' | sed s/\"/\\\n/g | sed '4q;d'`

  echo $OLD_VERSION
fi
