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

#include "common/darktable.h"
#include "common/exif.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "common/imageio_storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <glib/gstdio.h>

DT_MODULE(1)

// gui data
typedef struct disk_t
{
  GtkEntry *entry;
  GtkToggleButton *overwrite_btn;
}
disk_t;

// saved params
typedef struct dt_imageio_disk_t
{
  char filename[DT_MAX_PATH_LEN];
  dt_variables_params_t *vp;
  gboolean overwrite;
}
dt_imageio_disk_t;


const char*
name (const struct dt_imageio_module_storage_t *self)
{
  return _("file on disk");
}

static void
button_clicked (GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select directory"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  gchar *old = g_strdup(gtk_entry_get_text(d->entry));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
    char composed[DT_MAX_PATH_LEN];
    snprintf(composed, DT_MAX_PATH_LEN, "%s/$(FILE_NAME)", dir);
    gtk_entry_set_text(GTK_ENTRY(d->entry), composed);
    dt_conf_set_string("plugins/imageio/storage/disk/file_directory", composed);
    g_free(dir);
  }
  gtk_widget_destroy (filechooser);
}

void
gui_init (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)malloc(sizeof(disk_t));
  self->gui_data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);
  GtkWidget *widget;

  GtkWidget *hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget),GTK_WIDGET (hbox),TRUE,FALSE,0);

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(hbox), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/disk/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }

  dt_gtkentry_setup_completion(GTK_ENTRY(widget), dt_gtkentry_get_default_path_compl_list());

  char *tooltip_text = dt_gtkentry_build_completion_tooltip_text (
                         _("enter the path where to put exported images\nrecognized variables:"),
                         dt_gtkentry_get_default_path_compl_list());

  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (d->entry));
  g_object_set(G_OBJECT(widget), "tooltip-text", tooltip_text, (char *)NULL);
  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(widget, 18, 18);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("select directory"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  d->overwrite_btn = GTK_TOGGLE_BUTTON(gtk_check_button_new_with_label(_("overwrite")));
  gtk_box_pack_start(GTK_BOX(self->widget),GTK_WIDGET (d->overwrite_btn),TRUE,FALSE,0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overwrite_btn), FALSE);

  g_free(tooltip_text);
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect (GTK_WIDGET (d->entry));
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  // global default can be annoying:
  // gtk_entry_set_text(GTK_ENTRY(d->entry), "$(FILE_FOLDER)/darktable_exported/$(FILE_NAME)");
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", gtk_entry_get_text(d->entry));

  // this should prevent users from unintentional image overwrite
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overwrite_btn), FALSE);
}

int
store (dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
       const int num, const int total, const gboolean high_quality)
{
  disk_t *g = (disk_t *)self->gui_data;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)sdata;

  // since we're potentially called in parallel, we should uncheck button as early as possible
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->overwrite_btn), FALSE);

  char filename[DT_MAX_PATH_LEN]= {0};
  char dirname[DT_MAX_PATH_LEN]= {0};
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, dirname, DT_MAX_PATH_LEN, &from_cache);
  int fail = 0;
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {

    // if filenamepattern is a directory just let att ${FILE_NAME} as default..
    if ( g_file_test(d->filename, G_FILE_TEST_IS_DIR) || ((d->filename+strlen(d->filename))[0]=='/' || (d->filename+strlen(d->filename))[0]=='\\') )
      snprintf (d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "$(FILE_NAME)");

    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(d->filename, "$"))
    {
      snprintf(d->filename+strlen(d->filename), DT_MAX_PATH_LEN-strlen(d->filename), "_$(SEQUENCE)");
    }

    gchar* fixed_path = dt_util_fix_path(d->filename);
    g_strlcpy(d->filename, fixed_path, DT_MAX_PATH_LEN);
    g_free(fixed_path);

    d->vp->filename = dirname;
    d->vp->jobcode = "export";
    d->vp->imgid = imgid;
    d->vp->sequence = num;
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(d->vp), DT_MAX_PATH_LEN);
    g_strlcpy(dirname, filename, DT_MAX_PATH_LEN);

    const char *ext = format->extension(fdata);
    char *c = dirname + strlen(dirname);
    for(; c>dirname && *c != '/'; c--);
    if(*c == '/')
    {
      if(c > dirname) // /.../.../foo
        c[0] = '\0';
      else // /foo
        c[1] = '\0';
    }
    else if(c == dirname) // foo
    {
      c[0] = '.';
      c[1] = '\0';
    }

    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_disk] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }
    if(g_access(dirname, W_OK) != 0)
    {
      fprintf(stderr, "[imageio_storage_disk] could not write to directory: `%s'!\n", dirname);
      dt_control_log(_("could not write to directory `%s'!"), dirname);
      fail = 1;
      goto failed;
    }

    c = filename + strlen(filename);
    // remove everything after the last '.'. this destroys any file name with dots in it since $(FILE_NAME) already comes without the original extension.
//     for(; c>filename && *c != '.' && *c != '/' ; c--);
//     if(c <= filename || *c=='/') c = filename + strlen(filename);

    sprintf(c,".%s",ext);

    /* prevent overwrite of files */
failed:
    if (!d->overwrite) {
        int seq=1;
        if (!fail && g_file_test (filename,G_FILE_TEST_EXISTS))
        {
          do
          {
            sprintf(c,"_%.2d.%s",seq,ext);
            seq++;
          }
          while (g_file_test (filename,G_FILE_TEST_EXISTS));
        }
    }
  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(fail) return 1;

  /* export image to file */
  if(dt_imageio_export(imgid, filename, format, fdata, high_quality,TRUE,self,sdata) != 0)
  {
    fprintf(stderr, "[imageio_storage_disk] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

size_t
params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_disk_t) - sizeof(void *);
}

void init(dt_imageio_module_storage_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_disk_t,filename,char_path_length);
#endif
}

void*
get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)malloc(sizeof(dt_imageio_disk_t));
  memset(d, 0, sizeof(dt_imageio_disk_t));
  disk_t *g = (disk_t *)self->gui_data;
  d->vp = NULL;
  dt_variables_params_init(&d->vp);
  const char *text = gtk_entry_get_text(GTK_ENTRY(g->entry));
  g_strlcpy(d->filename, text, DT_MAX_PATH_LEN);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);
  d->overwrite = gtk_toggle_button_get_active(g->overwrite_btn);
  return d;
}

void
free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int
set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  disk_t *g = (disk_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);

  // we really do not want user to unintentionally overwrite image
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->overwrite_btn), FALSE);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
