#!/bin/bash

# Partly stolen from http://imagejdocu.tudor.lu/doku.php?id=diverse:commandline:imagej
# Detect readlink or realpath version
# if GNU readlink is known to be installed, this code can be replaced by "alias ReadLink='readlink -f'"
ReadLink='readlink' # Default
OS=$(uname)
if [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" ] && which greadlink >/dev/null 2>&1; then
        # Using GNU readlink on MacOS X or FreeBSD
        ReadLink='greadlink -f'
elif [ "$OS" = "Darwin" -o "$OS" = "FreeBSD" -o "$OS" = "Linux" ] && which realpath >/dev/null 2>&1; then
        if [ -f "$(which readlink)" ] && readlink --version | grep coreutils >/dev/null 2>&1; then
                ReadLink='readlink -f' # use GNU readlink
        else
                # Using realpath on MacOS X or FreeBSD
                ReadLink='realpath'
        fi
else
        ReadLink='echo'
        echo "Please install realpath or GNU readlink. Symbolic links may not be resolved properly" >&2
fi
