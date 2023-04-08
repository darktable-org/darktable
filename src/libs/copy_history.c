/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/hist_dialog.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <gtk/gtk.h>
#include <stdlib.h>

DT_MODULE(1)

typedef enum dt_history_copy_mode_t
{
  DT_COPY_HISTORY_APPEND    = 0,
  DT_COPY_HISTORY_OVERWRITE = 1
} dt_history_copy_mode_t;

typedef struct dt_lib_copy_history_t
{
  GtkWidget *pastemode;
  GtkWidget *paste, *paste_parts;
  GtkWidget *copy_button, *discard_button, *load_button, *write_button;
  GtkWidget *copy_parts_button;
  GtkWidget *compress_button;
  gboolean is_full_copy;
} dt_lib_copy_history_t;

const char *name(dt_lib_module_t *self)
{
  return _("history stack");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  const int nbimgs = dt_act_on_get_images_nb(TRUE, FALSE);
  const int act_on_any = (nbimgs > 0);
  const int act_on_one = (nbimgs == 1);
  const int act_on_mult = act_on_any && !act_on_one;
  const int act_on_img = dt_act_on_get_main_image();
  const gboolean can_paste =
    darktable.view_manager->copy_paste.copied_imageid > 0
    && (act_on_mult
        || (act_on_one
            && (darktable.view_manager->copy_paste.copied_imageid != act_on_img)));

  gtk_widget_set_sensitive(GTK_WIDGET(d->discard_button), act_on_any);
  gtk_widget_set_sensitive(GTK_WIDGET(d->compress_button), act_on_any);
  gtk_widget_set_sensitive(GTK_WIDGET(d->load_button), act_on_any);
  gtk_widget_set_sensitive(GTK_WIDGET(d->write_button), act_on_any);

  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_button), act_on_one);
  gtk_widget_set_sensitive(GTK_WIDGET(d->copy_parts_button), act_on_one);

  gtk_widget_set_sensitive(GTK_WIDGET(d->paste), can_paste);
  gtk_widget_set_sensitive(GTK_WIDGET(d->paste_parts), can_paste);
}

static void write_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_control_write_sidecar_files();
}

static void load_button_clicked(GtkWidget *widget, dt_lib_module_t *self)
{
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs)
    return;
  const int act_on_one = g_list_is_singleton(imgs); // list length == 1?
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("open sidecar file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
          _("_open"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(act_on_one)
  {
    //single image to load xmp to, assume we want to load from same dir
    const int imgid = GPOINTER_TO_INT(imgs->data);
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(img && img->film_id != -1)
    {
      char pathname[PATH_MAX] = { 0 };
      dt_image_film_roll_directory(img, pathname, sizeof(pathname));
      gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), pathname);
    }
    else
    {
      // handle situation where there's some problem with cache/film_id
      // i guess that's impossible, but better safe than sorry ;)
      dt_conf_get_folder_to_file_chooser("ui_last/import_path",
                                         GTK_FILE_CHOOSER(filechooser));
    }
    dt_image_cache_read_release(darktable.image_cache, img);
  }
  else
  {
    // multiple images, use "last import" preference
    dt_conf_get_folder_to_file_chooser("ui_last/import_path",
                                       GTK_FILE_CHOOSER(filechooser));
  }

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.xmp");
  gtk_file_filter_add_pattern(filter, "*.XMP");
  gtk_file_filter_set_name(filter, _("XMP sidecar files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dtfilename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    if(dt_history_load_and_apply_on_list(dtfilename, imgs) != 0)
    {
      GtkWidget *dialog
          = gtk_message_dialog_new(GTK_WINDOW(win),
                                   GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR,
                                   GTK_BUTTONS_CLOSE,
                                   _("error loading file '%s'"), dtfilename);
#ifdef GDK_WINDOWING_QUARTZ
      dt_osx_disallow_fullscreen(dialog);
#endif
      gtk_dialog_run(GTK_DIALOG(dialog));
      gtk_widget_destroy(dialog);
    }
    else
    {
      dt_collection_update_query(darktable.collection,
                                 DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                                 g_list_copy((GList *)imgs));
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED,
                                    g_list_copy((GList *)imgs), 0);
      dt_control_queue_redraw_center();
    }
    if(!act_on_one)
    {
      //remember last import path if applying history to multiple images
      dt_conf_set_folder_from_file_chooser("ui_last/import_path",
                                           GTK_FILE_CHOOSER(filechooser));
    }
    g_free(dtfilename);
  }
  g_object_unref(filechooser);
  g_list_free(imgs);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void compress_button_clicked(GtkWidget *widget, gpointer user_data)
{
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs) return;  // do nothing if no images to be acted on

  const int missing = dt_history_compress_on_list(imgs);

  dt_collection_update_query(darktable.collection,
                             DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, imgs);
  dt_control_queue_redraw_center();
  if(missing)
    dt_control_log(ngettext("no history compression of %d image",
                            "no history compression of %d images", missing), missing);
}


static void copy_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  const int id = dt_act_on_get_main_image();

  if(id > 0 && dt_history_copy(id))
  {
    d->is_full_copy = TRUE;
    _update(self);
  }
}

static void copy_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  const int id = dt_act_on_get_main_image();

  if(id > 0 && dt_history_copy_parts(id))
  {
    d->is_full_copy = FALSE;
    _update(self);
  }
}

