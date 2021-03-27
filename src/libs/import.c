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
#include "common/debug.h"
#include "common/exif.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#ifdef HAVE_GPHOTO2
#include "control/jobs/camera_jobs.h"
#endif
#include "dtgtk/expander.h"
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

typedef enum dt_import_cols_t
{
  DT_IMPORT_SEL_THUMB = 0,      // active / deactive thumbnails
  DT_IMPORT_THUMB,              // thumbnail
  DT_IMPORT_UI_FILENAME,        // displayed filename
  DT_IMPORT_FILENAME,           // filename
  DT_IMPORT_UI_DATETIME,        // displayed datetime
  DT_IMPORT_DATETIME,           // file datetime
  DT_IMPORT_NUM_COLS
} dt_import_cols_t;

typedef enum dt_import_case_t
{
  DT_IMPORT_INPLACE = 0,
  DT_IMPORT_COPY,
  DT_IMPORT_CAMERA,
  DT_IMPORT_TETHER
} dt_import_case_t;

typedef struct dt_expander_t
{
  GtkWidget *toggle;
  GtkWidget *widgets;
  GtkWidget *expander;
} dt_expander_t;

typedef struct dt_lib_import_t
{
#ifdef HAVE_GPHOTO2
  dt_camera_t *camera;
#endif
  GtkButton *import_inplace;
  GtkButton *import_copy;
  GtkButton *import_camera;
  GtkButton *tethered_shoot;

  GtkWidget *ignore_jpegs, *ignore_exif, *rating, *apply_metadata;
  dt_import_metadata_t metadata;
  GtkBox *devices;
  GtkBox *locked_devices;
  dt_expander_t exp;
  dt_import_case_t import_case;
  struct
  {
    GtkWidget *dialog;
    GtkListStore *store;
    GtkWidget *w;
    GtkTreeView *treeview;
    gchar *folder;
    GtkTreeIter iter;
    int event;
    guint nb;
    GdkPixbuf *eye;
    GtkTreeViewColumn *pixcol;
    GtkWidget *img_nb;
    GtkGrid *patterns;
    GtkWidget *datetime;
    dt_expander_t exp;
    guint fn_line;
    GtkWidget *info;
  } from;

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

int position()
{
  return 999;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "import from camera"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tethered shoot"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import in-place"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "copy and import"), GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  dt_accel_connect_button_lib(self, "import in-place", GTK_WIDGET(d->import_inplace));
  dt_accel_connect_button_lib(self, "copy and import", GTK_WIDGET(d->import_copy));
  if(d->tethered_shoot) dt_accel_connect_button_lib(self, "tethered shoot", GTK_WIDGET(d->tethered_shoot));
  if(d->import_camera) dt_accel_connect_button_lib(self, "import from camera", GTK_WIDGET(d->import_camera));
}

#ifdef HAVE_GPHOTO2

/* show import from camera dialog */
static void _lib_import_from_camera_callback(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->import_case = DT_IMPORT_CAMERA;
  d->from.folder = "";
  _import_from_dialog_new(self);
  _import_from_dialog_run(self);
  _import_from_dialog_free(self);
}

/* enter tethering mode for camera */
static void _lib_import_tethered_callback(GtkToggleButton *button, gpointer data)
{
  /* select camera to work with before switching mode */
  dt_camctl_select_camera(darktable.camctl, (dt_camera_t *)data);
  dt_ctl_switch_mode_to("tethering");
}

