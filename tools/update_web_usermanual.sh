#!/bin/sh
cd build || exit
rm -r doc/usermanual
make darktable-usermanual-html
rm -r ../doc/htdocs/usermanual
cp -r doc/usermanual/html ../doc/htdocs/usermanual
