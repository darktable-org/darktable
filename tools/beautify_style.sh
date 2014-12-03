#!/bin/sh

# this uses clang-format to standardize our indentation/braces etc

find src | grep -v "^src/external" | egrep "\.h$|\.hh$|\.c$|\.cc$" | xargs clang-format-3.6 -i

