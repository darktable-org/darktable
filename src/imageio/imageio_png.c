/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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
#include <assert.h>
#include <inttypes.h>
#include <memory.h>
#include <png.h>
#include <stdio.h>
#include <strings.h>

#include "common/colorspaces.h"
#include "common/darktable.h"
#include "imageio_png.h"
#include "common/exif.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "imageio_common.h"
#include "imageio_tiff.h"

int read_header(const char *filename, dt_imageio_png_t *png)
{
  png->f = g_fopen(filename, "rb");

  if(!png->f) return 1;

#define NUM_BYTES_CHECK (8)

  png_byte dat[NUM_BYTES_CHECK];

  size_t cnt = fread(dat, 1, NUM_BYTES_CHECK, png->f);

  if(cnt != NUM_BYTES_CHECK || png_sig_cmp(dat, (png_size_t)0, NUM_BYTES_CHECK))
  {
    fclose(png->f);
    return 1;
  }

  png->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if(!png->png_ptr)
  {
    fclose(png->f);
    return 1;
  }

  /* TODO: gate by version once known cICP chunk read support is added to libpng */
#ifdef PNG_STORE_UNKNOWN_CHUNKS_SUPPORTED
  png_set_keep_unknown_chunks(png->png_ptr, 3, (png_const_bytep) "cICP", 1);
#endif

  png->info_ptr = png_create_info_struct(png->png_ptr);
  if(!png->info_ptr)
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }

  if(setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);
    return 1;
  }

  png_init_io(png->png_ptr, png->f);

  // we checked some bytes
  png_set_sig_bytes(png->png_ptr, NUM_BYTES_CHECK);

  // image info
  png_read_info(png->png_ptr, png->info_ptr);

  png->bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  png->color_type = png_get_color_type(png->png_ptr, png->info_ptr);

  // image input transformations

  // palette => rgb
  if(png->color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png->png_ptr);

  // 1, 2, 4 bit => 8 bit
  if(png->color_type == PNG_COLOR_TYPE_GRAY && png->bit_depth < 8)
  {
    png_set_expand_gray_1_2_4_to_8(png->png_ptr);
    png->bit_depth = 8;
  }

  // strip alpha channel
  if(png->color_type & PNG_COLOR_MASK_ALPHA) png_set_strip_alpha(png->png_ptr);

  // grayscale => rgb
  if(png->color_type == PNG_COLOR_TYPE_GRAY || png->color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png->png_ptr);

  // reflect changes
  png_read_update_info(png->png_ptr, png->info_ptr);

  // png->bytespp = 3*bit_depth/8;
  png->width = png_get_image_width(png->png_ptr, png->info_ptr);
  png->height = png_get_image_height(png->png_ptr, png->info_ptr);

#undef NUM_BYTES_CHECK

  return 0;
}


int read_image(dt_imageio_png_t *png, void *out)
{
  if(setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);
    return 1;
  }

  png_bytep *row_pointers = malloc(sizeof(png_bytep) * png->height);

  png_bytep row_pointer = (png_bytep)out;
  const size_t rowbytes = png_get_rowbytes(png->png_ptr, png->info_ptr);

  for(int y = 0; y < png->height; y++)
  {
    row_pointers[y] = row_pointer + (size_t)y * rowbytes;
  }

  png_read_image(png->png_ptr, row_pointers);

  png_read_end(png->png_ptr, png->info_ptr);
  png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);

  free(row_pointers);
  fclose(png->f);
  return 0;
}