/** update the device list */
void _lib_import_ui_devices_update(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  /* cleanup of widgets in devices container*/
  dt_gui_container_remove_children(GTK_CONTAINER(d->devices));
  dt_gui_container_remove_children(GTK_CONTAINER(d->locked_devices));

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
      GtkWidget *ib = NULL, *tb = NULL;
      GtkWidget *vbx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      if(camera->can_import == TRUE)
      {
        gtk_box_pack_start(GTK_BOX(vbx), (ib = gtk_button_new_with_label(_("import from camera"))), FALSE,
                           FALSE, 0);
        gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(ib))), PANGO_ELLIPSIZE_END);
        d->import_camera = GTK_BUTTON(ib);
      }
      if(camera->can_tether == TRUE)
      {
        gtk_box_pack_start(GTK_BOX(vbx), (tb = gtk_button_new_with_label(_("tethered shoot"))), FALSE, FALSE,
                           0);
        d->tethered_shoot = GTK_BUTTON(tb);
      }

      if(ib)
      {
        d->camera = camera;
        g_signal_connect(G_OBJECT(ib), "clicked", G_CALLBACK(_lib_import_from_camera_callback), self);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(ib)), GTK_ALIGN_CENTER);
        dt_gui_add_help_link(ib, "lighttable_panels.html#import_from_camera");
      }
      if(tb)
      {
        g_signal_connect(G_OBJECT(tb), "clicked", G_CALLBACK(_lib_import_tethered_callback), camera);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(tb)), GTK_ALIGN_CENTER);
        dt_gui_add_help_link(tb, "lighttable_panels.html#import_from_camera");
      }
      gtk_box_pack_start(GTK_BOX(d->devices), vbx, FALSE, FALSE, 0);
    }
  }

  citem = camctl->locked_cameras;
  if(citem)
  {
    // Add detected but locked devices
    char buffer[512] = { 0 };
    for(; citem; citem = g_list_next(citem))
    {
      dt_camera_locked_t *camera = (dt_camera_locked_t *)citem->data;

      snprintf(buffer, sizeof(buffer), "Locked: %s on\n%s", camera->model, camera->port);

      /* add camera label */
      GtkWidget *label = dt_ui_section_label_new(buffer);
      gtk_box_pack_start(GTK_BOX(d->locked_devices), label, FALSE, FALSE, 0);

    }
  }

  dt_pthread_mutex_unlock(&camctl->lock);
  gtk_widget_show_all(GTK_WIDGET(d->devices));
  gtk_widget_show_all(GTK_WIDGET(d->locked_devices));
}

