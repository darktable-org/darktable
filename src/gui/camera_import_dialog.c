/*
    This file is part of darktable,
    copyright (c) 2010--2014 henrik andersson.

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

#include "gui/camera_import_dialog.h"
#include "common/camera_control.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <time.h>

#ifdef _WIN32
//MSVCRT does not have strptime implemented
#include "win/strptime.h"
#endif
/*

  g_object_ref(model); // Make sure the model stays with us after the tree view unrefs it

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL); // Detach model from view

  ... insert a couple of thousand rows ...

  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model); // Re-attach model to view

  g_object_unref(model);

*/


typedef struct _camera_gconf_widget_t
{
  GtkWidget *widget;
  GtkWidget *entry;
  gchar *value;
  struct _camera_import_dialog_t *dialog;
} _camera_gconf_widget_t;

typedef struct _camera_import_dialog_t
{
  GtkWidget *dialog;

  GtkWidget *notebook;

  struct
  {
    GtkWidget *page;
    _camera_gconf_widget_t *jobname;
    GtkWidget *treeview;
    GtkWidget *info;
  } import;

  struct
  {
    GtkWidget *page;

    /** Group of general import settings */
    struct
    {
      GtkWidget *ignore_jpeg;
      GtkWidget *date_override;
      GtkWidget *date_entry;
    } general;

  } settings;

  GtkListStore *store;
  dt_job_t *preview_job;
  dt_camera_import_dialog_param_t *params;
} _camera_import_dialog_t;


static void _check_button_callback(GtkWidget *cb, gpointer user_data)
{

  _camera_import_dialog_t *cid = (_camera_import_dialog_t *)user_data;

  if(cb == cid->settings.general.ignore_jpeg)
  {
    dt_conf_set_bool("ui_last/import_ignore_jpegs",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cid->settings.general.ignore_jpeg)));
  }
  else if(cb == cid->settings.general.date_override)
  {
    // Enable/disable the date entry widget
    gtk_widget_set_sensitive(cid->settings.general.date_entry, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
                                                                   cid->settings.general.date_override)));
  }
}

static void _gcw_store_callback(GtkDarktableButton *button, gpointer user_data)
{
  _camera_gconf_widget_t *gcw = (_camera_gconf_widget_t *)user_data;
  gchar *configstring = g_object_get_data(G_OBJECT(gcw->widget), "gconf:string");
  const gchar *newvalue = gtk_entry_get_text(GTK_ENTRY(gcw->entry));
  if(newvalue && *newvalue)
  {
    dt_conf_set_string(configstring, newvalue);
    g_free(gcw->value);
    gcw->value = g_strdup(newvalue);
  }
}

static void _gcw_reset_callback(GtkDarktableButton *button, gpointer user_data)
{
  _camera_gconf_widget_t *gcw = (_camera_gconf_widget_t *)user_data;
  gchar *configstring = g_object_get_data(G_OBJECT(gcw->widget), "gconf:string");
  gchar *value = dt_conf_get_string(configstring);
  if(value)
  {
    gtk_entry_set_text(GTK_ENTRY(gcw->entry), value);
    g_free(gcw->value);
    gcw->value = value;
  }
}


static void _entry_text_changed(_camera_gconf_widget_t *gcw, GtkEntryBuffer *entrybuffer)
{
  const gchar *value = gtk_entry_buffer_get_text(entrybuffer);
  g_free(gcw->value);
  gcw->value = g_strdup(value);
}

static void entry_dt_callback(GtkEntryBuffer *entrybuffer, guint a1, guint a2, gpointer user_data)
{
  _entry_text_changed((_camera_gconf_widget_t *)user_data, entrybuffer);
}

static void entry_it_callback(GtkEntryBuffer *entrybuffer, guint a1, gchar *a2, guint a3, gpointer user_data)
{
  _entry_text_changed((_camera_gconf_widget_t *)user_data, entrybuffer);
}

