#!/bin/sh
# Finding all the module names
modules=`grep -r 'add_library( *[a-zA-Z0-9]\+ *MODULE *[^)]\+' $1 | \
sed 's/[^:]*:[^a]*add_library( *\([^ ]\+\).*/\1/p' | sort | uniq`

words=`grep -r '\"<Darktable>' $1 | \
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
