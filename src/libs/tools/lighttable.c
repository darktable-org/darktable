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

#include "common/collection.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_tool_lighttable_t
{
  GtkWidget *zoom;
  GtkWidget *zoom_entry;
  GtkWidget *layout_combo;
  dt_lighttable_layout_t layout, base_layout;
  int current_zoom;
  GtkWidget *display_num_images; // slider & entry for number of images to be displayed in culling mode
  GtkWidget *display_num_images_entry;
  int current_display_num_images; // number of images to be displayed in culling mode
} dt_lib_tool_lighttable_t;

/* set zoom proxy function */
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom);
static gint _lib_lighttable_get_zoom(dt_lib_module_t *self);

/* get/set layout proxy function */
static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self);
static void _lib_lighttable_set_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout);

static void _lib_lighttable_set_display_num_images(dt_lib_module_t *self, const int display_num_images);
static int _lib_lighttable_get_display_num_images(dt_lib_module_t *self);

/* lightable layout changed */
static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data);
/* zoom slider change callback */
static void _lib_lighttable_zoom_slider_changed(GtkRange *range, gpointer user_data);
/* zoom entry change callback */
static gboolean _lib_lighttable_zoom_entry_changed(GtkWidget *entry, GdkEventKey *event,
                                                   dt_lib_module_t *self);

/* number of displayed images slider change callback */
static void _lib_lighttable_display_num_images_slider_changed(GtkRange *range, gpointer user_data);
/* number of displayed images entry change callback */
static gboolean _lib_lighttable_display_num_images_entry_changed(GtkWidget *entry, GdkEventKey *event,
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
static gboolean _lib_lighttable_key_accel_toggle_expose_mode(GtkAccelGroup *accel_group,
                                                             GObject *acceleratable, guint keyval,
                                                             GdkModifierType modifier, gpointer data);
static gboolean _lib_lighttable_key_accel_toggle_culling_mode(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                              guint keyval, GdkModifierType modifier,
                                                              gpointer data);

const char *name(dt_lib_module_t *self)
{
  return _("lighttable");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER;
}

int expandable(dt_lib_module_t *self)
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

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  d->layout =  dt_conf_get_int("plugins/lighttable/layout");
  d->base_layout = dt_conf_get_int("plugins/lighttable/base_layout");
  d->current_zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  d->current_display_num_images = dt_conf_get_int("plugins/lighttable/display_num_images");

  /* create layout selection combobox */
  d->layout_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("zoomable light table"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("file manager"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("expose"));
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->layout_combo), _("culling"));

  gtk_combo_box_set_active(GTK_COMBO_BOX(d->layout_combo), d->layout);

  g_signal_connect(G_OBJECT(d->layout_combo), "changed", G_CALLBACK(_lib_lighttable_layout_changed), (gpointer)self);

  gtk_box_pack_start(GTK_BOX(self->widget), d->layout_combo, TRUE, TRUE, 0);


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
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->zoom_entry), 3);
  dt_gui_key_accel_block_on_focus_connect(d->zoom_entry);
  gtk_box_pack_start(GTK_BOX(self->widget), d->zoom_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->zoom), "value-changed", G_CALLBACK(_lib_lighttable_zoom_slider_changed),
                   (gpointer)self);
  g_signal_connect(d->zoom_entry, "key-press-event", G_CALLBACK(_lib_lighttable_zoom_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->zoom), d->current_zoom);
  _lib_lighttable_zoom_slider_changed(GTK_RANGE(d->zoom), self); // the slider defaults to 1 and GTK doesn't
                                                                 // fire a value-changed signal when setting
                                                                 // it to 1 => empty text box
  gtk_widget_set_no_show_all(d->zoom, TRUE);
  gtk_widget_set_no_show_all(d->zoom_entry, TRUE);

  /* create horizontal display number of images slider */
  d->display_num_images = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 21, 1);
  gtk_widget_set_size_request(GTK_WIDGET(d->display_num_images), DT_PIXEL_APPLY_DPI(140), -1);
  gtk_scale_set_draw_value(GTK_SCALE(d->display_num_images), FALSE);
  gtk_range_set_increments(GTK_RANGE(d->display_num_images), 1, 1);
  gtk_box_pack_start(GTK_BOX(self->widget), d->display_num_images, TRUE, TRUE, 0);

  /* manual entry of the visible number of images in culling */
  d->display_num_images_entry = gtk_entry_new();
  gtk_entry_set_alignment(GTK_ENTRY(d->display_num_images_entry), 1.0);
  gtk_entry_set_max_length(GTK_ENTRY(d->display_num_images_entry), 2);
  gtk_entry_set_width_chars(GTK_ENTRY(d->display_num_images_entry), 3);
  gtk_entry_set_max_width_chars(GTK_ENTRY(d->display_num_images_entry), 3);
  dt_gui_key_accel_block_on_focus_connect(d->display_num_images_entry);
  gtk_box_pack_start(GTK_BOX(self->widget), d->display_num_images_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(d->display_num_images), "value-changed",
                   G_CALLBACK(_lib_lighttable_display_num_images_slider_changed), (gpointer)self);
  g_signal_connect(d->display_num_images_entry, "key-press-event",
                   G_CALLBACK(_lib_lighttable_display_num_images_entry_changed), self);
  gtk_range_set_value(GTK_RANGE(d->display_num_images), d->current_display_num_images);
  _lib_lighttable_display_num_images_slider_changed(GTK_RANGE(d->display_num_images),
                                                    self); // the slider defaults to 1 and GTK doesn't
                                                           // fire a value-changed signal when setting
                                                           // it to 1 => empty text box
  gtk_widget_set_no_show_all(d->display_num_images, TRUE);
  gtk_widget_set_no_show_all(d->display_num_images_entry, TRUE);

  _lib_lighttable_layout_changed(GTK_COMBO_BOX(d->layout_combo), self);

  darktable.view_manager->proxy.lighttable.module = self;
  darktable.view_manager->proxy.lighttable.set_zoom = _lib_lighttable_set_zoom;
  darktable.view_manager->proxy.lighttable.get_zoom = _lib_lighttable_get_zoom;
  darktable.view_manager->proxy.lighttable.get_layout = _lib_lighttable_get_layout;
  darktable.view_manager->proxy.lighttable.set_layout = _lib_lighttable_set_layout;
  darktable.view_manager->proxy.lighttable.get_display_num_images = _lib_lighttable_get_display_num_images;
  darktable.view_manager->proxy.lighttable.set_display_num_images = _lib_lighttable_set_display_num_images;
}

