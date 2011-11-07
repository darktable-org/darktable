/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#ifndef DT_IMAGEIO_EXR_H
#define DT_IMAGEIO_EXR_H

#include "common/image.h"
#include "common/mipmap_cache.h"

#ifdef __cplusplus
extern "C"
{
#endif

  dt_imageio_retval_t dt_imageio_open_exr (dt_image_t *img, const char *filename, dt_mipmap_cache_allocator_t a);

#ifdef __cplusplus
}
#endif
#endif
