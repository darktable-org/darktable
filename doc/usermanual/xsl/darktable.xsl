<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:import href="/usr/share/xml/docbook/stylesheet/docbook-xsl/fo/docbook.xsl"/>
<xsl:param name="fop1.extensions" select="1"></xsl:param>
<xsl:param name="paper.type" select="'A4'"/> 
<xsl:param name="page.margin.inner">0.75in</xsl:param>
<xsl:param name="page.margin.outer">0.50in</xsl:param>
<xsl:param name= "page.margin.top">0.17in</xsl:param>   
<xsl:param name="region.before.extent">0.17in</xsl:param>  
<xsl:param name="body.margin.top">0.33in</xsl:param>  
<xsl:param name="region.after.extent">0.35in</xsl:param>
<xsl:param name="page.margin.bottom">0.50in</xsl:param>
<xsl:param name="body.margin.bottom">0.65in</xsl:param>
<xsl:param name="double.sided">1</xsl:param>
</xsl:stylesheet>