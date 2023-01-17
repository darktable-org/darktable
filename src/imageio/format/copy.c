/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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
#include "common/utility.h"
#include "imageio/imageio_module.h"
#include "imageio/format/imageio_format_api.h"
#include "gui/gtk.h"
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

// FIXME: we can't rely on darktable to avoid file overwriting -- it doesn't know the filename (extension).
int write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                void *exif, int exif_len, int imgid, int num, int total, struct dt_dev_pixelpipe_t *pipe,
                const gboolean export_masks)
{
  int status = 1;
  gboolean from_cache = TRUE;
  char sourcefile[PATH_MAX];
  char *targetfile = NULL;
  char *xmpfile = NULL;

  dt_image_full_path(imgid, sourcefile, sizeof(sourcefile), &from_cache);

  char *extension = g_strrstr(sourcefile, ".");
  if(extension == NULL) goto END;
  targetfile = g_strconcat(filename, ++extension, NULL);

  if(!strcmp(sourcefile, targetfile)) goto END;

  dt_copy_file(sourcefile, targetfile);

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
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  gtk_container_add(GTK_CONTAINER(self->widget),
    dt_ui_label_new(_("do a 1:1 copy of the selected files.\nthe global options below do not apply!")));
}
void gui_cleanup(dt_imageio_module_format_t *self)
{
}
void gui_reset(dt_imageio_module_format_t *self)
{
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

