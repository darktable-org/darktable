#!/bin/sh

set -e

H_FILE=$1
NEW_VERSION=$2

VERSION_H_NEEDS_UPDATE=1
if [ -z "$NEW_VERSION" ]; then
  NEW_VERSION=`./tools/get_git_version_string.sh`
fi

if [ -n  "`echo -e $NEW_VERSION | grep  Format`" ]; then
  NEW_VERSION="unknown-version"
fi

# version.h exists => check if it containts the up-to-date version
if [ -f "$H_FILE" ]; then
  OLD_VERSION=`./tools/parse_version_h.sh "$H_FILE"`
  if [ "${OLD_VERSION}" = "${NEW_VERSION}" ]; then
    VERSION_H_NEEDS_UPDATE=0
  fi
fi

if [ $VERSION_H_NEEDS_UPDATE -eq 1 ]; then
  echo "#define PACKAGE_VERSION \"${NEW_VERSION}\"" > "$H_FILE"
fi

echo "Version string: ${NEW_VERSION}"