void init_key_accels(dt_lib_module_t *self)
{
  // view accels
  dt_accel_register_lib(self, NC_("accel", "zoom max"), GDK_KEY_1, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom in"), GDK_KEY_2, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom out"), GDK_KEY_3, GDK_MOD1_MASK);
  dt_accel_register_lib(self, NC_("accel", "zoom min"), GDK_KEY_4, GDK_MOD1_MASK);

  dt_accel_register_lib(self, NC_("accel", "toggle exposé mode"), GDK_KEY_x, 0);
  dt_accel_register_lib(self, NC_("accel", "toggle culling mode"), GDK_KEY_x, GDK_CONTROL_MASK);
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
  dt_accel_connect_lib(self, "toggle exposé mode",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_toggle_expose_mode), self, NULL));
  dt_accel_connect_lib(self, "toggle culling mode",
                       g_cclosure_new(G_CALLBACK(_lib_lighttable_key_accel_toggle_culling_mode), self, NULL));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(d->zoom_entry);
  dt_gui_key_accel_block_on_focus_disconnect(d->display_num_images_entry);
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
  d->current_zoom = i;
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

static void _lib_lighttable_display_num_images_slider_changed(GtkRange *range, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int i = gtk_range_get_value(range);
  dt_conf_set_int("plugins/lighttable/display_num_images", i);
  gchar *i_as_str = g_strdup_printf("%d", i);
  gtk_entry_set_text(GTK_ENTRY(d->display_num_images_entry), i_as_str);
  d->current_display_num_images = i;
  g_free(i_as_str);
  dt_control_queue_redraw_center();
}

static gboolean _lib_lighttable_display_num_images_entry_changed(GtkWidget *entry, GdkEventKey *event,
                                                                 dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:
    {
      // reset
      int i = dt_conf_get_int("plugins/lighttable/display_num_images");
      gchar *i_as_str = g_strdup_printf("%d", i);
      gtk_entry_set_text(GTK_ENTRY(d->display_num_images_entry), i_as_str);
      g_free(i_as_str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // apply display number of images
      const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->display_num_images_entry));
      int i = atoi(value);
      gtk_range_set_value(GTK_RANGE(d->display_num_images), i);
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

static void _lib_lighttable_change_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  const int current_layout = dt_conf_get_int("plugins/lighttable/layout");
  d->layout = layout;

  if(layout == DT_LIGHTTABLE_LAYOUT_EXPOSE)
  {
    gtk_widget_hide(d->zoom);
    gtk_widget_hide(d->zoom_entry);
    gtk_widget_hide(d->display_num_images);
    gtk_widget_hide(d->display_num_images_entry);
  }
  else if(layout == DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    gtk_widget_hide(d->zoom);
    gtk_widget_hide(d->zoom_entry);
    gtk_widget_show(d->display_num_images);
    gtk_widget_show(d->display_num_images_entry);

    // if we have a selected image activate the filmstrip
    GList *first_selected = dt_collection_get_selected(darktable.collection, 1);
    if(first_selected)
    {
      const int imgid = GPOINTER_TO_INT(first_selected->data);
      if(imgid >= 0 && dt_view_filmstrip_get_activated_imgid(darktable.view_manager) != imgid)
        dt_view_filmstrip_set_active_image(darktable.view_manager, imgid);
      g_list_free(first_selected);
    }
  }
  else
  {
    gtk_widget_show(d->zoom);
    gtk_widget_show(d->zoom_entry);
    gtk_widget_hide(d->display_num_images);
    gtk_widget_hide(d->display_num_images_entry);
  }

  // if changing from culling select all displayed images
  if(current_layout == DT_LIGHTTABLE_LAYOUT_CULLING && current_layout != layout)
  {
    GList *first_selected = dt_collection_get_selected(darktable.collection, -1);
    if(first_selected && g_list_length(first_selected) == 1)
    {
      const int sel_imgid = GPOINTER_TO_INT(first_selected->data);
      GList *collected = dt_collection_get_all(darktable.collection, -1);
      if(collected)
      {
        // search the selected image
        GList *l = collected;
        while(l)
        {
          const int id = GPOINTER_TO_INT(l->data);
          if(sel_imgid == id) break;

          l = g_list_next(l);
        }
        // now go to the last visible image
        int i = 1;
        while(l && i < d->current_display_num_images)
        {
          l = g_list_next(l);
          i++;
        }

        if(l) dt_selection_select_range(darktable.selection, GPOINTER_TO_INT(l->data));
      }

      if(collected) g_list_free(collected);
    }
    if(first_selected) g_list_free(first_selected);
  }

  if(current_layout != layout)
  {
    dt_conf_set_int("plugins/lighttable/layout", layout);
    if(layout == DT_LIGHTTABLE_LAYOUT_FILEMANAGER || layout == DT_LIGHTTABLE_LAYOUT_ZOOMABLE)
    {
      d->base_layout = layout;
      dt_conf_set_int("plugins/lighttable/base_layout", layout);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->layout_combo), layout);
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED);
  }
  else
  {
    dt_control_queue_redraw_center();
  }
}

