#!/usr/bin/env bash

# usage: publish_html.sh <sf_username>

PROJECT_WEB_PATH=/home/project-web/d/da/darktable/htdocs
cd doc
scp -Cr htdocs $1,darktable@frs.sf.net:$PROJECT_WEB_PATH