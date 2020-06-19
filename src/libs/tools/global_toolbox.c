/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/collection.h"
#include "common/darktable.h"
#include "common/l10n.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/culling.h"
#include "dtgtk/thumbtable.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_tool_preferences_t
{
  GtkWidget *preferences_button, *grouping_button, *overlays_button, *help_button;
  GtkWidget *over_popup, *thumbnails_box, *culling_box;
  GtkWidget *over_label, *over_r0, *over_r1, *over_r2, *over_r3, *over_r4, *over_r5, *over_r6, *over_timeout,
      *over_tt;
  GtkWidget *over_culling_label, *over_culling_r0, *over_culling_r3, *over_culling_r4, *over_culling_r6,
      *over_culling_timeout, *over_culling_tt;
  gboolean disable_over_events;
} dt_lib_tool_preferences_t;

/* callback for grouping button */
static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for preference button */
static void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data);
/* callback for help button */
static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("preferences");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}

static void _overlays_accels_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, gpointer data)
{
  dt_thumbnail_overlay_t over = (dt_thumbnail_overlay_t)GPOINTER_TO_INT(data);
  dt_thumbtable_set_overlays_mode(dt_ui_thumbtable(darktable.gui->ui), over);
}

static void _overlays_toggle_button(GtkWidget *w, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  if(d->disable_over_events) return;

  dt_thumbnail_overlay_t over = DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r0)))
    over = DT_THUMBNAIL_OVERLAYS_NONE;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r2)))
    over = DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r3)))
    over = DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r4)))
    over = DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r5)))
    over = DT_THUMBNAIL_OVERLAYS_MIXED;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_r6)))
    over = DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK;

  dt_ui_thumbtable(darktable.gui->ui)->show_tooltips = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_tt));
  dt_thumbtable_set_overlays_mode(dt_ui_thumbtable(darktable.gui->ui), over);

  gtk_widget_set_sensitive(d->over_timeout, (over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK));

  // we don't hide the popup in case of block overlay, as the user may want to tweak the duration
  if(over != DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) gtk_widget_hide(d->over_popup);

#ifdef USE_LUA
  gboolean show = (over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED);
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper, 0, NULL, NULL, LUA_ASYNC_TYPENAME, "const char*",
                          "global_toolbox-overlay_toggle", LUA_ASYNC_TYPENAME, "bool", show, LUA_ASYNC_DONE);
#endif // USE_LUA
}

static void _overlays_toggle_culling_button(GtkWidget *w, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  if(d->disable_over_events) return;

  dt_thumbnail_overlay_t over = DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK;
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_culling_r0)))
    over = DT_THUMBNAIL_OVERLAYS_NONE;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_culling_r3)))
    over = DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL;
  else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_culling_r4)))
    over = DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED;

  dt_culling_mode_t cmode = DT_CULLING_MODE_CULLING;
  if(dt_view_lighttable_preview_state(darktable.view_manager)) cmode = DT_CULLING_MODE_PREVIEW;
  gchar *txt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling/%d", cmode);
  dt_conf_set_int(txt, over);
  g_free(txt);
  txt = dt_util_dstrcat(NULL, "plugins/lighttable/tooltips/culling/%d", cmode);
  dt_conf_set_bool(txt, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->over_culling_tt)));
  g_free(txt);
  dt_view_lighttable_culling_preview_reload_overlays(darktable.view_manager);

  gtk_widget_set_sensitive(d->over_culling_timeout, (over == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK));

  // we don't hide the popup in case of block overlay, as the user may want to tweak the duration
  if(over != DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK) gtk_widget_hide(d->over_popup);

#ifdef USE_LUA
  gboolean show = (over == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL || over == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED);
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper, 0, NULL, NULL, LUA_ASYNC_TYPENAME, "const char*",
                          "global_toolbox-overlay_toggle", LUA_ASYNC_TYPENAME, "bool", show, LUA_ASYNC_DONE);
#endif // USE_LUA
}

static void _overlays_timeout_changed(GtkWidget *w, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  const int val = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));

  if(w == d->over_timeout)
  {
    dt_thumbtable_set_overlays_block_timeout(dt_ui_thumbtable(darktable.gui->ui), val);
  }
  else if(w == d->over_culling_timeout)
  {
    dt_culling_mode_t cmode = DT_CULLING_MODE_CULLING;
    if(dt_view_lighttable_preview_state(darktable.view_manager)) cmode = DT_CULLING_MODE_PREVIEW;
    gchar *txt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling_block_timeout/%d", cmode);
    dt_conf_set_int(txt, val);
    g_free(txt);

    dt_view_lighttable_culling_preview_reload_overlays(darktable.view_manager);
  }
}

