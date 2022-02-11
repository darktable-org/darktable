#!/bin/bash

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../../" && pwd -P)

cd "$DT_SRC_DIR" || exit

git shortlog -sne release-3.1.0..HEAD

echo "are you sure these guys received proper credit in the about dialog?"
echo "HINT: $ tools/generate-authors.sh release-3.1.0 HEAD > AUTHORS"
read -r answer

# prefix rc with ~, so debian thinks its less than
echo "* archiving git tree"

dt_decoration=$(git describe --tags | sed -e 's,^release-,,;s,-,+,;s,-,~,;' -e 's/rc/~rc/')

echo "* * creating root archive"
git archive --format tar HEAD --prefix=darktable-"$dt_decoration"/ -o darktable-"$dt_decoration".tar

echo "* * creating submodule archives"
# for each of git submodules append to the root archive
git submodule foreach --recursive 'git archive --format tar --verbose --prefix="darktable-'"$dt_decoration"'/$path/" HEAD --output "'"$DT_SRC_DIR"'/darktable-sub-$sha1.tar"'

if [ $(ls "$DT_SRC_DIR/darktable-sub-"*.tar | wc -l) != 0  ]; then
  echo "* * appending submodule archives, combining all tars"
  find "$DT_SRC_DIR" -maxdepth 1 -name "darktable-sub-*.tar" -exec tar --concatenate --file "$DT_SRC_DIR/darktable-$dt_decoration.tar" {} \;
  # remove sub tars
  echo "* * removing all sub tars"
  rm -rf "$DT_SRC_DIR/darktable-sub-"*.tar
fi

echo "* * done creating archive"

TMPDIR=$(mktemp -d -t darktable-XXXXXX)
cd "$TMPDIR" || exit

tar xf "$DT_SRC_DIR/darktable-$dt_decoration.tar"

# create version header for non-git tarball:
echo "* creating version header"
"$DT_SRC_DIR/tools/create_version_c.sh" "darktable-$dt_decoration/src/version_gen.c" "$dt_decoration"

# drop regression_tests. for internal use, and need git anyway
echo "* removing tools/regression_tests"
rm -rf darktable-"$dt_decoration"/tools/regression_tests

# drop integration tests
echo "* removing src/tests/integration"
rm -rf darktable-"$dt_decoration"/src/tests/integration

# drop all git-related stuff
find darktable-"$dt_decoration"/ -iname '.git*' -exec rm -fr {} \;

# ... and also remove RELEASE_NOTES. that file is just for internal use
#echo "* removing RELEASE_NOTES"
#rm -rf darktable-$dt_decoration/RELEASE_NOTES

# wrap it up again
echo "* creating final tarball"
tar cf darktable-$dt_decoration.tar darktable-$dt_decoration/ || exit
rm "$DT_SRC_DIR/darktable-$dt_decoration.tar"
xz -z -v -9 -e "darktable-$dt_decoration.tar"
cp "darktable-$dt_decoration.tar.xz" "$DT_SRC_DIR"

# now test the build:
echo "* test compiling"
rm -rf "darktable-$dt_decoration/"
tar xf "darktable-$dt_decoration.tar.xz"
cd "darktable-$dt_decoration/" || exit
./build.sh --prefix "$TMPDIR/darktable/"

echo
echo "to actually test this build you should do:"
echo "cd $TMPDIR/darktable-$dt_decoration/build && make install"
echo "then run darktable from:"
echo "$TMPDIR/darktable/bin/darktable"
