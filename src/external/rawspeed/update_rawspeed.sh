#!/bin/bash

wget https://github.com/klauspost/rawspeed/archive/develop.zip

unzip -j -o develop.zip rawspeed-develop/RawSpeed/* -d RawSpeed
unzip -j -o develop.zip rawspeed-develop/data/* -d data

rm develop.zip

rm RawSpeed/*.vcproj*

fromdos RawSpeed/*
fromdos data/*

git diff --unified=15
