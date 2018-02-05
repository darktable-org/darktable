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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#ifdef HAVE_GPHOTO2
#include "control/jobs/camera_jobs.h"
#endif
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gdk/gdkkeysyms.h>
#ifdef HAVE_GPHOTO2
#include "gui/camera_import_dialog.h"
#endif
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

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
  GtkButton *import_file;
  GtkButton *import_directory;
  GtkButton *import_camera;
  GtkButton *scan_devices;
  GtkButton *tethered_shoot;

  GtkBox *devices;
#ifdef USE_LUA
  GtkWidget *extra_lua_widgets;
#endif
} dt_lib_import_t;

typedef struct dt_lib_import_metadata_t
{
  GtkWidget *frame;
  GtkWidget *recursive;
  GtkWidget *ignore_jpeg;
  GtkWidget *expander;
  GtkWidget *apply_metadata;
  GtkWidget *presets;
  GtkWidget *creator;
  GtkWidget *publisher;
  GtkWidget *rights;
  GtkWidget *tags;
} dt_lib_import_metadata_t;

enum
{
  NAME_COLUMN,
  CREATOR_COLUMN,
  PUBLISHER_COLUMN,
  RIGHTS_COLUMN,
  N_COLUMNS
};

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
  dt_accel_register_lib(self, NC_("accel", "scan for devices"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import from camera"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tethered shoot"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import image"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "import folder"), GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;

  dt_accel_connect_button_lib(self, "scan for devices", GTK_WIDGET(d->scan_devices));
  dt_accel_connect_button_lib(self, "import image", GTK_WIDGET(d->import_file));
  dt_accel_connect_button_lib(self, "import folder", GTK_WIDGET(d->import_directory));
  if(d->tethered_shoot) dt_accel_connect_button_lib(self, "tethered shoot", GTK_WIDGET(d->tethered_shoot));
  if(d->import_camera) dt_accel_connect_button_lib(self, "import from camera", GTK_WIDGET(d->import_camera));
}

#ifdef HAVE_GPHOTO2

/* scan for new devices button callback */
static void _lib_import_scan_devices_callback(GtkButton *button, gpointer data)
{
  /* detect cameras */
  dt_camctl_detect_cameras(darktable.camctl);
  /* update UI */
  // this part is now asynchronously done by the signal connected to in gui_init()
}

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

  if((iter = item = gtk_container_get_children(GTK_CONTAINER(d->devices))) != NULL) do
    {
      gtk_container_remove(GTK_CONTAINER(d->devices), GTK_WIDGET(iter->data));
    } while((iter = g_list_next(iter)) != NULL);

  g_list_free(item);

  uint32_t count = 0;
  /* FIXME: Verify that it's safe to access camctl->cameras list here ? */
  if((citem = g_list_first(darktable.camctl->cameras)) != NULL)
  {
    // Add detected supported devices
    char buffer[512] = { 0 };
    do
    {
      dt_camera_t *camera = (dt_camera_t *)citem->data;
      count++;

      /* add camera label */
      GtkWidget *label = dt_ui_section_label_new(camera->model);
      gtk_widget_set_margin_top(label, DT_PIXEL_APPLY_DPI(15));
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
      GtkWidget *vbx = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
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
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(ib)), GTK_ALIGN_START);
        dt_gui_add_help_link(ib, "lighttable_panels.html#import_from_camera");
      }
      if(tb)
      {
        g_signal_connect(G_OBJECT(tb), "clicked", G_CALLBACK(_lib_import_tethered_callback), camera);
        gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(tb)), GTK_ALIGN_START);
        dt_gui_add_help_link(tb, "lighttable_panels.html#import_from_camera");
      }
      gtk_box_pack_start(GTK_BOX(d->devices), vbx, FALSE, FALSE, 0);
    } while((citem = g_list_next(citem)) != NULL);
  }

  if(count == 0)
  {
    // No supported devices is detected lets notice user..
    GtkWidget *label = gtk_label_new(_("no supported devices found"));
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    g_object_set(G_OBJECT(label), "xalign", 0.0, (gchar *)0);
    gtk_box_pack_start(GTK_BOX(d->devices), label, TRUE, TRUE, 0);
  }
  gtk_widget_show_all(GTK_WIDGET(d->devices));
}

