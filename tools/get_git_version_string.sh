#!/bin/sh

VERSION="$(git describe --tags --dirty)"

if [ $? -eq 0 ] ;
then
  echo "$VERSION" | sed 's,^release-,,;s,-,+,;s,-,~,;'
  exit 0
fi

# with shallow clones, there may be no tags, so the first ^
# try to get version string will fail

# in that case let's at least return the commit hash

VERSION="$(git describe --always --dirty)"
if [ $? -eq 0 ] ;
then
  echo "$VERSION"
  exit 0
fi

# failed for some reason. let's just propagate
echo "unknown-version"
exit 0 # to not fail the whole build.
