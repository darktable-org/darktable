<?xml version='1.0'?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="1.0">
<xsl:import href="http://docbook.sourceforge.net/release/xsl/current/fo/docbook.xsl"/>

<xsl:param name="fop1.extensions" select="1"></xsl:param>
<xsl:param name="paper.type" select="'A4'"/> 

<xsl:param name="page.margin.inner">0.90in</xsl:param>
<xsl:param name="page.margin.outer">1.10in</xsl:param>
<xsl:param name= "page.margin.top">1.00in</xsl:param>   
<xsl:param name="region.before.extent">0.20in</xsl:param>  
<xsl:param name="body.margin.top">0.25in</xsl:param>  
<xsl:param name="region.after.extent">0.20in</xsl:param>
<xsl:param name="page.margin.bottom">1.00in</xsl:param>
<xsl:param name="body.margin.bottom">0.25in</xsl:param>

<xsl:param name="double.sided">1</xsl:param>

<xsl:param name="header.rule">0</xsl:param>
<xsl:param name="footer.rule">0</xsl:param>

<xsl:param name="title.font.family">Ubuntu</xsl:param>
<xsl:param name="body.font.family">Ubuntu</xsl:param>

<xsl:param name="chapter.autolabel" select="1"></xsl:param>
<xsl:param name="section.autolabel" select="1"></xsl:param>
<xsl:param name="section.autolabel.max.depth">3</xsl:param>
<xsl:param name="section.label.includes.component.label" select="1"></xsl:param>

<xsl:param name="variablelist.as.blocks" select="1"></xsl:param>

<xsl:attribute-set name="formal.title.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 1.0"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>
    <xsl:attribute name="space-after.minimum">0.0em</xsl:attribute>
    <xsl:attribute name="space-after.optimum">0.4em</xsl:attribute>
    <xsl:attribute name="space-after.maximum">0.4em</xsl:attribute>
  </xsl:attribute-set> 

 <xsl:attribute-set name="section.title.properties">
  <xsl:attribute name="font-family">
    <xsl:value-of select="$title.font.family"/>
  </xsl:attribute>
  <xsl:attribute name="font-weight">bold</xsl:attribute>
  <xsl:attribute name="keep-with-next.within-column">always</xsl:attribute>
  <xsl:attribute name="text-align">left</xsl:attribute>
  <xsl:attribute name="space-before.minimum">0.8em</xsl:attribute>
  <xsl:attribute name="space-before.optimum">1.0em</xsl:attribute>
  <xsl:attribute name="space-before.maximum">1.2em</xsl:attribute> 
</xsl:attribute-set>

  <xsl:attribute-set name="section.title.level1.properties">
   <xsl:attribute name="break-before">page</xsl:attribute>
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 1.2"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>  
  </xsl:attribute-set> 

  <xsl:attribute-set name="section.title.level2.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 1.2"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>  
  </xsl:attribute-set> 

  <xsl:attribute-set name="section.title.level3.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 1.1"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>  
  </xsl:attribute-set> 

  <xsl:attribute-set name="section.title.level4.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 1.1"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>
  </xsl:attribute-set>

  <xsl:attribute-set name="section.title.level5.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 0.9"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>  
     <xsl:attribute name="start-indent">0.25in</xsl:attribute>
  </xsl:attribute-set> 

  <xsl:attribute-set name="section.title.level6.properties">
    <xsl:attribute name="font-size">
      <xsl:value-of select="$body.font.master * 0.8"/>
      <xsl:text>pt</xsl:text>
    </xsl:attribute>  
  </xsl:attribute-set> 
  
  <xsl:template name="header.content">  
  </xsl:template>

</xsl:stylesheet>
