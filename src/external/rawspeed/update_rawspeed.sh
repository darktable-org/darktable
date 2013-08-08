#!/bin/bash
svn export https://rawstudio.org/svn/rawspeed/RawSpeed --force
svn export https://rawstudio.org/svn/rawspeed/data --force

fromdos RawSpeed/*
fromdos data/*

sed -ri 's/printf\(\"/&[rawspeed] /' RawSpeed/RawDecoder.cpp

git checkout -- data/cameras.xml

git diff