/** camctl camera disconnect callback */
static gboolean _detect_async(gpointer user_data)
{
  dt_camctl_detect_cameras(darktable.camctl);
  return FALSE;
}

static void _camctl_camera_disconnected_callback(const dt_camera_t *camera, void *data)
{
  /* rescan connected cameras. do that asynchronously since otherwise we deadlock (#10314) */
  g_idle_add(_detect_async, NULL);

  /* update gui with detected devices */
  // this is done asynchronously in _camera_detected()
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
      if(child) do
      {
        if(!(GTK_IS_TOGGLE_BUTTON(child->data)
          && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child->data)) == TRUE))
          gtk_widget_set_sensitive(GTK_WIDGET(child->data), FALSE);
      } while((child = g_list_next(child)));
      g_list_free(list);
    }
    break;

    case CAMERA_CONTROL_AVAILABLE:
    {
      /* set all devices as accessible */
      GList *list, *child;
      list = child = gtk_container_get_children(GTK_CONTAINER(d->devices));
      if(child) do
      {
        gtk_widget_set_sensitive(GTK_WIDGET(child->data), TRUE);
      } while((child = g_list_next(child)));
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

static void _lib_import_metadata_changed(GtkWidget *widget, GtkComboBox *box)
{
  gtk_combo_box_set_active(box, -1);
}

static void _lib_import_apply_metadata_toggled(GtkWidget *widget, gpointer user_data)
{
  GtkWidget *grid = GTK_WIDGET(user_data);
  gtk_widget_set_sensitive(grid, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static void _lib_import_presets_changed(GtkWidget *widget, dt_lib_import_metadata_t *data)
{
  GtkTreeIter iter;

  if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter) == TRUE)
  {
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    GValue value = {
      0,
    };
    gchar *sv;

    gtk_tree_model_get_value(model, &iter, CREATOR_COLUMN, &value);
    if((sv = (gchar *)g_value_get_string(&value)) != NULL && sv[0] != '\0')
    {
      g_signal_handlers_block_by_func(data->creator, _lib_import_metadata_changed, data->presets);
      gtk_entry_set_text(GTK_ENTRY(data->creator), sv);
      g_signal_handlers_unblock_by_func(data->creator, _lib_import_metadata_changed, data->presets);
    }
    g_value_unset(&value);

    gtk_tree_model_get_value(model, &iter, PUBLISHER_COLUMN, &value);
    if((sv = (gchar *)g_value_get_string(&value)) != NULL && sv[0] != '\0')
    {
      g_signal_handlers_block_by_func(data->publisher, _lib_import_metadata_changed, data->presets);
      gtk_entry_set_text(GTK_ENTRY(data->publisher), sv);
      g_signal_handlers_unblock_by_func(data->publisher, _lib_import_metadata_changed, data->presets);
    }
    g_value_unset(&value);

    gtk_tree_model_get_value(model, &iter, RIGHTS_COLUMN, &value);
    if((sv = (gchar *)g_value_get_string(&value)) != NULL && sv[0] != '\0')
    {
      g_signal_handlers_block_by_func(data->rights, _lib_import_metadata_changed, data->presets);
      gtk_entry_set_text(GTK_ENTRY(data->rights), sv);
      g_signal_handlers_unblock_by_func(data->rights, _lib_import_metadata_changed, data->presets);
    }
    g_value_unset(&value);
  }
}

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

static void _check_button_callback(GtkWidget *widget, gpointer data)
{
    dt_conf_set_bool("ui_last/import_ignore_jpegs",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static GtkWidget *_lib_import_get_extra_widget(dt_lib_module_t *self,dt_lib_import_metadata_t *data, gboolean import_folder)
{
  // add extra lines to 'extra'. don't forget to destroy the widgets later.
  GtkWidget *expander = gtk_expander_new(_("import options"));
  gtk_expander_set_expanded(GTK_EXPANDER(expander), dt_conf_get_bool("ui_last/import_options_expanded"));

  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
  gtk_widget_set_hexpand(frame, TRUE);
  GtkWidget *event_box = gtk_event_box_new();

  gtk_widget_set_margin_start(event_box, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(event_box, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(event_box, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(event_box, DT_PIXEL_APPLY_DPI(8));

  gtk_container_add(GTK_CONTAINER(frame), event_box);
  gtk_container_add(GTK_CONTAINER(event_box), expander);

  GtkWidget *extra;
  extra = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(expander), extra);

  gtk_widget_set_margin_start(extra, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_end(extra, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_top(extra, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_set_margin_bottom(extra, DT_PIXEL_APPLY_DPI(8));

  GtkWidget *recursive = NULL, *ignore_jpeg = NULL;
  if(import_folder == TRUE)
  {
    // recursive opening.
    recursive = gtk_check_button_new_with_label(_("import directories recursively"));
    gtk_widget_set_tooltip_text(recursive,
                                _("recursively import subdirectories. each directory goes into a new film roll."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(recursive), dt_conf_get_bool("ui_last/import_recursive"));
    gtk_box_pack_start(GTK_BOX(extra), recursive, FALSE, FALSE, 0);

    // ignoring of jpegs. hack while we don't handle raw+jpeg in the same directories.
    ignore_jpeg = gtk_check_button_new_with_label(_("ignore JPEG files"));
    gtk_widget_set_tooltip_text(ignore_jpeg, _("do not load files with an extension of .jpg or .jpeg. this "
                                               "can be useful when there are raw+JPEG in a directory."));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ignore_jpeg),
                                 dt_conf_get_bool("ui_last/import_ignore_jpegs"));
    gtk_box_pack_start(GTK_BOX(extra), ignore_jpeg, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(ignore_jpeg), "clicked",
                   G_CALLBACK(_check_button_callback), ignore_jpeg);
  }

  // default metadata
  GtkWidget *apply_metadata;
  GtkWidget *grid, *label, *creator, *publisher, *rights, *tags;
  apply_metadata = gtk_check_button_new_with_label(_("apply metadata on import"));
  gtk_widget_set_tooltip_text(apply_metadata, _("apply some metadata to all newly imported images."));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(apply_metadata),
                               dt_conf_get_bool("ui_last/import_apply_metadata"));
  gtk_box_pack_start(GTK_BOX(extra), apply_metadata, FALSE, FALSE, 0);


  GValue value = {
    0,
  };
  g_value_init(&value, G_TYPE_INT);
  gtk_widget_style_get_property(apply_metadata, "indicator-size", &value);
  gint indicator_size = g_value_get_int(&value);
  gtk_widget_style_get_property(apply_metadata, "indicator-spacing", &value);
  gint indicator_spacing = g_value_get_int(&value);
  g_value_unset(&value);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_margin_start(grid,  2 * (indicator_spacing + indicator_size));
  gtk_box_pack_start(GTK_BOX(extra), grid, FALSE, FALSE, 0);

#ifdef USE_LUA
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  gtk_box_pack_start(GTK_BOX(extra),d->extra_lua_widgets , FALSE, FALSE, 0);
  gtk_container_foreach(GTK_CONTAINER(d->extra_lua_widgets),reset_child,NULL);
#endif

  creator = gtk_entry_new();
  gtk_widget_set_size_request(creator, DT_PIXEL_APPLY_DPI(300), -1);
  gchar *str = dt_conf_get_string("ui_last/import_last_creator");
  gtk_entry_set_text(GTK_ENTRY(creator), str);
  g_free(str);

  publisher = gtk_entry_new();
  str = dt_conf_get_string("ui_last/import_last_publisher");
  gtk_entry_set_text(GTK_ENTRY(publisher), str);
  g_free(str);

  rights = gtk_entry_new();
  str = dt_conf_get_string("ui_last/import_last_rights");
  gtk_entry_set_text(GTK_ENTRY(rights), str);
  g_free(str);

  tags = gtk_entry_new();
  str = dt_conf_get_string("ui_last/import_last_tags");
  gtk_widget_set_tooltip_text(tags, _("comma separated list of tags"));
  gtk_entry_set_text(GTK_ENTRY(tags), str);
  g_free(str);

  // presets from the metadata plugin
  GtkCellRenderer *renderer;
  GtkTreeIter iter;
  GtkListStore *model = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING /*name*/, G_TYPE_STRING /*creator*/,
                                           G_TYPE_STRING /*publisher*/, G_TYPE_STRING /*rights*/);

  GtkWidget *presets = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(presets), renderer, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(presets), renderer, "text", NAME_COLUMN, NULL);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, op_params FROM data.presets WHERE operation = \"metadata\"", -1, &stmt,
                              NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    void *op_params = (void *)sqlite3_column_blob(stmt, 1);
    int32_t op_params_size = sqlite3_column_bytes(stmt, 1);

    char *buf = (char *)op_params;
    char *title_str = buf;
    buf += strlen(title_str) + 1;
    char *description_str = buf;
    buf += strlen(description_str) + 1;
    char *rights_str = buf;
    buf += strlen(rights_str) + 1;
    char *creator_str = buf;
    buf += strlen(creator_str) + 1;
    char *publisher_str = buf;

    if(op_params_size
       == strlen(title_str) + strlen(description_str) + strlen(rights_str) + strlen(creator_str)
              + strlen(publisher_str) + 5)
    {
      gtk_list_store_append(model, &iter);
      gtk_list_store_set(model, &iter, NAME_COLUMN, (char *)sqlite3_column_text(stmt, 0), CREATOR_COLUMN,
                         creator_str, PUBLISHER_COLUMN, publisher_str, RIGHTS_COLUMN, rights_str, -1);
    }
  }
  sqlite3_finalize(stmt);

  g_object_unref(model);

  int line = 0;

  label = gtk_label_new(_("preset"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), presets, label, GTK_POS_RIGHT, 1, 1);

  label = gtk_label_new(_("creator"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), creator, label, GTK_POS_RIGHT, 1, 1);

  label = gtk_label_new(_("publisher"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), publisher, label, GTK_POS_RIGHT, 1, 1);

  label = gtk_label_new(_("rights"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), rights, label, GTK_POS_RIGHT, 1, 1);

  label = gtk_label_new(_("tags"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), label, 0, line, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(grid), tags, label, GTK_POS_RIGHT, 1, 1);

  gtk_widget_show_all(frame);

  if(data != NULL)
  {
    data->frame = frame;
    data->recursive = recursive;
    data->ignore_jpeg = ignore_jpeg;
    data->expander = expander;
    data->apply_metadata = apply_metadata;
    data->presets = presets;
    data->creator = creator;
    data->publisher = publisher;
    data->rights = rights;
    data->tags = tags;
  }

  g_signal_connect(apply_metadata, "toggled", G_CALLBACK(_lib_import_apply_metadata_toggled), grid);
  // needed since the apply_metadata starts being turned off, and setting it to off doesn't emit the 'toggled' signal ...
  _lib_import_apply_metadata_toggled(apply_metadata, grid);

  g_signal_connect(presets, "changed", G_CALLBACK(_lib_import_presets_changed), data);
  g_signal_connect(GTK_ENTRY(creator), "changed", G_CALLBACK(_lib_import_metadata_changed), presets);
  g_signal_connect(GTK_ENTRY(publisher), "changed", G_CALLBACK(_lib_import_metadata_changed), presets);
  g_signal_connect(GTK_ENTRY(rights), "changed", G_CALLBACK(_lib_import_metadata_changed), presets);

  return frame;
}

static void _lib_import_evaluate_extra_widget(dt_lib_import_metadata_t *data, gboolean import_folder)
{
  if(import_folder == TRUE)
  {
    dt_conf_set_bool("ui_last/import_recursive",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->recursive)));
    dt_conf_set_bool("ui_last/import_ignore_jpegs",
                     gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->ignore_jpeg)));
  }
  dt_conf_set_bool("ui_last/import_options_expanded", gtk_expander_get_expanded(GTK_EXPANDER(data->expander)));
  dt_conf_set_bool("ui_last/import_apply_metadata",
                   gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(data->apply_metadata)));
  dt_conf_set_string("ui_last/import_last_creator", gtk_entry_get_text(GTK_ENTRY(data->creator)));
  dt_conf_set_string("ui_last/import_last_publisher", gtk_entry_get_text(GTK_ENTRY(data->publisher)));
  dt_conf_set_string("ui_last/import_last_rights", gtk_entry_get_text(GTK_ENTRY(data->rights)));
  dt_conf_set_string("ui_last/import_last_tags", gtk_entry_get_text(GTK_ENTRY(data->tags)));
}

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
    /* load the dt logo as a brackground */
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