static guint _import_from_camera_set_file_list(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GList *imgs = dt_camctl_get_images_list(darktable.camctl, d->camera);
  int nb = 0;
  const gboolean include_jpegs = !dt_conf_get_bool("ui_last/import_ignore_jpegs");
  for(GList *img = imgs; img; img = g_list_next(img))
  {
    const char *ext = g_strrstr((char *)img->data, ".");
    if(include_jpegs || (ext && g_ascii_strncasecmp(ext, ".jpg", sizeof(".jpg"))
                             && g_ascii_strncasecmp(ext, ".jpeg", sizeof(".jpeg"))))
    {
      GtkTreeIter iter;
      gtk_list_store_append(d->from.store, &iter);
      gtk_list_store_set(d->from.store, &iter, DT_IMPORT_UI_FILENAME, img->data,
                                               DT_IMPORT_FILENAME, img->data,
                                               DT_IMPORT_UI_DATETIME, "-",
                                               DT_IMPORT_DATETIME, "-",
                                               DT_IMPORT_THUMB, d->from.eye, -1);
      nb++;
    }
  }
  g_list_free_full(imgs, g_free);
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

// maybe this should be (partly) in common/imageio.[c|h]?
static GdkPixbuf *_import_get_thumbnail(const gchar *filename)
{
  GdkPixbuf *pixbuf = NULL;
  gboolean have_preview = FALSE, no_preview_fallback = FALSE;

  if(filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    // don't create dng thumbnails to avoid crashes in libtiff when these are hdr:
    char *c = (char *)filename + strlen(filename);
    while(c > filename && *c != '.') c--;
    if(!strcasecmp(c, ".dng")) no_preview_fallback = TRUE;
  }
  else
  {
    no_preview_fallback = TRUE;
  }

  // Step 1: try to check whether the picture contains embedded thumbnail
  // In case it has, we'll use that thumbnail to show on the dialog
  if(!no_preview_fallback)
  {
    uint8_t *buffer = NULL;
    size_t size;
    char *mime_type = NULL;
    if(!dt_exif_get_thumbnail(filename, &buffer, &size, &mime_type))
    {
      // Scale the image to the correct size
      GdkPixbuf *tmp;
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
      if (!gdk_pixbuf_loader_write(loader, buffer, size, NULL)) goto cleanup;
      // Calling gdk_pixbuf_loader_close forces the data to be parsed by the
      // loader. We must do this before calling gdk_pixbuf_loader_get_pixbuf.
      if(!gdk_pixbuf_loader_close(loader, NULL)) goto cleanup;
      if (!(tmp = gdk_pixbuf_loader_get_pixbuf(loader))) goto cleanup;
      float ratio = 1.0 * gdk_pixbuf_get_height(tmp) / gdk_pixbuf_get_width(tmp);
      int width = 128, height = 128 * ratio;
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
  // this will not try to read DNG files at all
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
  // or in case of DNG files
  // just display the default darktable logo
  if(!have_preview || no_preview_fallback)
  {
    /* load the dt logo as a background */
    cairo_surface_t *surface = dt_util_get_logo(128.0);
    if(surface)
    {
      guint8 *image_buffer = cairo_image_surface_get_data(surface);
      int image_width = cairo_image_surface_get_width(surface);
      int image_height = cairo_image_surface_get_height(surface);

      pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, image_width, image_height);

      cairo_surface_destroy(surface);
      free(image_buffer);

      have_preview = TRUE;
    }
  }

return pixbuf;
}

static GdkPixbuf *_eye_thumbnail(GtkWidget *widget)
{
  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  const int dim = DT_PIXEL_APPLY_DPI(13);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dim, dim);
  cairo_t *cr = cairo_create(cst);
  gdk_cairo_set_source_rgba(cr, &fg_color);
  dtgtk_cairo_paint_eye(cr, 0, 0, dim, dim, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  cairo_destroy(cr);
  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, dim, dim);
  const size_t size = (size_t)dim * dim * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB, TRUE, 8, dim, dim, dim * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
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
    char *fullname = g_build_filename(d->from.folder, filename, NULL);
    pixbuf = thumb_sel ? _import_get_thumbnail(fullname) : d->from.eye;
    g_free(fullname);
  }
  gtk_list_store_set(d->from.store, iter, DT_IMPORT_SEL_THUMB, thumb_sel,
                                          DT_IMPORT_THUMB, pixbuf, -1);

  if(pixbuf) g_object_ref(pixbuf);
  g_free(filename);
}

