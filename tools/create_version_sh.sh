#!/bin/sh

dir=$(dirname $(readlink -f $0))

if [ -z $1 ]; then
    branch="HEAD"
else
    branch=$1
fi

echo dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;') > $dir/version.sh
echo dt_sha1sum=$(git rev-parse --short $branch) >> $dir/version.sh
