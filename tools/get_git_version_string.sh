#!/bin/sh

set -e

git describe --tags --dirty | sed 's,^release-,,;s,-,+,;s,-,~,;'
