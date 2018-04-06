/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "imageio/storage/imageio_storage_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(2)

// gui data
typedef struct disk_t
{
  GtkEntry *entry;
  GtkWidget *overwrite;
} disk_t;

// saved params
typedef struct dt_imageio_disk_t
{
  char filename[DT_MAX_PATH_FOR_PARAMS];
  gboolean overwrite;
  dt_variables_params_t *vp;
} dt_imageio_disk_t;


const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("file on disk");
}

void *legacy_params(dt_imageio_module_storage_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_imageio_disk_v1_t
    {
      char filename[1024];
      dt_variables_params_t *vp;
      gboolean overwrite;
    } dt_imageio_disk_v1_t;

    dt_imageio_disk_t *n = (dt_imageio_disk_t *)malloc(sizeof(dt_imageio_disk_t));
    dt_imageio_disk_v1_t *o = (dt_imageio_disk_v1_t *)old_params;

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));

    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

static void button_clicked(GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_cancel"),
      GTK_RESPONSE_CANCEL, _("_select as output destination"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gchar *old = g_strdup(gtk_entry_get_text(d->entry));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *composed = g_build_filename(dir, "$(FILE_NAME)", NULL);

    // composed can now contain '\': on Windows it's the path separator,
    // on other platforms it can be part of a regular folder name.
    // This would later clash with variable substitution, so we have to escape them
    gchar *escaped = dt_util_str_replace(composed, "\\", "\\\\");

    gtk_entry_set_text(GTK_ENTRY(d->entry), escaped); // the signal handler will write this to conf
    g_free(dir);
    g_free(composed);
    g_free(escaped);
  }
  gtk_widget_destroy(filechooser);
}

static void entry_changed_callback(GtkEntry *entry, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", gtk_entry_get_text(entry));
}

static void overwrite_toggle_callback(GtkWidget *widget, gpointer user_data)
{
  dt_conf_set_bool("plugins/imageio/storage/disk/overwrite", dt_bauhaus_combobox_get(widget) == 1);
}

void gui_init(dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)malloc(sizeof(disk_t));
  self->gui_data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));
  GtkWidget *widget;

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, FALSE, 0);

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/disk/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }

  dt_gtkentry_setup_completion(GTK_ENTRY(widget), dt_gtkentry_get_default_path_compl_list());

  char *tooltip_text = dt_gtkentry_build_completion_tooltip_text(
      _("enter the path where to put exported images\nvariables support bash like string manipulation\n"
        "recognized variables:"),
      dt_gtkentry_get_default_path_compl_list());

  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));
  gtk_entry_set_width_chars(GTK_ENTRY(widget), 0);
  gtk_widget_set_tooltip_text(widget, tooltip_text);
  g_signal_connect(G_OBJECT(widget), "changed", G_CALLBACK(entry_changed_callback), self);

  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_tooltip_text(widget, _("select directory"));
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  d->overwrite = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->overwrite, NULL, _("on conflict"));
  dt_bauhaus_combobox_add(d->overwrite, _("create unique filename"));
  dt_bauhaus_combobox_add(d->overwrite, _("overwrite"));
  gtk_box_pack_start(GTK_BOX(self->widget), d->overwrite, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->overwrite), "value-changed", G_CALLBACK(overwrite_toggle_callback), self);
  dt_bauhaus_combobox_set(d->overwrite, 0);

  g_free(tooltip_text);
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  // global default can be annoying:
  // gtk_entry_set_text(GTK_ENTRY(d->entry), "$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)");
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", gtk_entry_get_text(d->entry));

  // this should prevent users from unintentional image overwrite
  dt_bauhaus_combobox_set(d->overwrite, 0);
}

int store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality, const gboolean upscale, dt_colorspaces_color_profile_type_t icc_type,
          const gchar *icc_filename, dt_iop_color_intent_t icc_intent)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)sdata;

  char filename[PATH_MAX] = { 0 };
  char input_dir[PATH_MAX] = { 0 };
  char pattern[DT_MAX_PATH_FOR_PARAMS];
  g_strlcpy(pattern, d->filename, sizeof(pattern));
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, input_dir, sizeof(input_dir), &from_cache);
  int fail = 0;
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {
try_again:
    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(pattern, "$"))
    {
      snprintf(pattern + strlen(pattern), sizeof(pattern) - strlen(pattern), "_$(SEQUENCE)");
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

    // if filenamepattern is a directory just add ${FILE_NAME} as default..
    // this can happen if the filename component of the pattern is an empty variable
    char last_char = *(filename + strlen(filename) - 1);
    if(last_char == '/' || last_char == '\\')
    {
      // add to the end of the original pattern without caring about a
      // potentially added "_$(SEQUENCE)"
      if (snprintf(pattern, sizeof(pattern), "%s" G_DIR_SEPARATOR_S "$(FILE_NAME)", d->filename) < sizeof(pattern))
        goto try_again;
    }

    char *output_dir = g_path_get_dirname(filename);

    if(g_mkdir_with_parents(output_dir, 0755))
    {
      fprintf(stderr, "[imageio_storage_disk] could not create directory: `%s'!\n", output_dir);
      dt_control_log(_("could not create directory `%s'!"), output_dir);
      fail = 1;
      goto failed;
    }
    if(g_access(output_dir, W_OK | X_OK) != 0)
    {
      fprintf(stderr, "[imageio_storage_disk] could not write to directory: `%s'!\n", output_dir);
      dt_control_log(_("could not write to directory `%s'!"), output_dir);
      fail = 1;
      goto failed;
    }

    const char *ext = format->extension(fdata);
    char *c = filename + strlen(filename);
    size_t filename_free_space = sizeof(filename) - (c - filename);
    snprintf(c, filename_free_space, ".%s", ext);

  /* prevent overwrite of files */
  failed:
    g_free(output_dir);

    if(!fail && !d->overwrite)
    {
      int seq = 1;
      while(g_file_test(filename, G_FILE_TEST_EXISTS))
      {
        snprintf(c, filename_free_space, "_%.2d.%s", seq, ext);
        seq++;
      }
    }
  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(fail) return 1;

  /* export image to file */
  if(dt_imageio_export(imgid, filename, format, fdata, high_quality, upscale, TRUE, icc_type, icc_filename,
                       icc_intent, self, sdata, num, total) != 0)
  {
    fprintf(stderr, "[imageio_storage_disk] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  printf("[export_job] exported to `%s'\n", filename);
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
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_disk_t, filename,
                                char_path_length);
#endif
}

void *get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)calloc(1, sizeof(dt_imageio_disk_t));

  char *text = dt_conf_get_string("plugins/imageio/storage/disk/file_directory");
  g_strlcpy(d->filename, text, sizeof(d->filename));
  g_free(text);

  d->overwrite = dt_conf_get_bool("plugins/imageio/storage/disk/overwrite");

  d->vp = NULL;
  dt_variables_params_init(&d->vp);

  return d;
}

void free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  if(!params) return;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  disk_t *g = (disk_t *)self->gui_data;

  if(size != self->params_size(self)) return 1;

  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);

  // we really do not want user to unintentionally overwrite image
  dt_bauhaus_combobox_set(g->overwrite, 0);
  return 0;
}

void export_dispatched(dt_imageio_module_storage_t *self)
{
  disk_t *g = (disk_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->overwrite, 0);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
