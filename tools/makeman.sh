input=darktable.pod
output=darktable.1
version=$(cat config.h | grep PACKAGE_VERSION | cut -f 3 -d" " | tr -d "\"")
set -e; \
  d=`sed -n 's,/,-,g;s,.*\$$[D]ate: \(..........\).*,\1,p' $input`; \
  pod2man $input \
  | sed 's/^\.TH .*/.TH DARKTABLE 1 "'"$$d"'" "darktable-'$version'" "darktable"/' \
  | sed -e '/.*DREGGNAUTHORS.*/r AUTHORS' | sed -e '/.*DREGGNAUTHORS.*/d' \
  > tmp.$$$$ \
  && mv -f tmp.$$$$ $output

