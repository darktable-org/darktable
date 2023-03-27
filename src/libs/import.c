/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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
#include "common/file_location.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/metadata.h"
#include "common/datetime.h"
#include "control/conf.h"
#include "control/control.h"
#ifdef HAVE_GPHOTO2
#include "control/jobs/camera_jobs.h"
#endif
#include "dtgtk/expander.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/import_metadata.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef _WIN32
//MSVCRT does not have strptime implemented
#include "win/strptime.h"
#endif
#include <strings.h>
#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

#ifdef USE_LUA
#include "lua/widget/widget.h"
#endif
DT_MODULE(1)


#ifdef HAVE_GPHOTO2
/** helper function to update ui with available cameras and their actionbuttons */
static void _lib_import_ui_devices_update(dt_lib_module_t *self);
#endif
static void _import_from_dialog_new(dt_lib_module_t* self);
static void _import_from_dialog_run(dt_lib_module_t* self);
static void _import_from_dialog_free(dt_lib_module_t* self);
static void _do_select_all(dt_lib_module_t* self);
static void _do_select_none(dt_lib_module_t* self);
static void _do_select_new(dt_lib_module_t* self);
static void _update_places_list(dt_lib_module_t* self);
static void _update_folders_list(dt_lib_module_t* self);
static void _lib_import_select_folder(GtkWidget *widget, dt_lib_module_t *self);
static void _remove_place(gchar *folder, GtkTreeIter iter, dt_lib_module_t* self);
static GList* _get_custom_places();

typedef enum dt_import_cols_t
{
  DT_IMPORT_SEL_THUMB = 0,      // active / inactive thumbnails
  DT_IMPORT_THUMB,              // thumbnail
  DT_IMPORT_UI_FILENAME,        // displayed filename
  DT_IMPORT_FILENAME,           // filename
  DT_IMPORT_UI_DATETIME,        // displayed datetime
  DT_IMPORT_UI_EXISTS,          // whether the picture is already imported
  DT_IMPORT_DATETIME,           // file datetime
  DT_IMPORT_NUM_COLS
} dt_import_cols_t;

typedef enum dt_folder_cols_t
{
  DT_FOLDER_PATH = 0,
  DT_FOLDER_NAME,
  DT_FOLDER_EXPANDED,
  DT_FOLDER_NUM_COLS
} dt_folder_cols_t;

typedef enum dt_places_cols_t
{
  DT_PLACES_NAME = 0,
  DT_PLACES_PATH,
  DT_PLACES_TYPE,
  DT_PLACES_NUM_COLS
} dt_places_cols_t;

typedef enum dt_places_type_t
{
  DT_TYPE_HOME = 1,
  DT_TYPE_PIC,
  DT_TYPE_MOUNT,
  DT_TYPE_CUSTOM,
} dt_places_type_t;

typedef enum dt_import_case_t
{
  DT_IMPORT_INPLACE = 0,
  DT_IMPORT_COPY,
  DT_IMPORT_CAMERA,
  DT_IMPORT_TETHER
} dt_import_case_t;

typedef struct dt_lib_import_t
{
#ifdef HAVE_GPHOTO2
  dt_camera_t *camera;
#endif
  GtkButton *import_inplace;
  GtkButton *import_copy;
  GtkButton *import_camera;
  GtkButton *tethered_shoot;
  GtkButton *mount_camera;
  GtkButton *unmount_camera;

  GtkWidget *ignore_exif, *rating, *apply_metadata, *recursive;
  GtkWidget *import_new;
  dt_import_metadata_t metadata;
  GtkBox *devices;
  dt_import_case_t import_case;
  struct
  {
    GtkWidget *dialog;
    GtkListStore *store;
    GtkWidget *w;
    GtkTreeView *treeview;
    GtkWidget *thumbs;
    GtkTreeView *folderview;
    GtkTreeViewColumn *foldercol;
    GtkTreeIter iter;
    int event;
    guint nb;
    GdkPixbuf *eye;
    GtkTreeViewColumn *pixcol;
    GtkWidget *img_nb;
    GtkGrid *patterns;
    GtkWidget *datetime;
    dt_gui_collapsible_section_t cs;
    guint fn_line;
    GtkWidget *info;
  } from;
  GtkListStore *placesModel;
  GtkWidget *placesView;
  GtkTreeSelection *placesSelection;

  dt_gui_collapsible_section_t cs;

#ifdef USE_LUA
  GtkWidget *extra_lua_widgets;
#endif
} dt_lib_import_t;

const char *name(dt_lib_module_t *self)
{
  return _("import");
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 999;
}

#ifdef HAVE_GPHOTO2

/* show import from camera dialog */
static void _lib_import_from_camera_callback(GtkButton *button, dt_lib_module_t *self)
{
  dt_camctl_t *camctl = (dt_camctl_t *)darktable.camctl;
  camctl->import_ui = TRUE;

  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->import_case = DT_IMPORT_CAMERA;
  _import_from_dialog_new(self);
  _import_from_dialog_run(self);
  _import_from_dialog_free(self);
  camctl->import_ui = FALSE;
}

/* enter tethering mode for camera */
static void _lib_import_tethered_callback(GtkToggleButton *button, gpointer data)
{
  /* select camera to work with before switching mode */
  dt_camctl_select_camera(darktable.camctl, (dt_camera_t *)data);
  dt_ctl_switch_mode_to("tethering");
}

static void _lib_import_mount_callback(GtkToggleButton *button, gpointer data)
{
  dt_camera_unused_t *camera = (dt_camera_unused_t *)data;
  camera->trymount = TRUE;
  dt_camctl_t *camctl = (dt_camctl_t *)darktable.camctl;
  camctl->tickmask = 3;
}

static void _lib_import_unmount_callback(GtkToggleButton *button, gpointer data)
{
  dt_camera_t *camera = (dt_camera_t *)data;
  camera->unmount = TRUE;
  dt_camctl_t *camctl = (dt_camctl_t *)darktable.camctl;
  camctl->tickmask = 3;
}

/** update the device list */
void _lib_import_ui_devices_update(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  /* cleanup of widgets in devices container*/
  dt_gui_container_remove_children(GTK_CONTAINER(d->devices));
  d->import_camera = d->tethered_shoot = d->mount_camera = d->unmount_camera = NULL;
  dt_camctl_t *camctl = (dt_camctl_t *)darktable.camctl;
  dt_pthread_mutex_lock(&camctl->lock);

  GList *citem = camctl->cameras;

  if(citem)
  {
    // Add detected supported devices
    char buffer[512] = { 0 };
    for(; citem; citem = g_list_next(citem))
    {
      dt_camera_t *camera = (dt_camera_t *)citem->data;

      /* add camera label */
      GtkWidget *label = dt_ui_section_label_new(_(camera->model));
      gtk_box_pack_start(GTK_BOX(d->devices), label, TRUE, TRUE, 0);

      /* set camera summary if available */
      if(*camera->summary.text)
      {
        gtk_widget_set_tooltip_text(label, camera->summary.text);
      }
      else
      {
        snprintf(buffer, sizeof(buffer), _("device \"%s\" connected on port \"%s\"."), camera->model,
                 camera->port);
        gtk_widget_set_tooltip_text(label, buffer);
      }

      /* add camera actions buttons */
      GtkWidget *ib = NULL, *tb = NULL, *um = NULL;
      GtkWidget *vbx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      if(camera->can_import == TRUE)
      {
        gtk_box_pack_start(GTK_BOX(vbx), (ib = gtk_button_new_with_label(_("copy & import from camera"))), FALSE,
                           FALSE, 0);
        gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(ib))), PANGO_ELLIPSIZE_END);
        d->import_camera = GTK_BUTTON(ib);
        d->camera = camera;
        g_signal_connect(G_OBJECT(ib), "clicked", G_CALLBACK(_lib_import_from_camera_callback), self);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(ib)), GTK_ALIGN_CENTER);
        dt_gui_add_help_link(ib, dt_get_help_url("import_camera"));
      }
      if(camera->can_tether == TRUE)
      {
        gtk_box_pack_start(GTK_BOX(vbx), (tb = gtk_button_new_with_label(_("tethered shoot"))), FALSE, FALSE, 0);
        d->tethered_shoot = GTK_BUTTON(tb);
        g_signal_connect(G_OBJECT(tb), "clicked", G_CALLBACK(_lib_import_tethered_callback), camera);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(tb)), GTK_ALIGN_CENTER);
        dt_gui_add_help_link(tb, dt_get_help_url("import_camera"));
      }

      gtk_box_pack_start(GTK_BOX(vbx), (um = gtk_button_new_with_label(_("unmount camera"))), FALSE, FALSE, 0);
      d->unmount_camera = GTK_BUTTON(um);
      g_signal_connect(G_OBJECT(um), "clicked", G_CALLBACK(_lib_import_unmount_callback), camera);
      gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(um)), GTK_ALIGN_CENTER);
      dt_gui_add_help_link(um, dt_get_help_url("mount_camera"));

      gtk_box_pack_start(GTK_BOX(d->devices), vbx, FALSE, FALSE, 0);
    }
  }

  // Add list of locked cameras
  citem = camctl->unused_cameras;
  if(citem)
  {
    for(; citem; citem = g_list_next(citem))
    {
      dt_camera_unused_t *camera = (dt_camera_unused_t *)citem->data;
      GtkWidget *label = dt_ui_section_label_new(_(camera->model));
      gtk_box_pack_start(GTK_BOX(d->devices), label, FALSE, FALSE, 0);

      if(camera->used)
        gtk_widget_set_tooltip_text(label, _("camera is locked by another application\n"
                    "make sure it is no longer mounted\nor quit the locking application"));
      else if(camera->boring)
        gtk_widget_set_tooltip_text(label, _("tethering and importing is disabled for this camera"));

      GtkWidget *im = gtk_button_new_with_label(_("mount camera"));
      GtkWidget *vbx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

      gtk_box_pack_start(GTK_BOX(vbx), im, FALSE, FALSE, 0);
      gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(im))), PANGO_ELLIPSIZE_END);
      d->mount_camera = GTK_BUTTON(im);

      g_signal_connect(G_OBJECT(im), "clicked", G_CALLBACK(_lib_import_mount_callback), camera);
      gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(im)), GTK_ALIGN_CENTER);
      dt_gui_add_help_link(im, dt_get_help_url("mount_camera"));

      gtk_box_pack_start(GTK_BOX(d->devices), vbx, FALSE, FALSE, 0);
    }
  }
  dt_pthread_mutex_unlock(&camctl->lock);
  gtk_widget_show_all(GTK_WIDGET(d->devices));

  dt_action_define(DT_ACTION(self), NULL, N_("copy & import from camera"), GTK_WIDGET(d->import_camera), &dt_action_def_button);
  dt_action_define(DT_ACTION(self), NULL, N_("mount camera"), GTK_WIDGET(d->mount_camera), &dt_action_def_button);
  dt_action_define(DT_ACTION(self), NULL, N_("tethered shoot"), GTK_WIDGET(d->tethered_shoot), &dt_action_def_button);
  dt_action_define(DT_ACTION(self), NULL, N_("unmount camera"), GTK_WIDGET(d->unmount_camera), &dt_action_def_button);
}

