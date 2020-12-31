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
#include "common/film.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#ifdef HAVE_GPHOTO2
#include "control/jobs/camera_jobs.h"
#endif
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/import_metadata.h"
#include "gui/preferences_dialogs.h"
#include <gdk/gdkkeysyms.h>
#ifdef HAVE_GPHOTO2
#include "gui/camera_import_dialog.h"
#endif
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
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


typedef struct dt_lib_import_t
{
#ifdef HAVE_GPHOTO2
  dt_camctl_listener_t camctl_listener;
#endif
  GtkWidget *expander;
  GtkButton *import_file;
  GtkButton *import_directory;
  GtkButton *import_camera;
  GtkButton *tethered_shoot;

  GtkBox *devices;
  GtkBox *locked_devices;

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
  dt_accel_register_lib(self, NC_("accel", "import image"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import folder"), GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  dt_accel_connect_button_lib(self, "import image", GTK_WIDGET(d->import_file));
  dt_accel_connect_button_lib(self, "import folder", GTK_WIDGET(d->import_directory));
  if(d->tethered_shoot) dt_accel_connect_button_lib(self, "tethered shoot", GTK_WIDGET(d->tethered_shoot));
  if(d->import_camera) dt_accel_connect_button_lib(self, "import from camera", GTK_WIDGET(d->import_camera));
}

#ifdef HAVE_GPHOTO2

/* show import from camera dialog */
static void _lib_import_from_camera_callback(GtkButton *button, gpointer data)
{
  dt_camera_import_dialog_param_t *params
      = (dt_camera_import_dialog_param_t *)g_malloc0(sizeof(dt_camera_import_dialog_param_t));
  params->camera = (dt_camera_t *)data;

  dt_camera_import_dialog_new(params);
  if(params->result)
  {
    /* initialize a import job and put it on queue.... */
    dt_control_add_job(
        darktable.control, DT_JOB_QUEUE_USER_BG,
        dt_camera_import_job_create(params->jobcode, params->result, params->camera, params->time_override));
  }
  g_free(params->jobcode);
  g_list_free(params->result);
  g_free(params);
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

  GList *citem;

  /* cleanup of widgets in devices container*/
  GList *item, *iter;

  if((iter = item = gtk_container_get_children(GTK_CONTAINER(d->devices))) != NULL)
    do
    {
      gtk_container_remove(GTK_CONTAINER(d->devices), GTK_WIDGET(iter->data));
    }
    while((iter = g_list_next(iter)) != NULL);

  g_list_free(item);

  if((iter = item = gtk_container_get_children(GTK_CONTAINER(d->locked_devices))) != NULL)
    do
    {
      gtk_container_remove(GTK_CONTAINER(d->locked_devices), GTK_WIDGET(iter->data));
    }
    while((iter = g_list_next(iter)) != NULL);

  g_list_free(item);

  dt_camctl_t *camctl = (dt_camctl_t *)darktable.camctl;
  dt_pthread_mutex_lock(&camctl->lock);

  if((citem = g_list_first(camctl->cameras)) != NULL)
  {
    // The label for the section below could be "Mass Storage Camera" from gphoto2
    // let's add a translatable string for it.
    #define FOR_TRANSLATION_MSC N_("Mass Storage Camera")

    // Add detected supported devices
    char buffer[512] = { 0 };
    do
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
        g_signal_connect(G_OBJECT(ib), "clicked", G_CALLBACK(_lib_import_from_camera_callback), camera);
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
    while((citem = g_list_next(citem)) != NULL);
  }

  if((citem = g_list_first(camctl->locked_cameras)) != NULL)
  {
    // Add detected but locked devices
    char buffer[512] = { 0 };
    do
    {
      dt_camera_locked_t *camera = (dt_camera_locked_t *)citem->data;

      snprintf(buffer, sizeof(buffer), "Locked: %s on\n%s", camera->model, camera->port);

      /* add camera label */
      GtkWidget *label = dt_ui_section_label_new(buffer);
      gtk_box_pack_start(GTK_BOX(d->locked_devices), label, FALSE, FALSE, 0);

    }
    while((citem = g_list_next(citem)) != NULL);
  }

  dt_pthread_mutex_unlock(&camctl->lock);
  gtk_widget_show_all(GTK_WIDGET(d->devices));
  gtk_widget_show_all(GTK_WIDGET(d->locked_devices));
}

/** camctl status listener callback */
typedef struct _control_status_params_t
{
  dt_camctl_status_t status;
  dt_lib_module_t *self;
} _control_status_params_t;

static gboolean _camctl_camera_control_status_callback_gui_thread(gpointer user_data)
{
  _control_status_params_t *params = (_control_status_params_t *)user_data;

  dt_lib_import_t *d = (dt_lib_import_t *)params->self->data;

  /* handle camctl status */
  switch(params->status)
  {
    case CAMERA_CONTROL_BUSY:
    {
      /* set all devices as inaccessible */
      GList *list, *child;
      list = child = gtk_container_get_children(GTK_CONTAINER(d->devices));
      if(child)
        do
        {
          if(!(GTK_IS_TOGGLE_BUTTON(child->data)
               && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child->data)) == TRUE))
            gtk_widget_set_sensitive(GTK_WIDGET(child->data), FALSE);
        }
        while((child = g_list_next(child)));

      g_list_free(list);
    }
    break;

    case CAMERA_CONTROL_AVAILABLE:
    {
      /* set all devices as accessible */
      GList *list, *child;
      list = child = gtk_container_get_children(GTK_CONTAINER(d->devices));
      if(child)
        do
        {
          gtk_widget_set_sensitive(GTK_WIDGET(child->data), TRUE);
        }
        while((child = g_list_next(child)));
      g_list_free(list);
    }
    break;
  }

  free(params);
  return FALSE;
}

