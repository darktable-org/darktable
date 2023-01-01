#!/bin/bash
# command line: put this file in path before darktable as: /usr/local/bin/darktable 
# desktop icon: edit /usr/share/applications/org.darktable.darktable.desktop: Exec and TryExec pointing to /usr/local/bin/darktable  
# (ubuntu18.04, dt from git)
#

[ "${FLOCKER}" != "$0" ] && exec env FLOCKER="$0" flock -en "$0" "$0" "$@" || :

gnomenightlight="org.gnome.settings-daemon.plugins.color night-light-enabled"

trap "gsettings set ${gnomenightlight} $(gsettings get ${gnomenightlight})" EXIT

gsettings set ${gnomenightlight} false

/opt/darktable/bin/darktable "$@"
