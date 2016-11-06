#!/bin/sh

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../" && pwd -P)

cd $DT_SRC_DIR

git shortlog -sne release-2.0.0..HEAD

echo "are you sure these guys received proper credit in the about dialog?"
echo "HINT: $ tools/generate_authors.rb release-2.0.0..HEAD > AUTHORS"
read answer

# prefix rc with ~, so debian thinks its less than
echo "* archiving git tree"
dt_decoration=$(git describe --tags $branch | sed 's,^release-,,;s,-,+,;s,-,~,;' | sed 's/rc/~rc/')
git archive HEAD --prefix=darktable-$dt_decoration/ -o darktable-$dt_decoration.tar

TMPDIR=`mktemp -d -t darktable-XXXXXX`
cd "$TMPDIR"

tar xf "$DT_SRC_DIR/darktable-$dt_decoration.tar"

# create version header for non-git tarball:
echo "* creating version header"
"$DT_SRC_DIR/tools/create_version_c.sh" darktable-$dt_decoration/src/version_gen.c $dt_decoration

# remove usermanual, that's > 80 MB and released separately
echo "* removing usermanual"
rm -rf darktable-$dt_decoration/doc/usermanual

# ... and also remove RELEASE_NOTES. that file is just for internal use
#echo "* removing RELEASE_NOTES"
#rm -rf darktable-$dt_decoration/RELEASE_NOTES

# wrap it up again
echo "* creating final tarball"
tar cf darktable-$dt_decoration.tar darktable-$dt_decoration/
rm "$DT_SRC_DIR/darktable-$dt_decoration.tar"
xz -z -v -9 -e darktable-$dt_decoration.tar
cp darktable-$dt_decoration.tar.xz "$SRCDIR"

# now test the build:
echo "* test compiling"
rm -rf darktable-$dt_decoration/
tar xf darktable-$dt_decoration.tar.xz
cd darktable-$dt_decoration/
./build.sh --prefix "$TMPDIR/darktable/"

echo
echo "to actually test this build you should do:"
echo "cd $TMPDIR/darktable-$dt_decoration/build && make install"
echo "then run darktable from:"
echo "$TMPDIR/darktable/bin/darktable"



