<?xml version="1.0" encoding="utf-8"?>
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <xsl:output method="text" omit-xml-declaration="yes" indent="no" />
  <xsl:strip-space elements="*"/>
  <xsl:variable name="lowercase" select="'abcdefghijklmnopqrstuvwxyz'" />
  <xsl:variable name="uppercase" select="'ABCDEFGHIJKLMNOPQRSTUVWXYZ'" />
  <!-- The start of the gui generating functions -->
  <xsl:variable name="tab_start"> (GtkWidget *dialog, GtkWidget *stack)
{
  GtkWidget *widget, *label, *labelev, *viewport, *box;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  int line = 0;
  char tooltip[1024];
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE); // doesn't seem to work from gtkrc
  gtk_container_add(GTK_CONTAINER(scroll), viewport);
  gtk_container_add(GTK_CONTAINER(viewport), grid);
</xsl:variable>

  <xsl:variable name="tab_end">
  gtk_widget_show_all(stack);
}
</xsl:variable>

<xsl:variable name="dialog_start">(GtkWidget *dialog)
{
  GtkWidget *widget, *label, *labelev, *viewport, *box;
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  int line = 0;
  char tooltip[1024];
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  g_object_set_data(G_OBJECT(dialog), "local-dialog", GUINT_TO_POINTER(1));
</xsl:variable>

<xsl:variable name="dialog_end">
  gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);
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

#define NON_DEF_CHAR "●"

static gboolean handle_enter_key(GtkWidget *widget, GdkEvent *event, gpointer data)
{
  guint keyval;

  gdk_event_get_keyval ((GdkEvent*)event, &keyval);

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
     return TRUE;
  return FALSE;
}

static void set_widget_label_default(GtkWidget *widget, const char *confstr, GtkWidget *label, const float factor)
{
  gboolean is_default = TRUE;

  if (GTK_IS_CHECK_BUTTON(widget))
  {
    const gboolean c_default = dt_confgen_get_bool(confstr, DT_DEFAULT);
    const gboolean c_state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    is_default = (c_state == c_default);
  }
  else if(GTK_IS_COMBO_BOX(widget))
  {
    const gchar *c_default = dt_confgen_get(confstr, DT_DEFAULT);
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    const gint active = gtk_combo_box_get_active(GTK_COMBO_BOX(widget));
    gchar *c_state = NULL;
    gtk_tree_model_iter_nth_child(model, &iter, NULL, active);
    gtk_tree_model_get(model, &iter, 0, &c_state, -1);
    is_default = (g_strcmp0(c_state, c_default) == 0);
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
    g_object_set(label, "tooltip-text", NULL, (gchar *)0);
  }
  else
  {
    // replace space with *
    gtk_label_set_text(GTK_LABEL(label), NON_DEF_CHAR);
    g_object_set(label, "tooltip-text", _("this setting has been modified"), (gchar *)0);
  }
}

