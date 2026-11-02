#!/usr/bin/env bash

# This script is designed to be run without arguments, as it uses a fixed
# location for the WB presets JSON file within the darktable source tree.

scriptdir=$(dirname "$(readlink -f "$0")")

if hash check-jsonschema 2>/dev/null; then
  echo "check-jsonschema found and will be used"
  check-jsonschema --schemafile $scriptdir/../data/wb_presets.schema $scriptdir/../data/wb_presets.json
elif hash jsonschema 2>/dev/null; then
  echo "jsonschema found and will be used"
  jsonschema -i $scriptdir/../data/wb_presets.schema $scriptdir/../data/wb_presets.json 2>/dev/null
else
  echo "None of the JSON validation packages are installed."
  echo "Install the package containing the 'check-jsonschema' or 'jsonschema' command,"
  echo "either from your system's package repositories or via 'pipx'."
  exit 1
fi

EXITCODE=$?

test $? -eq 0 && echo "validation OK" || echo "validation FAILED"

exit $EXITCODE
