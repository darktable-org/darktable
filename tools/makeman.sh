input="$1"
config="$2"
authors="$3"
output="$4"
version=$(cat "$config" | grep PACKAGE_VERSION | head -1 | cut -f 3 -d" " | tr -d "\"")
set -e; \
  d=`sed -n 's,/,-,g;s,.*\$$[D]ate: \(..........\).*,\1,p' "$input"`; \
  pod2man "$input" \
  | sed 's/^\.TH .*/.TH DARKTABLE 1 "'"$$d"'" "darktable-'$version'" "darktable"/' \
  | sed -e '/.*DREGGNAUTHORS.*/r '"$authors" | sed -e '/.*DREGGNAUTHORS.*/d' \
  > tmp.$$$$ \
  && mv -f tmp.$$$$ "$output"

