
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

  png_bytep row_pointer = (png_bytep) buffer;
  unsigned long rowbytes = png_get_rowbytes(png_ptr, info_ptr);

  for (int y = 0; y < height; y++)
  {
    png_write_row(png_ptr, row_pointer);
    row_pointer += rowbytes;
  }	

  png_write_end(png_ptr, info_ptr);
  png_destroy_write_struct(&png_ptr, &info_ptr);
  fclose(f);
  return 0;
}


#if 0
int dt_imageio_png_read_header(const char *filename, dt_imageio_png_t *png)
{
}
int dt_imageio_png_read(dt_imageio_png_t *png, uint8_t *out)
{
    FILE *input = fopen(filename, "rb");

    if (!input) { // file open?
	cerr << "[makeTexturePNG] Could not open " << filename << "!" << endl;
	fclose(input);
	return;
    }

    // check if it's PNG
    const unsigned int NUM_BYTES_CHECK = 8;
    png_byte dat[NUM_BYTES_CHECK];

    fread(dat, 1, NUM_BYTES_CHECK, input);

    if (png_sig_cmp(dat, (png_size_t) 0, NUM_BYTES_CHECK)) {
	cerr << "[makeTexturePNG] " << filename << " doesn't appear to be an PNG file." << endl;
	fclose(input);
	return;
    }

    // init structs
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (!png_ptr) {
	fclose(input);
	return;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
	fclose(input);
	png_destroy_read_struct(&png_ptr, png_infopp_NULL, png_infopp_NULL);
	return;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
	fclose(input);
	png_destroy_read_struct(&png_ptr, png_infopp_NULL, png_infopp_NULL);
	return;
    }

    png_init_io(png_ptr, input);

    // we checked some bytes
    png_set_sig_bytes(png_ptr, NUM_BYTES_CHECK);

    // image info
    png_read_info(png_ptr, info_ptr);

    unsigned int bit_depth = png_get_bit_depth(png_ptr, info_ptr);
    unsigned int color_type = png_get_color_type(png_ptr, info_ptr);

    // image input transformations
    
    // palette => rgb
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    // 1, 2, 4 bit => 8 bit
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
	png_set_gray_1_2_4_to_8(png_ptr);

    // strip down to 8 bit channels
    if (bit_depth == 16) 
        png_set_strip_16(png_ptr);

    // strip alpha channel
    if (color_type & PNG_COLOR_MASK_ALPHA)
        png_set_strip_alpha(png_ptr);

    // grayscale => rgb
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
	png_set_gray_to_rgb(png_ptr);

    // reflect changes
    png_read_update_info(png_ptr, info_ptr);

    texWidth = png_get_image_width(png_ptr, info_ptr);
    texHeight = png_get_image_height(png_ptr, info_ptr);

    // allocate buffer memory
    texture = new unsigned char[texWidth * texHeight * 3];

    // read one row at a time
    png_bytep row_pointer = (png_bytep) texture;
    unsigned long rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    for (int y = 0; y < texHeight; y++) {
	png_read_row(png_ptr, row_pointer, NULL); // read into buffer data

	row_pointer += rowbytes; // increment pointer to data
    }

    png_read_end(png_ptr, info_ptr);

    png_destroy_read_struct(&png_ptr, &info_ptr, png_infopp_NULL);

    fclose(input);
}
#endif
