<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/html/onechunk.xsl"/>

<xsl:param name="html.stylesheet.type">text/css</xsl:param>
<xsl:param name="html.stylesheet" select="'usermanual.css'"/> 
<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.autolabel.max.depth" select="3"></xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>
<xsl:param name="draft.mode" select="1"></xsl:param>

<xsl:param name="use.id.as.filename" select="'1'"/>

<xsl:param name="variablelist.as.blocks" select="1"></xsl:param>

</xsl:stylesheet>