static void _free_camera_files(gpointer data)
{
  dt_camera_files_t *file = (dt_camera_files_t *)data;
  g_free(file->filename);
  g_free(file);
}

static guint _import_from_camera_set_file_list(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GList *imgs = dt_camctl_get_images_list(darktable.camctl, d->camera);
  int nb = 0;
  const gboolean include_jpegs = !dt_conf_get_bool("ui_last/import_ignore_jpegs");
  for(GList *img = imgs; img; img = g_list_next(img))
  {
    dt_camera_files_t *file = (dt_camera_files_t *)img->data;
    const char *ext = g_strrstr(file->filename, ".");
    if(include_jpegs || (ext && g_ascii_strncasecmp(ext, ".jpg", sizeof(".jpg"))
                             && g_ascii_strncasecmp(ext, ".jpeg", sizeof(".jpeg"))))
    {
      const time_t datetime = file->timestamp;
      GDateTime *dt_datetime = g_date_time_new_from_unix_local(datetime);
      gchar *dt_txt = g_date_time_format(dt_datetime, "%x %X");
      gchar *basename = g_path_get_basename(file->filename);
      char dtid[DT_DATETIME_EXIF_LENGTH];
      dt_datetime_unix_to_exif(dtid, sizeof(dtid), &datetime);
      const gboolean already_imported = dt_metadata_already_imported(basename, dtid);
      g_free(basename);
      GtkTreeIter iter;
      gtk_list_store_append(d->from.store, &iter);
      gtk_list_store_set(d->from.store, &iter,
                         DT_IMPORT_UI_EXISTS, already_imported ? "✔" : " ",
                         DT_IMPORT_UI_FILENAME, file->filename,
                         DT_IMPORT_FILENAME, file->filename,
                         DT_IMPORT_UI_DATETIME, dt_txt,
                         DT_IMPORT_DATETIME, datetime,
                         DT_IMPORT_THUMB, d->from.eye, -1);
      nb++;
      g_free(dt_txt);
      g_date_time_unref(dt_datetime);
    }
  }
  g_list_free_full(imgs, _free_camera_files);
  return nb;
}

#endif // HAVE_GPHOTO2

#ifdef USE_LUA
static void reset_child(GtkWidget* child, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",child, // the GtkWidget is an alias for the lua_widget
      LUA_ASYNC_TYPENAME,"const char*","reset",
      LUA_ASYNC_DONE);
}

// remove the extra portion from the filechooser before destroying it
static void detach_lua_widgets(GtkWidget *extra_lua_widgets)
{
  GtkWidget *parent = gtk_widget_get_parent(extra_lua_widgets);
  gtk_container_remove(GTK_CONTAINER(parent), extra_lua_widgets);
}
#endif

// maybe this should be (partly) in imageio/imageio.[c|h]?
static GdkPixbuf *_import_get_thumbnail(const gchar *filename)
{
  GdkPixbuf *pixbuf = NULL;
  gboolean have_preview = FALSE, no_preview_fallback = FALSE;

  if(!filename || !g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    no_preview_fallback = TRUE;
  }

  // Step 1: try to check whether the picture contains embedded thumbnail
  // In case it has, we'll use that thumbnail to show on the dialog
  if(!no_preview_fallback)
  {
    uint8_t *buffer = NULL;
    size_t size = 0;
    char *mime_type = NULL;
    if(!dt_exif_get_thumbnail(filename, &buffer, &size, &mime_type))
    {
      // Scale the image to the correct size
      GdkPixbuf *tmp;
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
      if(!gdk_pixbuf_loader_write(loader, buffer, size, NULL)) goto cleanup;
      // Calling gdk_pixbuf_loader_close forces the data to be parsed by the
      // loader. We must do this before calling gdk_pixbuf_loader_get_pixbuf.
      if(!gdk_pixbuf_loader_close(loader, NULL)) goto cleanup;
      if(!(tmp = gdk_pixbuf_loader_get_pixbuf(loader))) goto cleanup;
      const float ratio = 1.0 * gdk_pixbuf_get_height(tmp) / gdk_pixbuf_get_width(tmp);
      const int width = 128;
      const int height = 128 * ratio;
      pixbuf = gdk_pixbuf_scale_simple(tmp, width, height, GDK_INTERP_BILINEAR);

      have_preview = TRUE;

    cleanup:
      gdk_pixbuf_loader_close(loader, NULL);
      free(mime_type);
      free(buffer);
      g_object_unref(loader); // This should clean up tmp as well
    }
  }

  // Step 2: if we were not able to get a thumbnail at step 1,
  // read the whole file to get a small size thumbnail
  if(!have_preview && !no_preview_fallback)
  {
    pixbuf = gdk_pixbuf_new_from_file_at_size(filename, 128, 128, NULL);
    if(pixbuf != NULL) have_preview = TRUE;
  }

  // If we got a thumbnail (either embedded or reading the file directly)
  // we need to find out the rotation as well
  if(have_preview && !no_preview_fallback)
  {
    // get image orientation
    dt_image_t img = { 0 };
    (void)dt_exif_read(&img, filename);

    // Rotate the image to the correct orientation
    GdkPixbuf *tmp = pixbuf;

    if(img.orientation == ORIENTATION_ROTATE_CCW_90_DEG)
    {
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
    }
    else if(img.orientation == ORIENTATION_ROTATE_CW_90_DEG)
    {
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
    }
    else if(img.orientation == ORIENTATION_ROTATE_180_DEG)
    {
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
    }

    if(pixbuf != tmp)
    {
      g_object_unref(pixbuf);
      pixbuf = tmp;
    }
  }

  // if no thumbanail found or read failed for whatever reason
  // just display the default darktable logo
  if(!have_preview || no_preview_fallback)
  {
    /* load the dt logo as a background */
    cairo_surface_t *surface = dt_util_get_logo(128.0);
    if(surface)
    {
      guint8 *image_buffer = cairo_image_surface_get_data(surface);
      const int image_width = cairo_image_surface_get_width(surface);
      const int image_height = cairo_image_surface_get_height(surface);

      pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, image_width, image_height);

      cairo_surface_destroy(surface);
      free(image_buffer);

      have_preview = TRUE;
    }
  }

return pixbuf;
}

static void _thumb_set_in_listview(GtkTreeModel *model, GtkTreeIter *iter,
                                   const gboolean thumb_sel, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  gchar *filename;
  gtk_tree_model_get(model, iter, DT_IMPORT_FILENAME, &filename, -1);
  GdkPixbuf *pixbuf = NULL;
#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
    pixbuf = thumb_sel ? dt_camctl_get_thumbnail(darktable.camctl, d->camera, filename) : d->from.eye;
  else
#endif
  {
    const char *folder = dt_conf_get_string_const("ui_last/import_last_directory");
    char *fullname = g_build_filename(folder, filename, NULL);
    pixbuf = thumb_sel ? _import_get_thumbnail(fullname) : d->from.eye;
    g_free(fullname);
  }
  gtk_list_store_set(d->from.store, iter, DT_IMPORT_SEL_THUMB, thumb_sel,
                                          DT_IMPORT_THUMB, pixbuf, -1);

  if(pixbuf) g_object_ref(pixbuf);
  g_free(filename);
}

