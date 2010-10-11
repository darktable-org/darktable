#!/bin/bash
# this is called from autogen.sh and from src/Makefile.am
if [ -x ./tools/create_version_sh.sh ]; then
  if [ ! -f version.sh ]; then
    ./tools/create_version_sh.sh
  fi
  . version.sh
else
  if [ ! -f ../version.sh ]; then
    cd ..
    ./tools/create_version_sh.sh
    cd -
  fi
  . ../version.sh
fi

cat > common/version.h << EOF
#ifndef DT_VERSION_H
#define DT_VERSION_H
#define DT_VERSION_SHA1SUM "$dt_sha1sum"
#define DT_VERSION_DECORATION "$dt_decoration"
#endif
EOF

