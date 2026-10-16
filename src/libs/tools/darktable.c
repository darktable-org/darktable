/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/crawler.h"
#include "develop/develop.h"
#include "dtgtk/thumbtable.h"
#include "gui/gtk.h"
#include "gui/workspace.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "gui/about.h"

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)


typedef struct dt_lib_darktable_t
{
  // logo
  cairo_surface_t *image;
  guint8 *image_buffer;
  int image_width, image_height;
  // text with logo font
  cairo_surface_t *text;
  int text_width, text_height;
} dt_lib_darktable_t;


/* expose function for darktable module */
static gboolean _lib_darktable_draw_callback(GtkWidget *widget,
                                             cairo_t *cr,
                                             dt_lib_module_t *self);
/* button press callback */
static void _lib_darktable_clicked(GtkGestureSingle *gesture,
                                   gint n_press,
                                   gdouble x,
                                   gdouble y,
                                   dt_lib_module_t *self);

static void _workspace_switch_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data);
static void _workspace_create_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data);
static void _workspace_about_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return _("darktable");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_ALL;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_LEFT;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1001;
}

static void _lib_darktable_update_size(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = self->data;

  const char *wp = dt_conf_get_string_const("workspace/label");
  char *txt = g_strdup_printf("%s%s%s",
                              darktable_package_version,
                              (wp && *wp) ? " - " : "",
                              (wp && *wp) ? wp : "");

  PangoLayout *layout = gtk_widget_create_pango_layout(self->widget, txt);
  PangoFontDescription *font_desc = pango_font_description_new();
  pango_font_description_set_absolute_size(font_desc,
                                           DT_PIXEL_APPLY_DPI(10) * PANGO_SCALE);
  pango_layout_set_font_description(layout, font_desc);

  int subtitle_w = 0;
  pango_layout_get_pixel_size(layout, &subtitle_w, NULL);

  const int text_w = MAX(d->text_width, subtitle_w);
  gtk_widget_set_size_request
    (self->widget,
     d->image_width + text_w + (int)DT_PIXEL_APPLY_DPI(16),
     d->image_height + (int)DT_PIXEL_APPLY_DPI(8));

  g_free(txt);
  g_object_unref(layout);
  pango_font_description_free(font_desc);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_darktable_t *d = g_malloc0(sizeof(dt_lib_darktable_t));
  self->data = (void *)d;

  /* create drawing area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect(G_OBJECT(self->widget), "draw",
                   G_CALLBACK(_lib_darktable_draw_callback), self);
  dt_gui_connect_click(self->widget, _lib_darktable_clicked, NULL, self);

  /* create a cairo surface of dt icon */

  // first we try the SVG
  d->image = dt_util_get_logo(DT_PIXEL_APPLY_DPI(-1.0));
  if(d->image)
    d->image_buffer = cairo_image_surface_get_data(d->image);
  else
  {
    // let's fall back to the PNG
    char *logo;
    char datadir[PATH_MAX] = { 0 };

    dt_loc_get_datadir(datadir, sizeof(datadir));
    const dt_logo_season_t season = dt_util_get_logo_season();
    if(season != DT_LOGO_SEASON_NONE)
      logo = g_strdup_printf("idbutton-%d.png", (int)season);
    else
      logo = g_strdup("idbutton.png");
    char *filename = g_build_filename(datadir, "pixmaps", logo, NULL);

    cairo_surface_t *surface = cairo_image_surface_create_from_png(filename);
    g_free(logo);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "warning: can't load darktable logo from PNG file `%s'", filename);
      goto done;
    }
    const int png_width = cairo_image_surface_get_width(surface),
              png_height = cairo_image_surface_get_height(surface);

    // blow up the PNG. Ugly, but at least it has the correct size afterwards :-/
    const int width = DT_PIXEL_APPLY_DPI(png_width) * darktable.gui->ppd,
              height = DT_PIXEL_APPLY_DPI(png_height) * darktable.gui->ppd;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

    d->image_buffer = (guint8 *)calloc((size_t)stride * height, sizeof(guint8));
    d->image
        = dt_cairo_image_surface_create_for_data(d->image_buffer,
                                                 CAIRO_FORMAT_ARGB32,
                                                 width, height, stride);
    if(cairo_surface_status(d->image) != CAIRO_STATUS_SUCCESS)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "warning: can't load darktable logo from PNG file `%s'", filename);
      free(d->image_buffer);
      d->image_buffer = NULL;
      cairo_surface_destroy(d->image);
      d->image = NULL;
      goto done;
    }

    cairo_t *cr = cairo_create(d->image);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_scale(cr, darktable.gui->dpi_factor, darktable.gui->dpi_factor);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_fill(cr);
    cairo_destroy(cr);
    cairo_surface_flush(d->image);