static gboolean _files_button_press(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  if((event->type == GDK_BUTTON_PRESS && event->button == 1))
  {
    GtkTreePath *path = NULL;
    GtkTreeViewColumn *column = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y,
                                     &path, &column, NULL, NULL))
    {
      if(column == d->from.pixcol)
      {
        GtkTreeIter iter;
        GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
        gtk_tree_model_get_iter(model, &iter, path);
        gboolean thumb_sel;
        gtk_tree_model_get(model, &iter, DT_IMPORT_SEL_THUMB, &thumb_sel, -1);
        _thumb_set_in_listview(model, &iter, !thumb_sel, self);
        gtk_tree_path_free(path);
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
  }
  else if((event->type == GDK_2BUTTON_PRESS && event->button == 1))
  {
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y,
                                     &path, NULL, NULL, NULL))
    {
      GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
      gtk_tree_selection_unselect_all(selection);
      gtk_tree_selection_select_path(selection, path);
      gtk_dialog_response(GTK_DIALOG(d->from.dialog), GTK_RESPONSE_ACCEPT);
      gtk_tree_path_free(path);
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean _thumb_set(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  if(d->from.event)
  {
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    gboolean thumb_sel;
    gtk_tree_model_get(model, &d->from.iter, DT_IMPORT_SEL_THUMB, &thumb_sel, -1);
    if(!thumb_sel)
      _thumb_set_in_listview(model, &d->from.iter, TRUE, self);
    if(d->from.event && gtk_tree_model_iter_next(model, &d->from.iter))
      return TRUE;
  }
  d->from.event = 0;
  return FALSE;
}

static void _all_thumb_toggled(GtkTreeViewColumn *column, dt_lib_module_t *self)
{
  GtkWidget *toggle = gtk_tree_view_column_get_widget(column);
  const gboolean thumb_sel = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), thumb_sel);

  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  if(!thumb_sel)
  {
    // remove the thumbnails
    d->from.event = 0;
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    GtkTreeIter iter;
    for(gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid;
        valid = gtk_tree_model_iter_next(model, &iter))
      _thumb_set_in_listview(model, &iter, FALSE, self);
  }
  else if(!d->from.event)
  {
    // if the display is not yet started, start it
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    if(gtk_tree_model_get_iter_first(model, &d->from.iter))
      d->from.event = g_timeout_add_full(G_PRIORITY_LOW, 100, _thumb_set, self, NULL);
  }
}

static void _show_all_thumbs(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  const gboolean thumb_sel = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->from.thumbs));
  if(!d->from.event && thumb_sel)
  {
    // if the display is not yet started, start it
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    if(gtk_tree_model_get_iter_first(model, &d->from.iter))
      d->from.event = g_timeout_add_full(G_PRIORITY_LOW, 100, _thumb_set, self, NULL);
  }
}

static guint _import_set_file_list(const gchar *folder, const int folder_lgth,
                                   const int n, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GError *error = NULL;
  GFile *gfolder = g_file_new_for_path(folder);

  // if folder is root, consider one folder separator less
  int offset = (g_path_skip_root(folder)[0] ? folder_lgth + 1 : folder_lgth);

#ifdef WIN32
  // .. but for Windows UNC there will be a folder separator anyway
  if(dt_util_path_is_UNC(folder)) offset = folder_lgth + 1;
#endif

  GFileEnumerator *dir_files =
    g_file_enumerate_children(gfolder,
                              G_FILE_ATTRIBUTE_STANDARD_NAME ","
                              G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                              G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                              G_FILE_ATTRIBUTE_STANDARD_TYPE,
                              G_FILE_QUERY_INFO_NONE, NULL, &error);

  GFileInfo *info = NULL;
  guint nb = n;
  /* get filmroll id for current directory. if not present, checking the db whether
    the image has already been imported can be skipped */
  int32_t filmroll_id = dt_film_get_id(folder);
  const gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");
  const gboolean include_jpegs = !dt_conf_get_bool("ui_last/import_ignore_jpegs");

  if(dir_files)
  {
    while((info = g_file_enumerator_next_file(dir_files, NULL, &error)))
    {
      const char *uifilename = g_file_info_get_display_name(info);
      const char *filename = g_file_info_get_name(info);
      if(!filename)
        continue;

      /* g_file_info_get_is_hidden() always returns 0 on macOS,
        so we check if the filename starts with a '.' */
      const gboolean is_hidden = g_file_info_get_is_hidden(info) || filename[0] ==  '.';
      if (is_hidden)
        continue;

      const time_t datetime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
      GDateTime *dt_datetime = g_date_time_new_from_unix_local(datetime);
      gchar *dt_txt = g_date_time_format(dt_datetime, "%x %X");
      const GFileType filetype = g_file_info_get_file_type(info);
      gchar *uifullname = g_build_filename(folder, uifilename, NULL);
      gchar *fullname = g_build_filename(folder, filename, NULL);

      if(recursive && filetype == G_FILE_TYPE_DIRECTORY)
      {
        nb = _import_set_file_list(fullname, folder_lgth, nb, self);
      }
      // supported image format to import
      else if(filetype != G_FILE_TYPE_DIRECTORY && dt_supported_image(filename))
      {
        const char *ext = g_strrstr(filename, ".");
        if(include_jpegs || (ext && g_ascii_strncasecmp(ext, ".jpg", sizeof(".jpg"))
                                 && g_ascii_strncasecmp(ext, ".jpeg", sizeof(".jpeg"))))
        {
          gboolean already_imported = FALSE;
          if(d->import_case == DT_IMPORT_INPLACE)
          {
            /* check if image is already imported, using previously fetched filroll id */
            if(filmroll_id != -1)
              already_imported = dt_image_get_id(filmroll_id, filename) != -1 ? TRUE : FALSE;
          }
          else
          {
            gchar *basename = g_path_get_basename(filename);
            char dtid[DT_DATETIME_EXIF_LENGTH];
            dt_datetime_unix_to_exif(dtid, sizeof(dtid), &datetime);
            already_imported = dt_metadata_already_imported(basename, dtid);
            g_free(basename);
          }

          GtkTreeIter iter;
          gtk_list_store_append(d->from.store, &iter);
          gtk_list_store_set(d->from.store, &iter,
                             DT_IMPORT_UI_EXISTS, already_imported ? "✔" : " ",
                             DT_IMPORT_UI_FILENAME, &uifullname[offset],
                             DT_IMPORT_FILENAME, &fullname[offset],
                             DT_IMPORT_UI_DATETIME, dt_txt,
                             DT_IMPORT_DATETIME, datetime,
                             DT_IMPORT_THUMB, d->from.eye, -1);
          nb++;
        }
      }

      g_free(dt_txt);
      g_free(fullname);
      g_free(uifullname);
      g_date_time_unref(dt_datetime);
      g_object_unref(info);
    }
    g_file_enumerator_close(dir_files, NULL, NULL);
    g_object_unref(dir_files);
  }
  return nb;
}

static void _update_images_number(GtkWidget *label, const guint nb_sel, const guint nb)
{
  char text[256] = { 0 };
  snprintf(text, sizeof(text), ngettext("%d image out of %d selected", "%d images out of %d selected", nb_sel), nb_sel, nb);
  gtk_label_set_text(GTK_LABEL(label), text);
}

static void _import_from_selection_changed(GtkTreeSelection *selection, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  const guint nb_sel = gtk_tree_selection_count_selected_rows(selection);
  _update_images_number(d->from.img_nb, nb_sel, d->from.nb);
  gtk_dialog_set_response_sensitive(GTK_DIALOG(d->from.dialog),
                                    GTK_RESPONSE_ACCEPT, nb_sel ? TRUE : FALSE);
}

static void _update_layout(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  const gboolean usefn = dt_conf_get_bool("session/use_filename");
  for(int j = 0; j < 2 ; j++)
  {
    GtkWidget *w = gtk_grid_get_child_at(GTK_GRID(d->from.patterns), j, d->from.fn_line);
    if(GTK_IS_WIDGET(w))
      gtk_widget_set_sensitive(w, !usefn);
  }
}

static void _usefn_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  _update_layout(self);
}

static gboolean _update_files_list(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  // clear parallel thumb refresh
  d->from.event = 0;
  GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
  g_object_ref(model);
  gtk_tree_view_set_model(d->from.treeview, NULL);
  gtk_list_store_clear(d->from.store);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                       GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
  {
    d->from.nb = _import_from_camera_set_file_list(self);
    gtk_widget_hide(d->from.info);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                         DT_IMPORT_FILENAME, GTK_SORT_ASCENDING);
  }
  else
#endif
  {
    char *folder = dt_conf_get_string("ui_last/import_last_directory");
    d->from.nb = !folder[0] ? 0 : _import_set_file_list(folder, strlen(folder), 0, self);
    g_free(folder);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                         DT_IMPORT_DATETIME, GTK_SORT_ASCENDING);
  }
  gtk_tree_view_set_model(d->from.treeview, model);
  g_object_unref(model);

  if(dt_conf_get_bool("ui_last/import_select_new"))
    _do_select_new(self);
  else
    _do_select_all(self);

  return FALSE;
}

static void _import_new_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) _do_select_new(self);
}

static void _ignore_jpegs_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  _update_files_list(self);
  _show_all_thumbs(self);
}

static void _recursive_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  _update_files_list(self);
  _show_all_thumbs(self);
}

static void _do_select_all_clicked(GtkWidget *widget, dt_lib_module_t* self)
{
  _do_select_all(self);
}
static void _do_select_none_clicked(GtkWidget *widget, dt_lib_module_t* self)
{
  _do_select_none(self);
}

static void _do_select_new_clicked(GtkWidget *widget, dt_lib_module_t* self)
{
  _do_select_new(self);
}

static void _expander_create(dt_gui_collapsible_section_t *cs,
                             GtkBox *parent,
                             const char *label,
                             const char *pref_key,
                             dt_lib_module_t *self)
{
  dt_gui_new_collapsible_section
    (cs,
     pref_key,
     label,
     parent,
     DT_ACTION(self));
}

static void _resize_dialog(GtkWidget *widget, dt_lib_module_t* self)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_conf_set_int("ui_last/import_dialog_width", allocation.width);
  dt_conf_set_int("ui_last/import_dialog_height", allocation.height);
}

