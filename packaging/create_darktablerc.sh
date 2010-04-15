#!/bin/bash

cat darktable.schemas.in | grep "\<key\>" | sed -e 's/<key>//g' -e 's/<\/key>//g' -e 's/\/schemas\/apps\/darktable\///g' | nl -s: | sed -e 's/^[ \t]*//;s/[ \t]*$//' > dreggn1
cat darktable.schemas.in | grep "\<default\>" | sed -e 's/<default>//g' -e 's/<\/default>//g' | nl -s: | sed -e 's/^[ \t]*//;s/[ \t]*$//' > dreggn2
join -j 1 -o 1.2 2.2 dreggn1 dreggn2 | sed -e 's/ /=/g' > darktablerc
rm -f dreggn{1,2}
