#!/bin/bash

# generate translation for RELEASE_NOTES.md
# flag translations to be removed from release
#
# 1. Run this script, it just display information (dry-run)
#    ./tools/generate-translations.sh
#
# 2. Run with option DO to actually issue the git remove
#    ./tools/generate-translations.sh DO
#
# 3. Commit the languages removed for the release.
#
# 4. Run again the tool without DO option:
#
#    ./tools/generate-translations.sh
#
#    At this point only the kept languages are displayed, and the text can
#    be copy/pasted directly at the end of RELEASE_NOTES.md
#

CDPATH=

MAX_UNTRANSLATED=20
MAX_FUZZY_TRANSLATIONS=50

declare -A LANG_NAME=( [af]=Afrikaans
                       [fr]=French
                       [al]=Albanian
                       [ca]=Catalan
                       [cs]=Czech
                       [da]=Danish
                       [de]=German
                       [el]=Greek
                       [es]="European Spanish"
                       [eo]=Esperanto
                       [fi]=Finnish
                       [gl]=Galician
                       [he]=Hebrew
                       [hu]=Hungarian
                       [it]=Italian
                       [ja]=Japanese
                       [nb]="Norwegian Bokmål"
                       [nl]=Dutch
                       [pl]=Polish
                       [pt_BR]="Brazilian Portuguese"
                       [pt_PT]="European Portuguese"
                       [ro]=Romanian
                       [ru]=Russian
                       [sk]=Slovak
                       [sl]=Slovenian
                       [sq]=Albanian
                       [sr@latin]="Serbian Latin"
                       [sr]="Serbian Cyrilic"
                       [sv]=Swedish
                       [th]=Thai
                       [tr]=Turkish
                       [uk]=Ukrainian
                       [zh_CN]="Chinese - China"
                       [zh_TW]="Chinese - Taiwan" )



function lang-name()
{
    local CODE=$1
    echo ${LANG_NAME[$CODE]}
}

function check-lang()
{
    local CODE=$(echo $1 | cut -d: -f1)
    local LINE="$2"

    local T=0
    local FT=0
    local UT=0

    T=$(echo $LINE | sed 's/\([0-9]*\) translated.*/\1/g')

    echo $LINE | grep -q fuzzy
    [ $? == 0 ] && FT=$(echo $LINE | sed 's/.* \([0-9]*\) fuzzy.*/\1/g')

    echo $LINE | grep -q untranslated
    [ $? == 0 ] && UT=$(echo $LINE | sed 's/.* \([0-9]*\) untranslated.*/\1/g')

    if [ $FT -gt $MAX_FUZZY_TRANSLATIONS -o $UT -gt $MAX_UNTRANSLATED ]; then
        echo -n "remove language $CODE - $(lang-name $CODE): " 1>&2
        echo "fuzzy $FT, untranslated $UT" 1>&2
        if [ $DO == YES ]; then
            git rm $CODE.po
        else
            echo "$ git rm $CODE.po" 1>&2
        fi

    else
        echo "- $(lang-name $CODE)"
    fi
}

if [ "$1" == "DO" ]; then
    DO=YES
else
    DO=NO
fi

cd po

echo "## Translations"

intltool-update -r 2>&1 | grep -e '[^:]: [0-9]' > ../INTL.OUT
git restore *.po

cat ../INTL.OUT |
    while read CODE LINE; do
        check-lang $CODE "$LINE"
    done

# rm ../INTL.OUT
