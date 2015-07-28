#!/bin/bash

wget https://github.com/klauspost/rawspeed/archive/develop.zip

unzip -j -o develop.zip rawspeed-develop/RawSpeed/* -d RawSpeed
unzip -j -o develop.zip rawspeed-develop/data/* -d data

rm develop.zip

rm RawSpeed/*.vcproj*
rm RawSpeed/*.vcxproj*

rm RawSpeed/pugiconfig.hpp RawSpeed/pugixml* RawSpeed/pugixml-readme.txt

fromdos RawSpeed/*
fromdos data/*

git diff --unified=15
