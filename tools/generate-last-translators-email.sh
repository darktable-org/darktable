#!/bin/bash

# Get the name of the last two translators in the 2 past years for a
# all languages. This is expected to be used to send reminder e-mail
# for translating darktable.

cd po

SINCE_DATE="2 years"
NB_LAST_TRANSLATOR="3"

for TR in *.po; do
    # check if some output
    RES=$(git log --since="$SINCE_DATE" --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -$NB_LAST_TRANSLATOR )

    if [[ -z $RES ]]; then
        # no translator since 2 years, get the last one
        git log -100 --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -1
    else
        git log --since="$SINCE_DATE" --format="%an <%ae>" $TR | grep -v noreply | grep -v "@nowhere" | uniq | head -$NB_LAST_TRANSLATOR
    fi
done | sort | uniq | while read OUT; do
    echo -n "$OUT, "
done