static gboolean _find_iter_folder(GtkTreeModel *model, GtkTreeIter *iter,
                                  const char *folder)
{
  gboolean found = FALSE;
  if(!folder) return found;
  char *path;
  do
  {
    gtk_tree_model_get(model, iter, DT_FOLDER_PATH, &path, -1);
    found = !g_strcmp0(folder, path);
    g_free(path);
    if(found) return found;
    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_iter_folder(model, &child, folder);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

static void _get_folders_list(GtkTreeStore *store, GtkTreeIter *parent,
                              const gchar *folder, const char *selected)
{
  // each time a new folder is added, it is set as not expanded and assigned a fake child
  // when expanded, the children are added and the fake child is reused
  GError *error = NULL;
  GFile *gfolder = g_file_new_for_path(folder);
  GFileEnumerator *dir_files = g_file_enumerate_children(gfolder,
                                  G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                                  G_FILE_ATTRIBUTE_STANDARD_TYPE ","
                                  G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN ","
                                  G_FILE_ATTRIBUTE_ACCESS_CAN_READ,
                                  G_FILE_QUERY_INFO_NONE, NULL, &error);

  gboolean expanded = FALSE;
  GtkTreeIter iter;
  GtkTreeIter parent2;
  if(!parent)
  {
    gchar *basename = g_path_get_basename(folder);
    gtk_tree_store_append(store, &parent2, NULL);
    gtk_tree_store_set(store, &parent2, DT_FOLDER_NAME, basename,
                                     DT_FOLDER_PATH, folder,
                                     DT_FOLDER_EXPANDED, FALSE, -1);
    // fake child
    gtk_tree_store_append(store, &iter, &parent2);
    gtk_tree_store_set(store, &iter, DT_FOLDER_EXPANDED, FALSE, -1);
    g_free(basename);
  }
  else
  {
    parent2 = *parent;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &parent2, DT_FOLDER_EXPANDED, &expanded, -1);
  }
  GFileInfo *info = NULL;
  gint i = 0;
  if(dir_files)
  {
    while((info = g_file_enumerator_next_file(dir_files, NULL, &error)))
    {
      const char *filename = g_file_info_get_name(info);
      if(!filename)
        continue;
      const gboolean ishidden = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_STANDARD_IS_HIDDEN);
      const gboolean canread = g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ);
      const GFileType filetype = g_file_info_get_file_type(info);
      if(filetype == G_FILE_TYPE_DIRECTORY && !ishidden && canread)
      {
        gchar *fullname = g_build_filename(folder, filename, NULL);
        if(!expanded)
        {
          GtkTreeIter child;
          const char *uifilename = g_file_info_get_display_name(info);
          gchar *uifullname = g_build_filename(folder, uifilename, NULL);
          gchar *basename = g_path_get_basename(uifullname);
          if(!i)
            gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &iter, &parent2);
          else
            gtk_tree_store_append(store, &iter, &parent2);
          gtk_tree_store_set(store, &iter, DT_FOLDER_NAME, basename,
                                           DT_FOLDER_PATH, fullname,
                                           DT_FOLDER_EXPANDED, FALSE, -1);
          // fake child
          gtk_tree_store_append(store, &child, &iter);
          gtk_tree_store_set(store, &iter, DT_FOLDER_EXPANDED, FALSE, -1);
          g_free(uifullname);
          g_free(basename);
        }
        else
        {
          iter = parent2;
          if(!_find_iter_folder(GTK_TREE_MODEL(store), &iter, fullname))
          {
            g_free(fullname);
            g_object_unref(info);
            break;
          }
        }
        if(selected[0] && g_str_has_prefix(selected, fullname))
          _get_folders_list(store, &iter, fullname, selected);
        g_free(fullname);
        i++;
      }
      gtk_tree_store_set(store, &parent2, DT_FOLDER_EXPANDED, TRUE, -1);
      g_object_unref(info);
    }
    g_file_enumerator_close(dir_files, NULL, NULL);
    g_object_unref(dir_files);
  }
  if(!i)
  {
    // remove the fake child as there is no child
    gtk_tree_model_iter_children(GTK_TREE_MODEL(store), &iter, &parent2);
    gtk_tree_store_remove(store, &iter);
  }
}

// workaround to erase parasitic selection when click on expander or empty part of the view
static gboolean _clear_parasitic_selection(gpointer user_data)
{
  if(dt_conf_is_equal("ui_last/import_last_directory", ""))
  {
    dt_lib_module_t *self = (dt_lib_module_t *)user_data;
    dt_lib_import_t *d = (dt_lib_import_t *)self->data;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.folderview);
    if(gtk_tree_selection_count_selected_rows(selection))
      gtk_tree_selection_unselect_all(selection);
  }
  return FALSE;
}

static gboolean _places_button_press(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  gboolean res = FALSE;
  GtkTreePath *path = NULL;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter iter;
    gtk_tree_model_get_iter(model, &iter, path);

    char *folder_name, *folder_path;
    gtk_tree_model_get(model, &iter, 0, &folder_name, 1, &folder_path, -1);

    const int button_pressed = (event->type == GDK_BUTTON_PRESS) ? event->button : 0;

    // left-click: set as new root
    if(button_pressed == 1)
    {
      GtkTreeSelection *place_selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
      gtk_tree_selection_select_path(place_selection, path);
      dt_conf_set_string("ui_last/import_last_place", folder_path);
      dt_conf_set_string("ui_last/import_last_directory", "");
      _update_folders_list(self);
      _update_files_list(self);
    }
    // right-click: delete / hide place (if not selected)
    else if(button_pressed == 3)
    {
      if(g_strcmp0(folder_path, dt_conf_get_string_const("ui_last/import_last_place")))
        _remove_place(folder_path, iter, self);
      else
        dt_toast_log(_("you can't delete the selected place"));
    }

    g_free(folder_name);
    g_free(folder_path);

    res = TRUE;
  }

  gtk_tree_path_free(path);
  return res;
}

static gboolean _folders_button_press(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  gboolean res = FALSE;
  const int button_pressed = (event->type == GDK_BUTTON_PRESS) ? event->button : 0;
  const gboolean modifier = dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK);
  if((button_pressed == 1) && !modifier)
  {
    GtkTreePath *path = NULL;
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), event->x, event->y, &path, NULL, NULL, NULL))
    {
      GdkRectangle rect;
      gtk_tree_view_get_cell_area(GTK_TREE_VIEW(view), path, d->from.foldercol, &rect);
      const gboolean blank = gtk_tree_view_is_blank_at_pos(GTK_TREE_VIEW(view), event->x, event->y,
                                                           NULL, NULL, NULL, NULL);
      // select and save new folder only if not click on expander
      if(blank || (event->x > rect.x))
      {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.folderview);
        gtk_tree_selection_select_path(selection, path);
        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
        GtkTreeIter iter;
        gtk_tree_model_get_iter(model, &iter, path);
        char *folder;
        gtk_tree_model_get(model, &iter, DT_FOLDER_PATH, &folder, -1);
        dt_conf_set_string("ui_last/import_last_directory", folder);
        g_free(folder);
        _update_files_list(self);
        _show_all_thumbs(self);
        res = TRUE;
      }
    }
    gtk_tree_path_free(path);
  }

  if(event->type == GDK_DOUBLE_BUTTON_PRESS)
  {
    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), event->x, event->y, &path, NULL, NULL, NULL);
    if(gtk_tree_view_row_expanded(d->from.folderview, path))
      gtk_tree_view_collapse_row (d->from.folderview, path);
    else
      gtk_tree_view_expand_row (d->from.folderview, path, FALSE);
    gtk_tree_path_free(path);
  }

  g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 100, _clear_parasitic_selection, self, NULL);
	return res;
}

static void _folder_order_clicked(GtkTreeViewColumn *column, dt_lib_module_t *self)
{
  dt_conf_set_bool("ui_last/import_last_folder_descending",
                  !dt_conf_get_bool("ui_last/import_last_folder_descending"));
}

static void _row_expanded(GtkTreeView *view, GtkTreeIter *iter,
                          GtkTreePath *path, dt_lib_module_t *self)
{
  GtkTreeModel *model = gtk_tree_view_get_model(view);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                       GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  char *fullname;
  gtk_tree_model_get(model, iter, DT_FOLDER_PATH, &fullname, -1);
  _get_folders_list(GTK_TREE_STORE(model), iter, fullname, "");
  g_free(fullname);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), DT_FOLDER_PATH,
                                       dt_conf_get_bool("ui_last/import_last_folder_descending")
                                       ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

static void _paned_position_changed(GtkWidget *widget, dt_lib_module_t* self)
{
  const gint position = gtk_paned_get_position(GTK_PANED(widget));
  dt_conf_set_int("ui_last/import_dialog_paned_pos", position);
}

static void _paned_places_position_changed(GtkWidget *widget, dt_lib_module_t* self)
{
  const gint position = gtk_paned_get_position(GTK_PANED(widget));
  dt_conf_set_int("ui_last/import_dialog_paned_places_pos", position);
}

static void _places_reset_callback(GtkWidget *widget, dt_lib_module_t* self)
{
  dt_conf_set_bool("ui_last/import_dialog_show_home", TRUE);
  dt_conf_set_bool("ui_last/import_dialog_show_pictures", TRUE);
  dt_conf_set_bool("ui_last/import_dialog_show_mounted", TRUE);

  _update_places_list(self);
}

