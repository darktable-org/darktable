#!/bin/bash
#
# Script to install required homebrew packages
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Ensure we use system tools (BSD sed, find, etc.) even if user has GNU tools in PATH
export PATH="/usr/bin:/bin:$PATH"

# Check if brew exists
if ! [ -x "$(command -v brew)" ]; then
    echo 'Homebrew not found. Follow instructions as provided by https://brew.sh/ to install it.' >&2
    exit 1
else
    echo "Found homebrew running in $(uname -m)-based environment."
fi

# Make sure that homebrew is up-to-date
brew update
brew upgrade

# install packages from Brewfile
brew bundle install --file ../../.ci/Brewfile --verbose

