#!/bin/sh

set -e

H_FILE="$1"

# version.c exists => check if it contains the up-to-date version
if [ -f "$H_FILE" ]; then
  OLD_VERSION=$(tr '"' '\n' < "$H_FILE" | sed '8q;d')
  echo $OLD_VERSION
fi
