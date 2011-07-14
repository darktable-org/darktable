#!/bin/sh
# Finding all the module names
modules=`git grep 'add_library( *[a-zA-Z0-9]\+ *MODULE *[^)]\+' | \
sed 's/[^:]*:[^a]*add_library( *\([^ ]\+\).*/\1/p' | sort | uniq`

words=`git grep '\"<Darktable>' $1 | \
sed 's/[^"]*"\([^"]\+\)"[^"]*/\1/' | tr '/' '\n' | \
sort | uniq`

for module in $modules
do
  words=`echo "$words" | sed "s/^$module//"`
done

echo "$words" | \
awk '
BEGIN {
  print "void accel_strings(){"
}
/^[a-zA-Z].*/{
  if ($0 !~ /create_accel_strings.sh/) print "_(\"" $0 "\");"
}

END {
  print "}"
}
  
'
