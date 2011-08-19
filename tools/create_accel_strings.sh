#!/bin/sh
# Finding all the module names

FIXED_CATEGORIES="darkroom\nlighttable\nexport\ncopy" # ugly hack to keep these strings in the results

modules=`grep -r 'add_library( *[a-zA-Z0-9_]\+ *MODULE *[^)]\+' $1 | \
sed 's/[^:]*:[^a]*add_library( *\([^ ]\+\).*/\1/p' | sort | uniq`

words=`grep -r '\"<Darktable>' $1 | \
sed 's/.*"\(<Darktable\>[^"]\+\)".*/\1/' | tr '/' '\n' | \
sort | uniq`

for module in $modules
do
  words=`echo "$words" | sed "s/^$module$//"`
done

echo "$words\n$FIXED_CATEGORIES" | \
awk '
BEGIN {
  print "void accel_strings(){"
}
/^[a-zA-Z].*/{
  print "C_(\"accel\",\"" $0 "\");"
}

END {
  print "}"
}
  
'