static void _set_places_list(GtkWidget *places_paned, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->placesModel = gtk_list_store_new(DT_PLACES_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
  d->placesView = gtk_tree_view_new_with_model(GTK_TREE_MODEL(d->placesModel));

  GtkWidget *places_top_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *places_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_tooltip_text(places_header, _("choose the root of the folder tree below"));

  GtkWidget *places_label = gtk_label_new(NULL);
  gchar *markup = g_strdup_printf("<b>  %s</b>",_("places"));
  gtk_label_set_markup(GTK_LABEL(places_label), markup);
  g_free(markup);
  gtk_box_pack_start(GTK_BOX(places_header), places_label, FALSE, FALSE, 0);

  GtkWidget *places_reset = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  gtk_widget_set_tooltip_text(places_reset, _("restore all default places you have removed by right-click"));
  g_signal_connect(places_reset, "clicked", G_CALLBACK(_places_reset_callback), self);
  gtk_box_pack_end(GTK_BOX(places_header), places_reset, FALSE, FALSE, 0);

  GtkWidget *places_add = dtgtk_button_new(dtgtk_cairo_paint_plus_simple, 0, NULL);
  gtk_widget_set_tooltip_text(places_add, _("add a custom place\n\nright-click on a place to remove it"));
  g_signal_connect(places_add, "clicked", G_CALLBACK(_lib_import_select_folder), self);
  gtk_box_pack_end(GTK_BOX(places_header), places_add, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(places_top_box), places_header, FALSE, FALSE, 0);

  GtkWidget *placesWindow = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_tooltip_text(placesWindow, _("you can add custom places using the plus icon"));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(placesWindow), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->placesView),FALSE);
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->placesView), DT_PLACES_PATH);
  gtk_container_add(GTK_CONTAINER(placesWindow), GTK_WIDGET(d->placesView));

  GtkTreeViewColumn* placesColumn = gtk_tree_view_column_new_with_attributes("", gtk_cell_renderer_text_new(), "text", 0, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->placesView), placesColumn);

  gtk_box_pack_start(GTK_BOX(places_top_box), placesWindow, TRUE, TRUE, 0);
  gtk_paned_pack1(GTK_PANED(places_paned), places_top_box, TRUE, TRUE);


  g_signal_connect(G_OBJECT(d->placesView), "button-press-event", G_CALLBACK(_places_button_press), self);
}

static void _set_folders_list(GtkWidget *places_paned, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkTreeStore *store = gtk_tree_store_new(DT_FOLDER_NUM_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);
  d->from.folderview = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(d->from.folderview));
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->from.folderview), _("select a folder to see the content"));

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("folders"), renderer, "text",
                                                    DT_FOLDER_NAME, NULL);
  gtk_tree_view_append_column(d->from.folderview, column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_set_expander_column(d->from.folderview, column);
  g_signal_connect(d->from.folderview, "row-expanded", G_CALLBACK(_row_expanded), self);
  g_signal_connect(G_OBJECT(d->from.folderview), "button-press-event", G_CALLBACK(_folders_button_press), self);
  gtk_tree_view_column_set_sort_column_id(column, DT_FOLDER_PATH);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), DT_FOLDER_PATH,
                                dt_conf_get_bool("ui_last/import_last_folder_descending")
                                ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
  g_signal_connect(column, "clicked", G_CALLBACK(_folder_order_clicked), self);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  d->from.foldercol = column;
  gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(w), DT_PIXEL_APPLY_DPI(200));
  gtk_tree_view_set_model(d->from.folderview, GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_visible(d->from.folderview, TRUE);
  gtk_paned_pack2(GTK_PANED(places_paned), w, TRUE, TRUE);
}

static void _expand_folder(const char *folder, const gboolean select, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  if(folder && folder[0])
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->from.folderview));
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      if(_find_iter_folder(model, &iter, folder))
      {
        // except for root expand only to parent
        GtkTreeIter parent;
        if(!gtk_tree_model_iter_parent(model, &parent, &iter))
          parent = iter;
        GtkTreePath *parent_path = gtk_tree_model_get_path(model, &parent);
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(d->from.folderview, parent_path);
        gtk_tree_view_scroll_to_cell(d->from.folderview, path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_path_free(path);
        gtk_tree_path_free(parent_path);
        if(select)
        {
          GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.folderview);
          gtk_tree_selection_select_iter(selection, &iter);
        }
      }
    }
  }
}

static void _update_places_list(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  gtk_list_store_clear(d->placesModel);

  // add default folders

  GtkTreeIter iter, current_iter;
  d->placesSelection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->placesView));
  char *last_place = dt_conf_get_string("ui_last/import_last_place");
  char *current_place = NULL;

  if(dt_conf_get_bool("ui_last/import_dialog_show_home"))
  {
    gchar *homedir = dt_loc_get_home_dir(NULL);
    if(homedir)
    {
      current_place = homedir;
      gtk_list_store_insert_with_values(d->placesModel, &iter, -1, DT_PLACES_NAME, _("home"), DT_PLACES_PATH,
                                        current_place, DT_PLACES_TYPE, DT_TYPE_HOME, -1);
      if(!g_strcmp0(current_place, last_place))
        gtk_tree_selection_select_iter(d->placesSelection, &iter);
      current_iter = iter;
    }
  }

  if(dt_conf_get_bool("ui_last/import_dialog_show_pictures") && g_get_user_special_dir(G_USER_DIRECTORY_PICTURES))
  {
    g_free(current_place);
    current_place = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));
    gtk_list_store_insert_with_values(d->placesModel, &iter, -1, DT_PLACES_NAME, _("pictures"), DT_PLACES_PATH,
                                      current_place, DT_PLACES_TYPE, DT_TYPE_PIC, -1);
    if(!g_strcmp0(current_place, last_place))
      gtk_tree_selection_select_iter(d->placesSelection, &iter);
    current_iter = iter;
  }

  // set home/pictures as default
  if(last_place[0] == '\0' && current_place)
  {
    dt_conf_set_string("ui_last/import_last_place", current_place);
    gtk_tree_selection_select_iter(d->placesSelection, &current_iter);
  }
  g_free(current_place);

  // add mounted drives
  if(dt_conf_get_bool("ui_last/import_dialog_show_mounted"))
  {
    GVolumeMonitor *placesMonitor = g_volume_monitor_get();
    GList *drives, *drive, *volumes, *volume;
    drives = g_volume_monitor_get_connected_drives(placesMonitor);

    for(drive = drives; drive; drive = drive->next)
    {
      volumes = g_drive_get_volumes(drive->data);
      for(volume = volumes; volume; volume = volume->next)
      {
        GMount *placesMount = g_volume_get_mount(volume->data);
        if(placesMount)
        {
          GFile *placesFile = g_mount_get_root(placesMount);
          g_object_unref(placesMount);

          gchar *volname = g_volume_get_name(volume->data);
          gchar *path = g_file_get_path(placesFile);
          gtk_list_store_insert_with_values(d->placesModel, &iter, -1, DT_PLACES_NAME, volname,
                                            DT_PLACES_PATH, path, DT_PLACES_TYPE, DT_TYPE_MOUNT, -1);
          g_free(volname);
          g_free(path);

          if(!g_strcmp0(g_file_get_path(placesFile), last_place))
            gtk_tree_selection_select_iter(d->placesSelection, &iter);
        }
      }
      g_list_free(volumes);
    }
    g_list_free(drives);
  }

  // add folders added by user
  GList *places = _get_custom_places();

  for(GList *places_iter = places; places_iter; places_iter = places_iter->next)
  {
    gchar *basename = g_path_get_basename(places_iter->data);

#ifdef WIN32
    // special case: root folder shall keep the drive letter in the basename
    if(basename[0] == G_DIR_SEPARATOR && !basename[1])
    {
      g_free(basename);
      basename = g_strdup(places_iter->data);
    }
#endif

    gtk_list_store_insert_with_values(d->placesModel, &iter, -1, DT_PLACES_NAME, basename,
                                      DT_PLACES_PATH, (char *)places_iter->data, DT_PLACES_TYPE, DT_TYPE_CUSTOM, -1);
    g_free(basename);
    if(!g_strcmp0(places_iter->data, last_place))
      gtk_tree_selection_select_iter(d->placesSelection, &iter);
  }
  g_free(last_place);
  // the list returned by _get_custom_places references a single string that has been split on commas.  Release it
  // by freeing the data of the list's first node
  if(places)
    g_free(places->data);
  g_list_free(places);
}

static void _update_folders_list(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->from.folderview));
  g_object_ref(model);
  gtk_tree_view_set_model(d->from.folderview, NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(model));
  const char *last_place = dt_conf_get_string_const("ui_last/import_last_place");
  const char *folder = dt_conf_get_string_const("ui_last/import_last_directory");
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                       GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  _get_folders_list(GTK_TREE_STORE(model), NULL, last_place, folder);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), DT_FOLDER_PATH,
                                       dt_conf_get_bool("ui_last/import_last_folder_descending")
                                       ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
  gtk_tree_view_set_model(d->from.folderview, model);
  g_object_unref(model);

  if(folder[0] && strncmp(folder, last_place, strlen(last_place)) == 0)
    _expand_folder(folder, TRUE, self);
  else
    _expand_folder(last_place, FALSE, self);
}

static void _escape_place_name_comma(char *name)
{
  for(int i = 0; name && i < strlen(name); i++)
    if(name[i] == ',') name[i] = '\1';
}

static void _restore_place_name_comma(char *name)
{
  for(int i = 0; name && i < strlen(name); i++)
    if(name[i] == '\1') name[i] = ',';
}

