#!/bin/sh

set -e

C_FILE="$1"
NEW_VERSION="$2"

VERSION_H_NEEDS_UPDATE=1
if [ -z "$NEW_VERSION" ]; then
  NEW_VERSION=`./tools/get_git_version_string.sh`
fi

if [ -n  "`echo -e $NEW_VERSION | grep  Format`" ]; then
  NEW_VERSION="unknown-version"
fi

# version.c exists => check if it containts the up-to-date version
if [ -f "$C_FILE" ]; then
  OLD_VERSION=`./tools/parse_version_c.sh "$C_FILE"`
  if [ "${OLD_VERSION}" = "${NEW_VERSION}" ]; then
    VERSION_H_NEEDS_UPDATE=0
  fi
fi

if [ $VERSION_H_NEEDS_UPDATE -eq 1 ]; then
  echo "#ifdef HAVE_CONFIG_H" > "$C_FILE"
  echo "#include \"config.h\"" >> "$C_FILE"
  echo "#endif" >> "$C_FILE"

  echo "const char darktable_package_version[] = \"${NEW_VERSION}\";" >> "$C_FILE"
  echo "const char darktable_package_string[] = PACKAGE_NAME \" ${NEW_VERSION}\";" >> "$C_FILE"
fi

echo "Version string: ${NEW_VERSION}"