static void _lib_import_single_image_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t* self = (dt_lib_module_t*)user_data;
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

  dt_lib_import_metadata_t metadata;
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser),
                                    _lib_import_get_extra_widget(self, &metadata, FALSE));

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);

    _lib_import_evaluate_extra_widget(&metadata, FALSE);

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

#ifdef USE_LUA
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  detach_lua_widgets(d->extra_lua_widgets);
#endif

  gtk_widget_destroy(metadata.frame);
  gtk_widget_destroy(filechooser);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void _lib_import_folder_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t* self= (dt_lib_module_t*) user_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("import film"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
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

  dt_lib_import_metadata_t metadata;
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser),
                                    _lib_import_get_extra_widget(self, &metadata, TRUE));

  // run the dialog
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/import_last_directory", folder);
    g_free(folder);

    _lib_import_evaluate_extra_widget(&metadata, TRUE);

    char *filename = NULL, *first_filename = NULL;
    GSList *list = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(filechooser));
    GSList *it = list;

    /* reset filter so that view isn't empty */
    dt_view_filter_reset(darktable.view_manager, TRUE);

    /* for each selected folder add import job */
    while(it)
    {
      filename = (char *)it->data;
      dt_film_import(filename);
      if(!first_filename)
      {
        first_filename = g_strdup(filename);
        if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(metadata.recursive)))
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
      dt_collection_update_query(darktable.collection);
      g_free(first_filename);
    }


    g_slist_free(list);
  }

