/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "common/darktable.h"
#include "common/exif.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "gui/accelerators.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "imageio/storage/imageio_storage_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(4)

extern darktable_t darktable;

// options for conflict handling
typedef enum dt_disk_onconflict_actions_t
{
  DT_EXPORT_ONCONFLICT_UNIQUEFILENAME = 0,
  DT_EXPORT_ONCONFLICT_OVERWRITE = 1,
  DT_EXPORT_ONCONFLICT_OVERWRITE_IF_CHANGED = 2,
  DT_EXPORT_ONCONFLICT_SKIP = 3
} dt_disk_onconflict_actions_t;

// gui data
typedef struct disk_t
{
  GtkEntry *entry;
  GtkWidget *onsave_action;
} disk_t;

// saved params
typedef struct dt_imageio_disk_t
{
  char filename[DT_MAX_PATH_FOR_PARAMS];
  dt_disk_onconflict_actions_t onsave_action;
  dt_variables_params_t *vp;
} dt_imageio_disk_t;

const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("file on disk");
}

void *legacy_params(dt_imageio_module_storage_t *self,
                    const void *const old_params,
                    const size_t old_params_size,
                    const int old_version,
                    int *new_version,
                    size_t *new_size)
{
  typedef enum dt_disk_onconflict_actions_v3_t
  {
    DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V3 = 0,
    DT_EXPORT_ONCONFLICT_OVERWRITE_V3 = 1,
    DT_EXPORT_ONCONFLICT_SKIP_V3 = 2,
  } dt_disk_onconflict_actions_v3_t;

  typedef struct dt_imageio_disk_v3_t
  {
    char filename[DT_MAX_PATH_FOR_PARAMS];
    dt_disk_onconflict_actions_v3_t onsave_action;
    dt_variables_params_t *vp;
  } dt_imageio_disk_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_imageio_disk_v1_t
    {
      char filename[1024];
      dt_variables_params_t *vp;
      gboolean overwrite;
    } dt_imageio_disk_v1_t;

    const dt_imageio_disk_v1_t *o = (dt_imageio_disk_v1_t *)old_params;
    dt_imageio_disk_v3_t *n = malloc(sizeof(dt_imageio_disk_v3_t));

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    n->onsave_action = (o->overwrite)
      ? DT_EXPORT_ONCONFLICT_OVERWRITE_V3
      : DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V3;

    *new_version = 3;
    *new_size = sizeof(dt_imageio_disk_v3_t) - sizeof(void *);
    return n;
  }
  if(old_version == 2)
  {
    typedef struct dt_imageio_disk_v2_t
    {
      char filename[DT_MAX_PATH_FOR_PARAMS];
      gboolean overwrite;
      dt_variables_params_t *vp;
    } dt_imageio_disk_v2_t;

    const dt_imageio_disk_v2_t *o = (dt_imageio_disk_v2_t *)old_params;
    dt_imageio_disk_v3_t *n = malloc(sizeof(dt_imageio_disk_v3_t));

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    n->onsave_action = (o->overwrite)
      ? DT_EXPORT_ONCONFLICT_OVERWRITE_V3
      : DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V3;

    *new_version = 3;
    *new_size = sizeof(dt_imageio_disk_v3_t) - sizeof(void *);
    return n;
  }

  // incremental converts

  typedef enum dt_disk_onconflict_actions_v4_t
  {
    DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V4 = 0,
    DT_EXPORT_ONCONFLICT_OVERWRITE_V4 = 1,
    DT_EXPORT_ONCONFLICT_OVERWRITE_IF_CHANGED_V4 = 2,
    DT_EXPORT_ONCONFLICT_SKIP_V4 = 3
  } dt_disk_onconflict_actions_v4_t;

  typedef struct dt_imageio_disk_v4_t
  {
    char filename[DT_MAX_PATH_FOR_PARAMS];
    dt_disk_onconflict_actions_v4_t onsave_action;
    dt_variables_params_t *vp;
  } dt_imageio_disk_v4_t;

  // from 3 to 4 only
  if(old_version == 3)
  {
    const dt_imageio_disk_v3_t *o = (dt_imageio_disk_v3_t *)old_params;
    dt_imageio_disk_v4_t *n = malloc(sizeof(dt_imageio_disk_v4_t));

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    switch(o->onsave_action)
    {
      case DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V3:
        n->onsave_action = DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V4;
        break;
      case DT_EXPORT_ONCONFLICT_OVERWRITE_V3:
        n->onsave_action = DT_EXPORT_ONCONFLICT_OVERWRITE_V4;
        break;
      case DT_EXPORT_ONCONFLICT_SKIP_V3:
        n->onsave_action = DT_EXPORT_ONCONFLICT_SKIP_V4;
        break;
      default:
        n->onsave_action = DT_EXPORT_ONCONFLICT_UNIQUEFILENAME_V4;
    }

    // size of v4 did not change
    *new_size = sizeof(dt_imageio_disk_v4_t) - sizeof(void *);
    *new_version = 4;

    return n;
  }

  return NULL;
}