done:
    cairo_surface_destroy(surface);
    g_free(filename);
  }

  d->image_width = d->image ? dt_cairo_image_surface_get_width(d->image) : 0;
  d->image_height = d->image ? dt_cairo_image_surface_get_height(d->image) : 0;

  /* try to load program name as svg */
  d->text = dt_util_get_logo_text(DT_PIXEL_APPLY_DPI(-1.0));
  /* no png fallback, we'll use text */
  d->text_width = d->text ? dt_cairo_image_surface_get_width(d->text) : 0;
  d->text_height = d->text ? dt_cairo_image_surface_get_height(d->text) : 0;

  _lib_darktable_update_size(self);

  static const GActionEntry entries[] = {
    { "switch", _workspace_switch_activated, "s", "''", NULL },
    { "create", _workspace_create_activated, NULL, NULL, NULL },
    { "about", _workspace_about_activated, NULL, NULL, NULL },
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries,
                                  G_N_ELEMENTS(entries), self);
  gtk_widget_insert_action_group(self->widget, "workspace",
                                 G_ACTION_GROUP(group));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = self->data;
  cairo_surface_destroy(d->image);
  free(d->image_buffer);

  // don't leak mem via text logo surface data
  guint8 *text_img_buffer = NULL;
  if(d->text)
    text_img_buffer = cairo_image_surface_get_data(d->text);
  cairo_surface_destroy(d->text);
  free(text_img_buffer);
  g_free(self->data);
  self->data = NULL;
}

static gboolean _lib_darktable_draw_callback(GtkWidget *widget,
                                             cairo_t *cr,
                                             dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = self->data;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);

  GtkStateFlags state = gtk_widget_get_state_flags(widget);

  // Get the foreground color from the CSS stylesheet
  GdkRGBA *tmpcolor;
  gtk_style_context_get(context, state, "color", &tmpcolor, NULL);

  PangoFontDescription *font_desc = NULL;
  gtk_style_context_get(context, state, "font", &font_desc, NULL);

  /* paint icon image */
  if(d->image)
  {
    cairo_set_source_surface(cr, d->image, 0, (int)DT_PIXEL_APPLY_DPI(7));
    cairo_rectangle(cr, 0, 0, d->image_width + (int)DT_PIXEL_APPLY_DPI(8),
                    d->image_height + (int)DT_PIXEL_APPLY_DPI(8));
    cairo_fill(cr);
  }

  /* create a pango layout and print fancy name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout(widget, NULL);

  /* try to use logo text in svg */
  if(d->text)
  {
    cairo_set_source_surface(cr, d->text, d->image_width + (int)DT_PIXEL_APPLY_DPI(5),
                             (int)DT_PIXEL_APPLY_DPI(12));
    cairo_rectangle(cr, 0, 0, d->image_width + d->text_width + (int)DT_PIXEL_APPLY_DPI(11),
                    d->text_height + (int)DT_PIXEL_APPLY_DPI(13));
    cairo_fill(cr);
  }
  else
  {
    /* fallback using normal text */
    pango_font_description_set_weight(font_desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(font_desc,
                                             DT_PIXEL_APPLY_DPI(25) * PANGO_SCALE);

    pango_layout_set_font_description(layout, font_desc);

    pango_layout_set_text(layout, PACKAGE_NAME, -1);
    cairo_set_source_rgba(cr, tmpcolor->red, tmpcolor->green, tmpcolor->blue, 0.7);
    cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(3.0), DT_PIXEL_APPLY_DPI(5.0));
    pango_cairo_show_layout(cr, layout);
  }

  /* print version & workspace */
  const char *wp = dt_conf_get_string_const("workspace/label");
  char *txt = g_strdup_printf("%s%s%s",
                              darktable_package_version,
                              (wp && *wp) ? " - " : "",
                              (wp && *wp) ? wp    : "");
  pango_font_description_set_absolute_size(font_desc, DT_PIXEL_APPLY_DPI(10) * PANGO_SCALE);
  pango_layout_set_font_description(layout, font_desc);
  pango_layout_set_text(layout, txt, -1);
  cairo_move_to(cr, d->image_width + DT_PIXEL_APPLY_DPI(4.0), DT_PIXEL_APPLY_DPI(32.0));
  cairo_set_source_rgba(cr, tmpcolor->red, tmpcolor->green, tmpcolor->blue, 0.7);
  pango_cairo_show_layout(cr, layout);
  g_free(txt);
  /* cleanup */
  gdk_rgba_free(tmpcolor);
  g_object_unref(layout);
  pango_font_description_free(font_desc);

  return TRUE;
}