gboolean restart_required = FALSE;
]]></xsl:text>

  <!-- reset callbacks -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
      <xsl:text>static gboolean&#xA;reset_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *label, GdkEventButton *event, GtkWidget *widget)&#xA;{&#xA;  if(event->type == GDK_2BUTTON_PRESS)&#xA;  {&#xA;</xsl:text>
      <xsl:apply-templates select="." mode="reset"/>
      <xsl:text>&#xA;    return TRUE;&#xA;  }&#xA;  return FALSE;&#xA;}&#xA;&#xA;</xsl:text>
    </xsl:if>
  </xsl:for-each>

  <!-- response callbacks (on dialog close) -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
      <xsl:text>static void&#xA;preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/>
      <xsl:text> (GtkDialog *dialog, gint response_id, GtkWidget *widget)&#xA;</xsl:text>
      <xsl:text>{&#xA;</xsl:text>
      <xsl:text>  const gint dkind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(dialog), "local-dialog"));&#xA;</xsl:text>
      <xsl:text>  if(dkind)&#xA;</xsl:text>
      <xsl:text>  {&#xA;</xsl:text>
      <xsl:text>    if(response_id == GTK_RESPONSE_NONE) return;&#xA;</xsl:text>
      <xsl:text>    if(response_id == GTK_RESPONSE_DELETE_EVENT) return;&#xA;</xsl:text>
      <xsl:text>  }&#xA;</xsl:text>
      <xsl:text>  else&#xA;</xsl:text>
      <xsl:text>  {&#xA;</xsl:text>
      <xsl:text>    if(response_id != GTK_RESPONSE_DELETE_EVENT) return;&#xA;</xsl:text>
      <xsl:text>  }&#xA;</xsl:text>
      <xsl:text>  gtk_widget_set_can_focus(GTK_WIDGET(dialog), TRUE);&#xA;</xsl:text>
      <xsl:text>  gtk_widget_grab_focus(GTK_WIDGET(dialog));&#xA;</xsl:text>
      <xsl:apply-templates select="." mode="change"/>
      <xsl:text>&#xA;}&#xA;&#xA;</xsl:text>
    </xsl:if>
  </xsl:for-each>

  <!-- restart callbacks (on change) -->

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs or @dialog]">
    <xsl:text>static void&#xA;preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text> (GtkWidget *widget, gpointer user_data)&#xA;{&#xA;</xsl:text>
    <xsl:if test="@restart">
      <xsl:text>  restart_required = TRUE;&#xA;</xsl:text>
    </xsl:if>
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>  set_widget_label_default(widget, "</xsl:text>
    <xsl:value-of select="name"/><xsl:text>", GTK_WIDGET(user_data), factor);</xsl:text>
    <xsl:text>&#xA;}&#xA;&#xA;</xsl:text>
  </xsl:for-each>

  <!-- preferences tabs -->

  <!-- lighttable -->

  <xsl:text>&#xA;static void&#xA;init_tab_lighttable</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("lighttable"), _("lighttable"));&#xA;</xsl:text>

  <!-- general section -->
  <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("general"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='lighttable' and @section='general']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- module section -->

  <xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("thumbnails"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='lighttable' and @section='thumbs']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <xsl:value-of select="$tab_end" />

  <!-- darkroom -->

  <xsl:text>&#xA;static void&#xA;init_tab_darkroom</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("darkroom"), _("darkroom"));&#xA;</xsl:text>

  <!-- general section -->
  <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("general"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='darkroom' and @section='general']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- module section -->

  <xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("modules"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='darkroom' and @section='modules']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <xsl:value-of select="$tab_end" />

  <!-- processing -->

  <xsl:text>&#xA;static void&#xA;init_tab_processing</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("processing"), _("processing"));&#xA;</xsl:text>

  <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("image processing"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='processing' and @section='general']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- cpu/gpu/memory -->

  <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("cpu / gpu / memory"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='processing' and @section='cpugpu']">
    <xsl:if test="name != 'opencl' or $HAVE_OPENCL=1">
      <xsl:apply-templates select="." mode="tab_block"/>
    </xsl:if>
  </xsl:for-each>
  <xsl:value-of select="$tab_end" />

  <!-- security -->

  <xsl:text>&#xA;static void&#xA;init_tab_security</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("security"), _("security"));&#xA;</xsl:text>

  <!-- general (confirmations) section -->
  <xsl:text>
    {
      GtkWidget *seclabel = gtk_label_new(_("general"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
    }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='security' and @section='general']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- others section -->
  <xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("other"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='security' and @section='other']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$tab_end" />

  <!-- storage -->

  <xsl:text>&#xA;static void&#xA;init_tab_storage</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("storage"), _("storage"));&#xA;</xsl:text>

<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("database"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='storage' and @section='database']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("xmp"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='storage' and @section='xmp']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$tab_end" />

  <!-- miscellaneous -->

  <xsl:text>&#xA;static void&#xA;init_tab_misc</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("miscellaneous"), _("miscellaneous"));&#xA;</xsl:text>
<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("interface"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='misc' and @section='interface']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("tags"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='misc' and @section='tags']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("shortcuts with multiple instances"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
      g_object_set(lbox,  "tooltip-text", _("where multiple module instances are present, these preferences control rules that are applied (in order) to decide which module instance shortcuts will be applied to"), (gchar *)0);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='misc' and @section='accel']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- other views -->

  <xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("map / geolocalization view"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
  </xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='otherviews' and @section='geoloc']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>

  <!-- slideshow section -->
  <xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("slideshow view"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='otherviews' and @section='slideshow']">
    <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$tab_end" />

  <!-- import -->

  <xsl:text>&#xA;static void&#xA;init_tab_import</xsl:text><xsl:value-of select="$tab_start"/><xsl:text>  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("import"), _("import"));&#xA;</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='import' and @section='import']">
          <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
<xsl:text>
   {
      GtkWidget *seclabel = gtk_label_new(_("session options"));
      GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
      gtk_box_pack_start(GTK_BOX(lbox), seclabel, FALSE, FALSE, 0);
      gtk_widget_set_name(lbox, "pref_section");
      gtk_grid_attach(GTK_GRID(grid), lbox, 0, line++, 2, 1);
   }
</xsl:text>

  <xsl:for-each select="./dtconfiglist/dtconfig[@prefs='import' and @section='session']">
          <xsl:apply-templates select="." mode="tab_block"/>
  </xsl:for-each>
  <xsl:value-of select="$tab_end" />

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
    const gboolean is_default = dt_conf_is_default("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    GtkWidget *labdef;
    if(is_default)
    {
       labdef = gtk_label_new("");
    }
    else
    {
       labdef = gtk_label_new(NON_DEF_CHAR);
       g_object_set(labdef, "tooltip-text", _("this setting has been modified"), (gchar *)0);
    }
    gtk_widget_set_name(labdef, "preference_non_default");
    label = gtk_label_new(_("</xsl:text><xsl:value-of select="shortdescription"/><xsl:text>"));
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
</xsl:text>
  <xsl:apply-templates select="." mode="tab"/>
  <xsl:text>    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);</xsl:text>
  <xsl:if test="longdescription != ''">
    <xsl:if test="contains(longdescription,'%')">
      <xsl:text>    
    /* xgettext:no-c-format */</xsl:text>
    </xsl:if>
    <xsl:text>&#xA;    g_object_set(widget, "tooltip-text", _("</xsl:text><xsl:value-of select="longdescription"/><xsl:text>"), (gchar *)0);</xsl:text>
  </xsl:if>
        <xsl:choose>
                <xsl:when test="@capability">
                        <xsl:text>
    GtkWidget *notavailable = gtk_label_new(_("not available"));
    gtk_widget_set_halign(notavailable, GTK_ALIGN_START);
    gtk_widget_set_sensitive(notavailable, FALSE);
    g_object_set(notavailable, "tooltip-text", _("not available on this system"), (gchar *)0);
    gtk_widget_set_sensitive(labelev, dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"));
    gtk_widget_set_sensitive(widget, dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"));
    if(!dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>"))
      g_object_set(labelev, "tooltip-text", _("not available on this system"), (gchar *)0);
    gtk_grid_attach(GTK_GRID(grid), labelev, 0, line, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>") ? box : notavailable, 2, line++, 1, 1);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), (gpointer)widget);
</xsl:text>
                </xsl:when>
                <xsl:otherwise>
                        <xsl:text>
    gtk_widget_set_name(widget, "</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_grid_attach(GTK_GRID(grid), labelev, 0, line, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), labdef, 1, line, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), box, 2, line++, 1, 1);
    g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), (gpointer)widget);
</xsl:text>
                </xsl:otherwise>
        </xsl:choose>
  <xsl:text>
  }
</xsl:text>
</xsl:template>

<!-- Rules handling code specific for different types -->

<!-- RESET -->
  <xsl:template match="dtconfig[type='string']" mode="reset">
    <xsl:text>    gtk_entry_set_text(GTK_ENTRY(widget), "</xsl:text><xsl:value-of select="default"/><xsl:text>");</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='longstring']" mode="reset">
     <xsl:text>
        gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)), "</xsl:text><xsl:value-of select="default"/><xsl:text>", strlen("</xsl:text><xsl:value-of select="default"/><xsl:text>"));
     </xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="reset">
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="reset">
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="reset">
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='bool']" mode="reset">
    <xsl:text>    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), </xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="reset">
    <xsl:variable name="default" select="default"/>
    <xsl:for-each select="./type/enum/option">
      <xsl:if test="$default = .">
        <xsl:text>    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), </xsl:text><xsl:value-of select="position()-1"/><xsl:text>);</xsl:text>
      </xsl:if>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="reset">
    <xsl:text>    gchar *path = dt_conf_expand_default_dir("</xsl:text><xsl:value-of select="default"/><xsl:text>");
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", path);
    g_free(path);
    path = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(widget), path);
    g_free(path);</xsl:text>
  </xsl:template>