static void button_clicked(GtkWidget *widget,
                           dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        _("_select as output destination"), _("_cancel"));

  gchar *old = g_strdup(gtk_entry_get_text(d->entry));
  gchar *dirname;
  gchar *filename;
  if (g_file_test(old, G_FILE_TEST_IS_DIR))
  {
    // only a directory was specified, no filename
    // so we use the default $(FILE.NAME) for filename.
    dirname = g_strdup(old);
    filename = g_strdup("$(FILE.NAME)");
  }
  else
  {
    dirname = g_path_get_dirname(old);
    filename = g_path_get_basename(old);
  }

  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), dirname);
  g_free(old);
  g_free(dirname);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *composed = g_build_filename(dir, filename, NULL);

    // composed can now contain '\': on Windows it's the path separator,
    // on other platforms it can be part of a regular folder name.
    // This would later clash with variable substitution, so we have to escape them
    gchar *escaped = dt_util_str_replace(composed, "\\", "\\\\");

    gtk_entry_set_text(GTK_ENTRY(d->entry), escaped);
    // the signal handler will write this to conf
    gtk_editable_set_position(GTK_EDITABLE(d->entry), strlen(escaped));
    g_free(dir);
    g_free(composed);
    g_free(escaped);
  }
  g_free(filename);
  g_object_unref(filechooser);
}

static void entry_changed_callback(GtkEntry *entry,
                                   gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory",
                     gtk_entry_get_text(entry));
}

static void onsave_action_toggle_callback(GtkWidget *widget,
                                          gpointer user_data)
{
  dt_conf_set_int("plugins/imageio/storage/disk/overwrite",
                  dt_bauhaus_combobox_get(widget));
}

void gui_init(dt_imageio_module_storage_t *self)
{
  disk_t *d = malloc(sizeof(disk_t));
  self->gui_data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, FALSE, 0);

  d->entry =
    GTK_ENTRY(dt_action_entry_new
              (DT_ACTION(self), N_("path"), G_CALLBACK(entry_changed_callback), self,
               _("enter the path where to put exported images\nvariables support bash"
                 " like string manipulation\n"
                 "type '$(' to activate the completion and see the list of variables"),
               dt_conf_get_string_const("plugins/imageio/storage/disk/file_directory")));
  dt_gtkentry_setup_completion(d->entry, dt_gtkentry_get_default_path_compl_list());
  gtk_editable_set_position(GTK_EDITABLE(d->entry), -1);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->entry), TRUE, TRUE, 0);

  GtkWidget *widget = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_name(widget, "non-flat");
  gtk_widget_set_tooltip_text(widget, _("select directory"));
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->onsave_action, self, NULL, N_("on conflict"), NULL,
                               dt_conf_get_int("plugins/imageio/storage/disk/overwrite"),
                               onsave_action_toggle_callback, self,
                               N_("create unique filename"),
                               N_("overwrite"),
                               N_("overwrite if changed"),
                               N_("skip"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->onsave_action, TRUE, TRUE, 0);
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  gtk_entry_set_text(d->entry,
                     dt_confgen_get("plugins/imageio/storage/disk/file_directory",
                                    DT_DEFAULT));
  dt_bauhaus_combobox_set(d->onsave_action,
                          dt_confgen_get_int("plugins/imageio/storage/disk/overwrite",
                                             DT_DEFAULT));
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory",
                     gtk_entry_get_text(d->entry));
  dt_conf_set_int("plugins/imageio/storage/disk/overwrite",
                  dt_bauhaus_combobox_get(d->onsave_action));
}