static gboolean _find_iter_place(GtkTreeModel *model, GtkTreeIter *iter,
                                  const char *place)
{
  gboolean found = FALSE;
  if(!place) return found;
  char *path;
  do
  {
    gtk_tree_model_get(model, iter, DT_PLACES_PATH, &path, -1);
    found = !g_strcmp0(place, path);
    g_free(path);
    if(found) return found;
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

static void _add_custom_place(gchar *place, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkTreeIter iter;
  gtk_tree_model_get_iter_first(GTK_TREE_MODEL(d->placesModel), &iter);
  const gboolean found = _find_iter_place(GTK_TREE_MODEL(d->placesModel), &iter, place);

  if(!found)
  {
    const char *current_places = dt_conf_get_string_const("ui_last/import_custom_places");
    _escape_place_name_comma(place);
    gchar *places = g_strdup_printf("%s%s,", current_places, place);
    dt_conf_set_string("ui_last/import_custom_places", places);
    g_free(places);

    _restore_place_name_comma(place);
    gchar *basename = g_path_get_basename(place);
#ifdef WIN32
    // special case: root folder shall keep the drive letter in the basename
    if(G_IS_DIR_SEPARATOR(basename[0]) && !basename[1])
    {
      g_free(basename);
      basename = g_strdup(place);
    }
#endif
    gtk_list_store_insert_with_values(d->placesModel, &iter, -1, DT_PLACES_NAME, basename,
                                      DT_PLACES_PATH, (char *)place, DT_PLACES_TYPE, DT_TYPE_CUSTOM, -1);
    g_free(basename);
  }

  dt_conf_set_string("ui_last/import_last_place", place);
  gtk_tree_selection_select_iter(d->placesSelection, &iter);
}

static void _remove_place(gchar *place, GtkTreeIter iter, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  _escape_place_name_comma(place);
  const char *current_places = dt_conf_get_string_const("ui_last/import_custom_places");
  int type = 0;
  gtk_tree_model_get(GTK_TREE_MODEL(d->placesModel), &iter, DT_PLACES_TYPE, &type, -1);

  if(type == DT_TYPE_HOME)
    dt_conf_set_bool("ui_last/import_dialog_show_home", FALSE);
  if(type == DT_TYPE_PIC)
    dt_conf_set_bool("ui_last/import_dialog_show_pictures", FALSE);
  if(type == DT_TYPE_MOUNT)
    dt_conf_set_bool("ui_last/import_dialog_show_mounted", FALSE);
  if(type == DT_TYPE_CUSTOM)
  {
    gchar *pattern = g_strdup_printf("%s,", place);
    gchar *places = dt_util_str_replace(current_places, pattern, "");
    dt_conf_set_string("ui_last/import_custom_places", places);
    g_free(pattern);
    g_free(places);
  }

  _update_places_list(self);
}

static GList* _get_custom_places()
{
  GList *places = NULL;
  gchar *saved = dt_conf_get_string("ui_last/import_custom_places");
  const int nb_saved = saved[0] ? dt_util_str_occurence(saved, ",") + 1 : 0;

  for(int i = 0; i < nb_saved; i++)
  {
    char *next = g_strstr_len(saved, strlen(saved), ",");
    if(next)
      next[0] = '\0';
    if(saved[0])
    {
      places = g_list_append(places, saved);
      _restore_place_name_comma(saved);
      if(next)
        saved = next + 1;
    }
  }

  return places;
}

static void _lib_import_select_folder(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_open"), _("_cancel"));

  // run the native dialog
  dt_conf_get_folder_to_file_chooser("ui_last/import_last_place", GTK_FILE_CHOOSER(filechooser));
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *dirname = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    _add_custom_place(dirname, self);
    g_free(dirname);
  }
  g_object_unref(filechooser);

  dt_conf_set_string("ui_last/import_last_directory", "");
  dt_conf_set_bool("ui_last/import_recursive", FALSE);
  dt_gui_preferences_bool_update(d->recursive);
  _update_folders_list(self);
  _update_files_list(self);
}

static void _set_files_list(GtkWidget *rbox, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->from.store = gtk_list_store_new(DT_IMPORT_NUM_COLS, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_UINT64);
  d->from.eye = dt_draw_paint_to_pixbuf(GTK_WIDGET(d->from.dialog), 13, 0, dtgtk_cairo_paint_eye);

  // Create the treview with list model data store
  d->from.w = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->from.w), GTK_POLICY_NEVER,
                                 GTK_POLICY_ALWAYS);
  d->from.treeview = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(d->from.w), GTK_WIDGET(d->from.treeview));

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("✔", renderer, "text",
                                                    DT_IMPORT_UI_EXISTS, NULL);
  g_object_set(renderer, "xalign", 0.5, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(25));
  GtkWidget *header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header, _("mark already imported pictures"));

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("name"), renderer, "text",
                                                    DT_IMPORT_UI_FILENAME, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);

  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
  gtk_tree_view_column_set_sort_column_id(column, DT_IMPORT_FILENAME);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("modified"), renderer, "text",
                                                    DT_IMPORT_UI_DATETIME, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);
  gtk_tree_view_column_set_sort_column_id(column, DT_IMPORT_DATETIME);
  header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header, _("file 'modified date/time', may be different from 'Exif date/time'"));
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->from.store),
                                       DT_IMPORT_DATETIME, GTK_SORT_ASCENDING);

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes("", renderer, "pixbuf",
                                                    DT_IMPORT_THUMB, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);
  GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, 0, NULL);
  dt_gui_add_class(button, "dt_transparent_background");
  gtk_widget_show(button);
  header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header, _("show/hide thumbnails"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
  gtk_tree_view_column_set_widget(column, button);
  g_signal_connect(column, "clicked", G_CALLBACK(_all_thumb_toggled), self);
  d->from.thumbs = button;
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(128));
  d->from.pixcol = column;
  g_signal_connect(G_OBJECT(d->from.treeview), "button-press-event",
                   G_CALLBACK(_files_button_press), self);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(_import_from_selection_changed), self);

  gtk_tree_view_set_model(d->from.treeview, GTK_TREE_MODEL(d->from.store));
  gtk_tree_view_set_headers_visible(d->from.treeview, TRUE);

  gtk_box_pack_start(GTK_BOX(rbox), GTK_WIDGET(d->from.w), TRUE, TRUE, 0);
}

static void _browse_basedir_clicked(GtkWidget *widget, GtkEntry *basedir)
{
  GtkWidget *topwindow = gtk_widget_get_toplevel(widget);
  if(!GTK_IS_WINDOW(topwindow))
  {
    topwindow = dt_ui_main_window(darktable.gui->ui);
  }

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
      _("select directory"), GTK_WINDOW(topwindow), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_open"), _("_cancel"));

  gchar *old = g_strdup(gtk_entry_get_text(basedir));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));

    // dir can now contain '\': on Windows it's the path separator,
    // on other platforms it can be part of a regular folder name.
    // This would later clash with variable substitution, so we have to escape them
    gchar *escaped = dt_util_str_replace(dir, "\\", "\\\\");

    gtk_entry_set_text(basedir, escaped); // the signal handler will write this to conf
    gtk_editable_set_position(GTK_EDITABLE(basedir), strlen(escaped));
    g_free(dir);
    g_free(escaped);
  }
  g_object_unref(filechooser);
}

static void _set_expander_content(GtkWidget *rbox, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  // separator
  dt_gui_add_class(GTK_WIDGET(d->from.w), "dt_section_label");
  // job code
  GtkWidget *import_patterns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gint line = 0;
  dt_gui_preferences_string(grid, "ui_last/import_jobcode", 0, line++);
  gtk_box_pack_start(GTK_BOX(import_patterns), GTK_WIDGET(grid), FALSE, FALSE, 0);

  // collapsible section
  _expander_create(&d->from.cs, GTK_BOX(import_patterns),
                   _("naming rules"), "ui_last/session_expander_import", NULL);

  // import patterns
  grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  d->from.datetime = dt_gui_preferences_string(grid, "ui_last/import_datetime_override", 0, line++);
  GtkWidget *basedir = dt_gui_preferences_string(grid, "session/base_directory_pattern", 0, line++);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  g_object_ref(basedir);
  gtk_container_remove(GTK_CONTAINER(grid), basedir);
  gtk_box_pack_start(GTK_BOX(hbox), basedir, TRUE, TRUE, 0);
  g_object_unref(basedir);
  GtkWidget *browsedir = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_name(browsedir, "non-flat");
  gtk_widget_set_tooltip_text(browsedir, _("select directory"));

  gtk_box_pack_start(GTK_BOX(hbox), browsedir, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(browsedir), "clicked", G_CALLBACK(_browse_basedir_clicked), basedir);
  gtk_grid_attach_next_to(grid, hbox, gtk_grid_get_child_at(grid, 0, line - 1), GTK_POS_RIGHT, 1, 1);

  dt_gui_preferences_string(grid, "session/sub_directory_pattern", 0, line++);
  GtkWidget *usefn = dt_gui_preferences_bool(grid, "session/use_filename", 0, line++, FALSE);
  d->from.fn_line = line;
  dt_gui_preferences_string(grid, "session/filename_pattern", 0, line++);
  gtk_box_pack_start(GTK_BOX(d->from.cs.container), GTK_WIDGET(grid), FALSE, FALSE, 0);
  d->from.patterns = grid;
  _update_layout(self);
  g_signal_connect(usefn, "toggled", G_CALLBACK(_usefn_toggled), self);
  gtk_box_pack_start(GTK_BOX(rbox), import_patterns, FALSE, FALSE, 0);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  dt_gui_preferences_bool(grid, "ui_last/import_keep_open", 0, 0, TRUE);
  gtk_box_pack_end(GTK_BOX(box), GTK_WIDGET(grid), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(rbox), GTK_WIDGET(box), FALSE, FALSE, 0);
}

static const char *const _import_text[] =
{
  N_("add to library"),
  N_("copy & import"),
  N_("copy & import from camera")
};