static void _camctl_camera_control_status_callback(dt_camctl_status_t status, void *data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  _control_status_params_t *params = (_control_status_params_t *)malloc(sizeof(_control_status_params_t));
  if(!params) return;
  params->status = status;
  params->self = self;
  g_main_context_invoke(NULL, _camctl_camera_control_status_callback_gui_thread, params);
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
static void _lib_import_update_preview(GtkFileChooser *file_chooser, gpointer data)
{
  GtkWidget *preview;
  char *filename;
  GdkPixbuf *pixbuf = NULL;
  gboolean have_preview = FALSE, no_preview_fallback = FALSE;

  preview = GTK_WIDGET(data);
  filename = gtk_file_chooser_get_preview_filename(file_chooser);

  if(filename && g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    // don't create dng thumbnails to avoid crashes in libtiff when these are hdr:
    char *c = filename + strlen(filename);
    while(c > filename && *c != '.') c--;
    if(!strcasecmp(c, ".dng")) no_preview_fallback = TRUE;
  }
  else
  {
    no_preview_fallback = TRUE;
  }

  // Step 1: try to check whether the picture contains embedded thumbnail
  // In case it has, we'll use that thumbnail to show on the dialog
  if(!have_preview && !no_preview_fallback)
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
  if(have_preview) gtk_image_set_from_pixbuf(GTK_IMAGE(preview), pixbuf);
  if(pixbuf) g_object_unref(pixbuf);
  g_free(filename);

  gtk_file_chooser_set_preview_widget_active(file_chooser, have_preview);
}

static void _lib_import_single_image_callback(GtkWidget *widget, dt_lib_import_t* d)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("import image"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");
  if(last_directory != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
    g_free(last_directory);
  }

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  for(const char **i = dt_supported_extensions; *i != NULL; i++)
  {
    char *ext = g_strdup_printf("*.%s", *i);
    char *ext_upper = g_ascii_strup(ext, -1);
    gtk_file_filter_add_pattern(filter, ext);
    gtk_file_filter_add_pattern(filter, ext_upper);
    g_free(ext_upper);
    g_free(ext);
  }
  gtk_file_filter_set_name(filter, _("supported images"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  GtkWidget *preview = gtk_image_new();
  gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(filechooser), preview);
  g_signal_connect(filechooser, "update-preview", G_CALLBACK(_lib_import_update_preview), preview);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);

    char *filename = NULL;
    dt_film_t film;
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    GSList *it = list;
    int id = 0;
    int filmid = 0;

    /* reset filter so that view isn't empty */
    dt_view_filter_reset(darktable.view_manager, TRUE);

    while(it)
    {
      filename = (char *)it->data;
      gchar *directory = g_path_get_dirname((const gchar *)filename);
      filmid = dt_film_new(&film, directory);
      id = dt_image_import(filmid, filename, TRUE);
      if(!id) dt_control_log(_("error loading file `%s'"), filename);
      g_free(filename);
      g_free(directory);
      it = g_slist_next(it);
    }

    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
      gboolean loaded = (buf.buf != NULL);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      if(!loaded)
      {
        dt_control_log(_("file has unknown format!"));
      }
      else
      {
        dt_control_set_mouse_over_id(id);
        dt_ctl_switch_mode_to("darkroom");
      }
    }
  }

  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void _lib_import_folder_callback(GtkWidget *widget, dt_lib_module_t* self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("import folder"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");
  if(last_directory != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
    g_free(last_directory);
  }

  // run the dialog
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);

    char *filename = NULL, *first_filename = NULL;
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    GSList *it = list;

    /* reset filter so that view isn't empty */
    dt_view_filter_reset(darktable.view_manager, TRUE);

    /* for each selected folder add import job */
    const gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");
    while(it)
    {
      filename = (char *)it->data;
      dt_film_import(filename);
      if(!first_filename)
      {
        first_filename = g_strdup(filename);
        if(recursive)
          first_filename = dt_util_dstrcat(first_filename, "%%");
      }
      g_free(filename);
      it = g_slist_next(it);
    }

    /* update collection to view import */
    if(first_filename)
    {
      dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
      dt_conf_set_int("plugins/lighttable/collect/item0", 0);
      dt_conf_set_string("plugins/lighttable/collect/string0", first_filename);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, NULL);
      g_free(first_filename);
    }


    g_slist_free(list);
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

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("import settings"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("cancel"), GTK_RESPONSE_NONE,
                                                  _("save"), GTK_RESPONSE_YES, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *grid = dt_prefs_init_dialog_import(dialog);

  GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(area), main_box, TRUE, TRUE, 0);

  dt_import_metadata_t metadata;
  metadata.apply_metadata = NULL;
  // find the widget import_apply_metadata
  int i = 0;
  GtkWidget *b = gtk_grid_get_child_at(GTK_GRID(grid), 2, i) ;
  while(b && !metadata.apply_metadata)
  {
    GList *widgets = gtk_container_get_children(GTK_CONTAINER(b));
    for(GList *widget = widgets; widget; widget = g_list_next(widget))
    {
      GtkWidget *w = GTK_WIDGET(widget->data);
      const char *name = gtk_widget_get_name(w);
      if(!g_strcmp0("ui_last/import_apply_metadata", name))
      {
        metadata.apply_metadata = w;
        break;
      }
    }
    b = gtk_grid_get_child_at(GTK_GRID(grid), 2, ++i) ;
  }

  if(metadata.apply_metadata)
  {
    metadata.box = main_box;
    dt_import_metadata_dialog_new(&metadata);
  }

