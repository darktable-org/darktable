/*
    This file is part of darktable,
    copyright (c) 2010 Tobias Ellinghaus.
    copyright (c) 2011-2012 henrik andersson.

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
#include "common/debug.h"
#include "common/exif.h"
#include "common/imageio_module.h"
#include "imageio/format/imageio_format_api.h"
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

// FIXME: we can't rely on darktable to avoid file overwriting -- it doesn't know the filename (extension).
int write_image(dt_imageio_module_data_t *ppm, const char *filename, const void *in,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total)
{
  int status = 1;
  gboolean from_cache = TRUE;
  char sourcefile[PATH_MAX];
  char *targetfile = NULL;
  char *xmpfile = NULL;
  char *content = NULL;
  FILE *fin = NULL;
  FILE *fout = NULL;

  dt_image_full_path(imgid, sourcefile, sizeof(sourcefile), &from_cache);

  char *extension = g_strrstr(sourcefile, ".");
  if(extension == NULL) goto END;
  targetfile = g_strconcat(filename, ++extension, NULL);

  if(!strcmp(sourcefile, targetfile)) goto END;

  fin = g_fopen(sourcefile, "rb");
  fout = g_fopen(targetfile, "wb");
  if(fin == NULL || fout == NULL) goto END;

  fseek(fin, 0, SEEK_END);
  size_t end = ftell(fin);
  rewind(fin);

  content = (char *)g_malloc_n(end, sizeof(char));
  if(content == NULL) goto END;
  if(fread(content, sizeof(char), end, fin) != end) goto END;
  if(fwrite(content, sizeof(char), end, fout) != end) goto END;

  // we got a copy of the file, now write the xmp data
  xmpfile = g_strconcat(targetfile, ".xmp", NULL);
  if(dt_exif_xmp_write(imgid, xmpfile) != 0)
  {
    // something went wrong, unlink the copied image.
    g_unlink(targetfile);
    goto END;
  }

  status = 0;
END:
  g_free(targetfile);
  g_free(xmpfile);
  g_free(content);
  if(fin) fclose(fin);
  if(fout) fclose(fout);
  return status;
}

size_t params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_module_data_t);
}

void *get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_module_data_t *d = (dt_imageio_module_data_t *)calloc(1, sizeof(dt_imageio_module_data_t));
  return d;
}

void free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 0;
}

const char *mime(dt_imageio_module_data_t *data)
{
  return "x-copy";
}

const char *extension(dt_imageio_module_data_t *data)
{
  return "";
}

const char *name()
{
  return _("copy");
}

void init(dt_imageio_module_format_t *self)
{
}
void cleanup(dt_imageio_module_format_t *self)
{
}

void gui_init(dt_imageio_module_format_t *self)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
  self->widget = box;

  GtkWidget *label
      = gtk_label_new(_("do a 1:1 copy of the selected files.\nthe global options below do not apply!"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
}
void gui_cleanup(dt_imageio_module_format_t *self)
{
}
void gui_reset(dt_imageio_module_format_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