/** Creates a gconf widget. */
static _camera_gconf_widget_t *_camera_import_gconf_widget(_camera_import_dialog_t *dlg, gchar *label,
                                                           gchar *confstring)
{
  _camera_gconf_widget_t *gcw = calloc(1, sizeof(_camera_gconf_widget_t));
  GtkWidget *vbox, *hbox;
  gcw->widget = vbox = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  hbox = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  g_object_set_data(G_OBJECT(vbox), "gconf:string", confstring);
  gcw->dialog = dlg;

  gcw->entry = gtk_entry_new();
  char *value = dt_conf_get_string(confstring);
  if(value)
  {
    gtk_entry_set_text(GTK_ENTRY(gcw->entry), value);
    g_free(gcw->value);
    gcw->value = value;
  }

  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(gcw->entry), TRUE, TRUE, 0);

  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_store, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(button, _("store value as default"));
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(13), DT_PIXEL_APPLY_DPI(13));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_gcw_store_callback), gcw);

  button = dtgtk_button_new(dtgtk_cairo_paint_reset, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(button, _("reset value to default"));
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(13), DT_PIXEL_APPLY_DPI(13));
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_gcw_reset_callback), gcw);

  GtkWidget *l = gtk_label_new(label);
  gtk_widget_set_halign(l, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vbox), l, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(gtk_entry_get_buffer(GTK_ENTRY(gcw->entry))), "inserted-text",
                   G_CALLBACK(entry_it_callback), gcw);
  g_signal_connect(G_OBJECT(gtk_entry_get_buffer(GTK_ENTRY(gcw->entry))), "deleted-text",
                   G_CALLBACK(entry_dt_callback), gcw);

  return gcw;
}



