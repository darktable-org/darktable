#ifndef DT_IMAGEIO_JPEG_H
#define DT_IMAGEIO_JPEG_H

#include <stdlib.h>
#include <stdio.h>
#include <jpeglib.h>
#include <inttypes.h>

typedef struct dt_imageio_jpeg_t
{
  int width, height;
  struct jpeg_source_mgr src;
  struct jpeg_destination_mgr dest;
  struct jpeg_decompress_struct dinfo;
  struct jpeg_compress_struct   cinfo;
}
dt_imageio_jpeg_t;

/** reads the header and fills width/height in jpg struct. */
void dt_imageio_jpeg_header(const void *in, size_t length, dt_imageio_jpeg_t *jpg);
/** reads the whole image to the out buffer, which has to be large enough. */
int dt_imageio_jpeg_decompress(dt_imageio_jpeg_t *jpg, uint8_t *out);
/** compresses in to out buffer with given quality (0..100). out buffer must be large enough. returns actual data length. */
int dt_imageio_jpeg_compress(const uint8_t *in, uint8_t *out, const int width, const int height, const int quality);

#endif
