
#include "common/imageio_png.h"


int dt_imageio_png_write(const char *filename, const uint8_t *in, const int width, const int height)
{
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
    png_destroy_write_struct(&png_ptr, png_infopp_NULL);
    return 1;
  }

  if (setjmp(png_jmpbuf(png_ptr)))
  {
    fclose(f);
    png_destroy_write_struct(&png_ptr, png_infopp_NULL);
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
  return 0;
}


int dt_imageio_png_read_header(const char *filename, dt_imageio_png_t *png)
{
  png->f = fopen(filename, "rb");

  if(!png->f) return 1;

  const unsigned int NUM_BYTES_CHECK = 8;
  png_byte dat[NUM_BYTES_CHECK];

  fread(dat, 1, NUM_BYTES_CHECK, png->f);

  if (png_sig_cmp(dat, (png_size_t) 0, NUM_BYTES_CHECK))
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
    png_destroy_read_struct(&png->png_ptr, png_infopp_NULL, png_infopp_NULL);
    return 1;
  }

  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, png_infopp_NULL, png_infopp_NULL);
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
    png_set_gray_1_2_4_to_8(png->png_ptr);

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

int dt_imageio_png_read_assure_8(dt_imageio_png_t *png)
{
  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, png_infopp_NULL, png_infopp_NULL);
    return 1;
  }
  uint32_t bit_depth = png_get_bit_depth(png->png_ptr, png->info_ptr);
  // strip down to 8 bit channels
  if (bit_depth == 16) 
    png_set_strip_16(png->png_ptr);

  return 0;
}

int dt_imageio_png_read(dt_imageio_png_t *png, uint8_t *out)
{
  if (setjmp(png_jmpbuf(png->png_ptr)))
  {
    fclose(png->f);
    png_destroy_read_struct(&png->png_ptr, png_infopp_NULL, png_infopp_NULL);
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
  png_destroy_read_struct(&png->png_ptr, &png->info_ptr, png_infopp_NULL);

  fclose(png->f);
  return 0;
}