static void _lib_lighttable_layout_changed(GtkComboBox *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  const int new_layout = gtk_combo_box_get_active(widget);
  _lib_lighttable_change_layout(self, new_layout);
}

static void _lib_lighttable_set_layout(dt_lib_module_t *self, dt_lighttable_layout_t layout)
{
  _lib_lighttable_change_layout(self, layout);
}

static dt_lighttable_layout_t _lib_lighttable_get_layout(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->layout;
}

#define DT_LIBRARY_MAX_ZOOM 13
static void _lib_lighttable_set_zoom(dt_lib_module_t *self, gint zoom)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->zoom), zoom);
  d->current_zoom = zoom;
}

static gint _lib_lighttable_get_zoom(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->current_zoom;
}

static void _lib_lighttable_set_display_num_images(dt_lib_module_t *self, const int display_num_images)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  gtk_range_set_value(GTK_RANGE(d->display_num_images), display_num_images);
  d->current_display_num_images = display_num_images;
}

static int _lib_lighttable_get_display_num_images(dt_lib_module_t *self)
{
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;
  return d->current_display_num_images;
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

static gboolean _lib_lighttable_key_accel_toggle_expose_mode(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                                             GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->layout != DT_LIGHTTABLE_LAYOUT_EXPOSE)
  {
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_EXPOSE);
  }
  else
    _lib_lighttable_set_layout(self, d->base_layout);

  dt_control_queue_redraw_center();
  return TRUE;
}

static gboolean _lib_lighttable_key_accel_toggle_culling_mode(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                              guint keyval, GdkModifierType modifier,
                                                              gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tool_lighttable_t *d = (dt_lib_tool_lighttable_t *)self->data;

  if(d->layout != DT_LIGHTTABLE_LAYOUT_CULLING)
  {
    _lib_lighttable_set_layout(self, DT_LIGHTTABLE_LAYOUT_CULLING);
  }
  else
    _lib_lighttable_set_layout(self, d->base_layout);

  dt_control_queue_redraw_center();
  return TRUE;
}

#ifdef USE_LUA
static int layout_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const dt_lighttable_layout_t tmp = _lib_lighttable_get_layout(self);
  if(lua_gettop(L) > 0){
    dt_lighttable_layout_t value;
    luaA_to(L,dt_lighttable_layout_t,&value,1);
    _lib_lighttable_set_layout(self, value);
  }
  luaA_push(L, dt_lighttable_layout_t, &tmp);
  return 1;
}
static int zoom_level_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  const gint tmp = _lib_lighttable_get_zoom(self);
  if(lua_gettop(L) > 0){
    int value;
    luaA_to(L,int,&value,1);
    _lib_lighttable_set_zoom(self, value);
  }
  luaA_push(L, int, &tmp);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, layout_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "layout");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, zoom_level_cb,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "zoom_level");

  luaA_enum(L,dt_lighttable_layout_t);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_FIRST);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_ZOOMABLE);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_FILEMANAGER);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_EXPOSE);
  luaA_enum_value(L, dt_lighttable_layout_t, DT_LIGHTTABLE_LAYOUT_CULLING);
  luaA_enum_value(L,dt_lighttable_layout_t,DT_LIGHTTABLE_LAYOUT_LAST);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
