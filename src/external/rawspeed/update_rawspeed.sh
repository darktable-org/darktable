#!/bin/bash
svn export https://rawstudio.org/svn/rawspeed/RawSpeed --force
svn export https://rawstudio.org/svn/rawspeed/data --force

fromdos RawSpeed/*
fromdos data/*

git diff --unified=15