static void _import_from_dialog_new(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  d->from.dialog = gtk_dialog_new_with_buttons
    ( _(_import_text[d->import_case]), NULL, GTK_DIALOG_MODAL,
      _("cancel"), GTK_RESPONSE_CANCEL,
      _(_import_text[d->import_case]), GTK_RESPONSE_ACCEPT,
      NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->from.dialog);
#endif
  gtk_window_set_default_size(GTK_WINDOW(d->from.dialog),
                              dt_conf_get_int("ui_last/import_dialog_width"),
                              dt_conf_get_int("ui_last/import_dialog_height"));
  gtk_window_set_transient_for(GTK_WINDOW(d->from.dialog), GTK_WINDOW(win));
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d->from.dialog));
  g_signal_connect(d->from.dialog, "check-resize", G_CALLBACK(_resize_dialog), self);
  g_signal_connect(d->from.dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), self);

  // images numbers in action-box
  GtkWidget *box = dt_gui_container_first_child(GTK_CONTAINER(d->from.dialog));
  box = dt_gui_container_first_child(GTK_CONTAINER(box)); // action-box

  GtkWidget *select_all = gtk_button_new_with_label(_("select all"));
  gtk_box_pack_start(GTK_BOX(box), select_all, FALSE, FALSE, 2);
  g_signal_connect(select_all, "clicked", G_CALLBACK(_do_select_all_clicked), self);

  GtkWidget *select_none = gtk_button_new_with_label(_("select none"));
  gtk_box_pack_start(GTK_BOX(box), select_none, FALSE, FALSE, 2);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_do_select_none_clicked), self);

  GtkWidget *select_new = gtk_button_new_with_label(_("select new"));
  gtk_box_pack_start(GTK_BOX(box), select_new, FALSE, FALSE, 2);
  g_signal_connect(select_new, "clicked", G_CALLBACK(_do_select_new_clicked), self);

  d->from.img_nb = gtk_label_new("");
  gtk_widget_set_halign(d->from.img_nb, GTK_ALIGN_END);
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->from.img_nb), TRUE, TRUE, 0);

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(paned), dt_conf_get_int("ui_last/import_dialog_paned_pos"));

  // right pane
  GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_paned_pack2(GTK_PANED(paned), rbox, TRUE, FALSE);
  gtk_box_pack_start(GTK_BOX(content), paned, TRUE, TRUE, 0);

  guint line = 0;
  guint col = 0;
  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));

  d->import_new = dt_gui_preferences_bool(grid, "ui_last/import_select_new", col++, line, TRUE);
  gtk_widget_set_hexpand(gtk_grid_get_child_at(grid, col++, line), TRUE);
  g_signal_connect(G_OBJECT(d->import_new), "toggled", G_CALLBACK(_import_new_toggled), self);

  if(d->import_case != DT_IMPORT_CAMERA)
  {
    d->recursive = dt_gui_preferences_bool(grid, "ui_last/import_recursive", col++, line, TRUE);
    gtk_widget_set_hexpand(gtk_grid_get_child_at(grid, col++, line), TRUE);
    g_signal_connect(G_OBJECT(d->recursive), "toggled", G_CALLBACK(_recursive_toggled), self);
  }
  GtkWidget *ignore_jpegs = dt_gui_preferences_bool(grid, "ui_last/import_ignore_jpegs", col++, line, TRUE);
  gtk_widget_set_hexpand(gtk_grid_get_child_at(grid, col++, line++), TRUE);
  g_signal_connect(G_OBJECT(ignore_jpegs), "toggled", G_CALLBACK(_ignore_jpegs_toggled), self);
  gtk_box_pack_start(GTK_BOX(rbox), GTK_WIDGET(grid), FALSE, FALSE, 8);

  // files list
  _set_files_list(rbox, self);
  g_timeout_add_full(G_PRIORITY_LOW, 100, _update_files_list, self, NULL);

#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
  {
    d->from.info = dt_ui_label_new(_("please wait while prefetching the list of images from camera..."));
    gtk_label_set_single_line_mode(GTK_LABEL(d->from.info), FALSE);
    gtk_box_pack_start(GTK_BOX(rbox), d->from.info, FALSE, FALSE, 0);
  }
  else
#endif
  {
    // left pane
    g_signal_connect(paned, "notify::position", G_CALLBACK(_paned_position_changed), self);
    GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_paned_pack1(GTK_PANED(paned), lbox, TRUE, FALSE);

    GtkWidget *places_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);

    gtk_paned_set_position(GTK_PANED(places_paned), dt_conf_get_int("ui_last/import_dialog_paned_places_pos"));
    g_signal_connect(places_paned, "notify::position", G_CALLBACK(_paned_places_position_changed), self);

    _set_places_list(places_paned, self);
    _set_folders_list(places_paned, self);
    gtk_box_pack_start(GTK_BOX(lbox), places_paned, TRUE, TRUE, 0);

    _update_places_list(self);
    _update_folders_list(self);
  }

  // patterns expander
  if(d->import_case != DT_IMPORT_INPLACE)
  {
    _set_expander_content(rbox, self);
    gtk_widget_show_all(d->from.dialog);
    dt_gui_update_collapsible_section(&d->from.cs);
  }
  else
  {
    gtk_widget_show_all(d->from.dialog);
  }
}

static void _import_set_collection(const char *dirname)
{
  if(dirname)
  {
    dt_collection_properties_t property = dt_conf_get_int("plugins/lighttable/collect/item0");

    if(property != DT_COLLECTION_PROP_FOLDERS
       && property != DT_COLLECTION_PROP_FILMROLL)
    {
      // the current collection is not based on filmrolls or folders
      // fallback to DT_COLLECTION_PROP_FILMROLL. Otherwise we keep
      // the current property of the collection.
      property = DT_COLLECTION_PROP_FILMROLL;
    }
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/item0", property);
    dt_conf_set_string("plugins/lighttable/collect/string0", dirname);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static void _do_select_all(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
  gtk_tree_selection_select_all(selection);
}

static void _do_select_none(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
  gtk_tree_selection_unselect_all(selection);
}

static void _do_select_new(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->from.treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);

  gtk_tree_selection_unselect_all(selection);

  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      gchar *sel = NULL;
      gtk_tree_model_get(model, &iter, DT_IMPORT_UI_EXISTS, &sel, -1);
      if(sel && !strcmp(sel, " "))
        gtk_tree_selection_select_iter(selection, &iter);
    }
    while(gtk_tree_model_iter_next(model, &iter));
  }
}

static void _import_from_dialog_run(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  while(gtk_dialog_run(GTK_DIALOG(d->from.dialog)) == GTK_RESPONSE_ACCEPT)
  {
    // reset filter so that view isn't empty
    dt_view_filtering_reset(darktable.view_manager, TRUE);
    GList *imgs = NULL;
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
    GList *paths = gtk_tree_selection_get_selected_rows(selection, &model);
    char *folder = (d->import_case == DT_IMPORT_CAMERA) ? g_strdup("") :
                   dt_conf_get_string("ui_last/import_last_directory");
    for(GList *path = paths; path; path = g_list_next(path))
    {
      GtkTreeIter iter;
      gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)path->data);
      char *filename;
      gtk_tree_model_get(model, &iter, DT_IMPORT_FILENAME, &filename, -1);
      char *fullname = g_build_filename(folder, filename, NULL);
      imgs = g_list_prepend(imgs, fullname);
      g_free(filename);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    if(imgs)
    {
      const gboolean unique = !imgs->next;
      imgs = g_list_reverse(imgs);
      char datetime_override[DT_DATETIME_LENGTH] = {0};
      if(d->import_case != DT_IMPORT_INPLACE)
      {
        const char *entry = gtk_entry_get_text(GTK_ENTRY(d->from.datetime));
        if(entry[0] && !dt_datetime_entry_to_exif(datetime_override, sizeof(datetime_override), entry))
        {
          dt_control_log(_("invalid override date/time format"));
          break;
        }
        dt_gui_preferences_string_reset(d->from.datetime);
      }
#ifdef HAVE_GPHOTO2
      if(d->import_case == DT_IMPORT_CAMERA)
      {
        dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG,
                           dt_camera_import_job_create(imgs, d->camera, datetime_override));
      }
      else
#endif
        dt_control_import(imgs, datetime_override, d->import_case == DT_IMPORT_INPLACE);

      if(d->import_case == DT_IMPORT_INPLACE)
      {
        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->recursive)))
          folder = dt_util_dstrcat(folder, "%%");
        _import_set_collection(folder);
        const int imgid = dt_conf_get_int("ui_last/import_last_image");
        if(unique && imgid != -1)
        {
          dt_control_set_mouse_over_id(imgid);
          dt_ctl_switch_mode_to("darkroom");
        }
      }
    }
    gtk_tree_selection_unselect_all(selection);
    if((d->import_case == DT_IMPORT_INPLACE) || !dt_conf_get_bool("ui_last/import_keep_open"))
    {
      g_free(folder);
      break;
    }
    g_free(folder);
  }
}

static void _import_from_dialog_free(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->from.event = 0;
  g_object_unref(d->from.eye);
  g_object_unref(d->from.store);
  if(d->import_case != DT_IMPORT_CAMERA)
  {
    GtkTreeModel *model = gtk_tree_view_get_model(d->from.folderview);
    g_object_unref(GTK_TREE_STORE(model));
  }
  // Destroy and quit
  gtk_widget_destroy(d->from.dialog);
}

static void _lib_import_from_callback(GtkWidget *widget, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->import_case = (widget == GTK_WIDGET(d->import_inplace)) ? DT_IMPORT_INPLACE : DT_IMPORT_COPY;
  _import_from_dialog_new(self);
  _import_from_dialog_run(self);
  _import_from_dialog_free(self);
}

#ifdef HAVE_GPHOTO2
static void _camera_detected(gpointer instance, gpointer self)
{
  /* update gui with detected devices */
  _lib_import_ui_devices_update(self);
}
#endif
#ifdef USE_LUA
static int lua_register_widget(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  lua_widget widget;
  luaA_to(L,lua_widget,&widget,1);
  dt_lua_widget_bind(L,widget);
  gtk_box_pack_start(GTK_BOX(d->extra_lua_widgets),widget->widget, TRUE, TRUE, 0);
  return 0;
}