static gboolean _lib_darktable_restart_idle(gpointer data)
{
  (void)data;
  dt_stop_backthumbs_crawler(FALSE);
  dt_control_crawler_stop(FALSE);
  dt_control_restart();
  return G_SOURCE_REMOVE;
}

static void _workspace_switch_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data)
{
  (void)action;
  (void)user_data;
  const char *key = g_variant_get_string(parameter, NULL);
  const char *label = (!key || !strcmp(key, "default")) ? "" : key;

  const char *current = dt_conf_get_string_const("workspace/label");
  if(!current) current = "";
  if(!strcmp(label, current)) return;

  dt_workspace_apply(label);
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  dt_workspace_request_relaunch_skip_picker(datadir);
  g_idle_add(_lib_darktable_restart_idle, NULL);
}

static void _workspace_copy_toggled(GtkToggleButton *check, GtkWidget *combo)
{
  gtk_widget_set_sensitive(combo, gtk_toggle_button_get_active(check));
}

static void _workspace_create_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data)
{
  (void)action;
  (void)parameter;
  (void)user_data;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog =
    gtk_dialog_new_with_buttons(_("create new workspace"), GTK_WINDOW(win),
                                GTK_DIALOG_DESTROY_WITH_PARENT,
                                _("_cancel"), GTK_RESPONSE_CANCEL,
                                _("_create"), GTK_RESPONSE_ACCEPT,
                                NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  gtk_window_set_default_size(GTK_WINDOW(dialog),
                              DT_PIXEL_APPLY_DPI(400), -1);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("workspace name"));

  GtkWidget *copy_check =
    gtk_check_button_new_with_label(_("copy settings from existing workspace"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(copy_check), FALSE);

  GtkWidget *combo = gtk_combo_box_text_new();
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  GList *labels = dt_workspace_list(datadir);
  const char *current = dt_conf_get_string_const("workspace/label");
  if(!current) current = "";
  int active = 0;
  int idx = 0;
  for(GList *l = labels; l; l = g_list_next(l))
  {
    const char *canon = (const char *)l->data;
    if(!g_strcmp0(canon, "memory")) continue;
    const char *name = !*canon ? _("default") : canon;
    const char *id = !*canon ? "default" : canon;
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, name);
    if((!*canon && !*current) || !g_strcmp0(canon, current))
      active = idx;
    idx++;
  }
  g_list_free_full(labels, g_free);
  if(idx) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
  gtk_widget_set_sensitive(combo, FALSE);
  g_signal_connect(copy_check, "toggled",
                   G_CALLBACK(_workspace_copy_toggled), combo);
  dt_gui_dialog_add(GTK_DIALOG(dialog),
                    dt_gui_hbox(gtk_label_new(_("name:")),
                                dt_gui_expand(entry)));
  dt_gui_dialog_add(GTK_DIALOG(dialog), copy_check);
  dt_gui_dialog_add(GTK_DIALOG(dialog), combo);
  gtk_widget_show_all(dialog);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *label = g_strdup(text ? text : "");
    g_strstrip(label);

    const char *copy_from = NULL;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(copy_check)))
    {
      copy_from = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
      if(copy_from && !strcmp(copy_from, "default")) copy_from = "";
    }

    if(!*label)
      dt_control_log(_("enter a workspace name"));
    else if(g_ascii_strcasecmp(label, "default") == 0
            || g_ascii_strcasecmp(label, "memory") == 0
            || strcmp(label, _("default")) == 0
            || strcmp(label, _("memory")) == 0)
      dt_control_log(_("the names \"default\" and \"memory\" are reserved"));
    else if(!dt_workspace_new(datadir, label, copy_from))
      dt_control_log(_("could not create workspace \"%s\""), label);
    else
    {
      dt_workspace_request_relaunch_skip_picker(datadir);
      g_idle_add(_lib_darktable_restart_idle, NULL);
    }
    g_free(label);
  }

  gtk_widget_destroy(dialog);
}

