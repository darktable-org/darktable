#!/bin/bash

STYLE_DIR=$1
OUT=$2

# Get the l10n strings from styles. Such strings are prefixed by _l10n_
function get-l10n()
{
    #  1. Get the "name" node value using xsltproc
    #
    #  2. Cut substrings separated with | and keep only those
    #     with _l10n_ prefix.
    #
    #  3. Remove all _l10n_ sentinel with sed
    #
    #  4. Finaly generates into the while loop the pseudo C code for
    #     the strings to be translated.

    xsltproc <( echo '<?xml version="1.0"?>
                      <xsl:stylesheet version="1.0"
                               xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
                        <xsl:output indent="yes" omit-xml-declaration="yes"/>
                        <xsl:template match="info">
                          <xsl:value-of select="name"/>
                          <xsl:text>&#10;</xsl:text>
                          <xsl:value-of select="description"/>
                        </xsl:template>

                        <xsl:template match="style"/>

                      </xsl:stylesheet>' ) $1 |
        tr '|' '\n' | grep _l10n_ | sed 's/_l10n_//g' |
        while read line; do
            echo "_(\"$line\")"
        done
}

export IFS=$'\n'

{
    echo "// Not to be compiled, generated for translation only"
    echo

    #  Ensure that we remove the duplicate strings

    ls $STYLE_DIR/*.dtstyle | while read file; do
        get-l10n $file
    done | sort | uniq
} > $OUT
