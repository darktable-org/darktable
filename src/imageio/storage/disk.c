/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

// gui data
typedef struct disk_t
{
  GtkEntry *entry;
}
disk_t;

// saved params
typedef struct dt_imageio_disk_t
{
  char filename[1024];
  dt_variables_params_t *vp;
}
dt_imageio_disk_t;


const char*
name ()
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
    char composed[1024];
    snprintf(composed, 1024, "%s/$(FILE_NAME)", dir);
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
  self->widget = gtk_hbox_new(FALSE, 5);
  GtkWidget *widget;

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);
  gchar *dir = dt_conf_get_string("plugins/imageio/storage/disk/file_directory");
  if(dir)
  {
    gtk_entry_set_text(GTK_ENTRY(widget), dir);
    g_free(dir);
  }
  d->entry = GTK_ENTRY(widget);
  dt_gui_key_accel_block_on_focus (GTK_WIDGET (d->entry));
  g_object_set(G_OBJECT(widget), "tooltip-text", _("enter the path where to put exported images:\n"
               "$(ROLL_NAME) - roll of the input image\n"
               "$(FILE_DIRECTORY) - directory of the input image\n"
               "$(FILE_NAME) - basename of the input image\n"
               "$(FILE_EXTENSION) - extension of the input image\n"
               "$(SEQUENCE) - sequence number\n"
               "$(YEAR) - year\n"
               "$(MONTH) - month\n"
               "$(DAY) - day\n"
               "$(HOUR) - hour\n"
               "$(MINUTE) - minute\n"
               "$(SECOND) - second\n"
               "$(EXIF_YEAR) - exif year\n"
               "$(EXIF_MONTH) - exif month\n"
               "$(EXIF_DAY) - exif day\n"
               "$(EXIF_HOUR) - exif hour\n"
               "$(EXIF_MINUTE) - exif minute\n"
               "$(EXIF_SECOND) - exif second\n"
               "$(STARS) - star rating\n"
               "$(LABELS) - colorlabels\n"
               "$(PICTURES_FOLDER) - pictures folder\n"
               "$(HOME_FOLDER) - home folder\n"
               "$(DESKTOP_FOLDER) - desktop folder"
                                                  ), (char *)NULL);
  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(widget, 18, 18);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("select directory"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
  disk_t *d = (disk_t *)self->gui_data;
  // global default can be annoying:
  // gtk_entry_set_text(GTK_ENTRY(d->entry), "$(FILE_DIRECTORY)/darktable_exported/$(FILE_NAME)");
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", gtk_entry_get_text(d->entry));
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  const dt_image_t *img = dt_image_cache_read_get(&darktable.image_cache, imgid);
  if(!img) return 1;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)sdata;

  char filename[1024]= {0};
  char dirname[1024]= {0};
  dt_image_full_path(img->id, dirname, 1024);
  int fail = 0;
  // we're potentially called in parallel. have sequence number synchronized:
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  {

    // if filenamepattern is a directory just let att ${FILE_NAME} as default..
    if ( g_file_test(d->filename, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR) || ((d->filename+strlen(d->filename))[0]=='/' || (d->filename+strlen(d->filename))[0]=='\\') )
      snprintf (d->filename+strlen(d->filename), 1024-strlen(d->filename), "$(FILE_NAME)");

    // avoid braindead export which is bound to overwrite at random:
    if(total > 1 && !g_strrstr(d->filename, "$"))
    {
      snprintf(d->filename+strlen(d->filename), 1024-strlen(d->filename), "_$(SEQUENCE)");
    }

    gchar* fixed_path = dt_util_fix_path(d->filename);
    g_strlcpy(d->filename, fixed_path, 1024);
    g_free(fixed_path);

    d->vp->filename = dirname;
    d->vp->jobcode = "export";
    d->vp->img = img;
    d->vp->sequence = num;
    dt_variables_expand(d->vp, d->filename, TRUE);
    g_strlcpy(filename, dt_variables_get_result(d->vp), 1024);
    g_strlcpy(dirname, filename, 1024);

    const char *ext = format->extension(fdata);
    char *c = dirname + strlen(dirname);
    for(; c>dirname && *c != '/'; c--);
    if(*c == '/') *c = '\0';
    if(g_mkdir_with_parents(dirname, 0755))
    {
      fprintf(stderr, "[imageio_storage_disk] could not create directory: `%s'!\n", dirname);
      dt_control_log(_("could not create directory `%s'!"), dirname);
      dt_image_cache_release(img, 'r');
      fail = 1;
      goto failed;
    }

    c = filename + strlen(filename);
    for(; c>filename && *c != '.' && *c != '/' ; c--);
    if(c <= filename || *c=='/') c = filename + strlen(filename);

    sprintf(c,".%s",ext);

    /* prevent overwrite of files */
    int seq=1;
failed:
    if (!fail && g_file_test (filename,G_FILE_TEST_EXISTS))
    {
      do
      {
        sprintf(c,"_%.2d.%s",seq,ext);
        seq++;
      }
      while (g_file_test (filename,G_FILE_TEST_EXISTS));
    }

  } // end of critical block
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(fail) return 1;

  /* export image to file */
  dt_imageio_export(img, filename, format, fdata);
  dt_image_cache_read_release(&darktable.image_cache, img);

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

void*
get_params(dt_imageio_module_storage_t *self, int* size)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)malloc(sizeof(dt_imageio_disk_t));
  memset(d, 0, sizeof(dt_imageio_disk_t));
  // have to return the size of the struct to store (i.e. without all the variable pointers at the end)
  *size = sizeof(dt_imageio_disk_t) - sizeof(void *);
  disk_t *g = (disk_t *)self->gui_data;
  d->vp = NULL;
  dt_variables_params_init(&d->vp);
  const char *text = gtk_entry_get_text(GTK_ENTRY(g->entry));
  g_strlcpy(d->filename, text, 1024);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);
  return d;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int
set_params(dt_imageio_module_storage_t *self, void *params, int size)
{
  if(size != sizeof(dt_imageio_disk_t) - sizeof(void *)) return 1;
  dt_imageio_disk_t *d = (dt_imageio_disk_t *)params;
  disk_t *g = (disk_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/disk/file_directory", d->filename);
  return 0;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
