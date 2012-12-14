#!/bin/bash
gcc -g -std=c99 -Wall noiseprofile.c -lm  -o noiseprofile
gcc -g -std=c99 -fno-strict-aliasing -Wall floatdump.c -o floatdump
