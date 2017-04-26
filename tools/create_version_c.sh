#!/bin/sh

set -e

C_FILE="$1"
NEW_VERSION="$2"

VERSION_C_NEEDS_UPDATE=1
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
    VERSION_C_NEEDS_UPDATE=0
  fi
fi

MAJOR_VERSION=0
MINOR_VERSION=0
PATCH_VERSION=0
if echo "$NEW_VERSION" | grep -q "^[0-9]\+\.[0-9]\+\.[0-9]\+"; then
  MAJOR_VERSION=`echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\1/"`
  MINOR_VERSION=`echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\2/"`
  PATCH_VERSION=`echo "$NEW_VERSION" | sed "s/^\([0-9]\+\)\.\([0-9]\+\)\.\([0-9]\+\).*/\3/"`
fi

if [ $VERSION_C_NEEDS_UPDATE -eq 1 ]; then
  echo "#ifndef RC_BUILD" > "$C_FILE"
  echo "  #ifdef HAVE_CONFIG_H" >> "$C_FILE"
  echo "    #include \"config.h\"" >> "$C_FILE"
  echo "  #endif" >> "$C_FILE"

  echo "  const char darktable_package_version[] = \"${NEW_VERSION}\";" >> "$C_FILE"
  echo "  const char darktable_package_string[] = PACKAGE_NAME \" ${NEW_VERSION}\";" >> "$C_FILE"
  echo "#else" >> "$C_FILE"
  echo "  #define DT_MAJOR ${MAJOR_VERSION}" >> "$C_FILE"
  echo "  #define DT_MINOR ${MINOR_VERSION}" >> "$C_FILE"
  echo "  #define DT_PATCH ${PATCH_VERSION}" >> "$C_FILE"
  echo "#endif" >> "$C_FILE"
fi

echo "Version string: ${NEW_VERSION}"
echo "  Major: ${MAJOR_VERSION}"
echo "  Minor: ${MINOR_VERSION}"
echo "  Patch: ${PATCH_VERSION}"