<!-- CALLBACK -->
  <xsl:template match="dtconfig[type='string']" mode="change">
    <xsl:text>  dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_entry_get_text(GTK_ENTRY(widget)));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='longstring']" mode="change">
     <xsl:text>
        GtkTextIter start, end;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
        gtk_text_buffer_get_start_iter(buffer, &amp;start);
        gtk_text_buffer_get_end_iter(buffer, &amp;end);
        dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_text_buffer_get_text(buffer, &amp;start, &amp;end, FALSE));
     </xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>  dt_conf_set_int("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>  dt_conf_set_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="change">
    <xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>  dt_conf_set_float("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)) / factor);</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='bool']" mode="change">
    <xsl:text>  dt_conf_set_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="change">
    <xsl:text>  GtkTreeIter iter;
  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &amp;iter))
  {
    gchar *s = NULL;
    gtk_tree_model_get(gtk_combo_box_get_model(GTK_COMBO_BOX(widget)), &amp;iter, 0, &amp;s, -1);
    dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", s);
    g_free(s);
  }</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="change">
    <xsl:text>  gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  dt_conf_set_string("</xsl:text><xsl:value-of select="name"/><xsl:text>", folder);
  g_free(folder);</xsl:text>
  </xsl:template>

<!-- TAB -->
  <xsl:template match="dtconfig[type='longstring']" mode="tab">
  <xsl:text>    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    widget = gtk_text_view_new_with_buffer(buffer);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
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
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>
  <xsl:template match="dtconfig[type='string']" mode="tab">
    <xsl:text>    widget = gtk_entry_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widget, TRUE);
    gchar *setting = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_entry_set_text(GTK_ENTRY(widget), setting);
    g_free(setting);
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), "</xsl:text><xsl:value-of select="default"/><xsl:text>");
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='dir']" mode="tab">
    <xsl:text>    widget = gtk_file_chooser_button_new(_("select directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, TRUE, TRUE, 0);
    gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(widget), 20);
    gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(widget, TRUE);
    gchar *setting = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(widget), setting);
    g_free(setting);
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "selection-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    gchar *default_path = dt_conf_expand_default_dir("</xsl:text><xsl:value-of select="default"/><xsl:text>");
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), default_path);
    g_free(default_path);
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
    </xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int']" mode="tab">
    <xsl:text>    gint min = G_MININT;&#xA;    gint max = G_MAXINT;&#xA;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    double tmp;
    tmp = min * (double)factor; min = tmp;
    tmp = max * (double)factor; max = tmp;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(widget, FALSE);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%d'"), (int)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='int64']" mode="tab">
    <xsl:text>    gint64 min = G_MININT64;&#xA;    gint64 max = G_MAXINT64;&#xA;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 1);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(widget, FALSE);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_int64("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    char value[100];
    snprintf(value, 100, "%"G_GINT64_FORMAT"",(gint64)(</xsl:text><xsl:value-of select="default"/><xsl:text> * factor));
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), value);
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type='float']" mode="tab">
    <xsl:text>    float min = -1000000000.0f;&#xA;    float max = 1000000000.0f;&#xA;</xsl:text>
    <xsl:apply-templates select="type" mode="range"/>
    <xsl:text>  </xsl:text><xsl:apply-templates select="type" mode="factor"/>
    <xsl:text>    min *= factor; max *= factor;
    widget = gtk_spin_button_new_with_range(min, max, 0.001f);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(widget, FALSE);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(widget), 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), dt_conf_get_float("</xsl:text><xsl:value-of select="name"/><xsl:text>") * factor);
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%.03f'"), </xsl:text><xsl:value-of select="default"/><xsl:text> * factor);
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

    <xsl:template match="dtconfig[type='bool']" mode="tab">
    <xsl:text>    widget = gtk_check_button_new();
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), dt_conf_get_bool("</xsl:text><xsl:value-of select="name"/><xsl:text>"));
    </xsl:text>
    <xsl:text>g_signal_connect(G_OBJECT(widget), "toggled", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), C_("preferences", "</xsl:text><xsl:value-of select="translate(default, $lowercase, $uppercase)"/><xsl:text>"));
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

  <xsl:template match="dtconfig[type/enum]" mode="tab">
    <xsl:text>    GtkTreeIter iter;
    GtkListStore *store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_STRING);
    gchar *str = dt_conf_get_string("</xsl:text><xsl:value-of select="name"/><xsl:text>");
    gint pos = -1;
