#!/bin/sh

inputfile="$1"
outputdir="$2"
outputheader=metadata_gen.h
outputbody=metadata_gen.c

headerdefine=__$(printf "%s" "$outputheader" | tr '[:lower:].' '[:upper:]_')__
# header of the .h file
cat > "$outputdir/$outputheader" << EOF
/** generated file, do not edit! */

#ifndef $headerdefine
#define $headerdefine

typedef enum dt_metadata_t
{
EOF

# header of the .c file
cat > "$outputdir/$outputbody" << EOF
/** generated file, do not edit! */

#include <string.h>
#include "$outputheader"

dt_metadata_t dt_metadata_get_keyid(const char* key)
{
EOF

# iterate over the input
first=0
grep -v "^#" "$inputfile" | while IFS= read -r line; do
    enum=DT_METADATA_$(printf "%s" "$line" | tr '[:lower:].' '[:upper:]_')
    length=$(printf "%s" "$line" | wc -c)
    if [ "$first" -ne 0 ]; then
        printf ",\n" >> "$outputdir/$outputheader"
    fi
    printf "%s" "    $enum" >> "$outputdir/$outputheader"
    first=1

    cat >> "$outputdir/$outputbody" << EOF
    if(strncmp(key, "$(echo $line |tr -d '\r')", $length) == 0)
        return $enum;
EOF

done

# end of the .h file
cat >> "$outputdir/$outputheader" << EOF

}
dt_metadata_t;

dt_metadata_t dt_metadata_get_keyid(const char* key);

#endif

EOF

# end of the .c file
cat >> "$outputdir/$outputbody" << EOF
    return -1;
}

EOF
