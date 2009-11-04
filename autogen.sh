#!/bin/sh

# Automake 1.5 or higher is required.  Because conventions vary as to
# whether plain "automake" is old (1.4) or modern automake, search for
# versions by number from the most recent, taking the highest one
# found.

# XXX This search is a maintenance problem.  Add a way to use plain
# 'automake' and see if it is good enough.  Consider declaring a
# minimum version in Makefile.am and assuming that in 2006 the program
# automake will be a modern version.

AUTOMAKE=automake-1.11
ACLOCAL=aclocal-1.11
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.10
    ACLOCAL=aclocal-1.10
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.9
    ACLOCAL=aclocal-1.9
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.8
    ACLOCAL=aclocal-1.8
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.7
    ACLOCAL=aclocal-1.7
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.6
    ACLOCAL=aclocal-1.6
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    AUTOMAKE=automake-1.5
    ACLOCAL=aclocal-1.5
}
($AUTOMAKE --version) < /dev/null > /dev/null 2>&1 || {
    echo "You must have automake-1.5 or higher."
    exit 1
}
$ACLOCAL
autoconf
autoheader
$AUTOMAKE --foreign --add-missing --copy
intltoolize --copy --force --automake
