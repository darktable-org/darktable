<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text" omit-xml-declaration="yes" indent="no" />
  <xsl:strip-space elements="*"/>
  <xsl:variable name="lowercase" select="'abcdefghijklmnopqrstuvwxyz'" />
  <xsl:variable name="uppercase" select="'ABCDEFGHIJKLMNOPQRSTUVWXYZ'" />
  <!-- The start of the gui generating functions -->

<xsl:variable name="dialog_start">(GtkWidget *dialog)
{
  GtkWidget *widget, *label, *labelev, *viewport, *box;
  GtkWidget *grid = gtk_grid_new();
  GtkSizeGroup *widget_group = gtk_size_group_new(GTK_SIZE_GROUP_BOTH);
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  int line = 0;
  char tooltip[1024];
  g_object_set_data(G_OBJECT(dialog), "local-dialog", GUINT_TO_POINTER(1));
</xsl:variable>

<xsl:variable name="dialog_end">
  g_object_unref(widget_group);
  dt_gui_dialog_add(GTK_DIALOG(dialog), grid);
  return grid;
}
</xsl:variable>

  <xsl:param name="HAVE_OPENCL">1</xsl:param>

<!-- The basic structure of the generated file -->

<xsl:template match="/">
  <xsl:text><![CDATA[/** generated file, do not edit! */
#ifndef DT_PREFERENCES_H
#define DT_PREFERENCES_H

#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include <gtk/gtk.h>
#include "control/conf.h"
#include "common/calculator.h"
#include "gui/preferences.h"

#define NON_DEF_CHAR "●"

static gboolean handle_enter_key(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  guint keyval;

  gdk_event_get_keyval ((GdkEvent*)event, &keyval);

  if(keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    return TRUE;
  return FALSE;
}

static void set_widget_label_default(GtkWidget *widget,
                                     const char *confstr,
                                     GtkWidget *label,
                                     const float factor)
{
  gboolean is_default = TRUE;

  if(GTK_IS_CHECK_BUTTON(widget))
  {
    const gboolean c_default = dt_confgen_get_bool(confstr, DT_DEFAULT);
    const gboolean c_state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    is_default = (c_state == c_default);
  }
  else if(DT_IS_BAUHAUS_WIDGET(widget))
  {
    const int v_default = dt_bauhaus_combobox_get_default(widget);
    const int v_state = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
    is_default = (v_state == v_default);
  }
  else if(GTK_IS_SPIN_BUTTON(widget))
  {
    const gchar *c_default = dt_confgen_get(confstr, DT_DEFAULT);
    const float v_default = dt_calculator_solve(1, c_default) * factor;
    const float v_state = gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget));
    is_default = (v_state == v_default);
  }
  else if(GTK_IS_ENTRY(widget))
  {
    const gchar *c_default = dt_confgen_get(confstr, DT_DEFAULT);
    const gchar *c_state = gtk_entry_get_text(GTK_ENTRY(widget));
    is_default = (g_strcmp0(c_state, c_default) == 0);
  }
  else if(GTK_IS_FILE_CHOOSER(widget))
  {
    const gchar *c_default = dt_confgen_get(confstr, DT_DEFAULT);
    const gchar *c_state = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
    is_default = (g_strcmp0(c_state, c_default) == 0);
  }
  else
  {
    // other unsupported widgets
    return;
  }

  if(is_default)
  {
    // replace * with space
    gtk_label_set_text(GTK_LABEL(label), "");
    gtk_widget_set_tooltip_text(label, NULL);
  }
  else
  {
    // replace space with *
    gtk_label_set_text(GTK_LABEL(label), NON_DEF_CHAR);
    gtk_widget_set_tooltip_text(label, _("this setting has been modified"));
  }
}

static GtkWidget *create_tab(GtkWidget *stack,
                             const char *title,
                             const char *l10n_title)
{
  GtkWidget *widget, *box;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  GtkWidget *tab_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *scroll = dt_gui_scroll_wrap(grid);
  GtkWidget *help = gtk_button_new_with_label(_("?"));
  gtk_widget_set_halign(help, GTK_ALIGN_END);
  gchar *pref = g_strdup_printf("preferences-settings/%s", title);
  g_object_set_data_full(G_OBJECT(help), "dt-help-url", pref, g_free);
  g_signal_connect(help, "clicked", G_CALLBACK(dt_gui_show_help), NULL);
  gtk_box_pack_start(GTK_BOX(tab_box), scroll, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(tab_box), help, FALSE, FALSE, 0);
  gtk_stack_add_titled(GTK_STACK(stack), tab_box, l10n_title, l10n_title);
  return grid;
}

static GtkWidget *setup_pref(GtkWidget **label,
                             GtkWidget **labelev,
                             const gchar *pref,
                             const gchar *shortdes)
{
  const gboolean is_default = dt_conf_is_default(pref);
  GtkWidget *labdef = NULL;
  if(is_default)
  {
    labdef = gtk_label_new("");
  }
  else
  {
    labdef = gtk_label_new(NON_DEF_CHAR);
    gtk_widget_set_tooltip_text(labdef, _("this setting has been modified"));
  }
  gtk_widget_set_name(labdef, "preference_non_default");
  *label = gtk_label_new_with_mnemonic(shortdes);
  gtk_label_set_xalign(GTK_LABEL(*label), .0);
  *labelev = gtk_event_box_new();
  gtk_widget_add_events(*labelev, GDK_BUTTON_PRESS_MASK);
  gtk_container_add(GTK_CONTAINER(*labelev), *label);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(*labelev), FALSE);
  return labdef;
}

