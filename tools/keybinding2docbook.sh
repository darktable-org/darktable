#!/bin/sh
# Run from darktable root directory
git grep -A 2 -h gtk_accel_map_add_entry | tr -d '\n' | \
sed 's/gtk_accel_map_add_entry *( *\(\"[^\"]\+\"\) *, *\([^ ,]\+\) *, *\([^,)]\+\)) *;/%~\1;\2;\3%~/g' | \
tr '%~' '\n' | \
awk -f 'tools/keybinding2docbook.awk' | \
gcc $(pkg-config --cflags --libs gtk+-2.0) -o tmp -xc -

./tmp > ./doc/usermanual/addendum/keymappings.xml
rm ./tmp
