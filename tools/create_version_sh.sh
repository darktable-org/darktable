#!/bin/sh

. common.sh

dir=$(dirname $(ReadLink $0))

if [ -z $1 ]; then
    branch="HEAD"
else
    branch=$1
fi

# replace rc with ~rc, so debian thinks it's smaller.
echo dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;' | sed 's/rc/~rc/') > $dir/version.sh
echo dt_sha1sum=$(git rev-parse --short $branch) >> $dir/version.sh
