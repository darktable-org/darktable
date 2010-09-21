#!/bin/bash
dt_decoration=$(git log master^..master --decorate | head -1 | cut -d' ' -f4 | cut -f1 -d')')
dt_sha1sum=$(git log master^..master | head -1 | cut -f2 -d ' ' | cut -c -8)
sed -e "s/REPLACE_WITH_DT_VERSION/$dt_sha1sum/" < configure.ac.in > configure.ac
autoreconf --install --force
intltoolize --copy --force --automake
