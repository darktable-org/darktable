/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#ifdef HAVE_LIBRAW
#include "common/darktable.h"
#include "imageio.h"
#include "imageio_gm.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <assert.h>

#include <libraw/libraw.h>


/* LibRAW is expected to read only new canon CR3 files */

static gboolean _supported_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "cr3", NULL };
  gboolean supported = FALSE;
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      supported = TRUE;
      break;
    }
  return supported;
}


dt_imageio_retval_t dt_imageio_open_libraw(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  int err = DT_IMAGEIO_FILE_CORRUPTED;

  if(!_supported_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  libraw_data_t *raw_data = libraw_init(0);

  if(!libraw_open_file(raw_data, filename))
    goto error;

  img->buf_dsc.cst = iop_cs_RAW;
  img->flags |= DT_IMAGE_4BAYER;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags |= DT_IMAGE_RAW;
  img->loader = LOADER_LIBRAW;

  libraw_close(raw_data);

  return DT_IMAGEIO_OK;

error:
  libraw_close(raw_data);
  return err;
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