void init(dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L,self);
  lua_pushcclosure(L, lua_register_widget,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_widget");
}
#endif

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_import_t *d = (dt_lib_import_t *)g_malloc0(sizeof(dt_lib_import_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  // add import buttons
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *widget = dt_action_button_new(self, N_("add to library..."), _lib_import_from_callback, self,
                                           _("add existing images to the library"), 0, 0);
  d->import_inplace = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);

  widget = dt_action_button_new(self, N_("copy & import..."), _lib_import_from_callback, self,
                                _("copy and optionally rename images before adding them to the library"
                                  "\npatterns can be defined to rename the images and specify the destination folders"),
                                GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  d->import_copy = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

#ifdef HAVE_GPHOTO2
  /* add devices container for cameras */
  d->devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->devices), FALSE, FALSE, 0);

  _lib_import_ui_devices_update(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CAMERA_DETECTED, G_CALLBACK(_camera_detected),
                            self);
#endif

  // collapsible section

  _expander_create(&d->cs, GTK_BOX(self->widget), _("parameters"), "ui_last/expander_import", self);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  guint line = 0;
  d->ignore_exif = dt_gui_preferences_bool(grid, "ui_last/ignore_exif_rating", 0, line++, FALSE);
  d->rating = dt_gui_preferences_int(grid, "ui_last/import_initial_rating", 0, line++);
  d->apply_metadata = dt_gui_preferences_bool(grid, "ui_last/import_apply_metadata", 0, line++, FALSE);
  d->metadata.apply_metadata = d->apply_metadata;
  gtk_box_pack_start(GTK_BOX(d->cs.container), GTK_WIDGET(grid), FALSE, FALSE, 0);
  d->metadata.box = GTK_WIDGET(d->cs.container);
  dt_import_metadata_init(&d->metadata);

#ifdef USE_LUA
  /* initialize the lua area and make sure it survives its parent's destruction*/
  d->extra_lua_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
  g_object_ref_sink(d->extra_lua_widgets);
  gtk_box_pack_start(GTK_BOX(d->cs.container), d->extra_lua_widgets, FALSE, FALSE, 0);
  gtk_container_foreach(GTK_CONTAINER(d->extra_lua_widgets), reset_child, NULL);
#endif

  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  dt_gui_update_collapsible_section(&d->cs);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
#ifdef HAVE_GPHOTO2
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_camera_detected), self);
#endif
#ifdef USE_LUA
  detach_lua_widgets(d->extra_lua_widgets);
#endif
  dt_import_metadata_cleanup(&d->metadata);
  /* cleanup mem */
  g_free(self->data);
  self->data = NULL;
}

const struct
{
  char *key;
  char *name;
  int type;
} _pref[] = {
  {"ui_last/import_ignore_jpegs",       "ignore_jpegs",       DT_BOOL},
  {"ui_last/import_apply_metadata",     "apply_metadata",     DT_BOOL},
  {"ui_last/import_recursive",          "recursive",          DT_BOOL},
  {"ui_last/ignore_exif_rating",        "ignore_exif_rating", DT_BOOL},
  {"session/use_filename",              "use_filename",       DT_BOOL},
  {"session/base_directory_pattern",    "base_pattern",       DT_STRING},
  {"session/sub_directory_pattern",     "sub_pattern",        DT_STRING},
  {"session/filename_pattern",          "filename_pattern",   DT_STRING},
  {"ui_last/import_initial_rating",     "rating",             DT_INT}
};
static const guint pref_n = G_N_ELEMENTS(_pref);

static int _get_key_index(const char *name)
{
  if(!name || !name[0]) return -1;
  for(int i = 0; i < pref_n; i++)
    if(!g_strcmp0(name, _pref[i].name))
      return i;
  return -1;
}

static void _set_default_preferences(dt_lib_module_t *self)
{
  for(int i = 0; i < pref_n; i++)
  {
    switch(_pref[i].type)
    {
      case DT_BOOL:
      {
        const gboolean default_bool = dt_confgen_get_bool(_pref[i].key, DT_DEFAULT);
        dt_conf_set_bool(_pref[i].key, default_bool);
        break;
      }
      case DT_INT:
      {
        const int default_int = dt_confgen_get_int(_pref[i].key, DT_DEFAULT);
        dt_conf_set_int(_pref[i].key, default_int);
        break;
      }
      case DT_STRING:
      {
        const char *default_char = dt_confgen_get(_pref[i].key, DT_DEFAULT);
        dt_conf_set_string(_pref[i].key, default_char);
        break;
      }
    }
  }
  // metadata
  for(int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const char *metadata_name = dt_metadata_get_name(i);
      char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
      const uint32_t flag = (dt_conf_get_int(setting) | DT_METADATA_FLAG_IMPORTED);
      dt_conf_set_int(setting, flag);
      g_free(setting);
      setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
      dt_conf_set_string(setting, "");
      g_free(setting);
    }
  }
  // tags
  {
    dt_conf_set_bool("ui_last/import_last_tags_imported", TRUE);
    dt_conf_set_string("ui_last/import_last_tags", "");
  }
}

static char *_get_current_configuration(dt_lib_module_t *self)
{
  char *pref = NULL;
  for(int i = 0; i < pref_n; i++)
  {
    if(_pref[i].type == DT_BOOL)
    {
      pref = dt_util_dstrcat(pref, "%s=%d,", _pref[i].name, dt_conf_get_bool(_pref[i].key) ? 1 : 0);
    }
    else if(_pref[i].type == DT_INT)
    {
      pref = dt_util_dstrcat(pref, "%s=%d,", _pref[i].name, dt_conf_get_int(_pref[i].key));
    }
    else if(_pref[i].type == DT_STRING)
    {
      const char *s = dt_conf_get_string_const(_pref[i].key);
      pref = dt_util_dstrcat(pref, "%s=%s,", _pref[i].name, s);
    }
  }

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type_by_display_order(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const char *metadata_name = dt_metadata_get_name_by_display_order(i);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag",
                                       metadata_name);
      const gboolean imported = dt_conf_get_int(setting) & DT_METADATA_FLAG_IMPORTED;
      g_free(setting);

      setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
      const char *metadata_value = dt_conf_get_string_const(setting);
      pref = dt_util_dstrcat(pref, "%s=%d%s,", metadata_name, imported ? 1 : 0, metadata_value);
      g_free(setting);
    }
  }
  // must be the last (comma separated list)
  const gboolean imported = dt_conf_get_bool("ui_last/import_last_tags_imported");
  const char *tags_value = dt_conf_get_string_const("ui_last/import_last_tags");
  pref = dt_util_dstrcat(pref, "%s=%d%s,", "tags", imported ? 1 : 0, tags_value);
  if(pref && *pref) pref[strlen(pref) - 1] = '\0';

  return pref;
}

static void _apply_preferences(const char *pref, dt_lib_module_t *self)
{
  if(!pref || !pref[0]) return;
  _set_default_preferences(self);

  // set the presets
  GList *prefs = dt_util_str_to_glist(",", pref);
  for(GList *iter = prefs; iter; iter = g_list_next(iter))
  {
    char *value = g_strstr_len(iter->data, -1, "=");
    if(!value) continue;
    value[0] = '\0';
    value++;
    char *metadata_name = (char *)iter->data;
    const int i = _get_key_index(metadata_name);
    if(i != -1)
    {
      // standard preferences
      if(_pref[i].type == DT_BOOL)
      {
        dt_conf_set_bool(_pref[i].key, (value[0] == '1') ? TRUE : FALSE);
      }
      else if(_pref[i].type == DT_INT)
      {
        dt_conf_set_int(_pref[i].key, (int)atol(value));
      }
      else if(_pref[i].type == DT_STRING)
      {
        dt_conf_set_string(_pref[i].key, value);
      }
    }
    else if(g_strcmp0(metadata_name, "tags"))
    {
      // metadata
      const int j = dt_metadata_get_keyid_by_name(metadata_name);
      if(j == -1) continue;
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", metadata_name);
      const uint32_t flag = (dt_conf_get_int(setting) & ~DT_METADATA_FLAG_IMPORTED) |
                            ((value[0] == '1') ? DT_METADATA_FLAG_IMPORTED : 0);
      dt_conf_set_int(setting, flag);
      g_free(setting);
      value++;
      setting = g_strdup_printf("ui_last/import_last_%s", metadata_name);
      dt_conf_set_string(setting, value);
      g_free(setting);
    }
    else
    {
      // tags
      if(value[0] == '0' || value[0] == '1')
      {
        dt_conf_set_bool("ui_last/import_last_tags_imported", (value[0] == '1'));
        value++;
      }
      else
        dt_conf_set_bool("ui_last/import_last_tags_imported", TRUE);
      // get all the tags back - ugly but allow to keep readable presets
      char *tags = g_strdup(value);
      for(GList *iter2 = g_list_next(iter); iter2; iter2 = g_list_next(iter2))
      {
        if(strlen((char *)iter2->data))
          tags = dt_util_dstrcat(tags, ",%s", (char *)iter2->data);
      }
      dt_conf_set_string("ui_last/import_last_tags", tags);
      g_free(tags);
      break;  // must be the last setting
    }
  }
  g_list_free_full(prefs, g_free);

  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  dt_gui_preferences_bool_update(d->ignore_exif);
  dt_gui_preferences_int_update(d->rating);
  dt_gui_preferences_bool_update(d->apply_metadata);
  dt_import_metadata_update(&d->metadata);
}

void init_presets(dt_lib_module_t *self)
{
}

void *get_params(dt_lib_module_t *self, int *size)
{
  *size = 0;
  char *params = _get_current_configuration(self);
  if(params)
    *size =strlen(params) + 1;
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;
  _apply_preferences(params, self);
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