static void _overlays_show_popup(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  d->disable_over_events = TRUE;

  gboolean show = FALSE;

  // thumbnails part
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  gboolean thumbs_state;
  if(g_strcmp0(cv->module_name, "slideshow") == 0)
  {
    thumbs_state = FALSE;
  }
  else if(g_strcmp0(cv->module_name, "lighttable") == 0)
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager)
       || dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING)
    {
      thumbs_state = dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM);
    }
    else
    {
      thumbs_state = TRUE;
    }
  }
  else
  {
    thumbs_state = dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM);
  }


  if(thumbs_state)
  {
    // we write the label with the size categorie
    gchar *txt = dt_util_dstrcat(NULL, "%s %d (%d %s)", _("thumbnails overlays for size"),
                                 dt_ui_thumbtable(darktable.gui->ui)->prefs_size,
                                 dt_ui_thumbtable(darktable.gui->ui)->thumb_size, _("px"));
    gtk_label_set_text(GTK_LABEL(d->over_label), txt);
    g_free(txt);

    // we get and set the current value
    dt_thumbnail_overlay_t mode = dt_ui_thumbtable(darktable.gui->ui)->overlays;

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->over_timeout),
                              dt_ui_thumbtable(darktable.gui->ui)->overlays_block_timeout);
    gtk_widget_set_sensitive(d->over_timeout, FALSE);

    if(mode == DT_THUMBNAIL_OVERLAYS_NONE)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r0), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r2), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r3), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r4), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_MIXED)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r5), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r6), TRUE);
      gtk_widget_set_sensitive(d->over_timeout, TRUE);
    }
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_r1), TRUE);

    if(mode == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      gtk_widget_set_tooltip_text(d->over_timeout,
                                  _("duration before the block overlay is hidden after each mouse movement on the "
                                    "image\nset -1 to never hide the overlay"));
    }
    else
    {
      gtk_widget_set_tooltip_text(d->over_timeout, _("timeout only available for block overlay"));
    }

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_tt), dt_ui_thumbtable(darktable.gui->ui)->show_tooltips);

    gtk_widget_show_all(d->thumbnails_box);
    show = TRUE;
  }
  else
  {
    gtk_widget_hide(d->thumbnails_box);
  }

  // and we do the same for culling/preview if needed
  if(g_strcmp0(cv->module_name, "lighttable") == 0
     && (dt_view_lighttable_preview_state(darktable.view_manager)
         || dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING))
  {
    dt_culling_mode_t cmode = DT_CULLING_MODE_CULLING;
    if(dt_view_lighttable_preview_state(darktable.view_manager)) cmode = DT_CULLING_MODE_PREVIEW;

    // we write the label text
    if(cmode == DT_CULLING_MODE_CULLING)
      gtk_label_set_text(GTK_LABEL(d->over_culling_label), _("culling overlays"));
    else
      gtk_label_set_text(GTK_LABEL(d->over_culling_label), _("preview overlays"));

    // we get and set the current value
    gchar *otxt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling/%d", cmode);
    dt_thumbnail_overlay_t mode = dt_conf_get_int(otxt);
    g_free(otxt);

    otxt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/culling_block_timeout/%d", cmode);
    int timeout = 2;
    if(!dt_conf_key_exists(otxt))
      timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
    else
      timeout = dt_conf_get_int(otxt);
    g_free(otxt);

    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->over_culling_timeout), timeout);
    gtk_widget_set_sensitive(d->over_culling_timeout, FALSE);

    if(mode == DT_THUMBNAIL_OVERLAYS_NONE)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_culling_r0), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_culling_r3), TRUE);
    else if(mode == DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_culling_r4), TRUE);
    else
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_culling_r6), TRUE);
      gtk_widget_set_sensitive(d->over_culling_timeout, TRUE);
    }

    if(mode == DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK)
    {
      gtk_widget_set_tooltip_text(d->over_culling_timeout,
                                  _("duration before the block overlay is hidden after each mouse movement on the "
                                    "image\nset -1 to never hide the overlay"));
    }
    else
    {
      gtk_widget_set_tooltip_text(d->over_culling_timeout, _("timeout only available for block overlay"));
    }

    otxt = dt_util_dstrcat(NULL, "plugins/lighttable/tooltips/culling/%d", cmode);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->over_culling_tt), dt_conf_get_bool(otxt));
    g_free(otxt);

    gtk_widget_show_all(d->culling_box);
    show = TRUE;
  }
  else
  {
    gtk_widget_hide(d->culling_box);
  }


  if(show) gtk_widget_show(d->over_popup);
  else
    dt_control_log(_("overlays not available here..."));

  d->disable_over_events = FALSE;
}