static gboolean _thumb_toggled(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
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
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
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
  gboolean thumb_sel = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
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

static guint _import_set_file_list(const gchar *folder, const int root_lgth,
                                   const int n, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GError *error = NULL;
  GFile *gfolder = g_file_parse_name(folder);

  GFileEnumerator *dir_files = g_file_enumerate_children(gfolder,
                                  G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                  G_FILE_ATTRIBUTE_STANDARD_DISPLAY_NAME ","
                                  G_FILE_ATTRIBUTE_TIME_MODIFIED ","
                                  G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                  G_FILE_QUERY_INFO_NONE, NULL, &error);

  GFileInfo *info = NULL;
  guint nb = n;
  const gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");
  const gboolean include_jpegs = !dt_conf_get_bool("ui_last/import_ignore_jpegs");
  while((info = g_file_enumerator_next_file(dir_files, NULL, &error)))
  {
    const char *uifilename = g_file_info_get_display_name(info);
    const char *filename = g_file_info_get_name(info);
    if(!filename)
      continue;
    const guint64 datetime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
    GDateTime *dt_datetime = g_date_time_new_from_unix_local(datetime);
    gchar *dt_txt = g_date_time_format(dt_datetime, "%x %X");
    const GFileType filetype = g_file_info_get_file_type(info);
    gchar *uifullname = g_build_filename(folder, uifilename, NULL);
    gchar *fullname = g_build_filename(folder, filename, NULL);

    if(recursive && filetype == G_FILE_TYPE_DIRECTORY)
    {
      nb = _import_set_file_list(fullname, root_lgth, nb, self);
    }
    // supported image format to import
    else if(filetype != G_FILE_TYPE_DIRECTORY && dt_supported_image(filename))
    {
      const char *ext = g_strrstr(filename, ".");
      if(include_jpegs || (ext && g_ascii_strncasecmp(ext, ".jpg", sizeof(".jpg"))
                               && g_ascii_strncasecmp(ext, ".jpeg", sizeof(".jpeg"))))
      {
        GtkTreeIter iter;
        gtk_list_store_append(d->from.store, &iter);
        gtk_list_store_set(d->from.store, &iter, DT_IMPORT_UI_FILENAME, &uifullname[root_lgth + 1],
                                         DT_IMPORT_FILENAME, &fullname[root_lgth + 1],
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
  if(dir_files)
  {
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
  guint nb_sel = gtk_tree_selection_count_selected_rows(selection);
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

static time_t _parse_date_time(const char *date_time_text)
{
  struct tm t;
  memset(&t, 0, sizeof(t));

  const char *end = NULL;
  if((end = strptime(date_time_text, "%Y-%m-%dT%T", &t)) && *end == 0) return mktime(&t);
  if((end = strptime(date_time_text, "%Y-%m-%d", &t)) && *end == 0) return mktime(&t);

  return 0;
}

static gboolean _update_files_list(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
  g_object_ref(model);
  gtk_tree_view_set_model(d->from.treeview, NULL);
  gtk_list_store_clear(d->from.store);
#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
  {
    d->from.nb = _import_from_camera_set_file_list(self);
    gtk_widget_hide(d->from.info);
  }
  else
#endif
    d->from.nb = _import_set_file_list(d->from.folder, strlen(d->from.folder),
                                       0, self);
  gtk_tree_view_set_model(d->from.treeview, model);
  g_object_unref(model);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
  gtk_tree_selection_select_all(selection);
  gtk_widget_grab_focus(gtk_dialog_get_widget_for_response(GTK_DIALOG(d->from.dialog), GTK_RESPONSE_ACCEPT));
  return FALSE;
}

static void _ignore_jpegs_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  _update_files_list(self);
  dt_gui_preferences_bool_update(d->ignore_jpegs);
}

static void _recursive_toggled(GtkWidget *widget, dt_lib_module_t* self)
{
  _update_files_list(self);
}

static void _expander_update(GtkWidget *toggle, GtkWidget *expander)
{
  const char *key = gtk_widget_get_name(expander);
  const gboolean active = dt_conf_get_bool(key);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), active);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(toggle), dtgtk_cairo_paint_solid_arrow,
                               CPF_STYLE_BOX | (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
}

static void _expander_button_changed(GtkWidget *toggle, GtkWidget *expander)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  const char *key = gtk_widget_get_name(expander);
  dt_conf_set_bool(key, active);
  _expander_update(GTK_WIDGET(toggle), expander);
}

static void _expander_click(GtkWidget *expander, GdkEventButton *e, GtkWidget *toggle)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return;
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), !active);
}

static void _expander_create(dt_expander_t *exp, const char *label,
                             const char *pref_key, const char *css_key)
{
  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *header_evb = gtk_event_box_new();
  GtkStyleContext *context = gtk_widget_get_style_context(destdisp_head);
  gtk_style_context_add_class(context, "section-expander");
  GtkWidget *destdisp = dt_ui_section_label_new(_(label));
  gtk_container_add(GTK_CONTAINER(header_evb), destdisp);

  GtkWidget *toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_STYLE_BOX | CPF_DIRECTION_LEFT, NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), TRUE);
  gtk_widget_set_name(toggle, "control-button");

  gtk_box_pack_start(GTK_BOX(destdisp_head), header_evb, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), toggle, FALSE, FALSE, 0);

  GtkWidget *widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *expander = dtgtk_expander_new(destdisp_head, widgets);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(expander), TRUE);
  GtkWidget *expander_frame = dtgtk_expander_get_frame(DTGTK_EXPANDER(expander));
  if(css_key)
    gtk_widget_set_name(expander_frame, css_key);

  gtk_widget_set_name(expander, pref_key);
  g_signal_connect(G_OBJECT(toggle), "toggled", G_CALLBACK(_expander_button_changed),  (gpointer)expander);
  g_signal_connect(G_OBJECT(header_evb), "button-release-event", G_CALLBACK(_expander_click),
                   (gpointer)toggle);

  exp->toggle = toggle;
  exp->widgets = widgets;
  exp->expander = expander;
}

