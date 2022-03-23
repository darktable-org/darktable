#!/bin/bash

# this appends modelines to all source and header files to make sure kate and
# vim know how to format their stuff.
# 
# python script finds lines beginning with clang-format off and modelines and ending clang-format on
# these will be replaced by lines defined in that script
# 
# For useful options in the vim modeline see also the one used in this file
#

set -e

# thank you for collecting these files in tools/beautify_style.sh
SOURCES=$(find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v src/external)

for f in $SOURCES
do
  python3 tools/update_modelines.py $f
done

# vim: et sw=2 ts=2 list listchars=trail\:·,eol\:¶,tab\:->,extends\:»,precedes\:«
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
