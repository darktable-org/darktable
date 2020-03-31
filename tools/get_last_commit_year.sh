#!/bin/sh

DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../" && pwd -P)

LAST_COMMIT_YEAR=$(git --git-dir="${DT_SRC_DIR}/.git" log -n1 --pretty=%ci)

if [ $? -eq 0 ]; then
  echo "${LAST_COMMIT_YEAR}" | cut -b 1-4
  exit 0
fi

# fallback in case git above failed

date -u "+%Y"
