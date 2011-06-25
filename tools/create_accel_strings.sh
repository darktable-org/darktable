#!/bin/sh
# Run from darktable root directory
git grep '\"<Darktable>' $1 | \
sed 's/[^"]*"\([^"]\+\)"[^"]*/\1/' | tr '/' '\n' | \
sort | uniq | \
awk '
BEGIN {
  print "void accel_strings(){"
}

/^[a-zA-Z].*/{
  print "_(\"" $0 "\");"
}

END {
  print "}"
}
  
'