static void _resize_dialog(GtkWidget *widget, dt_lib_module_t* self)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_conf_set_int("ui_last/import_dialog_width", allocation.width);
  dt_conf_set_int("ui_last/import_dialog_height", allocation.height);
}

static const char *_import_text[] =
{
  N_("import in-place"),
  N_("copy and import"),
  N_("import from camera")
};

static void _import_from_dialog_new(dt_lib_module_t* self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->from.dialog = gtk_dialog_new_with_buttons( _import_text[d->import_case], NULL, GTK_DIALOG_MODAL,
                                                _("cancel"), GTK_RESPONSE_NONE,
                                                _import_text[d->import_case], GTK_RESPONSE_ACCEPT, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->from.dialog);
#endif
  gtk_window_set_default_size(GTK_WINDOW(d->from.dialog),
                              dt_conf_get_int("ui_last/import_dialog_width"),
                              dt_conf_get_int("ui_last/import_dialog_height"));
  gtk_window_set_transient_for(GTK_WINDOW(d->from.dialog), GTK_WINDOW(win));
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d->from.dialog));
  GtkWidget *import_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_signal_connect(d->from.dialog, "check-resize", G_CALLBACK(_resize_dialog), self);

  // images numbers
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->from.dialog));
  GtkWidget *box = GTK_WIDGET(children->data);
  g_list_free(children);
  children = gtk_container_get_children(GTK_CONTAINER(box));
  box = GTK_WIDGET(children->data); // action-box
  g_list_free(children);

  d->from.img_nb = gtk_label_new("");
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(d->from.img_nb), TRUE, TRUE, 0);

  // general info
#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
  {
    d->from.info = dt_ui_label_new(_("please wait while prefetching the list of images from camera..."));
    gtk_label_set_single_line_mode(GTK_LABEL(d->from.info), FALSE);
    gtk_box_pack_start(GTK_BOX(import_list), d->from.info, FALSE, FALSE, 0);
  }
  else
#endif
  {
    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = dt_ui_label_new(_("folder"));
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    label = dt_ui_label_new(d->from.folder);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(import_list), box, FALSE, FALSE, 0);
  }
  guint line = 0;
  guint col = 0;
  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
#ifdef HAVE_GPHOTO2
  if(d->import_case != DT_IMPORT_CAMERA)
#endif
  {
    GtkWidget *recursive = dt_gui_preferences_bool(grid, "ui_last/import_recursive", col++, line, TRUE);
    gtk_widget_set_hexpand(gtk_grid_get_child_at(grid, col++, line), TRUE);
    g_signal_connect(G_OBJECT(recursive), "toggled", G_CALLBACK(_recursive_toggled), self);
  }
  GtkWidget *ignore_jpegs = dt_gui_preferences_bool(grid, "ui_last/import_ignore_jpegs", col++, line, TRUE);
  gtk_widget_set_hexpand(gtk_grid_get_child_at(grid, col++, line++), TRUE);
  g_signal_connect(G_OBJECT(ignore_jpegs), "toggled", G_CALLBACK(_ignore_jpegs_toggled), self);
  gtk_box_pack_start(GTK_BOX(import_list), GTK_WIDGET(grid), FALSE, FALSE, 8);

  d->from.store = gtk_list_store_new(DT_IMPORT_NUM_COLS, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT64);
  // fulfill the store
  d->from.eye = _eye_thumbnail(GTK_WIDGET(d->from.dialog));
  g_timeout_add_full(G_PRIORITY_LOW, 100, _update_files_list, self, NULL);

  // Create the treview with list model data store
  d->from.w = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->from.w), GTK_POLICY_NEVER,
                                 GTK_POLICY_ALWAYS);
  d->from.treeview = GTK_TREE_VIEW(gtk_tree_view_new());
  gtk_container_add(GTK_CONTAINER(d->from.w), GTK_WIDGET(d->from.treeview));

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("name"), renderer, "text",
                                                    DT_IMPORT_UI_FILENAME, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
  gtk_tree_view_column_set_sort_column_id(column, DT_IMPORT_FILENAME);

