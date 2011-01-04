#!/usr/bin/env bash

# Stolen from http://imagejdocu.tudor.lu/doku.php?id=diverse:commandline:imagej
# Detect readlink version 
# if GNU readlink is known to be installed, this code can be replaced by "alias ReadLink='readlink -f'"
alias ReadLink='readlink' # Default
if [[ $(uname) == Darwin || $(uname) == FreeBSD ]] && which greadlink &>/dev/null ; then  
        # Using GNU readlink on MacOS X or FreeBSD
        alias ReadLink='greadlink -f'
elif [[ -f $(which readlink) ]] && readlink --version | grep coreutils &>/dev/null ; then
        alias ReadLink='readlink -f'  # use GNU readlink
else
        alias ReadLink='echo'
        echo "Please install GNU readlink. Symbolic links may not be resolved properly" >&2
fi
