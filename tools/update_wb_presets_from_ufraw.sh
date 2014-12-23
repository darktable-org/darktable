#!/bin/sh
#
# Script updates our wb_presets which we regularly steal from UFRaw.
#

TEMP_FILE=`tempfile -p dtwb -s .c`
OUT_FILE="../src/external/wb_presets.c"

echo "Downloading new wb_presets.c into ${TEMP_FILE}"

wget http://ufraw.cvs.sourceforge.net/viewvc/ufraw/ufraw/wb_presets.c?content-type=text%2Fplain -O ${TEMP_FILE}

echo "Processing ${TEMP_FILE} into ${OUT_FILE}, this may take a while"

IFS="\n"
cat ${TEMP_FILE} | while read LINE; do
  if [ "${LINE}" = '#include "ufraw.h"' ]; then
    echo '#ifdef HAVE_CONFIG_H'
    echo '#include "config.h"'
    echo '#endif'
    echo ''
  elif [ "${LINE}" = '#include <glib/gi18n.h>' ]; then
    echo '#include <glib.h>'
    echo '#include <glib/gi18n.h>'
    echo ''
    echo 'typedef struct'
    echo '{'
    echo '  const char *make;'
    echo '  const char *model;'
    echo '  const char *name;'
    echo '  int tuning;'
    echo '  double channel[4];'
    echo '}'
    echo 'wb_data;'
  else
    echo "${LINE}" | grep -v 'K", ' | grep -v ', uf_'
    echo "${LINE}" | grep '"2700K",'
    echo "${LINE}" | grep '"3000K",'
    echo "${LINE}" | grep '"3300K",'
    echo "${LINE}" | grep '"5000K",'
    echo "${LINE}" | grep '"5500K",'
    echo "${LINE}" | grep '"6500K",'
  fi
done > ${OUT_FILE}

rm ${TEMP_FILE}