#ifdef USE_LUA
  gtk_box_pack_start(GTK_BOX(main_box), d->extra_lua_widgets , FALSE, FALSE, 0);
  gtk_container_foreach(GTK_CONTAINER(d->extra_lua_widgets), reset_child, NULL);
#endif

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
//    dt_import_metadata_evaluate(&metadata);
  }
#ifdef USE_LUA
  detach_lua_widgets(d->extra_lua_widgets);
#endif
  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
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

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  /* add import single image buttons */
  GtkWidget *widget = dt_ui_button_new(_("image..."), _("select one or more images to import"), "lighttable_panels.html#import_from_fs");
  d->import_file = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_single_image_callback), d);

  /* adding the import folder button */
  widget = dt_ui_button_new(_("folder..."), _("select a folder to import as film roll"), "lighttable_panels.html#import_from_fs");
  d->import_directory = GTK_BUTTON(widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_folder_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

#ifdef HAVE_GPHOTO2
  /* add devices container for cameras */
  d->devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->devices), FALSE, FALSE, 0);

   /* add devices container for locked cameras */
  d->locked_devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->locked_devices), FALSE, FALSE, 0);

 _lib_import_ui_devices_update(self);

  /* initialize camctl listener and update devices */
  d->camctl_listener.data = self;
  d->camctl_listener.control_status = _camctl_camera_control_status_callback;
  dt_camctl_register_listener(darktable.camctl, &d->camctl_listener);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CAMERA_DETECTED, G_CALLBACK(_camera_detected),
                            self);