static void setup_not_available(GtkWidget **widget,
                                GtkWidget *labelev)
{
  gtk_widget_destroy(*widget);
  *widget = gtk_label_new(_("not available"));
  gtk_widget_set_halign(*widget, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(labelev, _("not available on this system"));
  gtk_widget_set_tooltip_text(*widget, _("not available on this system"));
  gtk_widget_set_sensitive(labelev, FALSE);
  gtk_widget_set_sensitive(*widget, FALSE);
}

static void wrapup_pref(const gchar *name,
                        GtkWidget *grid,
                        GtkWidget *labelev,
                        GtkWidget *labdef,
                        GtkWidget *label,
                        GtkWidget *widget,
                        int *line,
                        gboolean (*callback)(GtkWidget *, GdkEventButton *, GtkWidget*))
{
  gtk_widget_set_name(widget, name);
  gtk_grid_attach(GTK_GRID(grid), labelev, 0, *line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), labdef, 1, *line, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), widget, 2, (*line)++, 1, 1);
  gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(callback), (gpointer)widget);
}

static gboolean click_widget_label(GtkWidget *label, GdkEventButton *event, GtkWidget *widget)
{
  if(event->type == GDK_BUTTON_PRESS && GTK_IS_BUTTON(widget))
  {
    gtk_button_clicked(GTK_BUTTON(widget));
    return TRUE;
  }
  else
    return FALSE;
}

static gboolean preferences_response_callback(GtkDialog *dialog, gint response_id, GtkWidget *widget)
{
  const gint dkind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "local-dialog"));
  if(dkind)
  {
    if(response_id == GTK_RESPONSE_NONE) return TRUE;
    if(response_id == GTK_RESPONSE_DELETE_EVENT) return TRUE;
  }
  else
  {
    if(response_id != GTK_RESPONSE_DELETE_EVENT) return TRUE;
  }
  gtk_widget_set_can_focus(GTK_WIDGET(dialog), TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(dialog));
  return FALSE;
}

static gboolean click_widget_toggle_set(GtkWidget *label,
                                        GdkEventButton *event,
                                        GtkWidget *widget,
                                        const gboolean value)
{
  if(!click_widget_label(label, event, widget)
     && event->type == GDK_2BUTTON_PRESS)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), value);
    return TRUE;
  }
  return FALSE;
}

static gboolean click_widget_toggle_true(GtkWidget *label,
                                         GdkEventButton *event,
                                         GtkWidget *widget)
{
  return click_widget_toggle_set(label, event, widget, TRUE);
}

static gboolean click_widget_toggle_false(GtkWidget *label,
                                          GdkEventButton *event,
                                          GtkWidget *widget)
{
  return click_widget_toggle_set(label, event, widget, FALSE);
}

static gboolean click_widget_enum(GtkWidget *label,
                                  GdkEventButton *event,
                                  GtkWidget *widget)
{
  if(!click_widget_label(label, event, widget)
     && event->type == GDK_2BUTTON_PRESS)
  {
    dt_bauhaus_widget_reset(widget);
    return TRUE;
  }
  return FALSE;
  }