int store(dt_imageio_module_storage_t *self,
          dt_imageio_module_data_t *sdata,
          const dt_imgid_t imgid,
          dt_imageio_module_format_t *format,
          dt_imageio_module_data_t *fdata,
          const int num,
          const int total,
          const gboolean high_quality,
          const gboolean upscale,
          const gboolean export_masks,
          dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename,
          dt_iop_color_intent_t icc_intent,
          dt_export_metadata_t *metadata)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)sdata;

  char filename[PATH_MAX] = { 0 };
  char input_dir[PATH_MAX] = { 0 };
  char pattern[DT_MAX_PATH_FOR_PARAMS];
  g_strlcpy(pattern, d->filename, sizeof(pattern));
  dt_image_full_path(imgid, input_dir, sizeof(input_dir), NULL);
  // set variable values to expand them afterwards in darktable variables
  dt_variables_set_max_width_height(d->vp, fdata->max_width, fdata->max_height);
  dt_variables_set_upscale(d->vp, upscale);

  gboolean fail = FALSE;
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {
try_again:
    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(pattern, "$"))
    {
      snprintf(pattern + strlen(pattern),
               sizeof(pattern) - strlen(pattern), "_$(SEQUENCE)");
    }

    gchar *fixed_path = dt_util_fix_path(pattern);
    g_strlcpy(pattern, fixed_path, sizeof(pattern));
    g_free(fixed_path);

    d->vp->filename = input_dir;
    d->vp->jobcode = "export";
    d->vp->imgid = imgid;
    d->vp->sequence = num;

    gchar *result_filename = dt_variables_expand(d->vp, pattern, TRUE);
    g_strlcpy(filename, result_filename, sizeof(filename));
    g_free(result_filename);

    // if filenamepattern is a directory just add ${FILE_NAME} as
    // default..  this can happen if the filename component of the
    // pattern is an empty variable
    char last_char = *(filename + strlen(filename) - 1);
    if(last_char == '/' || last_char == '\\')
    {
      // add to the end of the original pattern without caring about a
      // potentially added "_$(SEQUENCE)"
      if(snprintf(pattern, sizeof(pattern), "%s"
                  G_DIR_SEPARATOR_S "$(FILE_NAME)", d->filename) < sizeof(pattern))
        goto try_again;
    }

    // get the directory path of the output file
    char *output_dir = g_path_get_dirname(filename);

    // try to create the output directory (including parent
    // directories, if necessary)
    if(g_mkdir_with_parents(output_dir, 0755))
    {
      // output directory could not be created
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_storage_disk] could not create directory: `%s'!",
               output_dir);
      dt_control_log(_("could not create directory `%s'!"), output_dir);
      fail = TRUE;
      goto failed;
    }
    // make sure the outpur directory is writeable
    if(g_access(output_dir, W_OK | X_OK) != 0)
    {
      // output directory is not writeable
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_storage_disk] could not write to directory: `%s'!",
               output_dir);
      dt_control_log(_("could not write to directory `%s'!"), output_dir);
      fail = TRUE;
      goto failed;
    }

    const char *ext = format->extension(fdata);
    char *c = filename + strlen(filename);
    size_t filename_free_space = sizeof(filename) - (c - filename);
    snprintf(c, filename_free_space, ".%s", ext);

  /* prevent overwrite of files */
  failed:
    g_free(output_dir);

    // conflict handling option: unique filename is generated if the
    // file already exists
    if(!fail && d->onsave_action == DT_EXPORT_ONCONFLICT_UNIQUEFILENAME)
    {
      int seq = 1;

      // increase filename suffix until a filename is generated that is unique
      while(g_file_test(filename, G_FILE_TEST_EXISTS))
      {
        snprintf(c, filename_free_space, "_%.2d.%s", seq, ext);
        seq++;
      }
    }

    // conflict handling option: skip
    if(!fail && d->onsave_action == DT_EXPORT_ONCONFLICT_SKIP)
    {
      // check if the file exists
      if(g_file_test(filename, G_FILE_TEST_EXISTS))
      {
        // file exists, skip
        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
        dt_print(DT_DEBUG_ALWAYS, "[export_job] skipping `%s'", filename);
        dt_control_log(ngettext("%d/%d skipping `%s'", "%d/%d skipping `%s'", num),
                       num, total, filename);
        return 0;
      }
    }

    // conflict handling option: overwrite if newer
    if(!fail && d->onsave_action == DT_EXPORT_ONCONFLICT_OVERWRITE_IF_CHANGED)
    {
      // check if the file exists. If not, it will be exported again, regardless
      // of the changes.
      if(g_file_test(filename, G_FILE_TEST_EXISTS))
      {
        // get the image data
        const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
        GTimeSpan change_timestamp = img->change_timestamp;
        GTimeSpan export_timestamp = img->export_timestamp;
        dt_image_cache_read_release(darktable.image_cache, img);

        // check if the export timestamp in the database is more recent than the change
        // date, if yes skip the image
        if(export_timestamp > change_timestamp)
        {
          dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
          dt_print(DT_DEBUG_ALWAYS, "[export_job] skipping (not modified since export) `%s'", filename);
          dt_control_log(ngettext("%d/%d skipping (not modified since export) `%s'",
                                  "%d/%d skipping (not modified since export) `%s'", num),
                         num, total, filename);
          return 0;
        }
      }
    }
  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(fail) return 1;

  /* export image to file */
  if(dt_imageio_export(imgid, filename, format, fdata, high_quality,
                       upscale, TRUE, export_masks, icc_type,
                       icc_filename, icc_intent, self, sdata,
                       num, total, metadata) != 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_storage_disk] could not export to file: `%s'!",
             filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  dt_print(DT_DEBUG_ALWAYS, "[export_job] exported to `%s'", filename);
  dt_control_log(ngettext("%d/%d exported to `%s'", "%d/%d exported to `%s'", num),
                 num, total, filename);
  return 0;
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_disk_t) - sizeof(void *);
}

