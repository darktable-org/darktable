#!/bin/bash
#
# Author: Gregor Quade <u3dreal@yahoo.de>

#inspired by Aaron Voisine <aaron@voisine.org>


PREFIX="/opt/local" # macports install dir

FOO="`pwd`/darktable.app/Contents/Resources/"

BIN=`echo $PREFIX/bin/darktable`

SO=`echo $PREFIX/lib/gtk-2.0/*/*/*.so $PREFIX/lib/pango/*/*/*.so`

PLUGINS=`echo $PREFIX/share/darktable/plugins/*.so`

LIB=`otool -L $BIN $SO $PLUGINS | grep -v : | grep $PREFIX/ | awk '{print $1;}' | sed -E 's/([0-9][.])dylib/\1*dylib/g' | sort -u | xargs echo`

ETC="$PREFIX/etc/gtk-2.0/ $PREFIX/etc/pango/ $PREFIX/etc/fonts"

SHARE=`echo $PREFIX/share/darktable/ $PREFIX/share/lensfun/`

LENSFUN=`echo $PREFIX/lib/liblensfun*`

cp ScriptExecCocoa/build/Release/ScriptExec.app/Contents/MacOS/ScriptExec $FOO/../MacOS/darktable

cd $PREFIX

echo $BIN $SO $LIB $LENSFUN $ETC $SHARE | sed "s|$PREFIX/"'*||g' | xargs tar -cf /tmp/dtguts.tar

cd $FOO

tar -xf /tmp/dtguts.tar
rm /tmp/dtguts.tar

cat $PREFIX/etc/gtk-2.0/gtk.immodules | sed 's|'"$PREFIX"'/*|${CWD}/|g' > $FOO/etc/gtk-2.0/gtk.immodules
cat $PREFIX/etc/gtk-2.0/gdk-pixbuf.loaders | sed 's|'"$PREFIX"'/*|${CWD}/|g' > $FOO/etc/gtk-2.0/gdk-pixbuf.loaders
cat $PREFIX/etc/pango/pangorc | sed 's|'"$PREFIX"'/*etc/pango|etc/pango|g' > $FOO/etc/pango/pangorc
cat $PREFIX/etc/pango/pango.modules | sed 's|'"$PREFIX"'[^ ]*|"&"|g' | sed 's|'"$PREFIX"'/*||g' > $FOO/etc/pango/pango.modules