#ifdef HAVE_GPHOTO2
  if(d->import_case == DT_IMPORT_CAMERA)
  {
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->from.store),
                                         DT_IMPORT_FILENAME, GTK_SORT_ASCENDING);
  }
  else
#endif
  {
    renderer = gtk_cell_renderer_text_new();
    column = gtk_tree_view_column_new_with_attributes(_("modified"), renderer, "text",
                                                      DT_IMPORT_UI_DATETIME, NULL);
    gtk_tree_view_append_column(d->from.treeview, column);
    gtk_tree_view_column_set_sort_column_id(column, DT_IMPORT_DATETIME);
    GtkWidget *header = gtk_tree_view_column_get_button(column);
    gtk_widget_set_tooltip_text(header, _("file 'modified date/time', may be different from 'Exif date/time'"));
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->from.store),
                                         DT_IMPORT_DATETIME, GTK_SORT_ASCENDING);
  }

  renderer = gtk_cell_renderer_pixbuf_new();
  column = gtk_tree_view_column_new_with_attributes("", renderer, "pixbuf",
                                                    DT_IMPORT_THUMB, NULL);
  gtk_tree_view_append_column(d->from.treeview, column);
  GtkWidget *button = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, CPF_STYLE_FLAT, NULL);
  gtk_widget_show(button);
  GtkWidget *header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header, _("show/hide thumbnails"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
  gtk_tree_view_column_set_widget(column, button);
  g_signal_connect(column, "clicked", G_CALLBACK(_all_thumb_toggled), self);
  gtk_tree_view_column_set_alignment(column, 0.5);
  gtk_tree_view_column_set_clickable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(128));
  d->from.pixcol = column;
  g_signal_connect(G_OBJECT(d->from.treeview), "button-press-event",
                   G_CALLBACK(_thumb_toggled), self);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(_import_from_selection_changed), self);

  gtk_tree_view_set_model(d->from.treeview, GTK_TREE_MODEL(d->from.store));
  gtk_tree_view_set_headers_visible(d->from.treeview, TRUE);

  gtk_box_pack_start(GTK_BOX(import_list), GTK_WIDGET(d->from.w), TRUE, TRUE, 0);
  // separator
  gtk_widget_set_name(GTK_WIDGET(d->from.w), "section_label");

  if(d->import_case != DT_IMPORT_INPLACE)
  {
    GtkWidget *import_patterns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
    line = 0;
    dt_gui_preferences_string(grid, "ui_last/import_jobcode", 0, line++);
    gtk_box_pack_start(GTK_BOX(import_patterns), GTK_WIDGET(grid), FALSE, FALSE, 0);

    // collapsible section
    _expander_create(&d->from.exp, _("naming rules"), "ui_last/session_expander_import", "import_metadata");
    gtk_box_pack_start(GTK_BOX(import_patterns), d->from.exp.expander, FALSE, FALSE, 0);

    // patterns
    grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
    d->from.datetime = dt_gui_preferences_string(grid, "ui_last/import_datetime_override", 0, line++);
    dt_gui_preferences_string(grid, "session/base_directory_pattern", 0, line++);
    dt_gui_preferences_string(grid, "session/sub_directory_pattern", 0, line++);
    GtkWidget *usefn = dt_gui_preferences_bool(grid, "session/use_filename", 0, line++, FALSE);
    d->from.fn_line = line;
    dt_gui_preferences_string(grid, "session/filename_pattern", 0, line++);
    gtk_box_pack_start(GTK_BOX(d->from.exp.widgets), GTK_WIDGET(grid), FALSE, FALSE, 0);
    d->from.patterns = grid;
    _update_layout(self);
    g_signal_connect(usefn, "toggled", G_CALLBACK(_usefn_toggled), self);
    gtk_box_pack_start(GTK_BOX(import_list), import_patterns, FALSE, FALSE, 0);

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    grid = GTK_GRID(gtk_grid_new());
    gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
    dt_gui_preferences_bool(grid, "ui_last/import_keep_open", 0, 0, TRUE);
    gtk_box_pack_end(GTK_BOX(box), GTK_WIDGET(grid), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(import_list), GTK_WIDGET(box), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(import_list), TRUE, TRUE, 0);
    gtk_widget_show_all(d->from.dialog);
    _expander_update(d->from.exp.toggle, d->from.exp.expander);
  }
  else
  {
    gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(import_list), TRUE, TRUE, 0);
    gtk_widget_show_all(d->from.dialog);
  }
}