static void _main_icons_register_size(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{

  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  /* get the css geometry properties */
  GtkBorder margin, border, padding;
  gtk_style_context_get_margin(context, state, &margin);
  gtk_style_context_get_border(context, state, &border);
  gtk_style_context_get_padding(context, state, &padding);

  /* we first remove css margin border and padding from allocation */
  int width = allocation->width - margin.left - margin.right - border.left - border.right - padding.left - padding.right;

  GtkStyleContext *ccontext = gtk_widget_get_style_context(DTGTK_BUTTON(widget)->canvas);
  GtkBorder cmargin;
  gtk_style_context_get_margin(ccontext, state, &cmargin);

  /* we remove the extra room for optical alignment */
  width = round((float)width * (1.0 - (cmargin.left + cmargin.right) / 100.0f));

  // we store the icon size in order to keep in sync thumbtable overlays
  darktable.gui->icon_size = width;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)g_malloc0(sizeof(dt_lib_tool_preferences_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* create the grouping button */
  d->grouping_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_grouping, CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), d->grouping_button, FALSE, FALSE, 0);
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(d->grouping_button, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(d->grouping_button, _("collapse grouped images"));
  g_signal_connect(G_OBJECT(d->grouping_button), "clicked", G_CALLBACK(_lib_filter_grouping_button_clicked),
                   NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), darktable.gui->grouping);

  /* create the "show/hide overlays" button */
  d->overlays_button = dtgtk_button_new(dtgtk_cairo_paint_overlays, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(d->overlays_button, _("click to change the type of overlays shown on thumbnails"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->overlays_button, FALSE, FALSE, 0);
  d->over_popup = gtk_popover_new(d->overlays_button);
  gtk_widget_set_size_request(d->over_popup, 350, -1);
#if GTK_CHECK_VERSION(3, 16, 0)
  g_object_set(G_OBJECT(d->over_popup), "transitions-enabled", FALSE, NULL);
#endif
  g_signal_connect_swapped(G_OBJECT(d->overlays_button), "button-press-event", G_CALLBACK(_overlays_show_popup),
                           self);
  // we register size of overlay icon to keep in sync thumbtable overlays
  g_signal_connect(G_OBJECT(d->overlays_button), "size-allocate", G_CALLBACK(_main_icons_register_size), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  gtk_container_add(GTK_CONTAINER(d->over_popup), vbox);

  // thumbnails overlays
  d->thumbnails_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->over_label = gtk_label_new(_("overlay mode for size"));
  gtk_widget_set_name(d->over_label, "overlays_label");
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_label, TRUE, TRUE, 0);
  d->over_r0 = gtk_radio_button_new_with_label(NULL, _("no overlays"));
  g_signal_connect(G_OBJECT(d->over_r0), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r0, TRUE, TRUE, 0);
  d->over_r1
      = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0), _("overlays on mouse hover"));
  g_signal_connect(G_OBJECT(d->over_r1), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r1, TRUE, TRUE, 0);
  d->over_r2 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0),
                                                           _("extended overlays on mouse hover"));
  g_signal_connect(G_OBJECT(d->over_r2), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r2, TRUE, TRUE, 0);
  d->over_r3 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0), _("permanent overlays"));
  g_signal_connect(G_OBJECT(d->over_r3), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r3, TRUE, TRUE, 0);
  d->over_r4 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0),
                                                           _("permanent extended overlays"));
  g_signal_connect(G_OBJECT(d->over_r4), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r4, TRUE, TRUE, 0);
  d->over_r5 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0),
                                                           _("permanent overlays extended on mouse hover"));
  g_signal_connect(G_OBJECT(d->over_r5), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_r5, TRUE, TRUE, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->over_r6 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_r0),
                                                           _("overlays block on mouse hover during (s) "));
  g_signal_connect(G_OBJECT(d->over_r6), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_box_pack_start(GTK_BOX(hbox), d->over_r6, TRUE, TRUE, 0);
  d->over_timeout = gtk_spin_button_new_with_range(-1, 99, 1);
  g_signal_connect(G_OBJECT(d->over_timeout), "value-changed", G_CALLBACK(_overlays_timeout_changed), self);
  gtk_box_pack_start(GTK_BOX(hbox), d->over_timeout, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), hbox, TRUE, TRUE, 0);
  d->over_tt = gtk_check_button_new_with_label(_("show tooltip"));
  g_signal_connect(G_OBJECT(d->over_tt), "toggled", G_CALLBACK(_overlays_toggle_button), self);
  gtk_widget_set_name(d->over_tt, "show-tooltip");
  gtk_box_pack_start(GTK_BOX(d->thumbnails_box), d->over_tt, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), d->thumbnails_box, TRUE, TRUE, 0);

  // culling/preview overlays
  d->culling_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  d->over_culling_label = gtk_label_new(_("overlay mode for size"));
  gtk_widget_set_name(d->over_culling_label, "overlays_label");
  gtk_box_pack_start(GTK_BOX(d->culling_box), d->over_culling_label, TRUE, TRUE, 0);
  d->over_culling_r0 = gtk_radio_button_new_with_label(NULL, _("no overlays"));
  g_signal_connect(G_OBJECT(d->over_culling_r0), "toggled", G_CALLBACK(_overlays_toggle_culling_button), self);
  gtk_box_pack_start(GTK_BOX(d->culling_box), d->over_culling_r0, TRUE, TRUE, 0);
  d->over_culling_r3
      = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_culling_r0), _("permanent overlays"));
  g_signal_connect(G_OBJECT(d->over_culling_r3), "toggled", G_CALLBACK(_overlays_toggle_culling_button), self);
  gtk_box_pack_start(GTK_BOX(d->culling_box), d->over_culling_r3, TRUE, TRUE, 0);
  d->over_culling_r4 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_culling_r0),
                                                                   _("permanent extended overlays"));
  g_signal_connect(G_OBJECT(d->over_culling_r4), "toggled", G_CALLBACK(_overlays_toggle_culling_button), self);
  gtk_box_pack_start(GTK_BOX(d->culling_box), d->over_culling_r4, TRUE, TRUE, 0);
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->over_culling_r6 = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(d->over_culling_r0),
                                                                   _("overlays block on mouse hover during (s) "));
  g_signal_connect(G_OBJECT(d->over_culling_r6), "toggled", G_CALLBACK(_overlays_toggle_culling_button), self);
  gtk_box_pack_start(GTK_BOX(hbox), d->over_culling_r6, TRUE, TRUE, 0);
  d->over_culling_timeout = gtk_spin_button_new_with_range(-1, 99, 1);
  g_signal_connect(G_OBJECT(d->over_culling_timeout), "value-changed", G_CALLBACK(_overlays_timeout_changed), self);
  gtk_box_pack_start(GTK_BOX(hbox), d->over_culling_timeout, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(d->culling_box), hbox, TRUE, TRUE, 0);
  d->over_culling_tt = gtk_check_button_new_with_label(_("show tooltip"));
  g_signal_connect(G_OBJECT(d->over_culling_tt), "toggled", G_CALLBACK(_overlays_toggle_culling_button), self);
  gtk_widget_set_name(d->over_culling_tt, "show-tooltip");
  gtk_box_pack_start(GTK_BOX(d->culling_box), d->over_culling_tt, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), d->culling_box, TRUE, TRUE, 0);
  gtk_widget_show(vbox);

  /* create the widget help button */
  d->help_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_help, CPF_STYLE_FLAT, NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), d->help_button, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->help_button, _("enable this, then click on a control element to see its online help"));
  g_signal_connect(G_OBJECT(d->help_button), "clicked", G_CALLBACK(_lib_help_button_clicked), d);
  dt_gui_add_help_link(d->help_button, dt_get_help_url("global_toolbox_help"));

  // the rest of these is added in reverse order as they are always put at the end of the container.
  // that's done so that buttons added via Lua will come first.

  /* create the preference button */
  d->preferences_button = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_FLAT, NULL);
  gtk_box_pack_end(GTK_BOX(self->widget), d->preferences_button, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(d->preferences_button, _("show global preferences"));
  g_signal_connect(G_OBJECT(d->preferences_button), "clicked", G_CALLBACK(_lib_preferences_button_clicked),
                   NULL);
  dt_gui_add_help_link(d->preferences_button, dt_get_help_url("global_toolbox_preferences"));
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void _lib_preferences_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}