</xsl:text>
    <xsl:for-each select="./type/enum/option">
        <xsl:if test="@capability">
          <xsl:text>if(dt_capabilities_check("</xsl:text><xsl:value-of select="@capability"/><xsl:text>")) {</xsl:text>
        </xsl:if>
        <xsl:text>
        gtk_list_store_append(store, &amp;iter);
        gtk_list_store_set(store, &amp;iter, 0, "</xsl:text><xsl:value-of select="."/><xsl:text>", 1, C_("preferences", "</xsl:text><xsl:value-of select="."/><xsl:text>"), -1);
        if(pos == -1 &amp;&amp; strcmp(str, "</xsl:text><xsl:value-of select="."/><xsl:text>") == 0)
          pos = </xsl:text><xsl:value-of select="position()-1" /><xsl:text>;
      </xsl:text>
        <xsl:if test="@capability">
          <xsl:text>}
          </xsl:text>
        </xsl:if>
    </xsl:for-each>
    <xsl:text>

    g_free(str);

    widget = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
    gtk_widget_set_hexpand(widget, FALSE);
    g_object_unref(store);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_cell_renderer_set_padding(renderer, 0, 0);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(widget), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(widget), renderer, "text", 1, NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), pos);
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(box), widget, FALSE, FALSE, 0);
    </xsl:text>
    <xsl:text> g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(preferences_changed_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), labdef);</xsl:text>
    <xsl:text>
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(preferences_response_callback_</xsl:text><xsl:value-of select="generate-id(.)"/><xsl:text>), widget);
    snprintf(tooltip, 1024, _("double click to reset to `%s'"), C_("preferences", "</xsl:text><xsl:value-of select="default"/><xsl:text>"));
    g_object_set(labelev,  "tooltip-text", tooltip, (gchar *)0);
</xsl:text>
  </xsl:template>

<!-- Grab min/max from input. Is there a better way? -->
  <xsl:template match="type[@min and @max]" mode="range" priority="5">
    <xsl:text>    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;&#xA;</xsl:text>
    <xsl:text>    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="type[@min]" mode="range" priority="3">
    <xsl:text>    min = </xsl:text><xsl:value-of select="@min"/><xsl:text>;&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="type[@max]" mode="range" priority="3">
    <xsl:text>    max = </xsl:text><xsl:value-of select="@max"/><xsl:text>;&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="type" mode="range"  priority="1">
    <xsl:text>    min = 0;&#xA;</xsl:text>
  </xsl:template>

<!-- Also look for the factor used in the GUI. -->
  <xsl:template match="type[@factor]" mode="factor" priority="3">
    <xsl:text>  float factor = </xsl:text><xsl:value-of select="@factor"/><xsl:text>;&#xA;</xsl:text>
  </xsl:template>

  <xsl:template match="type" mode="factor"  priority="1">
    <xsl:text>  float factor = 1.0f;&#xA;</xsl:text>
  </xsl:template>


</xsl:stylesheet>
