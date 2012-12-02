#!/usr/bin/env bash
cd build 
rm -r doc/usermanual
make darktable-usermanual-html
rm -r ../doc/htdocs/usermanual
cp -r doc/usermanual/html ../doc/htdocs/usermanual