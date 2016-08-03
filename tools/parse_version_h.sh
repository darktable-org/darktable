#!/bin/sh

set -e

H_FILE=$1

# version.h exists => check if it containts the up-to-date version
if [ -f "$H_FILE" ]; then
  OLD_VERSION=`sed 's/.*"\(.*\)".*/\1/' < "$H_FILE"`

  echo $OLD_VERSION
fi