static void _lib_filter_grouping_button_clicked(GtkWidget *widget, gpointer user_data)
{

  darktable.gui->grouping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(darktable.gui->grouping)
    gtk_widget_set_tooltip_text(widget, _("expand grouped images"));
  else
    gtk_widget_set_tooltip_text(widget, _("collapse grouped images"));
  dt_conf_set_bool("ui_last/grouping", darktable.gui->grouping);
  darktable.gui->expanded_group_id = -1;
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","global_toolbox-grouping_toggle",
      LUA_ASYNC_TYPENAME,"bool",darktable.gui->grouping,
      LUA_ASYNC_DONE);
#endif // USE_LUA
}

// TODO: this doesn't work for all widgets. the reason being that the GtkEventBox we put libs/iops into catches events.
static char *get_help_url(GtkWidget *widget)
{
  while(widget)
  {
    // if the widget doesn't have a help url set go up the widget hierarchy to find a parent that has an url
    gchar *help_url = g_object_get_data(G_OBJECT(widget), "dt-help-url");

    if(help_url)
      return help_url;

    // TODO: shall we cross from libs/iops to the core gui? if not, here is the place to break out of the loop

    widget = gtk_widget_get_parent(widget);
  }

  return NULL;
}

static void _main_do_event(GdkEvent *event, gpointer data)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)data;

  gboolean handled = FALSE;

  switch(event->type)
  {
    case GDK_BUTTON_PRESS:
    {
      // reset GTK to normal behaviour
      dt_control_allow_change_cursor();
      dt_control_change_cursor(GDK_LEFT_PTR);
      gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
      g_signal_handlers_block_by_func(d->help_button, _lib_help_button_clicked, d);
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->help_button), FALSE);
      g_signal_handlers_unblock_by_func(d->help_button, _lib_help_button_clicked, d);

      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        // TODO: When the widget doesn't have a help url set we should probably look at the parent(s)
        gchar *help_url = get_help_url(event_widget);
        if(help_url && *help_url)
        {
          GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
          dt_print(DT_DEBUG_CONTROL, "[context help] opening `%s'\n", help_url);
          char *base_url = dt_conf_get_string("context_help/url");
          // if url is https://www.darktable.org/usermanual/, it is the old deprecated
          // url and we need to update it
          if(!base_url || !*base_url || (0 == strcmp(base_url, "https://www.darktable.org/usermanual/")))
          {
            g_free(base_url);
            base_url = NULL;

            // ask the user if darktable.org may be accessed
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                                       GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                                       _("do you want to access https://darktable.gitlab.io/doc/?"));
#ifdef GDK_WINDOWING_QUARTZ
            dt_osx_disallow_fullscreen(dialog);
#endif

            gtk_window_set_title(GTK_WINDOW(dialog), _("access the online usermanual?"));
            gint res = gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            if(res == GTK_RESPONSE_YES)
            {
              base_url = g_strdup("https://darktable.gitlab.io/doc/");
              dt_conf_set_string("context_help/url", base_url);
            }
          }
          if(base_url)
          {
            gboolean is_language_supported = FALSE;
            char *lang = "en";
            GError *error = NULL;
            if(darktable.l10n!=NULL)
            {
              dt_l10n_language_t *language = NULL;
              if(darktable.l10n->selected!=-1)
                  language = (dt_l10n_language_t *)g_list_nth(darktable.l10n->languages, darktable.l10n->selected)->data;
              if (language != NULL)
                lang = language->code;
              // array of languages the usermanual supports.
              // NULL MUST remain the last element of the array
              const char *supported_languages[] = { "en", "fr", "it", "es", "de", "pl", NULL };
              int i = 0;
              while(supported_languages[i])
              {
                if(!strcmp(lang, supported_languages[i]))
                {
                  is_language_supported = TRUE;
                  break;
                }
                i++;
              }
            }
            if(!is_language_supported) lang = "en";
            char *url = g_build_path("/", base_url, lang, help_url, NULL);
            // TODO: call the web browser directly so that file:// style base for local installs works
            const gboolean uri_success = gtk_show_uri_on_window(GTK_WINDOW(win), url, gtk_get_current_event_time(), &error);
            g_free(base_url);
            g_free(url);
            if(uri_success)
            {
              dt_control_log(_("help url opened in web browser"));
            }
            else
            {
              dt_control_log(_("error while opening help url in web browser"));
              if (error != NULL) // uri_success being FALSE should guarantee that
              {
                fprintf (stderr, "Unable to read file: %s\n", error->message);
                g_error_free (error);
              }
            }
          }
        }
        else
        {
          dt_control_log(_("there is no help available for this element"));
        }
      }
      handled = TRUE;
      break;
    }
    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    {
      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        gchar *help_url = get_help_url(event_widget);
        if(help_url)
        {
          // TODO: find a better way to tell the user that the hovered widget has a help link
          dt_cursor_t cursor = event->type == GDK_ENTER_NOTIFY ? GDK_QUESTION_ARROW : GDK_X_CURSOR;
          dt_control_allow_change_cursor();
          dt_control_change_cursor(cursor);
          dt_control_forbid_change_cursor();
        }
      }
      break;
    }
    default:
      break;
  }

  if(!handled) gtk_main_do_event(event);
}