void init(dt_imageio_module_storage_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self,
                                dt_imageio_disk_t, filename,
                                char_path_length);
#endif
}

void *get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_disk_t *d = calloc(1, sizeof(dt_imageio_disk_t));

  const char *text =
    dt_conf_get_string_const("plugins/imageio/storage/disk/file_directory");
  g_strlcpy(d->filename, text, sizeof(d->filename));

  d->onsave_action = dt_conf_get_int("plugins/imageio/storage/disk/overwrite");

  d->vp = NULL;
  dt_variables_params_init(&d->vp);

  return d;
}

void free_params(dt_imageio_module_storage_t *self,
                 dt_imageio_module_data_t *params)
{
  if(!params) return;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int set_params(dt_imageio_module_storage_t *self,
               const void *params,
               const int size)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  disk_t *g = (disk_t *)self->gui_data;

  if(size != self->params_size(self)) return 1;

  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  gtk_editable_set_position(GTK_EDITABLE(g->entry), strlen(d->filename));
  dt_bauhaus_combobox_set(g->onsave_action, d->onsave_action);
  return 0;
}

char *ask_user_confirmation(dt_imageio_module_storage_t *self)
{
  disk_t *g = (disk_t *)self->gui_data;
  if(dt_bauhaus_combobox_get(g->onsave_action) == DT_EXPORT_ONCONFLICT_OVERWRITE
     && dt_conf_get_bool("plugins/lighttable/export/ask_before_export_overwrite"))
  {
    return g_strdup(_("you are going to export in overwrite mode,"
                      " this will overwrite any existing images\n\n"
                      "do you really want to continue?"));
  }
  else
  {
    return NULL;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
