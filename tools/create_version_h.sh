#!/bin/sh

H_FILE=$1

VERSION_H_NEEDS_UPDATE=1

NEW_VERSION=`git describe --tags HEAD | sed 's,^release-,,;s,-,+,;s,-,~,;'`

# version.h exists => check if it containts the up-to-date version
if [ -f ${H_FILE} ]; then
  OLD_VERSION=`sed 's/.*"\(.*\)".*/\1/' < ${H_FILE}`
  if [ "${OLD_VERSION}" = "${NEW_VERSION}" ]; then
    VERSION_H_NEEDS_UPDATE=0
  fi
fi

if [ $VERSION_H_NEEDS_UPDATE -eq 1 ]; then
  echo "#define PACKAGE_VERSION \"${NEW_VERSION}\"" > ${H_FILE}
fi
