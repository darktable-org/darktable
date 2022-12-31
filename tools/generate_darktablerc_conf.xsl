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

static void _insert_default(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->def);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->def = g_strdup(value);
}

static void _insert_values(const char *name, const char *values)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->enum_values);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->enum_values = g_strdup(values);
}

static void _insert_min(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->min);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->min = g_strdup(value);
}

static void _insert_max(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->max);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->max = g_strdup(value);
}

static void _insert_type(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(!item)
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }

  if      (!strcmp(value, "int"))   item->type = DT_INT;
  else if (!strcmp(value, "int64")) item->type = DT_INT64;
  else if (!strcmp(value, "bool"))  item->type = DT_BOOL;
  else if (!strcmp(value, "float")) item->type = DT_FLOAT;
  else if (!strcmp(value, "enum"))  item->type = DT_ENUM;
  else if (!strcmp(value, "dir"))   item->type = DT_PATH;
  else                              item->type = DT_STRING;
}

static void _insert_shortdescription(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->shortdesc);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->shortdesc = g_strdup(value);
}

static void _insert_longdescription(const char *name, const char *value)
{
  dt_confgen_value_t *item = (dt_confgen_value_t *)g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
     g_free(item->longdesc);
  }
  else
  {
     item = (dt_confgen_value_t *)g_malloc0(sizeof(dt_confgen_value_t));
     g_hash_table_insert(darktable.conf->x_confgen, g_strdup(name), item);
  }
  item->longdesc = g_strdup(value);
}

static void _free_confgen_value(void *value)
{
  dt_confgen_value_t *s = (dt_confgen_value_t *)value;
  g_free(s->def);
  g_free(s->min);
  g_free(s->max);
  g_free(s->enum_values);
  g_free(s->shortdesc);
  g_free(s->longdesc);
  g_free(s);
}

void dt_confgen_init()
{
   darktable.conf->x_confgen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _free_confgen_value);

]]></xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig">
    <xsl:variable name="default" select="default"/>
    <xsl:variable name="name" select="name"/>
    <xsl:variable name="type" select="type"/>
    <xsl:variable name="uui" select="@ui"/>
    <xsl:variable name="shortdescription" select="shortdescription"/>
    <xsl:variable name="longdescription" select="longdescription"/>

    <xsl:text>   // </xsl:text><xsl:value-of select="$name" />
    <xsl:text>&#xA;</xsl:text>

    <xsl:apply-templates select="type"/>

    <xsl:apply-templates select="shortdescription"/>

    <xsl:apply-templates select="longdescription"/>

    <xsl:text>&#xA;</xsl:text>
  </xsl:for-each>

  <xsl:text>}</xsl:text>
  <xsl:text>&#xA;</xsl:text>
<xsl:text>#endif</xsl:text>
</xsl:template>

<xsl:template match="type">
  <xsl:choose>
    <xsl:when test="enum">
      <xsl:text>   _insert_type("</xsl:text><xsl:value-of select="../name" />
      <xsl:text>", "enum");</xsl:text>
      <xsl:text>&#xA;</xsl:text>

      <xsl:text>   _insert_values("</xsl:text><xsl:value-of select="../name" />
      <xsl:text>", "</xsl:text><xsl:apply-templates select="enum"/>
      <xsl:text>");</xsl:text>
      <xsl:text>&#xA;</xsl:text>

      <!-- generate translation strings for each enum -->
      <xsl:apply-templates select="enum" mode="value"/>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>   _insert_type("</xsl:text><xsl:value-of select="../name" />
      <xsl:text>", "</xsl:text><xsl:value-of select="."/>
      <xsl:text>");</xsl:text>
      <xsl:text>&#xA;</xsl:text>
    </xsl:otherwise>
  </xsl:choose>

  <xsl:choose>
    <xsl:when test="../type = 'dir'">
      <xsl:text>   gchar *default_path = dt_conf_expand_default_dir("</xsl:text><xsl:apply-templates select="../default"/>
	  <xsl:text>");</xsl:text>
	  <xsl:text>&#xA;</xsl:text>
      <xsl:text>   _insert_default("</xsl:text><xsl:value-of select="../name" />
	  <xsl:text>", default_path);</xsl:text>
	  <xsl:text>&#xA;</xsl:text>
	  <xsl:text>   g_free(default_path);&#xA;</xsl:text>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>   _insert_default("</xsl:text><xsl:value-of select="../name" />
	  <xsl:text>", "</xsl:text><xsl:apply-templates select="../default"/>
	  <xsl:text>");</xsl:text>
	  <xsl:text>&#xA;</xsl:text>
    </xsl:otherwise>
  </xsl:choose>

  <xsl:if test="@min">
    <xsl:text>   _insert_min("</xsl:text><xsl:value-of select="../name" />
    <xsl:text>", "</xsl:text><xsl:value-of select="@min" />
    <xsl:text>");</xsl:text>
    <xsl:text>&#xA;</xsl:text>
  </xsl:if>

  <xsl:if test="@max">
    <xsl:text>   _insert_max("</xsl:text><xsl:value-of select="../name" />
    <xsl:text>", "</xsl:text><xsl:value-of select="@max" />
    <xsl:text>");</xsl:text>
    <xsl:text>&#xA;</xsl:text>
  </xsl:if>
</xsl:template>

<xsl:template match="shortdescription">
  <xsl:variable name="uui" select="../@ui"/>

  <xsl:text>   _insert_shortdescription("</xsl:text><xsl:value-of select="../name" />
  <xsl:if test="not($uui)">
    <xsl:text>", "</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes'">
    <xsl:text>", _("</xsl:text>
  </xsl:if>

  <xsl:value-of select="."/>

  <xsl:if test="not($uui)">
    <xsl:text>");</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes'">
    <xsl:text>"));</xsl:text>
  </xsl:if>

  <xsl:text>&#xA;</xsl:text>
</xsl:template>

<xsl:template match="longdescription">
  <xsl:variable name="uui" select="../@ui"/>
  <xsl:variable name="des" select="."/>

  <xsl:text>   _insert_longdescription("</xsl:text><xsl:value-of select="../name" />

  <xsl:if test="not($uui) or $des = ''">
    <xsl:text>", "</xsl:text>
  </xsl:if>

  <xsl:if test="$uui = 'yes' and $des != ''">
    <xsl:text>", _("</xsl:text>
  </xsl:if>

  <xsl:value-of select="."/>

  <xsl:if test="not($uui) or $des = ''">
    <xsl:text>");</xsl:text>
  </xsl:if>
  <xsl:if test="$uui = 'yes' and $des != ''">
    <xsl:text>"));</xsl:text>
  </xsl:if>

  <xsl:text>&#xA;</xsl:text>
</xsl:template>

<xsl:template match="default">
  <xsl:value-of select="." />
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
      <xsl:text>   const char *</xsl:text>
      <xsl:value-of select="generate-id(.)" />
      <xsl:text> = C_("preferences", "</xsl:text>
      <xsl:value-of select="." />
      <xsl:text>");&#xA;</xsl:text>
    </xsl:if>
  </xsl:for-each>
</xsl:template>

</xsl:stylesheet>
