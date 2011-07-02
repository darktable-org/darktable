#!/bin/bash
dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;')
git archive master --prefix=darktable-$dt_decoration/ -o darktable-$dt_decoration.tar

mkdir -p tmp
cd tmp
tar xvf ../darktable-$dt_decoration.tar
echo "set(PROJECT_VERSION \"$dt_decoration\")" >> darktable-$dt_decoration/cmake/version.cmake
tar cvzf darktable-$dt_decoration.tar.gz darktable-$dt_decoration/
rm ../darktable-$dt_decoration.tar
mv darktable-$dt_decoration.tar.gz ..

# now test the build:
rm -rf darktable-*
tar xvzf ../darktable-$dt_decoration.tar.gz
cd darktable-*
./build.sh