#endif
#ifdef USE_LUA
  /* initialize the lua area  and make sure it survives its parent's destruction*/
  d->extra_lua_widgets = gtk_box_new(GTK_ORIENTATION_VERTICAL,5);
  g_object_ref_sink(d->extra_lua_widgets);
#endif
}

void gui_cleanup(dt_lib_module_t *self)
{
#ifdef HAVE_GPHOTO2
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_camera_detected), self);
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  /* unregister camctl listener */
  dt_camctl_unregister_listener(darktable.camctl, &d->camctl_listener);
#endif

  /* cleanup mem */
  g_free(self->data);
  self->data = NULL;
}

static char *_get_current_configuration(dt_lib_module_t *self)
{
  char *pref = NULL;

  pref = dt_util_dstrcat(pref, "ignore_jpeg=%d,", dt_conf_get_bool("ui_last/import_ignore_jpegs") ? 1 : 0);
  pref = dt_util_dstrcat(pref, "apply_metadata=%d,", dt_conf_get_bool("ui_last/import_apply_metadata") ? 1 : 0);
  pref = dt_util_dstrcat(pref, "recursive=%d,", dt_conf_get_bool("ui_last/import_recursive") ? 1 : 0);
  pref = dt_util_dstrcat(pref, "rating=%d,", dt_conf_get_int("ui_last/import_initial_rating"));
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
  char *tags_value = dt_conf_get_string("ui_last/import_last_tags");
  pref = dt_util_dstrcat(pref, "%s=%s,", "tags", tags_value);
  g_free(tags_value);
  if(pref && *pref) pref[strlen(pref) - 1] = '\0';

  return pref;
}

const struct
{
  char *key;
  char *name;
  int type;
} _pref[] = {
  {"ui_last/import_ignore_jpegs", "ignore_jpegs", DT_BOOL},
  {"ui_last/import_apply_metadata", "apply_metadata", DT_BOOL},
  {"ui_last/import_recursive", "recursive", DT_BOOL},
  {"ui_last/import_initial_rating", "rating", DT_INT},
  {"ui_last/import_last_tags", "tags", DT_STRING}
};
static const guint pref_n = G_N_ELEMENTS(_pref);

static void _apply_preferences(const char *pref, dt_lib_module_t *self)
{
  if(!pref || !pref[0]) return;

  // set to default preference
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

  // set the presets
  GList *prefs = dt_util_str_to_glist(",", pref);
  for(GList *iter = prefs; iter; iter = g_list_next(iter))
  {
    char *value = g_strstr_len(iter->data, -1, "=");
    if(!value) continue;
    value[0] = '\0';
    value++;
    char *metadata_name = (char *)iter->data;
    if(g_str_has_prefix(metadata_name, "ignore_jpeg"))
      dt_conf_set_bool("ui_last/import_ignore_jpegs", (value[0] == '1') ? TRUE : FALSE);
    else if (g_str_has_prefix(metadata_name, "apply_metadata"))
      dt_conf_set_bool("ui_last/import_apply_metadata", (value[0] == '1')  ? TRUE : FALSE);
    else if (g_str_has_prefix(metadata_name, "recursive"))
      dt_conf_set_bool("ui_last/import_recursive", (value[0] == '1')  ? TRUE : FALSE);
    else if (g_str_has_prefix(metadata_name, "rating"))
      dt_conf_set_int("ui_last/import_initial_rating", value[0] & 7);
    else if (g_str_has_prefix(metadata_name, "tags"))
    {
      // get all the tags back - ugly but allow to keep readable presets
      char *tags = g_strdup(value);
      for(GList *iter2 = g_list_next(iter); iter2; iter2 = g_list_next(iter2))
      {
        if(strlen((char *)iter2->data))
          tags = dt_util_dstrcat(tags, ",%s", (char *)iter2->data);
      }
      dt_conf_set_string("ui_last/import_last_tags", tags);
      g_free(tags);
      break;
    }
    else
    {
      const int i = dt_metadata_get_keyid_by_name(metadata_name);
      if(i == -1) continue;
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
  }
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