static void _lib_help_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_control_change_cursor(GDK_X_CURSOR);
  dt_control_forbid_change_cursor();
  gdk_event_handler_set(_main_do_event, user_data, NULL);
}



void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_global(NC_("accel", "grouping"), 0, 0);
  dt_accel_register_global(NC_("accel", "preferences"), 0, 0);

  dt_accel_register_global(NC_("accel", "thumbnail overlays/no overlays"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/overlays on mouse hover"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/extended overlays on mouse hover"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/permanent overlays"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/permanent extended overlays"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/permanent overlays extended on mouse hover"), 0, 0);
  dt_accel_register_global(NC_("accel", "thumbnail overlays/overlays block on mouse hover"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;

  dt_accel_connect_button_lib_as_global(self, "grouping", d->grouping_button);
  dt_accel_connect_button_lib_as_global(self, "preferences", d->preferences_button);

  dt_accel_connect_lib_as_global( self, "thumbnail overlays/no overlays",
      g_cclosure_new(G_CALLBACK(_overlays_accels_callback), GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_NONE), NULL));
  dt_accel_connect_lib_as_global(self, "thumbnail overlays/overlays on mouse hover",
                       g_cclosure_new(G_CALLBACK(_overlays_accels_callback),
                                      GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL), NULL));
  dt_accel_connect_lib_as_global(self, "thumbnail overlays/extended overlays on mouse hover",
                       g_cclosure_new(G_CALLBACK(_overlays_accels_callback),
                                      GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED), NULL));
  dt_accel_connect_lib_as_global(self, "thumbnail overlays/permanent overlays",
                       g_cclosure_new(G_CALLBACK(_overlays_accels_callback),
                                      GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL), NULL));
  dt_accel_connect_lib_as_global(self, "thumbnail overlays/permanent extended overlays",
                       g_cclosure_new(G_CALLBACK(_overlays_accels_callback),
                                      GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED), NULL));
  dt_accel_connect_lib_as_global(
      self, "thumbnail overlays/permanent overlays extended on mouse hover",
      g_cclosure_new(G_CALLBACK(_overlays_accels_callback), GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_MIXED), NULL));
  dt_accel_connect_lib_as_global(self, "thumbnail overlays/overlays block on mouse hover",
                       g_cclosure_new(G_CALLBACK(_overlays_accels_callback),
                                      GINT_TO_POINTER(DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK), NULL));
}

#ifdef USE_LUA

static int grouping_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->grouping);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->grouping != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->grouping_button), value);
    }
  }
  return 0;
}

static int show_overlays_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_tool_preferences_t *d = (dt_lib_tool_preferences_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, darktable.gui->show_overlays);
    return 1;
  }
  else
  {
    gboolean value = lua_toboolean(L, 3);
    if(darktable.gui->show_overlays != value)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlays_button), value);
    }
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);

  lua_pushcfunction(L, grouping_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "grouping");
  lua_pushcfunction(L, show_overlays_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_type(L, my_type, "show_overlays");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-grouping_toggle");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "global_toolbox-overlay_toggle");
}

#endif // USE_LUA

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
