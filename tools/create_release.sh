#!/bin/bash

git log release-0.8..master | grep ^Author: | sed 's/ <.*//; s/^Author: //' | sort | uniq -c | sort -nr

echo "are you sure these guys received proper credit in the about dialog?"
read

dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;')
git archive master --prefix=darktable-$dt_decoration/ -o darktable-$dt_decoration.tar

mkdir -p tmp
cd tmp
tar xvf ../darktable-$dt_decoration.tar

# create version header for non-git tarball:
echo "set(PROJECT_VERSION \"$dt_decoration\")" >> darktable-$dt_decoration/cmake/version.cmake

# remove docs, that's > 45 MB
rm -rf darktable-$dt_decoration/doc/htdocs
cp darktable-$dt_decoration/doc/usermanual/CMakeLists.txt dreggn.txt
rm -rf darktable-$dt_decoration/doc/usermanual/*
mv dreggn.txt darktable-$dt_decoration/doc/usermanual/CMakeLists.tx
tar cvzf darktable-$dt_decoration.tar.gz darktable-$dt_decoration/
rm ../darktable-$dt_decoration.tar
mv darktable-$dt_decoration.tar.gz ..

# now test the build:
rm -rf darktable-*
tar xvzf ../darktable-$dt_decoration.tar.gz
cd darktable-*
./build.sh

echo "actually to test this build you should do:"
echo "cd tmp/darktable-$dt_decoration/build && sudo make install"



