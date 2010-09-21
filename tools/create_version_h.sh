#!/bin/bash
dt_decoration=$(git log master^..master --decorate | head -1 | cut -d' ' -f4 | cut -f1 -d')')
dt_sha1sum=$(git log master^..master | head -1 | cut -f2 -d ' ' | cut -c -8)

cat > common/version.h << EOF
#ifndef DT_VERSION_H
#define DT_VERSION_H
#define DT_VERSION_SHA1SUM "$dt_sha1sum"
#define DT_VERSION_DECORATION "$dt_decoration"
#endif
EOF

