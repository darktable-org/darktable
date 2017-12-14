#!/bin/sh

# TODO: extract basecurves, color matrices etc and create a complete html table for the web:
echo "cameras with profiled presets for denoising:"
grep '"model"' < data/noiseprofiles.json | sed 's/.*"model"[ \t]*:[ \t]*"\([^"]*\).*/\1/' | tr "[:upper:]" "[:lower:]" | sort | sed 's/$/<\/li>/' | sed 's/^/<li>/'
