<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
	<xsl:output
		method="text"
		omit-xml-declaration="yes"
		indent="no"
	/>
	<xsl:strip-space elements="*"/>

        <xsl:param name="HAVE_OPENCL">1</xsl:param>

<xsl:template match="/">
  <xsl:text><![CDATA[/** generated file, do not edit! */
#ifndef DT_CONFGEN_H
#define DT_CONFGEN_H

void dt_confgen_init()
{
]]></xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig">
    <xsl:variable name="default" select="default"/>
    <xsl:variable name="name" select="name"/>
    <xsl:variable name="type" select="type"/>

    <xsl:for-each select="type">
      <xsl:text>   g_hash_table_insert(darktable.conf->x_type, g_strdup("</xsl:text>
      <xsl:value-of select="$name" />
      <xsl:text>"), g_strdup("</xsl:text>
      <xsl:value-of select="$type" />
      <xsl:text>"));</xsl:text>
      <xsl:text>&#xA;</xsl:text>

      <xsl:text>   g_hash_table_insert(darktable.conf->x_default, g_strdup("</xsl:text>
      <xsl:value-of select="$name" />
      <xsl:text>"), g_strdup("</xsl:text>
      <xsl:value-of select="$default" />
      <xsl:text>"));</xsl:text>
      <xsl:text>&#xA;</xsl:text>

      <xsl:if test="@min">
        <xsl:text>   g_hash_table_insert(darktable.conf->x_min, g_strdup("</xsl:text>
        <xsl:value-of select="$name" />
        <xsl:text>"), g_strdup("</xsl:text>
        <xsl:value-of select="@min" />
        <xsl:text>"));</xsl:text>
        <xsl:text>&#xA;</xsl:text>
      </xsl:if>

      <xsl:if test="@max">
        <xsl:text>   g_hash_table_insert(darktable.conf->x_max, g_strdup("</xsl:text>
        <xsl:value-of select="$name" />
        <xsl:text>"), g_strdup("</xsl:text>
        <xsl:value-of select="@max" />
        <xsl:text>"));</xsl:text>
        <xsl:text>&#xA;</xsl:text>
      </xsl:if>
    </xsl:for-each>

    <xsl:text>&#xA;</xsl:text>
  </xsl:for-each>

  <xsl:text>}</xsl:text>
  <xsl:text>&#xA;</xsl:text>
<xsl:text>#endif</xsl:text>
</xsl:template>

</xsl:stylesheet>
