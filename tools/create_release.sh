#!/bin/sh

git log ^release-1.4.2 HEAD| grep ^Author: | sed 's/ <.*//; s/^Author: //' | sort | uniq -c | sort -nr

echo "are you sure these guys received proper credit in the about dialog?"
read answer

# prefix rc with ~, so debian thinks its less than
dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;' | sed 's/rc/~rc/')
git archive HEAD --prefix=darktable-$dt_decoration/ -o darktable-$dt_decoration.tar

mkdir -p tmp
cd tmp
tar xvf ../darktable-$dt_decoration.tar

# create version header for non-git tarball:
echo "#define PACKAGE_VERSION \"$dt_decoration\"" > darktable-$dt_decoration/src/version_gen.h

# remove docs, that's > 45 MB
rm -rf darktable-$dt_decoration/doc/htdocs
rm -rf darktable-$dt_decoration/doc/usermanual
tar cvzf darktable-$dt_decoration.tar.gz darktable-$dt_decoration/
tar cvJf darktable-$dt_decoration.tar.xz darktable-$dt_decoration/
rm ../darktable-$dt_decoration.tar
mv darktable-$dt_decoration.tar.gz ..
mv darktable-$dt_decoration.tar.xz ..

# now test the build:
rm -rf darktable-*
tar xvzf ../darktable-$dt_decoration.tar.gz
cd darktable-*
./build.sh

echo "actually to test this build you should do:"
echo "cd tmp/darktable-$dt_decoration/build && sudo make install"



