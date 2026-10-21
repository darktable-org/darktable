#!/usr/bin/env bash
#
# Extracts the user-visible strings of data/masks_group_presets.json (preset
# names and descriptions, group names and every page of their notes) as
# _("...") calls, so that xgettext finds them. The output is not compiled:
# darktable reads the JSON at runtime and translates each string with _(),
# like styles_string.h.
#
# This is not a JSON parser: it relies on the file's layout, with each
# "name" and "description" on its own line and each note on its own line of
# a "notes" array. It fails rather than skip a note it cannot read. JSON
# string escapes are those of C, so the literals are copied unchanged.
#
# usage: generate_masks_presets_strings.sh <masks_group_presets.json> <output.h>

SRC=$1
OUT=$2

set -o pipefail

{
    echo "// Not to be compiled, generated for translation only"
    echo

    awk '
        # a JSON string literal, quotes included
        function literal(s)
        {
            return match(s, /^"([^"\\]|\\.)*"/) ? substr(s, 1, RLENGTH) : ""
        }

        function emit(s)
        {
            if(s != "" && s != "\"\"") print "_(" s ")"
        }

        in_notes && /^[[:space:]]*\]/ { in_notes = 0; next }

        in_notes {
            line = $0
            sub(/^[[:space:]]*/, "", line)
            s = literal(line)
            if(s == "")
            {
                printf("%s:%d: unreadable note\n", FILENAME, FNR) > "/dev/stderr"
                failed = 1
                exit 1
            }
            emit(s)
            next
        }

        /"notes"[[:space:]]*:[[:space:]]*\[[[:space:]]*$/ { in_notes = 1; next }

        match($0, /"(name|description)"[[:space:]]*:[[:space:]]*/) {
            emit(literal(substr($0, RSTART + RLENGTH)))
        }

        END { if(in_notes && !failed) { printf("%s: unterminated notes\n", FILENAME) > "/dev/stderr"; exit 1 } }
    ' "$SRC" | LC_ALL=C sort -u
} > "$OUT" || { rm -f "$OUT"; exit 1; }
