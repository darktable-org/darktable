/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include <gdk/gdkkeysyms.h>

#include "common/selection.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *zoom;
  GtkWidget *zoom_entry;
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);

/* lightable layout changed */
static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data);
/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data);
/* zoom entry change callback */
static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event,
                                                   dt_lib_module_t *self);
/* zoom key accel callback */
static gboolean _lib_lighttable_key_accel_zoom_max_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_min_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_in_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                           guint keyval, GdkModifierType modifier,
                                                           gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_out_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_select_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                          guint keyval, GdkModifierType modifier,
                                                          gpointer data);


const char *name()
{
  return _("lighttable");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)g_malloc0(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

  GtkWidget *widget;

  /* create layout selection combobox */
  widget = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("zoomable light table"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(widget), _("file manager"));

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_conf_get_int("plugins/lighttable/layout"));

  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(_lib_lighttable_layout_changed), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);


  /* create horizontal zoom slider */
  d->zoom = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 21, 1);
  gtk_widget_set_size_request(GTK_WIDGET(d->zoom), DT_PIXEL_APPLY_DPI(140), -1);
  gtk_scale_set_draw_value(GTK_SCALE(d->zoom), FALSE);
  gtk_range_set_increments(GTK_RANGE(d->zoom), 1, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom, TRUE, TRUE, 0);

  /* manual entry of the zoom level */
  d->zoom_entry = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(d->zoom_entry), 1.0);
  gtk_entry_set_max_length(GTK_ENTRY(d->zoom_entry), 2);
  gtk_entry_set_width_chars(GTK_ENTRY(d->zoom_entry), 3);
#if GTK_CHECK_VERSION(3, 12, 0)
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->zoom_entry), 3);
#endif
  dt_gui_key_accel_block_on_focus_connect(d->zoom_entry);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->zoom), "value-changed", G_CALLBACK(_lib_lighttable_zoom_slider_changed),
                   (gpointer)self);
  g_signal_connect(d->zoom_entry, "key-press-event", G_CALLBACK(_lib_lighttable_zoom_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->zoom), dt_conf_get_int("plugins/lighttable/images_in_row"));
  _lib_lighttable_zoom_slider_changed(GTK_RANGE(d->zoom), self); // the slider defaults to 1 and GTK doesn't
                                                                 // fire a value-changed signal when setting
                                                                 // it to 1 => empty text box

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
}

void init_key_accels(dt_lib_module_t *self)
{
  // view accels
  dt_accel_register_lib(self, NC_("accel", "zoom max"), GDK_KEY_1, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom in"), GDK_KEY_2, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom out"), GDK_KEY_3, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom min"), GDK_KEY_4, GDK_MOD1_MASK);

  // selection accels
  dt_accel_register_lib(self, NC_("accel", "select all"), GDK_KEY_a, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "select none"), GDK_KEY_a, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib(self, NC_("accel", "invert selection"), GDK_KEY_i, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "select film roll"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "select untouched"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  /* setup key accelerators */

  // view accels
  dt_accel_connect_lib(self, "zoom max",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_zoom_max_callback), self, NULL));
  dt_accel_connect_lib(self, "zoom in",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_zoom_in_callback), self, NULL));
  dt_accel_connect_lib(self, "zoom out",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_zoom_out_callback), self, NULL));
  dt_accel_connect_lib(self, "zoom min",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_zoom_min_callback), self, NULL));

  // selection accels
  dt_accel_connect_lib(
      self, "select all",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_select_callback), GINT_TO_POINTER(0), NULL));
  dt_accel_connect_lib(
      self, "select none",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_select_callback), GINT_TO_POINTER(1), NULL));
  dt_accel_connect_lib(
      self, "invert selection",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_select_callback), GINT_TO_POINTER(2), NULL));
  dt_accel_connect_lib(
      self, "select film roll",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_select_callback), GINT_TO_POINTER(3), NULL));
  dt_accel_connect_lib(
      self, "select untouched",
      g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_select_callback), GINT_TO_POINTER(4), NULL));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(d->zoom_entry);
  g_free(self->data);
  self->data = NULL;
}

static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int i = gtk_range_get_value(range);
  dt_conf_set_int("plugins/lighttable/images_in_row", i);
  gchar *i_as_str = g_strdup_printf("%d", i);
  gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
  g_free(i_as_str);
  dt_control_queue_redraw_center();
}

static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:
    {
      // reset
      int i = dt_conf_get_int("plugins/lighttable/images_in_row");
      gchar *i_as_str = g_strdup_printf("%d", i);
      gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
      g_free(i_as_str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // apply zoom level
      const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->zoom_entry));
      int i = atoi(value);
      gtk_range_set_value(GTK_RANGE(d->zoom), i);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow 0 .. 9, left/right movement using arrow keys and del/backspace
    case GDK_KEY_0:
    case GDK_KEY_KP_0:
    case GDK_KEY_1:
    case GDK_KEY_KP_1:
    case GDK_KEY_2:
    case GDK_KEY_KP_2:
    case GDK_KEY_3:
    case GDK_KEY_KP_3:
    case GDK_KEY_4:
    case GDK_KEY_KP_4:
    case GDK_KEY_5:
    case GDK_KEY_KP_5:
    case GDK_KEY_6:
    case GDK_KEY_KP_6:
    case GDK_KEY_7:
    case GDK_KEY_KP_7:
    case GDK_KEY_8:
    case GDK_KEY_KP_8:
    case GDK_KEY_9:
    case GDK_KEY_KP_9:

    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Delete:
    case GDK_KEY_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data)
{
  const int i = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/lighttable/layout", i);
  dt_control_queue_redraw_center();
}

#define DT_LIBRARY_MAX_ZOOM 13
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
}

static gboolean _lib_lighttable_key_accel_zoom_max_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), 1);
  // FIXME: scroll to active image
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_min_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), DT_LIBRARY_MAX_ZOOM);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_in_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                           guint keyval, GdkModifierType modifier,
                                                           gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  if(zoom <= 1)
    zoom = 1;
  else
    zoom--;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_out_callback(GtkAccelGroup *accel_group,
                                                            GObject *acceleratable, guint keyval,
                                                            GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  if(zoom >= 2 * DT_LIBRARY_MAX_ZOOM)
    zoom = 2 * DT_LIBRARY_MAX_ZOOM;
  else
    zoom++;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_select_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                          guint keyval, GdkModifierType modifier,
                                                          gpointer data)
{
  switch(GPOINTER_TO_INT(data))
  {
    case 0: // all
      dt_selection_select_all(darktable.selection);
      break;
    case 1: // none
      dt_selection_clear(darktable.selection);
      break;
    case 2: // invert
      dt_selection_invert(darktable.selection);
      break;
    case 4: // untouched
      dt_selection_select_unaltered(darktable.selection);
      break;
    default: // case 3: same film roll
      dt_selection_select_filmroll(darktable.selection);
  }

  dt_control_queue_redraw_center();
  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