#ifdef USE_LUA
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  detach_lua_widgets(d->extra_lua_widgets);
#endif

  gtk_widget_destroy(metadata.frame);
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
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  dt_gui_add_help_link(self->widget, "lighttable_panels.html#import");

  /* add import single image buttons */
  GtkWidget *widget = gtk_button_new_with_label(_("image"));
  dt_gui_add_help_link(widget, "lighttable_panels.html#import_from_fs");
  d->import_file = GTK_BUTTON(widget);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(widget, _("select one or more images to import"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_single_image_callback), self);

  /* adding the import folder button */
  widget = gtk_button_new_with_label(_("folder"));
  dt_gui_add_help_link(widget, "lighttable_panels.html#import_from_fs");
  d->import_directory = GTK_BUTTON(widget);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(widget)), GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(widget, _("select a folder to import as film roll"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(_lib_import_folder_callback), self);

#ifdef HAVE_GPHOTO2
  /* add the rescan button */
  GtkButton *scan = GTK_BUTTON(gtk_button_new_with_label(_("scan for devices")));
  dt_gui_add_help_link(GTK_WIDGET(scan), "lighttable_panels.html#import_from_camera");
  d->scan_devices = scan;
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(scan)), GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(GTK_WIDGET(scan), _("scan for newly attached devices"));
  g_signal_connect(G_OBJECT(scan), "clicked", G_CALLBACK(_lib_import_scan_devices_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(scan), TRUE, TRUE, 0);

  /* add devices container for cameras */
  d->devices = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->devices), FALSE, FALSE, 0);

  _lib_import_ui_devices_update(self);

  /* initialize camctl listener and update devices */
  d->camctl_listener.data = self;
  d->camctl_listener.control_status = _camctl_camera_control_status_callback;
  d->camctl_listener.camera_disconnected = _camctl_camera_disconnected_callback;
  dt_camctl_register_listener(darktable.camctl, &d->camctl_listener);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CAMERA_DETECTED, G_CALLBACK(_camera_detected),
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
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_camera_detected), self);
  dt_lib_import_t *d = (dt_lib_import_t *)self->data;
  /* unregister camctl listener */
  dt_camctl_unregister_listener(darktable.camctl, &d->camctl_listener);
#endif

  /* cleanup mem */
  g_free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