gboolean restart_required = FALSE;

]]></xsl:text>

  <!-- reset callbacks -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
      <xsl:choose>
        <xsl:when test="type = 'bool'">
          <!-- do not generate CB as using a generic one -->
        </xsl:when>
        <xsl:when test="type/enum">
          <!-- do not generate CB as using a generic one -->
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>static gboolean&#xA;click_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *label, GdkEventButton *event, GtkWidget *widget)&#xA;{&#xA;  if(!click_widget_label(label, event, widget)&#xA;     &amp;&amp; event->type == GDK_2BUTTON_PRESS)&#xA;  {</xsl:text>
          <xsl:apply-templates select="." mode="reset"/>
          <xsl:text>&#xA;    return TRUE;&#xA;  }&#xA;  return FALSE;&#xA;}&#xA;&#xA;</xsl:text>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:if>
  </xsl:for-each>

  <!-- response callbacks (on dialog close) -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
      <xsl:text>static void&#xA;preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/>
      <xsl:text> (GtkDialog *dialog, gint response_id, GtkWidget *widget)&#xA;</xsl:text>
      <xsl:text>{&#xA;</xsl:text>
      <xsl:text>  if(!preferences_response_callback(dialog, response_id, widget))</xsl:text>
      <xsl:text>&#xA;  {</xsl:text>
      <xsl:apply-templates select="." mode="change"/>
      <xsl:text>&#xA;  }&#xA;</xsl:text>
      <xsl:text>}&#xA;&#xA;</xsl:text>
    </xsl:if>
  </xsl:for-each>

  <!-- restart callbacks (on change) -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:text>static void&#xA;preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *widget, gpointer user_data)&#xA;{&#xA;</xsl:text>
    <xsl:if test="@restart">
      <xsl:text>  restart_required = TRUE;&#xA;</xsl:text>
    </xsl:if>
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
  set_widget_label_default(widget, "</xsl:text>
    <xsl:value-of select="name"/><xsl:text>", GTK_WIDGET(user_data), factor);</xsl:text>
    <xsl:text>&#xA;}&#xA;&#xA;</xsl:text>
  </xsl:for-each>

  <!-- preferences tabs -->

<xsl:text>
static void init_tab_generated(GtkWidget *dialog, GtkWidget *stack)
{
  GtkSizeGroup *widget_group = gtk_size_group_new(GTK_SIZE_GROUP_BOTH);</xsl:text>

<xsl:for-each select="/dtconfiglist/dttab">
  <xsl:variable name="pref" select="@name"/>

  <xsl:text>
  {
    GtkWidget *widget, *label, *viewport, *labelev;
    GtkWidget *grid = create_tab(stack,
                                 "</xsl:text><xsl:value-of select="@title"/><xsl:text>",
                                 _("</xsl:text><xsl:value-of select="@title"/><xsl:text>"));
    int line = 0;
    char tooltip[1024];
  </xsl:text>

  <xsl:for-each select="./section">
    <xsl:variable name="section" select="@name"/>
    <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("</xsl:text><xsl:value-of select="@title"/><xsl:text>"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }</xsl:text>

    <xsl:for-each select="/dtconfiglist/dtconfig[((@section != 'general' and @section = $section) or
                                                  (@section  = 'general' and @prefs = $pref) and ($section = 'general' or not($section)))
                                                 and (name != 'opencl' or $HAVE_OPENCL = 1)]">
      <xsl:apply-templates select="." mode="tab_block"/>
    </xsl:for-each>
  </xsl:for-each>
  <xsl:text>
  }</xsl:text>
</xsl:for-each>
  <xsl:text>

  g_object_unref(widget_group);
}</xsl:text>

  <!-- dialog: collect -->

  <xsl:text>&#xA;GtkWidget *dt_prefs_init_dialog_collect</xsl:text><xsl:value-of select="$dialog_start"/>
  <xsl:for-each select="./dtconfiglist/dtconfig[@dialog='collect']">
      <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$dialog_end" />

  <!-- dialog: recentcollect -->

  <xsl:text>&#xA;GtkWidget *dt_prefs_init_dialog_recentcollect</xsl:text><xsl:value-of select="$dialog_start"/>
  <xsl:for-each select="./dtconfiglist/dtconfig[@dialog='recentcollect']">
      <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$dialog_end" />

  <!-- dialog: import -->

  <xsl:text>&#xA;GtkWidget *dt_prefs_init_dialog_import</xsl:text><xsl:value-of select="$dialog_start"/>
  <xsl:for-each select="./dtconfiglist/dtconfig[@dialog='import']">
      <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$dialog_end" />

  <!-- dialog: tagging -->

  <xsl:text>&#xA;GtkWidget *dt_prefs_init_dialog_tagging</xsl:text><xsl:value-of select="$dialog_start"/>
  <xsl:for-each select="./dtconfiglist/dtconfig[@dialog='tagging']">
      <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$dialog_end" />

  <!-- closing credits -->
  <xsl:text>&#xA;#endif&#xA;</xsl:text>

