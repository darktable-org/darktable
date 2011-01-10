#!/bin/sh
# this is called from autogen.sh and from src/Makefile.am

. common.sh

dir=$(dirname $(ReadLink $0))

if [ ! -f $dir/version.sh ]; then
    $dir/create_version_sh.sh
fi

. $dir/version.sh

mkdir -p common
cat > common/version.h << EOF
#ifndef DT_VERSION_H
#define DT_VERSION_H
#define DT_VERSION_SHA1SUM "$dt_sha1sum"
#define DT_VERSION_DECORATION "$dt_decoration"
#endif
EOF

