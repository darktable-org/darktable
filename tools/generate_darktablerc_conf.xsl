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

#pragma GCC diagnostic ignored "-Wunused-variable"

#include "control/conf.h"

#define WRAP_TRANSLATION(text)

typedef struct {
   const char *name;		// configuration variable's name (path)
   const char *type;		// variable's type (int, string, enum, etc.)
   const char *def;		// default value
   const char *enum_values;	// listing of possible values for an enum, in format "[A][B][C]...[Z]"
   const char *min;		// minimum value (optional, may be NULL or empty string)
   const char *max;		// maximum value (optional, may be NULL or empty string)
   const char *shortdesc;	// short one-line description
   const char *longdesc;	// long, potentially multi-line description (optional)
} _default_config_t;

static void _clear_confgen_value(void *value)
{
  dt_confgen_value_t *s = (dt_confgen_value_t *)value;
  g_free(s->def);
  g_free(s->min);
  s->min = NULL;
  g_free(s->max);
  s->max = NULL;
  g_free(s->enum_values);
  s->enum_values = NULL;
  g_free(s->shortdesc);
  s->shortdesc = NULL;
  g_free(s->longdesc);
  s->longdesc = NULL;
}

static void _free_confgen_value(void *value)
{
  dt_confgen_value_t *s = (dt_confgen_value_t *)value;
  _clear_confgen_value(value);
  g_free(s);
}

static char *_copy_string(const char *s)
{
  return s && *s ? g_strdup(s) : NULL;
}

static _default_config_t _config_variables[] =
{
]]></xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig">
    <xsl:text>  {&#xA;    "</xsl:text>
    <xsl:value-of select="name" />
    <xsl:text>",&#xA;    "</xsl:text>
    <xsl:apply-templates select="type"/>
    <xsl:text>    "</xsl:text>
    <xsl:value-of select="type/@min" />
    <xsl:text>", "</xsl:text>
    <xsl:value-of select="type/@max" />
    <xsl:text>",&#xA;    </xsl:text>
    <xsl:apply-templates select="shortdescription"/>
    <xsl:text>,&#xA;    </xsl:text>
    <xsl:apply-templates select="longdescription"/>
    <xsl:text>&#xA;  },&#xA;</xsl:text>
  </xsl:for-each>

  <xsl:text><![CDATA[};

void dt_confgen_init()
{
   darktable.conf->x_confgen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _free_confgen_value);
   for(int i = 0; i < sizeof(_config_variables)/sizeof(_config_variables[0]); i++)
   {
     _default_config_t *var = &_config_variables[i];
     dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, var->name);
     if(item)
     {
       _clear_confgen_value(item);
     }
     else
     {
       item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
       g_hash_table_insert(darktable.conf->x_confgen, g_strdup(var->name), item);
     }
     if      (!strcmp(var->type, "int"))   item->type = DT_INT;
     else if (!strcmp(var->type, "int64")) item->type = DT_INT64;
     else if (!strcmp(var->type, "bool"))  item->type = DT_BOOL;
     else if (!strcmp(var->type, "float")) item->type = DT_FLOAT;
     else if (!strcmp(var->type, "enum"))  item->type = DT_ENUM;
     else if (!strcmp(var->type, "dir"))   item->type = DT_PATH;
     else                                  item->type = DT_STRING;
     if(item->type == DT_PATH)
       item->def = dt_conf_expand_default_dir(var->def);
     else
       item->def = g_strdup(var->def);
     item->min = _copy_string(var->min);
     item->max = _copy_string(var->max);
     item->enum_values = _copy_string(var->enum_values);
     item->shortdesc = _copy_string(var->shortdesc);
     item->longdesc = _copy_string(var->longdesc);
   }
}

#endif
]]></xsl:text>
</xsl:template>

<xsl:template match="type">
  <xsl:choose>
    <xsl:when test="enum">
      <xsl:text>enum", "</xsl:text><xsl:value-of select="../default"/>
      <xsl:text>", "</xsl:text><xsl:apply-templates select="enum"/>
      <xsl:text>",&#xA;</xsl:text>
      <!-- generate translation strings for each enum -->
      <xsl:apply-templates select="enum" mode="value"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:value-of select="."/>
      <xsl:text>", "</xsl:text><xsl:value-of select="../default"/>
      <xsl:text>", "",&#xA;</xsl:text>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template match="shortdescription">
  <xsl:variable name="uui" select="../@ui"/>
  <xsl:if test="not($uui)">
    <xsl:text>"</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes'">
    <xsl:text>N_("</xsl:text>
  </xsl:if>

  <xsl:value-of select="."/>

  <xsl:if test="not($uui)">
    <xsl:text>"</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes'">
    <xsl:text>")</xsl:text>
  </xsl:if>

</xsl:template>

<xsl:template match="longdescription">
  <xsl:variable name="uui" select="../@ui"/>
  <xsl:variable name="des" select="."/>

  <xsl:if test="not($uui) or $des = ''">
    <xsl:text>"</xsl:text>
  </xsl:if>

  <xsl:if test="$uui = 'yes' and $des != ''">
    <xsl:text>N_("</xsl:text>
  </xsl:if>

  <xsl:value-of select="."/>

  <xsl:if test="not($uui) or $des = ''">
    <xsl:text>"</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes' and $des != ''">
    <xsl:text>")</xsl:text>
  </xsl:if>

</xsl:template>

<xsl:template match="enum">
  <xsl:for-each select="option">
    <xsl:text>[</xsl:text>
    <xsl:value-of select="." />
    <xsl:text>]</xsl:text>
  </xsl:for-each>
</xsl:template>

<xsl:template match="enum" mode="value">
  <xsl:for-each select="option">
    <xsl:if test="number(.) != .">
      <xsl:text>     WRAP_TRANSLATION(C_("preferences", "</xsl:text>
      <xsl:value-of select="." />
      <xsl:text>"))&#xA;</xsl:text>
    </xsl:if>
  </xsl:for-each>
</xsl:template>

</xsl:stylesheet>