static void _camera_import_dialog_new(_camera_import_dialog_t *data)
{
  data->dialog = gtk_dialog_new_with_buttons(_("import images from camera"), NULL, GTK_DIALOG_MODAL,
                                             _("cancel"), GTK_RESPONSE_NONE, C_("camera import", "import"),
                                             GTK_RESPONSE_ACCEPT, NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(data->dialog);
#endif
  gtk_window_set_default_size(GTK_WINDOW(data->dialog), 100, 600);
  gtk_window_set_transient_for(GTK_WINDOW(data->dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(data->dialog));

  // List - setup store
  data->store = gtk_list_store_new(2, GDK_TYPE_PIXBUF, G_TYPE_STRING);

  // IMPORT PAGE
  data->import.page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_container_set_border_width(GTK_CONTAINER(data->import.page), 5);

  // Top info
  data->import.info = gtk_label_new(_("please wait while prefetching thumbnails of images from camera..."));
  gtk_label_set_single_line_mode(GTK_LABEL(data->import.info), FALSE);
  gtk_widget_set_halign(data->import.info, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(data->import.page), data->import.info, FALSE, FALSE, 0);

  // jobcode
  data->import.jobname
      = _camera_import_gconf_widget(data, _("jobcode"), "plugins/capture/camera/import/jobcode");
  gtk_box_pack_start(GTK_BOX(data->import.page), GTK_WIDGET(data->import.jobname->widget), FALSE, FALSE, 0);


  // Create the treview with list model data store
  data->import.treeview = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(data->import.treeview), GTK_POLICY_NEVER,
                                 GTK_POLICY_ALWAYS);

  gtk_container_add(GTK_CONTAINER(data->import.treeview), gtk_tree_view_new());
  GtkTreeView *treeview = GTK_TREE_VIEW(gtk_bin_get_child(GTK_BIN(data->import.treeview)));

  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
  GtkTreeViewColumn *column
      = gtk_tree_view_column_new_with_attributes(_("thumbnail"), renderer, "pixbuf", 0, (char *)NULL);
  gtk_tree_view_append_column(treeview, column);

  renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("storage file"), renderer, "text", 1, (char *)NULL);
  gtk_tree_view_append_column(treeview, column);
  gtk_tree_view_column_set_expand(column, TRUE);


  GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

  gtk_tree_view_set_model(treeview, GTK_TREE_MODEL(data->store));
  gtk_tree_view_set_headers_visible(treeview, FALSE);

  gtk_box_pack_start(GTK_BOX(data->import.page), data->import.treeview, TRUE, TRUE, 0);


  // SETTINGS PAGE
  data->settings.page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_container_set_border_width(GTK_CONTAINER(data->settings.page), 5);

  // general settings
  gtk_box_pack_start(GTK_BOX(data->settings.page), gtk_label_new(_("general")), FALSE, FALSE, 0);

  // ignoring of jpegs. hack while we don't handle raw+jpeg in the same directories.
  data->settings.general.ignore_jpeg = gtk_check_button_new_with_label(_("ignore JPEG files"));
  gtk_widget_set_tooltip_text(data->settings.general.ignore_jpeg,
               _("do not load files with an extension of .jpg or .jpeg. this can be useful when there are "
                 "raw+JPEG in a directory."));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(data->settings.general.ignore_jpeg),
                               dt_conf_get_bool("ui_last/import_ignore_jpegs"));
  gtk_box_pack_start(GTK_BOX(data->settings.page), data->settings.general.ignore_jpeg, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(data->settings.general.ignore_jpeg), "clicked",
                   G_CALLBACK(_check_button_callback), data);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  data->settings.general.date_override = gtk_check_button_new_with_label(_("override today's date"));
  gtk_box_pack_start(GTK_BOX(hbox), data->settings.general.date_override, FALSE, FALSE, 0);
  gtk_widget_set_tooltip_text(data->settings.general.date_override,
               _("check this, if you want to override the timestamp used when expanding variables:\n$(YEAR), "
                 "$(MONTH), $(DAY),\n$(HOUR), $(MINUTE), $(SECONDS)"));

  data->settings.general.date_entry = gtk_entry_new();
  gtk_widget_set_sensitive(data->settings.general.date_entry, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(
                                                                  data->settings.general.date_override)));
  gtk_box_pack_start(GTK_BOX(hbox), data->settings.general.date_entry, TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(data->settings.general.date_override), "clicked",
                   G_CALLBACK(_check_button_callback), data);

  gtk_box_pack_start(GTK_BOX(data->settings.page), hbox, FALSE, FALSE, 0);


  // THE NOTEBOOK
  data->notebook = gtk_notebook_new();
  gtk_notebook_append_page(GTK_NOTEBOOK(data->notebook), data->import.page, gtk_label_new(_("images")));
  gtk_notebook_append_page(GTK_NOTEBOOK(data->notebook), data->settings.page, gtk_label_new(_("settings")));

  // end
  gtk_box_pack_start(GTK_BOX(content), data->notebook, TRUE, TRUE, 0);
  // gtk_widget_set_size_request(content, DT_PIXEL_APPLY_DPI(400), DT_PIXEL_APPLY_DPI(400));
}

typedef struct _image_filename_t
{
  char *file_info;
  GdkPixbuf *thumb;
  GtkListStore *store;
} _image_filename_t;

static gboolean _camera_storage_image_filename_gui_thread(gpointer user_data)
{
  _image_filename_t *params = (_image_filename_t *)user_data;

  GtkTreeIter iter;

  gtk_list_store_append(params->store, &iter);
  gtk_list_store_set(params->store, &iter, 0, params->thumb, 1, params->file_info, -1);

  if(params->thumb) g_object_ref(params->thumb);
  free(params->file_info);
  free(params);
  return FALSE;
}

static int _camera_storage_image_filename(const dt_camera_t *camera, const char *filename,
                                          CameraFile *preview, CameraFile *exif, void *user_data)
{
  _camera_import_dialog_t *data = (_camera_import_dialog_t *)user_data;
  const char *img;
  unsigned long size;
  GdkPixbuf *pixbuf = NULL;
  GdkPixbuf *thumb = NULL;

  /* stop fetching previews if job is cancelled */
  if(data->preview_job && dt_control_job_get_state(data->preview_job) == DT_JOB_STATE_CANCELLED) return 0;

  char exif_info[1024] = { 0 };

  if(preview)
  {
    gp_file_get_data_and_size(preview, &img, &size);
    if(size > 0)
    {
      // we got preview image data lets create a pixbuf from image blob
      GError *err = NULL;
      GInputStream *stream;
      if((stream = g_memory_input_stream_new_from_data(img, size, NULL)) != NULL)
        pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, &err);
    }

    if(pixbuf)
    {
      // Scale pixbuf to a thumbnail
      double sw = gdk_pixbuf_get_width(pixbuf);
      double scale = 75.0 / gdk_pixbuf_get_height(pixbuf);
      thumb = gdk_pixbuf_scale_simple(pixbuf, sw * scale, 75, GDK_INTERP_BILINEAR);
    }
  }