static void _import_set_collection(char *dirname)
{
  if(dirname)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", dirname);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, NULL);
    g_free(dirname);
  }
}

static void _import_from_dialog_run(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  while(gtk_dialog_run(GTK_DIALOG(d->from.dialog)) == GTK_RESPONSE_ACCEPT)
  {
    // reset filter so that view isn't empty
    dt_view_filter_reset(darktable.view_manager, TRUE);
    GList *imgs = NULL;
    GtkTreeModel *model = GTK_TREE_MODEL(d->from.store);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(d->from.treeview);
    GList *paths = gtk_tree_selection_get_selected_rows(selection, &model);
    for(GList *path = paths; path; path = g_list_next(path))
    {
      GtkTreeIter iter;
      gtk_tree_model_get_iter(model, &iter, (GtkTreePath *)path->data);
      char *filename;
      gtk_tree_model_get(model, &iter, DT_IMPORT_FILENAME, &filename, -1);
      char *fullname = g_build_filename(d->from.folder, filename, NULL);
      imgs = g_list_prepend(imgs, fullname);
      g_free(filename);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    if(imgs)
    {
      imgs = g_list_reverse(imgs);
      time_t datetime_override = 0;
      if(d->import_case !=  DT_IMPORT_INPLACE)
      {
        char *dto = g_strdup(gtk_entry_get_text(GTK_ENTRY(d->from.datetime)));
        dto = g_strstrip(dto);
        datetime_override = dto[0] ? _parse_date_time(dto) : 0;
        g_free(dto);
        dt_gui_preferences_string_reset(d->from.datetime);
      }
#ifdef HAVE_GPHOTO2
      if(d->import_case == DT_IMPORT_CAMERA)
        dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG,
                           dt_camera_import_job_create(imgs, d->camera, datetime_override));
      else
#endif
        dt_control_import(imgs, datetime_override, d->import_case == DT_IMPORT_INPLACE);

      if(d->import_case == DT_IMPORT_INPLACE)
        _import_set_collection(g_path_get_dirname((char *)imgs->data));
    }
    gtk_tree_selection_unselect_all(selection);
    if((d->import_case == DT_IMPORT_INPLACE) || !dt_conf_get_bool("ui_last/import_keep_open"))
      break;
  }
}

static void _import_from_dialog_free(dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  d->from.event = 0;
  g_object_unref(d->from.eye);
  gtk_list_store_clear(d->from.store);
  g_object_unref(d->from.store);
  // Destroy and quit
  gtk_widget_destroy(d->from.dialog);
}

static void _lib_import_from_callback(GtkWidget *widget, dt_lib_module_t* self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("open folder"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
  g_free(last_directory);

  // run the dialog
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gtk_widget_hide(filechooser);
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    d->from.folder = (char *)list->data;
    g_slist_free(list);

    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);

    d->import_case = (widget == GTK_WIDGET(d->import_inplace)) ? DT_IMPORT_INPLACE : DT_IMPORT_COPY;
    _import_from_dialog_new(self);
    _import_from_dialog_run(self);
    _import_from_dialog_free(self);

    g_free(folder);
  }

  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
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
  dt_gui_add_help_link(self->widget, "lighttable_panels.html#import");

  // add import buttons
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *widget = dt_ui_button_new(_("import in-place..."), _("select a folder to import from"), "lighttable_panels.html#import_from_fs");
  d->import_inplace = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_from_callback), self);
  widget = dt_ui_button_new(_("copy and import..."), _("select a folder to import from"), "lighttable_panels.html#import_from_fs");
  d->import_copy = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_from_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

