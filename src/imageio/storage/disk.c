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
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(1)

const char*
name ()
{
  return _("file on disk");
}

void
gui_init (dt_imageio_module_storage_t *self)
{
}

void
gui_cleanup (dt_imageio_module_storage_t *self)
{
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
  strncpy(c+1, ext, strlen(ext));
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

