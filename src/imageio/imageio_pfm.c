/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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
#include "develop/imageop.h"         // for IOP_CS_RGB
#include "imageio/imageio_pfm.h"
#include "common/pfm.h"
#include "common/imagebuf.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

dt_imageio_retval_t dt_imageio_open_pfm(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf)
{
  int wd, ht, ch, error = 0;
  float *readbuf = dt_read_pfm(filename, &error, &wd, &ht, &ch, 4);

  if(error == DT_IMAGEIO_FILE_NOT_FOUND)
    return DT_IMAGEIO_FILE_NOT_FOUND;
  else if(error == DT_IMAGEIO_FILE_CORRUPTED)
    return DT_IMAGEIO_FILE_CORRUPTED;
  else if(error != DT_IMAGEIO_OK || !readbuf)
    return DT_IMAGEIO_IOERROR;

  img->width = wd;
  img->height = ht;
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    dt_free_align(readbuf);
    return DT_IMAGEIO_CACHE_FULL;
  }

  dt_iop_image_copy(buf, readbuf, (size_t)img->width * img->height * 4);
  dt_free_align(readbuf);

  img->buf_dsc.cst = IOP_CS_RGB;
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags |= DT_IMAGE_HDR;
  img->loader = LOADER_PFM;
  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