</xsl:template>

<!-- The common blocks inside of the tabs -->

<xsl:template match="dtconfig" mode="tab_block">
  <xsl:text>
  {
    GtkWidget *labdef = setup_pref(&amp;label,
                                   &amp;labelev,
                                   "</xsl:text><xsl:value-of select="name"/><xsl:text>",
                                   _("</xsl:text><xsl:value-of select="shortdescription"/><xsl:text>"));
</xsl:text>
  <xsl:apply-templates select="." mode="tab"/>
  <xsl:if test="longdescription != ''">
    <xsl:if test="contains(longdescription,'%')">
      <xsl:text>
    /* xgettext:no-c-format */</xsl:text>
    </xsl:if>
      <xsl:text>
    gtk_widget_set_tooltip_text(widget, _("</xsl:text><xsl:value-of select="longdescription"/><xsl:text>"));</xsl:text>
  </xsl:if>
    <xsl:if test="@capability">
      <xsl:text>
    if(!dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"))
    {
      setup_not_available(&amp;widget, labelev);
    }</xsl:text>
    </xsl:if>
    <xsl:text>
    wrapup_pref("</xsl:text><xsl:value-of select="name"/><xsl:text>",
                grid, labelev, labdef, label, widget, &amp;line,
                </xsl:text>
   <xsl:choose>
    <xsl:when test="type = 'bool'">
      <xsl:text>click_widget_toggle_</xsl:text><xsl:value-of select="default"/>
    </xsl:when>
    <xsl:when test="type/enum">
      <xsl:text>click_widget_enum</xsl:text>
    </xsl:when>
    <xsl:otherwise>
      <xsl:text>click_widget_</xsl:text><xsl:value-of select="generate-id(.)"/>
    </xsl:otherwise>
  </xsl:choose><xsl:text>);</xsl:text>
  <xsl:text>
  }</xsl:text>
</xsl:template>

<!-- Rules handling code specific for different types -->

<!-- RESET -->
  <xsl:template match="dtconfig[type='string']" mode="reset">
    <xsl:text>
    gtk_entry_set_text(GTK_ENTRY(widget), "</xsl:text><xsl:value-of select="default"/><xsl:text>");</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='longstring']" mode="reset">
    <xsl:text>
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)), "</xsl:text><xsl:value-of select="default"/><xsl:text>", strlen("</xsl:text><xsl:value-of select="default"/><xsl:text>"));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="reset">
    <xsl:text>    </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="reset">
    <xsl:text>    </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="reset">
    <xsl:text>    </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='bool']" mode="reset">
    <xsl:text>
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), </xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="reset">
    <xsl:variable name="default" select="default"/>
    <xsl:for-each select="./type/enum/option">
      <xsl:if test="$default = .">
    <xsl:text>
    dt_bauhaus_widget_reset(widget);</xsl:text>
      </xsl:if>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="reset">
    <xsl:text>
    gchar *path = dt_conf_expand_default_dir("</xsl:text><xsl:value-of select="default"/><xsl:text>");
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", path);
    g_free(path);
    path = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(widget), path);
    g_free(path);</xsl:text>
  </xsl:template>