#if 0
  // libgphoto only supports fetching exif in jpegs, not raw
  char buffer[1024]= {0};
  if ( exif )
  {
    const char *exif_data;
    char *value=NULL;
    gp_file_get_data_and_size(exif, &exif_data, &size);
    if( size > 0 )
    {
      void *exif=dt_exif_data_new((uint8_t *)exif_data,size);
      if( (value=g_strdup( dt_exif_data_get_value(exif,"Exif.Photo.ExposureTime",buffer,1024) ) ) != NULL);
      snprintf(exif_info, sizeof(exif_info), "exposure: %s\n", value);
    }
    else fprintf(stderr,"No exifdata read\n");
  }
#endif

  _image_filename_t *params = (_image_filename_t *)malloc(sizeof(_image_filename_t));
  if(!params)
  {
    if(pixbuf) g_object_unref(pixbuf);
    if(thumb) g_object_unref(thumb);
    return 0;
  }

  // filename\n 1/60 f/2.8 24mm iso 160
  params->file_info = g_strdup_printf("%s%c%s", filename, *exif_info ? '\n' : '\0',
                                      *exif_info ? exif_info : "");
  params->thumb = thumb;
  params->store = data->store;
  g_main_context_invoke(NULL, _camera_storage_image_filename_gui_thread, params);

  if(pixbuf) g_object_unref(pixbuf);

  return 1;
}

static void _camera_import_dialog_free(_camera_import_dialog_t *data)
{
  gtk_list_store_clear(data->store);
  g_object_unref(data->store);
}

static void _control_status(dt_camctl_status_t status, void *user_data)
{
  _camera_import_dialog_t *data = (_camera_import_dialog_t *)user_data;
  switch(status)
  {
    case CAMERA_CONTROL_BUSY:
    {
      gtk_dialog_set_response_sensitive(GTK_DIALOG(data->dialog), GTK_RESPONSE_ACCEPT, FALSE);
      gtk_dialog_set_response_sensitive(GTK_DIALOG(data->dialog), GTK_RESPONSE_NONE, FALSE);
    }
    break;
    case CAMERA_CONTROL_AVAILABLE:
    {
      gtk_dialog_set_response_sensitive(GTK_DIALOG(data->dialog), GTK_RESPONSE_ACCEPT, TRUE);
      gtk_dialog_set_response_sensitive(GTK_DIALOG(data->dialog), GTK_RESPONSE_NONE, TRUE);
    }
    break;
  }
}

static void _preview_job_state_changed(dt_job_t *job, dt_job_state_t state)
{
  _camera_import_dialog_t *data = dt_camera_previews_job_get_data(job);
  /* store job reference if needed for cancellation */
  if(state == DT_JOB_STATE_RUNNING)
    data->preview_job = job;
  else if(state == DT_JOB_STATE_FINISHED)
    data->preview_job = NULL;
}

static gboolean _dialog_close(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  _camera_import_dialog_t *data = (_camera_import_dialog_t *)user_data;

  if(data->preview_job)
  {
    /* cancel preview fetching job */
    dt_control_job_cancel(data->preview_job);

    /* wait for job execution to signal finished */
    dt_control_job_wait(data->preview_job);
  }
  return FALSE;
}

static time_t parse_date_time(const char *date_time_text)
{
  struct tm t;
  memset(&t, 0, sizeof(t));

  const char *end = NULL;
  if((end = strptime(date_time_text, "%Y-%m-%dT%T", &t)) && *end == 0) return mktime(&t);
  if((end = strptime(date_time_text, "%Y-%m-%d", &t)) && *end == 0) return mktime(&t);

  return 0;
}

