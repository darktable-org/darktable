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
#include <memory.h>
#include <stdio.h>
#include <png.h>
#include <inttypes.h>
#include <strings.h>
#include <assert.h>

#include "common/darktable.h"
#include "imageio.h"
#include "imageio_tiff.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

typedef struct dt_imageio_png_t
{
  int max_width, max_height;
  int width, height;
  int color_type, bit_depth;
  int bpp;
  FILE *f;
  png_structp png_ptr;
  png_infop info_ptr;
} dt_imageio_png_t;


int read_header(const char *filename, dt_imageio_png_t *png)
{
  png->f = fopen(filename, "rb");

  if(!png->f) return 1;

  const size_t NUM_BYTES_CHECK = 8;
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
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
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

  // png->bytespp = 3*bit_depth/8;
  png->width = png_get_image_width(png->png_ptr, png->info_ptr);
  png->height = png_get_image_height(png->png_ptr, png->info_ptr);

  return 0;
}


int read_image(dt_imageio_png_t *png, void *out)
{
  if(setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }
  // reflect changes
  png_read_update_info(png->png_ptr, png->info_ptr);

  png_bytep row_pointer = (png_bytep)out;
  unsigned long rowbytes = png_get_rowbytes(png->png_ptr, png->info_ptr);

  for(int y = 0; y < png->height; y++)
  {
    png_read_row(png->png_ptr, row_pointer, NULL);
    row_pointer += rowbytes;
  }

  png_read_end(png->png_ptr, png->info_ptr);
  png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);

  fclose(png->f);
  return 0;
}



dt_imageio_retval_t dt_imageio_open_png(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  const char *ext = filename + strlen(filename);
  while(*ext != '.' && ext > filename) ext--;
  if(strncmp(ext, ".png", 4) && strncmp(ext, ".PNG", 4)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  dt_imageio_png_t image;
  uint8_t *buf = NULL;
  uint32_t width, height;
  uint16_t bpp;


  if(read_header(filename, &image) != 0) return DT_IMAGEIO_FILE_CORRUPTED;

  width = img->width = image.width;
  height = img->height = image.height;
  bpp = image.bit_depth;

  img->bpp = 4 * sizeof(float);

  float *mipbuf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!mipbuf)
  {
    fclose(image.f);
    png_destroy_read_struct(&image.png_ptr, NULL, NULL);
    fprintf(stderr, "[png_open] could not alloc full buffer for image `%s'\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  buf = dt_alloc_align(16, (size_t)width * height * 3 * (bpp < 16 ? 1 : 2));
  if(!buf)
  {
    fclose(image.f);
    png_destroy_read_struct(&image.png_ptr, NULL, NULL);
    fprintf(stderr, "[png_open] could not alloc intermediate buffer for image `%s'\n", img->filename);
    return DT_IMAGEIO_CACHE_FULL;
  }

  if(read_image(&image, (void *)buf) != 0)
  {
    dt_free_align(buf);
    fprintf(stderr, "[png_open] could not read image `%s'\n", img->filename);
    return DT_IMAGEIO_FILE_CORRUPTED;
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
  return DT_IMAGEIO_OK;
}

int dt_imageio_png_read_profile(const char *filename, uint8_t **out)
{
  dt_imageio_png_t image;
  png_charp name;
  int compression_type;
  png_uint_32 proflen;

#if PNG_LIBPNG_VER >= 10500 /* 1.5.0 */
  png_bytep profile;
#else
  png_charp profile;
#endif

  if(!(filename && *filename && out)) return 0;

  if(read_header(filename, &image) != 0) return DT_IMAGEIO_FILE_CORRUPTED;

#ifdef PNG_iCCP_SUPPORTED
  if(png_get_valid(image.png_ptr, image.info_ptr, PNG_INFO_iCCP) != 0
     && png_get_iCCP(image.png_ptr, image.info_ptr, &name, &compression_type, &profile, &proflen) != 0)
  {
    *out = (uint8_t *)malloc(proflen);
    memcpy(*out, profile, proflen);
  }
  else
#endif
    proflen = 0;

  png_destroy_read_struct(&image.png_ptr, NULL, NULL);
  fclose(image.f);

  return proflen;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
