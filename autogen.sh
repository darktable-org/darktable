#!/bin/sh

dir=$(dirname $(readlink -f $0))/tools

if [ ! -f $dir/version.sh ]; then
    $dir/create_version_sh.sh
fi
. $dir/version.sh

sed -e "s/REPLACE_WITH_DT_VERSION/$dt_decoration/" < configure.ac.in > configure.ac
autoreconf --install --force
intltoolize --copy --force --automake