static void discard_button_clicked(GtkWidget *widget, gpointer user_data)
{
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!imgs) return;

  const int number = g_list_length((GList *)imgs);

  if(!dt_conf_get_bool("ask_before_discard")
     || dt_gui_show_yes_no_dialog(_("delete images' history?"),
          ngettext("do you really want to clear history of %d selected image?",
                   "do you really want to clear history of %d selected images?", number),
                                  number))
  {
    dt_history_delete_on_list(imgs, TRUE);
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                               imgs); // frees imgs
    dt_control_queue_redraw_center();
  }
  else
  {
    g_list_free(imgs);
  }
}

static void paste_button_clicked(GtkWidget *widget, gpointer user_data)
{

  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)self->data;

  const int current_mode = dt_bauhaus_combobox_get(d->pastemode);

  /* get past mode and store, overwrite / merge */
  const int mode = d->is_full_copy
    ? DT_COPY_HISTORY_OVERWRITE
    : current_mode;

  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  /* copy history from previously copied image and past onto selection */
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);

  if(dt_history_paste_on_list(imgs, TRUE))
  {
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF, imgs);
  }
  else
  {
    g_list_free(imgs);
  }

  // restore mode
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", current_mode);
}

static void paste_parts_button_clicked(GtkWidget *widget, gpointer user_data)
{
  /* copy history from previously copied image and past onto selection */
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);

  if(dt_history_paste_parts_on_list(imgs, TRUE))
  {
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                               imgs); // frees imgs
  }
  else
  {
    g_list_free(imgs);
  }
}

static void pastemode_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  const int mode = dt_bauhaus_combobox_get(widget);
  dt_conf_set_int("plugins/lighttable/copy_history/pastemode", mode);

  _update((dt_lib_module_t *)user_data);
}

static void _image_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

static void _collection_updated_callback(gpointer instance,
                                         dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property,
                                         const gpointer imgs,
                                         const int next,
                                         dt_lib_module_t *self)
{
  _update(self);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _update);
}

void gui_reset(dt_lib_module_t *self)
{
  _update(self);
}

int position(const dt_lib_module_t *self)
{
  return 600;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_copy_history_t *d = (dt_lib_copy_history_t *)malloc(sizeof(dt_lib_copy_history_t));
  self->data = (void *)d;
  self->timeout_handle = 0;

  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  d->copy_parts_button = dt_action_button_new
    (self,
     N_("selective copy..."), copy_parts_button_clicked, self,
     _("choose which modules to copy from the source image"),
     GDK_KEY_c, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_grid_attach(grid, d->copy_parts_button, 0, line, 3, 1);

  d->copy_button = dt_action_button_new
    (self, N_("copy"), copy_button_clicked, self,
     _("copy history stack of\nfirst selected image"),
     GDK_KEY_c, GDK_CONTROL_MASK);
  gtk_grid_attach(grid, d->copy_button, 3, line++, 3, 1);

  d->paste_parts = dt_action_button_new
    (self, N_("selective paste..."), paste_parts_button_clicked, self,
     _("choose which modules to paste to the target image(s)"),
     GDK_KEY_v, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_widget_set_sensitive(d->paste_parts, FALSE);
  gtk_grid_attach(grid, d->paste_parts, 0, line, 3, 1);

  d->paste = dt_action_button_new(self, N_("paste"), paste_button_clicked, self,
                                  _("paste history stack to\nall selected images"),
                                  GDK_KEY_v, GDK_CONTROL_MASK);
  gtk_widget_set_sensitive(d->paste, FALSE);
  gtk_grid_attach(grid, d->paste, 3, line++, 3, 1);

  d->compress_button = dt_action_button_new
    (self, N_("compress history"), compress_button_clicked, self,
     _("compress history stack of\nall selected images"), 0, 0);
  gtk_grid_attach(grid, d->compress_button, 0, line, 3, 1);

  d->discard_button = dt_action_button_new
    (self, N_("discard history"), discard_button_clicked, self,
     _("discard history stack of\nall selected images"), 0, 0);
  gtk_grid_attach(grid, d->discard_button, 3, line++, 3, 1);

  DT_BAUHAUS_COMBOBOX_NEW_FULL
    (d->pastemode, self, NULL, N_("mode"),
     _("how to handle existing history"),
     dt_conf_get_int("plugins/lighttable/copy_history/pastemode"),
     pastemode_combobox_changed, self,
     N_("append"),     // DT_COPY_HISTORY_APPEND
     N_("overwrite")); // DT_COPY_HISTORY_OVERWRITE
  dt_gui_add_help_link(d->pastemode, dt_get_help_url("history"));
  gtk_grid_attach(grid, d->pastemode, 0, line++, 6, 1);

  d->load_button = dt_action_button_new
    (self, N_("load sidecar file..."), load_button_clicked, self,
     _("open an XMP sidecar file\nand apply it to selected images"), 0, 0);
  gtk_grid_attach(grid, d->load_button, 0, line, 3, 1);

  d->write_button = dt_action_button_new
    (self, N_("write sidecar files"), write_button_clicked, self,
     _("write history stack and tags to XMP sidecar files"), 0, 0);
  gtk_grid_attach(grid, d->write_button, 3, line, 3, 1);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  _update(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_image_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                                     G_CALLBACK(_collection_updated_callback), self);

  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
