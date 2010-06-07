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

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif
#include "common/darktable.h"
#include "common/imageio_module.h"
#include "common/colorspaces.h"
#include "control/conf.h"
#include "dtgtk/slider.h"
#include <lcms.h>
#include <stdlib.h>
#include <stdio.h>
#include <png.h>
#include <inttypes.h>

DT_MODULE(1)

typedef struct dt_imageio_png_t
{
  int max_width, max_height;
  int width, height;
  int bytespp;
  FILE *f;
  png_structp png_ptr;
  png_infop info_ptr;
}
dt_imageio_png_t;


static int
write_image_with_icc_profile_static (dt_imageio_png_t *p, const char *filename, const void *in_void, void *exif, int exif_len, int imgid)
{
  const int width = p->width, height = p->height;
  const uint8_t *in = (uint8_t *)in_void;
  FILE *f = fopen(filename, "wb");
  if (!f) return 1;

  png_structp png_ptr;
  png_infop info_ptr;

  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png_ptr)
  {
    fclose(f);
    return 1;
  }

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr)
  {
    fclose(f);
    png_destroy_write_struct(&png_ptr, NULL);
    return 1;
  }

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    fclose(f);
    png_destroy_write_struct(&png_ptr, NULL);
    return 1;
  }

  png_init_io(png_ptr, f);

  png_set_compression_level(png_ptr, Z_BEST_COMPRESSION);
  png_set_compression_mem_level(png_ptr, 8);
  png_set_compression_strategy(png_ptr, Z_DEFAULT_STRATEGY);
  png_set_compression_window_bits(png_ptr, 15);
  png_set_compression_method(png_ptr, 8);
  png_set_compression_buffer_size(png_ptr, 8192);

  png_set_IHDR(png_ptr, info_ptr, width, height,
      8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_write_info(png_ptr, info_ptr);

  // png_bytep row_pointer = (png_bytep) in;
  png_byte row[3*width];
  // unsigned long rowbytes = png_get_rowbytes(png_ptr, info_ptr);

  for (int y = 0; y < height; y++)
  {
    for(int x=0;x<width;x++) for(int k=0;k<3;k++) row[3*x+k] = in[4*width*y + 4*x + k];
    png_write_row(png_ptr, row);
  }	

  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  fclose(f);
  // TODO: append exif and embed icc profile!
  return 0;
}

int write_image_with_icc_profile (dt_imageio_png_t *p, const char *filename, const void *in_void, void *exif, int exif_len, int imgid)
{
  return write_image_with_icc_profile_static (p, filename, in_void, exif, exif_len, imgid);
}

int write_image (dt_imageio_png_t *p, const char *filename, const void *in_void, void *exif, int exif_len)
{
  return write_image_with_icc_profile_static (p, filename, in_void, exif, exif_len, -1);
}

int read_header(const char *filename, dt_imageio_png_t *png)
{
  png->f = fopen(filename, "rb");

  if(!png->f) return 1;

  const unsigned int NUM_BYTES_CHECK = 8;
  png_byte dat[NUM_BYTES_CHECK];

  int cnt = fread(dat, 1, NUM_BYTES_CHECK, png->f);

  if (cnt != NUM_BYTES_CHECK || png_sig_cmp(dat, (png_size_t) 0, NUM_BYTES_CHECK))
  {
	  fclose(png->f);
    return 1;
  }

  png->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (!png->png_ptr)
  {
    fclose(png->f);
    return 1;
  }

  png->info_ptr = png_create_info_struct(png->png_ptr);
  if (!png->info_ptr)
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }

  if (setjmp(png_jmpbuf(png->png_ptr)))
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

  uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  uint32_t color_type = png_get_color_type(png->png_ptr, png->info_ptr);

  // image input transformations

  // palette => rgb
  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png->png_ptr);

  // 1, 2, 4 bit => 8 bit
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png->png_ptr);

  // strip alpha channel
  if (color_type & PNG_COLOR_MASK_ALPHA)
    png_set_strip_alpha(png->png_ptr);

  // grayscale => rgb
  if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
	png_set_gray_to_rgb(png->png_ptr);

  png->bytespp = 3*bit_depth/8;
  png->width  = png_get_image_width(png->png_ptr, png->info_ptr);
  png->height = png_get_image_height(png->png_ptr, png->info_ptr);

  return 0;
}

#if 0
int dt_imageio_png_read_assure_8(dt_imageio_png_t *png)
{
  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }
  uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  // strip down to 8 bit channels
  if (bit_depth == 16) 
    png_set_strip_16(png->png_ptr);

  return 0;
}
#endif

int read_image (dt_imageio_png_t *png, uint8_t *out)
{
  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, NULL, NULL);
    return 1;
  }
  // reflect changes
  png_read_update_info(png->png_ptr, png->info_ptr);

  png_bytep row_pointer = (png_bytep) out;
  unsigned long rowbytes = png_get_rowbytes(png->png_ptr, png->info_ptr);

  for (int y = 0; y < png->height; y++)
  {
  	png_read_row(png->png_ptr, row_pointer, NULL);
	  row_pointer += rowbytes;
  }

  png_read_end(png->png_ptr, png->info_ptr);
  png_destroy_read_struct(&png->png_ptr, &png->info_ptr, NULL);

  fclose(png->f);
  return 0;
}

void*
get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_png_t *d = (dt_imageio_png_t *)malloc(sizeof(dt_imageio_png_t));
  return d;
}

void
free_params(dt_imageio_module_format_t *self, void *params)
{
  free(params);
}

int bpp(dt_imageio_module_data_t *p)
{
  return 8;
}

const char*
extension(dt_imageio_module_data_t *data)
{
  return "png";
}

const char*
name ()
{
  return _("8-bit png");
}

// TODO: some quality/compression stuff?
void gui_init    (dt_imageio_module_format_t *self) {}
void gui_cleanup (dt_imageio_module_format_t *self) {}
void gui_reset   (dt_imageio_module_format_t *self) {}


