#ifndef DT_IMAGEIO_JPEG_H
#define DT_IMAGEIO_JPEG_H

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
// this fixes a rather annoying, long time bug in libjpeg :(
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H
#include <jpeglib.h>
#undef HAVE_STDLIB_H
#undef HAVE_STDDEF_H

typedef struct dt_imageio_jpeg_t
{
  int width, height;
  struct jpeg_source_mgr src;
  struct jpeg_destination_mgr dest;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_compress_struct   cinfo;
  FILE *f;
}
dt_imageio_jpeg_t;

/** reads the header and fills width/height in jpg struct. */
int dt_imageio_jpeg_decompress_header(const void *in, size_t length, dt_imageio_jpeg_t *jpg);
/** reads the whole image to the out buffer, which has to be large enough. */
int dt_imageio_jpeg_decompress(dt_imageio_jpeg_t *jpg, uint8_t *out);
/** compresses in to out buffer with given quality (0..100). out buffer must be large enough. returns actual data length. */
int dt_imageio_jpeg_compress(const uint8_t *in, uint8_t *out, const int width, const int height, const int quality);

/** write jpeg to file, with exif if not NULL. */
int dt_imageio_jpeg_write(const char *filename, const uint8_t *in, const int width, const int height, const int quality, void *exif, int exif_len);
/** read jpeg header from file, leave file descriptor open until jpeg_read is called. */
int dt_imageio_jpeg_read_header(const char *filename, dt_imageio_jpeg_t *jpg);
/** reads the jpeg to the (sufficiently allocated) buffer, closes file. */
int dt_imageio_jpeg_read(dt_imageio_jpeg_t *jpg, uint8_t *out);
#endif
