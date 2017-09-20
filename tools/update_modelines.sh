#!/bin/sh

# this appends modelines to all source and header files to make sure kate and
# vim know how to format their stuff.
# 
# There is currently no check whether any existing modelines are up to date.
# Whenever run, this file will remove any modelines and append the ones below
# to the file.
# additionally, this script will also remove any comments starting with "^// modelines:"
# 
# For useful options in the vim modeline see also the one used in this file
#
NOTIFICATION_LINE='// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh'
VIM_MODELINE='// vim: shiftwidth=2 expandtab tabstop=2 cindent'
KATE_MODELINE='// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;'

# thank you for collecting these files in tools/beautify_style.sh
SOURCES=$(find src/ -name "*.c" -or -name "*.cc" -or -name "*.h" | grep -v src/external)

for f in $SOURCES
do
  # debug
  #  echo Current file is "$f"
  #  echo " removing any old modelines"

  TEMPFILE=$(tempfile)
  # Check for lines beginning with a comment and a modeline keyword
  grep -v "^// vim:\|^// kate:\|^// modelines:" "$f" > "$TEMPFILE"

  #
  # echo "vim_modeline is: $VIM_MODELINE"
  # echo "kate_modeline is: $KATE_MODELINE"
  for m in "$NOTIFICATION_LINE" "$VIM_MODELINE" "$KATE_MODELINE";  do
    # debug
    # echo "  appending $m"
    echo "$m" >> "$TEMPFILE"
  done
  mv "$TEMPFILE" "$f" 
  echo "[x] $f has been updated."
done

# vim: et sw=2 ts=2 list listchars=trail\:·,eol\:¶,tab\:->,extends\:»,precedes\:«
# kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
