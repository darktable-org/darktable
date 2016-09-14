#!/bin/bash

wget https://github.com/klauspost/rawspeed/archive/develop.zip

unzip -o develop.zip rawspeed-develop/* -d ./
rm develop.zip

cp -a ./rawspeed-develop/. ./ && rm -rf rawspeed-develop

rm RawSpeed/*.vcproj*
rm RawSpeed/*.vcxproj*

rm *.sln*

rm -rf dll
rm -rf include
rm -rf lib lib64

rm RawSpeed/pugiconfig.hpp RawSpeed/pugixml* RawSpeed/pugixml-readme.txt
rm RawSpeed/RawSpeed.cpp

git diff --unified=15