dt_imageio_retval_t dt_imageio_open_png(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".png", 4) && strncmp(ext, ".PNG", 4)) return DT_IMAGEIO_LOAD_FAILED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  dt_imageio_png_t image;
  uint8_t *buf = NULL;
  uint32_t width, height;
  uint16_t bpp;


  if(read_header(filename, &image) != 0) return DT_IMAGEIO_LOAD_FAILED;

  width = img->width = image.width;
  height = img->height = image.height;
  bpp = image.bit_depth;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    fclose(image.f);
    png_destroy_read_struct(&image.png_ptr, &image.info_ptr, NULL);
    fprintf(stderr, "[png_open] could not alloc full buffer for image `%s'\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  buf = dt_alloc_align(64, (size_t)image.height * png_get_rowbytes(image.png_ptr, image.info_ptr));

  if(!buf)
  {
    fclose(image.f);
    png_destroy_read_struct(&image.png_ptr, &image.info_ptr, NULL);
    fprintf(stderr, "[png_open] could not alloc intermediate buffer for image `%s'\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  if(read_image(&image, (void *)buf) != 0)
  {
    dt_free_align(buf);
    fprintf(stderr, "[png_open] could not read image `%s'\n", img->filename);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  for(size_t j = 0; j < height; j++)
  {
    if(bpp < 16)
      for(size_t i = 0; i < width; i++)
        for(int k = 0; k < 3; k++)
          mipbuf[4 * (j * width + i) + k] = buf[3 * (j * width + i) + k] * (1.0f / 255.0f);
    else
      for(size_t i = 0; i < width; i++)
        for(int k = 0; k < 3; k++)
          mipbuf[4 * (j * width + i) + k] = (256.0f * buf[2 * (3 * (j * width + i) + k)]
                                             + buf[2 * (3 * (j * width + i) + k) + 1]) * (1.0f / 65535.0f);
  }

  dt_free_align(buf);
  img->loader = LOADER_PNG;
  return DT_IMAGEIO_OK;
}

int dt_imageio_png_read_profile(const char *filename, uint8_t **out, dt_colorspaces_cicp_t *cicp)
{
  /* set default return values */
  *out = NULL;
  cicp->color_primaries = DT_CICP_COLOR_PRIMARIES_UNSPECIFIED;
  cicp->transfer_characteristics = DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
  cicp->matrix_coefficients = DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED;

  dt_imageio_png_t image;
  png_charp name;
  png_uint_32 proflen = 0;

#if PNG_LIBPNG_VER >= 10500 /* 1.5.0 */
  png_bytep profile;
#else
  png_charp profile;
#endif

  if(!(filename && *filename && out)) return 0;

  if(read_header(filename, &image) != 0) return DT_IMAGEIO_LOAD_FAILED;

  /* TODO: also add check for known cICP chunk read support once added to libpng */
#ifdef PNG_STORE_UNKNOWN_CHUNKS_SUPPORTED
  png_unknown_chunkp unknowns = NULL;
  const int num = png_get_unknown_chunks(image.png_ptr, image.info_ptr, &unknowns);
  for(size_t c = 0; c < num; ++c)
    if(!strcmp((const char *)unknowns[c].name, "cICP"))
    {
      /* only RGB (i.e. matrix coeffs 0 in data[2]) and full range (1 in data[3]) pixel values are supported by the
       * loader above and dt color management */
      if(!unknowns[c].data[2] && unknowns[c].data[3])
      {
        cicp->color_primaries = (dt_colorspaces_cicp_color_primaries_t)unknowns[c].data[0];
        cicp->transfer_characteristics = (dt_colorspaces_cicp_transfer_characteristics_t)unknowns[c].data[1];
        cicp->matrix_coefficients = (dt_colorspaces_cicp_matrix_coefficients_t)unknowns[c].data[2];
      }
      else
        dt_print(DT_DEBUG_IMAGEIO, "[png_open] encountered YUV and/or narrow-range image `%s', assuming unknown CICP\n", filename);
      break;
    }
#endif

#ifdef PNG_iCCP_SUPPORTED
  if(png_get_valid(image.png_ptr, image.info_ptr, PNG_INFO_iCCP) != 0
     && png_get_iCCP(image.png_ptr, image.info_ptr, &name, NULL, &profile, &proflen) != 0)
  {
    *out = (uint8_t *)g_malloc(proflen);
    memcpy(*out, profile, proflen);
  }
#endif

  png_destroy_read_struct(&image.png_ptr, &image.info_ptr, NULL);
  fclose(image.f);

  return proflen;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
