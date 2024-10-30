#!/bin/bash

function usage()
{
    echo "usage: $0 [--clang|--help]"
    echo "     --help       : Display this help message."
    echo "     --clang <n>  : Minimal CLang version to allow."
    exit 1
}

CLANG_VERSION=

OPTIONS=$(getopt -u --long clang:,help -- $0 $@)
[[ $? -eq 0 ]] || usage

set -- $OPTIONS

while [[ -n $1 ]]; do
    case $1 in
        --help)
            usage
            ;;
        --clang)
            shift
            CLANG_VERSION=$1
            ;;
        (--)
            ;;
    esac
    shift
done

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../../" && pwd -P)

cd "$DT_SRC_DIR" || exit

#  Get base of previous release
PREV_TAG=$(git tag --sort=-creatordate | grep '[13579].0$' | head -2 | tail -1)

git shortlog -sne $PREV_TAG..HEAD

echo "are you sure these guys received proper credit in the about dialog?"
echo "HINT: $ tools/generate-authors.sh $PREV_TAG HEAD > AUTHORS"
read -r answer

# prefix rc with ~, so debian thinks its less than
echo "* archiving git tree"

dt_decoration=$(git describe --tags --match release-* |
                    sed -e 's,^release-,,;s,-,+,;s,-,~,;' -e 's/rc/~rc/')

# Hot-fix for the compiler version if necessary
if [[ -n $CLANG_VERSION ]]; then
    CV=$CLANG_VERSION
    sed -i "s/\(.*\"Clang\".*\)COMPILER_VERSION VERSION_LESS [0-9][0-9]/\1COMPILER_VERSION VERSION_LESS $CV/" \
        cmake/compiler-versions.cmake \
        src/external/rawspeed/cmake/compiler-versions.cmake
fi

echo "* * creating root archive"
git archive --format tar HEAD \
    --prefix=darktable-"$dt_decoration"/ \
    -o darktable-"$dt_decoration".tar

echo "* * creating submodule archives"
# for each of git submodules append to the root archive
git submodule foreach \
    --recursive 'git archive --format tar --verbose --prefix="darktable-'"$dt_decoration"'/$path/" HEAD --output "'"$DT_SRC_DIR"'/darktable-sub-$sha1.tar"'

if [ $(ls "$DT_SRC_DIR/darktable-sub-"*.tar | wc -l) != 0  ]; then
  echo "* * appending submodule archives, combining all tars"
  find "$DT_SRC_DIR" -maxdepth 1 -name "darktable-sub-*.tar" \
       -exec tar --concatenate --file "$DT_SRC_DIR/darktable-$dt_decoration.tar" {} \;
  # remove sub tars
  echo "* * removing all sub tars"
  rm -rf "$DT_SRC_DIR/darktable-sub-"*.tar
fi

echo "* * done creating archive"

if [[ -n $CLANG_VERSION ]]; then
    git reset --hard
    git submodule foreach git reset --hard
fi

TMPDIR=$(mktemp -d -t darktable-XXXXXX)
cd "$TMPDIR" || exit

tar xf "$DT_SRC_DIR/darktable-$dt_decoration.tar"

# create version header for non-git tarball:
echo "* creating version header"
"$DT_SRC_DIR/tools/create_version_c.sh" \
    "darktable-$dt_decoration/src/version_gen.c" "$dt_decoration"

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
