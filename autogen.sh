#!/bin/bash
if [ ! -f version.sh ]; then
    ./tools/create_version_sh.sh
fi
. version.sh

sed -e "s/REPLACE_WITH_DT_VERSION/$dt_sha1sum/" < configure.ac.in > configure.ac
autoreconf --install --force
intltoolize --copy --force --automake