static void _workspace_about_activated(GSimpleAction *action,
                                        GVariant *parameter,
                                        gpointer user_data)
{
  (void)action;
  (void)parameter;
  (void)user_data;
  darktable_show_about_dialog();
}

static void _lib_darktable_clicked(GtkGestureSingle *gesture,
                                   gint n_press,
                                   gdouble x,
                                   gdouble y,
                                   dt_lib_module_t *self)
{
  (void)gesture;
  (void)n_press;
  (void)x;
  (void)y;

  if(!dt_conf_get_bool("database/multiple_workspace"))
  {
    darktable_show_about_dialog();
    return;
  }

  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  GList *labels = dt_workspace_list(datadir);

  GMenu *menu = g_menu_new();

  GMenu *workspaces = g_menu_new();
  for(GList *l = labels; l; l = g_list_next(l))
  {
    const char *canon = (const char *)l->data;
    const char *name = !*canon ? _("default")
                       : !g_strcmp0(canon, "memory") ? _("memory")
                       : canon;
    const char *key = !*canon ? "default" : canon;
    gchar *action = g_strdup_printf("workspace.switch::%s", key);
    g_menu_append(workspaces, name, action);
    g_free(action);
  }
  g_list_free_full(labels, g_free);
  g_menu_append_section(menu, _("workspaces"), G_MENU_MODEL(workspaces));
  g_object_unref(workspaces);

  GMenu *actions = g_menu_new();
  g_menu_append(actions, _("create new workspace"), "workspace.create");
  g_menu_append(actions, _("about darktable"), "workspace.about");
  g_menu_append_section(menu, _("actions"), G_MENU_MODEL(actions));
  g_object_unref(actions);

  const char *current = dt_conf_get_string_const("workspace/label");
  if(!current) current = "";
  const char *current_key = !*current ? "default" : current;

  GAction *switch_action =
    g_action_map_lookup_action(
      G_ACTION_MAP(gtk_widget_get_action_group(self->widget, "workspace")),
      "switch");
  if(switch_action)
    g_simple_action_set_state(G_SIMPLE_ACTION(switch_action),
                              g_variant_new_string(current_key));

  GtkWidget *pop = dt_gui_popover_menu_from_model(self->widget, menu);
  g_object_unref(menu);
  /* floor width so short names don't shrink the section rules */
  gtk_widget_set_size_request(pop, DT_PIXEL_APPLY_DPI(220), -1);
  gtk_popover_popup(GTK_POPOVER(pop));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
