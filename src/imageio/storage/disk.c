/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

typedef struct disk_t
{
  GtkEntry *entry;
}
disk_t;

const char*
name ()
{
  return _("file on disk");
}

static void
button_clicked (GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  GtkWidget *win = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select $(FILE_DIRECTORY)"),
              GTK_WINDOW (win),
              GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
              GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
              GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
              NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
    g_free(dir);
  }
  gtk_widget_destroy (filechooser);
}

void
gui_init (dt_imageio_module_storage_t *self)
{
#if 1
  disk_t *d = (disk_t *)malloc(sizeof(disk_t));
  self->gui_data = (void *)d;
  self->widget = gtk_hbox_new(FALSE, 5);
  GtkWidget *widget;

  // TODO: default FILE_DIRECTORY to first image in selection?

  widget = gtk_entry_new();
  gtk_box_pack_start(GTK_BOX(self->widget), widget, TRUE, TRUE, 0);
  gtk_entry_set_text(GTK_ENTRY(widget), "$(FILE_DIRECTORY)/darktable_exported");
  d->entry = GTK_ENTRY(widget);
  gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("enter the path where to put exported images:\n"
                                                       "$(FILE_DIRECTORY) - select with button to the right\n"
                                                       "TODO - document the other vars!"), NULL);
  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, 0);
  gtk_widget_set_size_request(widget, 18, 18);
  gtk_object_set(GTK_OBJECT(widget), "tooltip-text", _("select $(FILE_DIRECTORY)"), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), widget, FALSE, FALSE, 0);
#endif
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void
gui_reset (dt_imageio_module_storage_t *self)
{
}

int
store (dt_imageio_module_data_t *sdata, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total)
{
  dt_image_t *img = dt_image_cache_use(imgid, 'r');

  char filename[1024];
  char dirname[1024];
  dt_image_export_path(img, filename, 1024);
  strncpy(dirname, filename, 1024);

  char *c = dirname + strlen(dirname);
  for(;c>dirname && *c != '/';c--);
  *c = '\0';
  if(g_mkdir_with_parents(dirname, 0755))
  {
    fprintf(stderr, "[imageio_storage_disk] could not create directory %s!\n", dirname);
    dt_control_log(_("could not create directory %s!"), dirname);
    dt_image_cache_release(img, 'r');
    return 1;
  }

  // TODO: get storage path from paramters (which also need to be passed here)
  // and only avoid if filename and c match exactly!
  c = filename + strlen(filename);
  for(;c>filename && *c != '.';c--);
  if(c <= filename) c = filename + strlen(filename);

  const char *ext = format->extension(fdata);

  // avoid name clashes for single images:
  if(img->film_id == 1 && !strcmp(c+1, ext)) { strncpy(c, "_dt", 3); c += 3; }
  strncpy(c+1, ext, strlen(ext)+1);
  dt_imageio_export(img, filename, format, fdata);
  dt_image_cache_release(img, 'r');

  printf("[export_job] exported to `%s'\n", filename);
  char *trunc = filename + strlen(filename) - 32;
  if(trunc < filename) trunc = filename;
  dt_control_log(_("%d/%d exported to `%s%s'"), num, total, trunc != filename ? ".." : "", trunc);
  return 0;
}

void*
get_params(dt_imageio_module_storage_t *self)
{
  // TODO: collect (and partially expand?) regexp where to put img
  return NULL;
}

void
free_params(dt_imageio_module_storage_t *self, void *params)
{
  free(params);
}

