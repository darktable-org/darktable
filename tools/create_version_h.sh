#!/bin/bash
if [ ! -f version.sh ]; then
    ./tools/create_version_sh.sh
fi
. version.sh

cat > common/version.h << EOF
#ifndef DT_VERSION_H
#define DT_VERSION_H
#define DT_VERSION_SHA1SUM "$dt_sha1sum"
#define DT_VERSION_DECORATION "$dt_decoration"
#endif
EOF