<!-- CHANGE -->
  <xsl:template match="dtconfig[type='string']" mode="change">
  <xsl:text>
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_entry_get_text(GTK_ENTRY(widget)));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='longstring']" mode="change">
  <xsl:text>
    GtkTextIter start, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    gtk_text_buffer_get_start_iter(buffer, &amp;start);
    gtk_text_buffer_get_end_iter(buffer, &amp;end);
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_text_buffer_get_text(buffer, &amp;start, &amp;end, FALSE));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
  <xsl:text>
    dt_conf_set_int("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
  <xsl:text>
    dt_conf_set_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
  <xsl:text>
    dt_conf_set_float("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='bool']" mode="change">
  <xsl:text>
    dt_conf_set_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="change">
  <xsl:text>
    const gchar *index = dt_bauhaus_combobox_get_data(widget);
    gchar *s = g_strndup(index, strchr(index, ']') - index);
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", s);
    g_free(s);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="change">
  <xsl:text>
    gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", folder);
    g_free(folder);</xsl:text>
  </xsl:template>

<!-- TAB -->
  <xsl:template match="dtconfig[type='longstring']" mode="tab">
    <xsl:text>
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    widget = gtk_text_view_new_with_buffer(buffer);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(widget), GTK_WRAP_WORD);
    gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(widget), FALSE);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widget, TRUE);
    gchar *setting = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_text_buffer_set_text(buffer, setting, strlen(setting));
    g_free(setting);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(handle_enter_key), NULL);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="default"/><xsl:text>");
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>
  <xsl:template match="dtconfig[type='string']" mode="tab">
    <xsl:text>
    widget = gtk_entry_new();
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widget, TRUE);
    gchar *setting = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_entry_set_text(GTK_ENTRY(widget), setting);
    g_free(setting);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), widget);
    g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="default"/><xsl:text>");
    gtk_widget_set_tooltip_text(labelev,  tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="tab">
    <xsl:text>
    widget = gtk_file_chooser_button_new(_("select directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(widget), 20);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widget, TRUE);
    gchar *setting = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(widget), setting);
    g_free(setting);
    g_signal_connect(G_OBJECT(widget), "selection-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    gchar *default_path = dt_conf_expand_default_dir("</xsl:text><xsl:value-of select="default"/><xsl:text>");
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), default_path);
    g_free(default_path);
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="tab">
    <xsl:text>
    gint min = G_MININT;
    gint max = G_MAXINT;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    double tmp;
    tmp = min * (double)factor; min = tmp;
    tmp = max * (double)factor; max = tmp;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_size_group_add_widget(widget_group, widget);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%d'"), (int)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="tab">
    <xsl:text>
    gint64 min = G_MININT64;
    gint64 max = G_MAXINT64;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_size_group_add_widget(widget_group, widget);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    char value[100];
    snprintf(value, 100, "%"G_GINT64_FORMAT"",(gint64)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), value);
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="tab">
    <xsl:text>
    float min = -1000000000.0f;
    float max = 1000000000.0f;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>
    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 0.001f);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_float("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%.03f'"), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='bool']" mode="tab">
    <xsl:text>
    widget = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>"));
    g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), C_("preferences", "</xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>"));
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="tab">
    <xsl:text>
    widget = dt_gui_preferences_enum(NULL, "</xsl:text><xsl:value-of select="name"/><xsl:text>");</xsl:text>
    <xsl:for-each select="./type/enum/option">
      <xsl:sort select="position()" order="descending"/>
      <xsl:if test="@capability">
        <xsl:text>
    if(!dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"))
      dt_bauhaus_combobox_remove_at(widget, </xsl:text><xsl:value-of select="last()-position()"/><xsl:text>);</xsl:text>
      </xsl:if>
    </xsl:for-each>
    <xsl:text>
    gtk_widget_set_halign(widget, GTK_ALIGN_START);
    gtk_size_group_add_widget(widget_group, widget);
    g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), C_("preferences", "</xsl:text><xsl:value-of select="default"/><xsl:text>"));
    gtk_widget_set_tooltip_text(labelev, tooltip);</xsl:text>
  </xsl:template>

<!-- Grab min/max from input. Is there a better way? -->
  <xsl:template match="type[@min and @max]" mode="range" priority="5">
    <xsl:text>
    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;
    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="type[@min]" mode="range" priority="3">
    <xsl:text>
    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="type[@max]" mode="range" priority="3">
    <xsl:text>
    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="type" mode="range"  priority="1">
    <xsl:text>
    min = 0;</xsl:text>
  </xsl:template>

<!-- Also look for the factor used in the GUI. -->
  <xsl:template match="type[@factor]" mode="factor" priority="3">
    <xsl:text>
    const float factor = </xsl:text><xsl:value-of select="@factor"/><xsl:text>;</xsl:text>
  </xsl:template>

  <xsl:template match="type" mode="factor"  priority="1">
    <xsl:text>
    const float factor = 1.0f;</xsl:text>
  </xsl:template>


</xsl:stylesheet>
