#!/bin/sh

if [ -z $1 ]; then
    branch="HEAD"
else
    branch=$1
fi

echo dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;') > version.sh
echo dt_sha1sum=$(git rev-parse --short $branch) >> version.sh
