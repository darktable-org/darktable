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
}
dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);

/* lightable layout changed */
static void _lib_lighttable_layout_changed (GtkComboBox *widget, gpointer user_data);
/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed (GtkRange *range, gpointer user_data);
/* zoom entry change callback */
static gboolean _lib_lighttable_zoom_entry_changed (GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self);
/* zoom key accel callback */
static gboolean _lib_lighttable_key_accel_zoom_max_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_min_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_in_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
						       guint keyval, GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_zoom_out_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data);


const char* name()
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
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)g_malloc(sizeof(dt_lib_tool_lighttable_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_tool_lighttable_t));

  self->widget = gtk_hbox_new(FALSE,2);

  GtkWidget* widget;

  /* create layout selection combobox */
  widget = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("zoomable light table"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("file manager"));

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget), dt_conf_get_int("plugins/lighttable/layout"));

  g_signal_connect (G_OBJECT (widget), "changed",
                    G_CALLBACK (_lib_lighttable_layout_changed),
                    (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);


  /* create horizontal zoom slider */
  d->zoom = gtk_hscale_new_with_range(1, 26, 1);
  gtk_widget_set_size_request (GTK_WIDGET(d->zoom), 160, -1);
  gtk_scale_set_draw_value(GTK_SCALE(d->zoom), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom, TRUE, TRUE, 0);

  /* manual entry of the zoom level */
  d->zoom_entry = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(d->zoom_entry), 1.0);
  gtk_entry_set_max_length(GTK_ENTRY(d->zoom_entry), 2);
  g_object_set(G_OBJECT(d->zoom_entry), "width-chars", 3, (char *)NULL);
  dt_gui_key_accel_block_on_focus(d->zoom_entry);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_entry, TRUE, TRUE, 0);

  g_signal_connect (G_OBJECT(d->zoom), "value-changed",
                    G_CALLBACK (_lib_lighttable_zoom_slider_changed),
                    (gpointer)self);
  g_signal_connect(d->zoom_entry, "key-press-event", G_CALLBACK(_lib_lighttable_zoom_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->zoom), dt_conf_get_int("plugins/lighttable/images_in_row"));

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;

}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "zoom max"),
                        GDK_1, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom in"),
                        GDK_2, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom out"),
                        GDK_3, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom min"),
                        GDK_4, GDK_MOD1_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  /* setup key accelerators */

  dt_accel_connect_lib(
      self, "zoom max",
      g_cclosure_new(
          G_CALLBACK(_lib_lighttable_key_accel_zoom_max_callback),
          self, NULL));
  dt_accel_connect_lib(
      self, "zoom in",
      g_cclosure_new(
          G_CALLBACK(_lib_lighttable_key_accel_zoom_in_callback),
          self, NULL));
  dt_accel_connect_lib(
      self, "zoom out",
      g_cclosure_new(
          G_CALLBACK(_lib_lighttable_key_accel_zoom_out_callback),
          self, NULL));
dt_accel_connect_lib(
    self, "zoom min",
    g_cclosure_new(
        G_CALLBACK(_lib_lighttable_key_accel_zoom_min_callback),
        self, NULL));
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

static void _lib_lighttable_zoom_slider_changed (GtkRange *range, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t*)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t*)self->data;

  const int i = gtk_range_get_value(range);
  dt_conf_set_int("plugins/lighttable/images_in_row", i);
  gchar *i_as_str = g_strdup_printf("%d", i);
  gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
  g_free(i_as_str);
  dt_control_queue_redraw_center();
}

static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t*)self->data;
  switch(event->keyval) {
    case GDK_Escape:
    case GDK_Tab:{
      // reset
      int i = dt_conf_get_int("plugins/lighttable/images_in_row");
      gchar *i_as_str = g_strdup_printf("%d", i);
      gtk_entry_set_text(GTK_ENTRY(d->zoom_entry), i_as_str);
      g_free(i_as_str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_Return:
    case GDK_KP_Enter:{
      // apply zoom level
      const gchar* value = gtk_entry_get_text(GTK_ENTRY(d->zoom_entry));
      int i = atoi(value);
      gtk_range_set_value(GTK_RANGE(d->zoom), i);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow 0 .. 9, left/right movement using arrow keys and del/backspace
    case GDK_0:
    case GDK_KP_0:
    case GDK_1:
    case GDK_KP_1:
    case GDK_2:
    case GDK_KP_2:
    case GDK_3:
    case GDK_KP_3:
    case GDK_4:
    case GDK_KP_4:
    case GDK_5:
    case GDK_KP_5:
    case GDK_6:
    case GDK_KP_6:
    case GDK_7:
    case GDK_KP_7:
    case GDK_8:
    case GDK_KP_8:
    case GDK_9:
    case GDK_KP_9:

    case GDK_Left:
    case GDK_Right:
    case GDK_Delete:
    case GDK_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static void _lib_lighttable_layout_changed (GtkComboBox *widget, gpointer user_data)
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

static gboolean _lib_lighttable_key_accel_zoom_max_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), 1);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_min_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), DT_LIBRARY_MAX_ZOOM);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_in_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  if(zoom <= 1) zoom = 1;
  else zoom--;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_zoom_out_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                        guint keyval, GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  if(zoom >= 2*DT_LIBRARY_MAX_ZOOM)
    zoom = 2*DT_LIBRARY_MAX_ZOOM;
  else
    zoom++;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  return TRUE;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
