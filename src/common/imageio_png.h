#ifndef DT_IMAGEIO_PNG_H
#define DT_IMAGEIO_PNG_H

#include <stdlib.h>
#include <stdio.h>
#include <png.h>
#include <inttypes.h>

typedef struct dt_imageio_png_t
{
  int width, height;
  int bytespp;
  FILE *f;
  png_structp png_ptr;
  png_infop info_ptr;
}
dt_imageio_png_t;


/** write png to file, with exif if not NULL. */
int dt_imageio_png_write(const char *filename, const uint8_t *in, const int width, const int height);
/** read png header from file, leave file descriptor open until png_read is called. */
int dt_imageio_png_read_header(const char *filename, dt_imageio_png_t *png);
/** reads the png to the (sufficiently allocated) buffer, closes file. */
int dt_imageio_png_read(dt_imageio_png_t *png, uint8_t *out);
/** if called directly after read_header, will strip 16-bit content of png down to 8-bit output. */
int dt_imageio_png_read_assure_8(dt_imageio_png_t *png);
#endif