#ifdef HAVE_GPHOTO2
  /* add devices container for cameras */
  d->devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->devices), FALSE, FALSE, 0);

   /* add devices container for locked cameras */
  d->locked_devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->locked_devices), FALSE, FALSE, 0);

  _lib_import_ui_devices_update(self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CAMERA_DETECTED, G_CALLBACK(_camera_detected),
                            self);
#endif

  // collapsible section
  _expander_create(&d->exp, _("parameters"), "ui_last/expander_import", "import_metadata");
  gtk_box_pack_start(GTK_BOX(self->widget), d->exp.expander, FALSE, FALSE, 0);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  guint line = 0;
  d->ignore_jpegs = dt_gui_preferences_bool(grid, "ui_last/import_ignore_jpegs", 0, line++, FALSE);
  d->ignore_exif = dt_gui_preferences_bool(grid, "ui_last/ignore_exif_rating", 0, line++, FALSE);
  d->rating = dt_gui_preferences_int(grid, "ui_last/import_initial_rating", 0, line++);
  d->apply_metadata = dt_gui_preferences_bool(grid, "ui_last/import_apply_metadata", 0, line++, FALSE);
  d->metadata.apply_metadata = d->apply_metadata;
  gtk_box_pack_start(GTK_BOX(d->exp.widgets), GTK_WIDGET(grid), FALSE, FALSE, 0);
  d->metadata.box = d->exp.widgets;
  dt_import_metadata_init(&d->metadata);

#ifdef USE_LUA
  /* initialize the lua area  and make sure it survives its parent's destruction*/
  d->extra_lua_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
  g_object_ref_sink(d->extra_lua_widgets);
  gtk_box_pack_start(GTK_BOX(d->exp.widgets), d->extra_lua_widgets, FALSE, FALSE, 0);
  gtk_container_foreach(GTK_CONTAINER(d->extra_lua_widgets), reset_child, NULL);
#endif

  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);
  _expander_update(d->exp.toggle, d->exp.expander);
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
      char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", metadata_name);
      const uint32_t flag = (dt_conf_get_int(setting) | DT_METADATA_FLAG_IMPORTED);
      dt_conf_set_int(setting, flag);
      g_free(setting);
      setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", metadata_name);
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
      char *s = dt_conf_get_string(_pref[i].key);
      pref = dt_util_dstrcat(pref, "%s=%s,", _pref[i].name, s);
      g_free(s);
    }
  }

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type_by_display_order(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const char *metadata_name = dt_metadata_get_name_by_display_order(i);
      char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag",
                                      metadata_name);
      const gboolean imported = dt_conf_get_int(setting) & DT_METADATA_FLAG_IMPORTED;
      g_free(setting);

      setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", metadata_name);
      char *metadata_value = dt_conf_get_string(setting);
      pref = dt_util_dstrcat(pref, "%s=%d%s,", metadata_name, imported ? 1 : 0, metadata_value);
      g_free(setting);
      g_free(metadata_value);
    }
  }
  // must be the last (comma separated list)
  const gboolean imported = dt_conf_get_bool("ui_last/import_last_tags_imported");
  char *tags_value = dt_conf_get_string("ui_last/import_last_tags");
  pref = dt_util_dstrcat(pref, "%s=%d%s,", "tags", imported ? 1 : 0, tags_value);
  g_free(tags_value);
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
      char *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", metadata_name);
      const uint32_t flag = (dt_conf_get_int(setting) & ~DT_METADATA_FLAG_IMPORTED) |
                            ((value[0] == '1') ? DT_METADATA_FLAG_IMPORTED : 0);
      dt_conf_set_int(setting, flag);
      g_free(setting);
      value++;
      setting = dt_util_dstrcat(NULL, "ui_last/import_last_%s", metadata_name);
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
  dt_gui_preferences_bool_update(d->ignore_jpegs);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