static void _camera_import_dialog_run(_camera_import_dialog_t *data)
{
  gtk_widget_show_all(data->dialog);

  // Populate store

  // Setup a listener for previews of all files on camera
  // then initiate fetch of all previews from camera
  if(data->params->camera != NULL)
  {
    /* setup a camctl listener */
    dt_camctl_listener_t listener = { 0 };
    listener.data = data;
    listener.control_status = _control_status;
    listener.camera_storage_image_filename = _camera_storage_image_filename;

    dt_job_t *job
        = dt_camera_get_previews_job_create(data->params->camera, &listener, CAMCTL_IMAGE_PREVIEW_DATA, data);
    if(job)
    {
      dt_control_job_set_state_callback(job, _preview_job_state_changed);
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_SYSTEM_FG, job);
    }
  }
  else
    return;

  // Lets run dialog
  gtk_label_set_text(GTK_LABEL(data->import.info),
                     _("select the images from the list below that you want to import into a new filmroll"));
  gboolean all_good = FALSE;
  g_signal_connect(G_OBJECT(data->dialog), "delete-event", G_CALLBACK(_dialog_close), data);
  while(!all_good)
  {
    gint result = gtk_dialog_run(GTK_DIALOG(data->dialog));
    if(result == GTK_RESPONSE_ACCEPT)
    {
      GtkTreeIter iter;
      all_good = TRUE;
      GtkTreeSelection *selection
          = gtk_tree_view_get_selection(GTK_TREE_VIEW(gtk_bin_get_child(GTK_BIN(data->import.treeview))));
      // Now build up result from store into GList **result
      g_list_free(data->params->result);
      data->params->result = NULL;
      GtkTreeModel *model = GTK_TREE_MODEL(data->store);
      GList *sp = gtk_tree_selection_get_selected_rows(selection, &model);
      if(sp)
      {
        do
        {
          GValue value = {
            0,
          };
          gtk_tree_model_get_iter(GTK_TREE_MODEL(data->store), &iter, (GtkTreePath *)sp->data);
          gtk_tree_model_get_value(GTK_TREE_MODEL(data->store), &iter, 1, &value);
          if(G_VALUE_HOLDS_STRING(&value))
            data->params->result = g_list_append(data->params->result, g_strdup(g_value_get_string(&value)));
          g_value_unset(&value);
        } while((sp = g_list_next(sp)));
      }

      /* get jobcode from import dialog */
      data->params->jobcode = data->import.jobname->value;

      /* get time override if used */
      data->params->time_override = 0;
      if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->settings.general.date_override)))
        data->params->time_override
            = parse_date_time(gtk_entry_get_text(GTK_ENTRY(data->settings.general.date_entry)));

      if(data->params->jobcode == NULL || data->params->jobcode[0] == '\0')
      {
        g_free(data->params->jobcode); // might just be a string of length 0
        data->params->jobcode = dt_conf_get_string("plugins/capture/camera/import/jobcode");
      }
      else if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->settings.general.date_override))
              && data->params->time_override == 0)
      {
        GtkWidget *dialog
            = gtk_message_dialog_new(NULL, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                     _("please use YYYY-MM-DD format for date override"));
#ifdef GDK_WINDOWING_QUARTZ
        dt_osx_disallow_fullscreen(dialog);
#endif
        g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_dialog_run(GTK_DIALOG(dialog));
        all_good = FALSE;
      }
    }
    else
    {
      data->params->result = NULL;
      all_good = TRUE;
    }
  }

  // Destroy and quit
  gtk_widget_destroy(data->dialog);
}

void dt_camera_import_dialog_new(dt_camera_import_dialog_param_t *params)
{
  _camera_import_dialog_t data;
  memset(&data, 0, sizeof(_camera_import_dialog_t)); // needed to initialize pointers to null
  data.params = params;
  _camera_import_dialog_new(&data);
  _camera_import_dialog_run(&data);
  _camera_import_dialog_free(&data);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
