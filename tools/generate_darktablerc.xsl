<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
	<xsl:output
		method="text"
		omit-xml-declaration="yes"
		indent="no"
	/>
	<xsl:strip-space elements="*"/>

	<xsl:template match="/">
		<xsl:apply-templates select="dtconfiglist/dtconfig"/>
	</xsl:template>

	<xsl:template match="dtconfig">
		<xsl:value-of select="name" />=<xsl:value-of select="default" />
		<xsl:text>&#xA;</xsl:text>
	</xsl:template>

</xsl:stylesheet>
