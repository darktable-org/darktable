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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/colorlabels.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#ifdef HAVE_OPENEXR
#include "common/imageio_exr.h"
#endif
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#ifdef HAVE_LIBJXL
#include "common/imageio_jpegxl.h"
#endif
#include "common/image_compression.h"
#include "common/imageio_gm.h"
#include "common/imageio_im.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_pfm.h"
#include "common/imageio_png.h"
#include "common/imageio_pnm.h"
#include "common/imageio_rawspeed.h"
#include "common/imageio_libraw.h"
#include "common/imageio_rgbe.h"
#include "common/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "common/imageio_avif.h"
#endif
#ifdef HAVE_LIBHEIF
#include "common/imageio_heif.h"
#endif
#ifdef HAVE_WEBP
#include "common/imageio_webp.h"
#endif
#include "common/imageio_libraw.h"
#include "common/mipmap_cache.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"

#ifdef HAVE_GRAPHICSMAGICK
#include <magick/api.h>
#include <magick/blob.h>
#elif defined HAVE_IMAGEMAGICK
  #ifdef HAVE_IMAGEMAGICK7
  #include <MagickWand/MagickWand.h>
  #else
  #include <wand/MagickWand.h>
  #endif
#endif

#include <assert.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef USE_LUA
#include "lua/image.h"
#endif

// Note: 'dng' is not included as it can contain anything. We will need to open and examine dng images to find out the type of content.
static const gchar *_supported_raw[]
    = { "3fr", "ari", "arw", "bay", "cr2", "cr3", "crw", "dc2", "dcr", "erf", "fff",
        "ia",  "iiq", "k25", "kc2", "kdc", "mdc", "mef", "mos", "mrw", "nef", "nrw",
        "orf", "pef", "raf", "raw", "rw2", "rwl", "sr2", "srf", "srw", "sti", "x3f", NULL };
static const gchar *_supported_ldr[]
    = { "bmp",  "bmq", "cap", "cine", "cs1", "dcm", "gif", "gpr", "j2c", "j2k", "jng", "jp2", "jpc", "jpeg", "jpg",
        "miff", "mng", "ori", "pbm",  "pfm", "pgm", "png", "pnm", "ppm", "pxn", "qtk", "rdc", "tif", "tiff", "webp", NULL };
static const gchar *_supported_hdr[] = { "avif", "exr", "hdr", "heic", "heif", "hif", "pfm", NULL };

// get the type of image from its extension
dt_image_flags_t dt_imageio_get_type_from_extension(const char *extension)
{
  const char *ext = g_str_has_prefix(extension, ".") ? extension + 1 : extension;
  for(const char **i = _supported_raw; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_RAW;
    }
  }
  for(const char **i = _supported_hdr; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_HDR;
    }
  }
  for(const char **i = _supported_ldr; *i != NULL; i++)
  {
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return DT_IMAGE_LDR;
    }
  }
  // default to 0
  return 0;
}

// load a full-res thumbnail:
int dt_imageio_large_thumbnail(const char *filename, uint8_t **buffer, int32_t *width, int32_t *height,
                               dt_colorspaces_color_profile_type_t *color_space)
{
  int res = 1;

  uint8_t *buf = NULL;
  char *mime_type = NULL;
  size_t bufsize;

  // get the biggest thumb from exif
  if(dt_exif_get_thumbnail(filename, &buf, &bufsize, &mime_type)) goto error;

  if(strcmp(mime_type, "image/jpeg") == 0)
  {
    // Decompress the JPG into our own memory format
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(buf, bufsize, &jpg)) goto error;
    *buffer = (uint8_t *)dt_alloc_align(64, sizeof(uint8_t) * 4 * jpg.width * jpg.height);
    if(!*buffer) goto error;

    *width = jpg.width;
    *height = jpg.height;
    // TODO: check if the embedded thumbs have a color space set! currently we assume that it's always sRGB
    *color_space = DT_COLORSPACE_SRGB;
    if(dt_imageio_jpeg_decompress(&jpg, *buffer))
    {
      dt_free_align(*buffer);
      *buffer = NULL;
      goto error;
    }

    res = 0;
  }
  else
  {
#ifdef HAVE_GRAPHICSMAGICK
    ExceptionInfo exception;
    Image *image = NULL;
    ImageInfo *image_info = NULL;

    GetExceptionInfo(&exception);
    image_info = CloneImageInfo((ImageInfo *)NULL);

    image = BlobToImage(image_info, buf, bufsize, &exception);

    if(exception.severity != UndefinedException) CatchException(&exception);

    if(!image)
    {
      fprintf(stderr, "[dt_imageio_large_thumbnail GM] thumbnail not found?\n");
      goto error_gm;
    }

    *width = image->columns;
    *height = image->rows;
    *color_space = DT_COLORSPACE_SRGB; // FIXME: this assumes that embedded thumbnails are always srgb

    *buffer = (uint8_t *)dt_alloc_align(64, sizeof(uint8_t) * 4 * image->columns * image->rows);
    if(!*buffer) goto error_gm;

    for(uint32_t row = 0; row < image->rows; row++)
    {
      uint8_t *bufprt = *buffer + (size_t)4 * row * image->columns;
      int gm_ret = DispatchImage(image, 0, row, image->columns, 1, "RGBP", CharPixel, bufprt, &exception);

      if(exception.severity != UndefinedException) CatchException(&exception);

      if(gm_ret != MagickPass)
      {
        fprintf(stderr, "[dt_imageio_large_thumbnail GM] error_gm reading thumbnail\n");
        dt_free_align(*buffer);
        *buffer = NULL;
        goto error_gm;
      }
    }

    // fprintf(stderr, "[dt_imageio_large_thumbnail GM] successfully decoded thumbnail\n");
    res = 0;

  error_gm:
    if(image) DestroyImage(image);
    if(image_info) DestroyImageInfo(image_info);
    DestroyExceptionInfo(&exception);
    if(res) goto error;
#elif defined HAVE_IMAGEMAGICK
    MagickWand *image = NULL;
	MagickBooleanType mret;

    image = NewMagickWand();
	mret = MagickReadImageBlob(image, buf, bufsize);
    if(mret != MagickTrue)
    {
      fprintf(stderr, "[dt_imageio_large_thumbnail IM] thumbnail not found?\n");
      goto error_im;
    }

    *width = MagickGetImageWidth(image);
    *height = MagickGetImageHeight(image);
    switch(MagickGetImageColorspace(image)) {
    case sRGBColorspace:
      *color_space = DT_COLORSPACE_SRGB;
      break;
    default:
      fprintf(stderr,
          "[dt_imageio_large_thumbnail IM] could not map colorspace, using sRGB");
      *color_space = DT_COLORSPACE_SRGB;
      break;
    }

    *buffer = malloc(sizeof(uint8_t) * (*width) * (*height) * 4);
    if(*buffer == NULL) goto error_im;

    mret = MagickExportImagePixels(image, 0, 0, *width, *height, "RGBP", CharPixel, *buffer);
    if(mret != MagickTrue) {
      free(*buffer);
      *buffer = NULL;
      fprintf(stderr,
          "[dt_imageio_large_thumbnail IM] error while reading thumbnail\n");
      goto error_im;
    }

    res = 0;

error_im:
    DestroyMagickWand(image);
    if(res != 0) goto error;
#else
    fprintf(stderr,
      "[dt_imageio_large_thumbnail] error: The thumbnail image is not in "
      "JPEG format, and DT was built without neither GraphicsMagick or "
      "ImageMagick. Please rebuild DT with GraphicsMagick or ImageMagick "
      "support enabled.\n");
#endif
  }

  if(res)
  {
    fprintf(
        stderr,
        "[dt_imageio_large_thumbnail] error: Not a supported thumbnail image format or broken thumbnail: %s\n",
        mime_type);
    goto error;
  }

error:
  free(mime_type);
  free(buf);
  return res;
}

gboolean dt_imageio_has_mono_preview(const char *filename)
{
  dt_colorspaces_color_profile_type_t color_space;
  uint8_t *tmp = NULL;
  int32_t thumb_width = 0, thumb_height = 0;
  gboolean mono = FALSE;

  if(dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, &color_space))
    goto cleanup;
  if((thumb_width < 32) || (thumb_height < 32) || (tmp == NULL))
    goto cleanup;

  mono = TRUE;
  for(int y = 0; y < thumb_height; y++)
  {
    uint8_t *in = (uint8_t *)tmp + (size_t)4 * y * thumb_width;
    for(int x = 0; x < thumb_width; x++, in += 4)
    {
      if((in[0] != in[1]) || (in[0] != in[2]) || (in[1] != in[2]))
      {
        mono = FALSE;
        goto cleanup;
      }
    }
  }

  cleanup:

  dt_print(DT_DEBUG_IMAGEIO,"[dt_imageio_has_mono_preview] testing `%s', yes/no %i, %ix%i\n", filename, mono, thumb_width, thumb_height);
  if(tmp) dt_free_align(tmp);
  return mono;
}

void dt_imageio_flip_buffers(char *out, const char *in, const size_t bpp, const int wd, const int ht,
                             const int fwd, const int fht, const int stride,
                             const dt_image_orientation_t orientation)
{
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ht, wd, bpp, stride) \
    shared(in, out) \
    schedule(static)
#endif
    for(int j = 0; j < ht; j++) memcpy(out + (size_t)j * bpp * wd, in + (size_t)j * stride, bpp * wd);
    return;
  }
  int ii = 0, jj = 0;
  int si = bpp, sj = wd * bpp;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = bpp;
    si = ht * bpp;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(wd, bpp, ht, stride) \
  shared(in, out, jj, ii, sj, si) \
  schedule(static)
#endif
  for(int j = 0; j < ht; j++)
  {
    char *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + (size_t)sj * j;
    const char *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      memcpy(out2, in2, bpp);
      in2 += bpp;
      out2 += si;
    }
  }
}

void dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white,
                                          const int ch, const int wd, const int ht, const int fwd,
                                          const int fht, const int stride,
                                          const dt_image_orientation_t orientation)
{
  const float scale = 1.0f / (white - black);
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(wd, scale, black, ht, ch, stride) \
    shared(in, out) \
    schedule(static)
#endif
    for(int j = 0; j < ht; j++)
      for(int i = 0; i < wd; i++)
        for(int k = 0; k < ch; k++)
          out[4 * ((size_t)j * wd + i) + k] = (in[(size_t)j * stride + (size_t)ch * i + k] - black) * scale;
    return;
  }
  int ii = 0, jj = 0;
  int si = 4, sj = wd * 4;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = 4;
    si = ht * 4;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(wd, ch, scale, black, stride, ht) \
  shared(in, out, jj, ii, sj, si) \
  schedule(static)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + sj * j;
    const uint8_t *in2 = in + (size_t)stride * j;
    for(int i = 0; i < wd; i++)
    {
      for(int k = 0; k < ch; k++) out2[k] = (in2[k] - black) * scale;
      in2 += ch;
      out2 += si;
    }
  }
}

size_t dt_imageio_write_pos(int i, int j, int wd, int ht, float fwd, float fht,
                            dt_image_orientation_t orientation)
{
  int ii = i, jj = j, w = wd, fw = fwd, fh = fht;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    w = ht;
    ii = j;
    jj = i;
    fw = fht;
    fh = fwd;
  }
  if(orientation & ORIENTATION_FLIP_X) ii = (int)fw - ii - 1;
  if(orientation & ORIENTATION_FLIP_Y) jj = (int)fh - jj - 1;
  return (size_t)jj * w + ii;
}

dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf) return DT_IMAGEIO_OK;
  // needed to alloc correct buffer size:
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
  img->buf_dsc.cst = IOP_CS_RGB;
  dt_imageio_retval_t ret;
  dt_image_loader_t loader;
#ifdef HAVE_OPENEXR
  loader = LOADER_EXR;
  ret = dt_imageio_open_exr(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
#endif
  loader = LOADER_RGBE;
  ret = dt_imageio_open_rgbe(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  loader = LOADER_PFM;
  ret = dt_imageio_open_pfm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;

return_label:
  if(ret == DT_IMAGEIO_OK)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags |= DT_IMAGE_HDR;
    img->loader = loader;
  }
  return ret;
}

/* magic data: exclusion,offset,length, xx, yy, ...
    just add magic bytes to match to this struct
    to extend match on LDR formats.
*/
static const uint8_t _imageio_ldr_magic[] = {
  /* jpeg magics */
  0x00, 0x00, 0x02, 0xff, 0xd8, // SOI marker

#ifdef HAVE_OPENJPEG
  /* jpeg 2000, jp2 format */
  0x00, 0x00, 0x0c, 0x0,  0x0,  0x0,  0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A,

  /* jpeg 2000, j2k format */
  0x00, 0x00, 0x05, 0xFF, 0x4F, 0xFF, 0x51, 0x00,
#endif

  /* webp image */
  0x00, 0x08, 0x04, 'W', 'E', 'B', 'P',

  /* png image */
  0x00, 0x01, 0x03, 0x50, 0x4E, 0x47, // ASCII 'PNG'


  /* Canon CR2/CRW is like TIFF with additional magic numbers so must come
     before tiff as an exclusion */

  /* Most CR2 */
  0x01, 0x00, 0x0a, 0x49, 0x49, 0x2a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x43, 0x52,

  /* CR3 (ISO Media) */
  0x01, 0x00, 0x18, 0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'c', 'r', 'x', ' ', 0x00, 0x00, 0x00, 0x01, 'c', 'r', 'x', ' ', 'i', 's', 'o', 'm',

  // Older Canon RAW format with TIF Extension (i.e. 1Ds and 1D)
  0x01, 0x00, 0x0a, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x10, 0xba, 0xb0,

  // Older Canon RAW format with TIF Extension (i.e. D2000)
  0x01, 0x00, 0x0a, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00, 0x11, 0x34, 0x00, 0x04,

  // Older Canon RAW format with TIF Extension (i.e. DCS1)
  0x01, 0x00, 0x0a, 0x49, 0x49, 0x2a, 0x00, 0x00, 0x03, 0x00, 0x00, 0xff, 0x01,

  // Older Kodak RAW format with TIF Extension (i.e. DCS520C)
  0x01, 0x00, 0x0a, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00, 0x11, 0xa8, 0x00, 0x04,

  // Older Kodak RAW format with TIF Extension (i.e. DCS560C)
  0x01, 0x00, 0x0a, 0x4d, 0x4d, 0x00, 0x2a, 0x00, 0x00, 0x11, 0x76, 0x00, 0x04,

  // Older Kodak RAW format with TIF Extension (i.e. DCS460D)
  0x01, 0x00, 0x0a, 0x49, 0x49, 0x2a, 0x00, 0x00, 0x03, 0x00, 0x00, 0x7c, 0x01,

  /* IIQ raw images, may be either .IIQ, or .TIF */
  0x01, 0x08, 0x04, 0x49, 0x49, 0x49, 0x49,

  /* tiff image, intel */
  0x00, 0x00, 0x04, 0x4d, 0x4d, 0x00, 0x2a,

  /* tiff image, motorola */
  0x00, 0x00, 0x04, 0x49, 0x49, 0x2a, 0x00,

  /* binary NetPNM images: pbm, pgm and pbm */
  0x00, 0x00, 0x02, 0x50, 0x34,
  0x00, 0x00, 0x02, 0x50, 0x35,
  0x00, 0x00, 0x02, 0x50, 0x36
};

gboolean dt_imageio_is_ldr(const char *filename)
{
  FILE *fin = g_fopen(filename, "rb");
  if(fin)
  {
    size_t offset = 0;
    uint8_t block[32] = { 0 }; // keep this big enough for whatever magic size we want to compare to!
    /* read block from file */
    size_t s = fread(block, sizeof(block), 1, fin);
    fclose(fin);

    /* compare magic's */
    while(s)
    {
      if(_imageio_ldr_magic[offset + 2] > sizeof(block)
        || offset + 3 + _imageio_ldr_magic[offset + 2] > sizeof(_imageio_ldr_magic))
      {
        fprintf(stderr, "error: buffer in %s is too small!\n", __FUNCTION__);
        return FALSE;
      }
      if(memcmp(_imageio_ldr_magic + offset + 3, block + _imageio_ldr_magic[offset + 1],
                _imageio_ldr_magic[offset + 2]) == 0)
      {
        if(_imageio_ldr_magic[offset] == 0x01)
          return FALSE;
        else
          return TRUE;
      }
      offset += 3 + (_imageio_ldr_magic + offset)[2];

      /* check if finished */
      if(offset >= sizeof(_imageio_ldr_magic)) break;
    }
  }
  return FALSE;
}

int dt_imageio_is_hdr(const char *filename)
{
  const char *c = filename + strlen(filename);
  while(c > filename && *c != '.') c--;
  if(*c == '.')
    if(!strcasecmp(c, ".pfm") || !strcasecmp(c, ".hdr")
#ifdef HAVE_OPENEXR
       || !strcasecmp(c, ".exr")
#endif
      )
      return 1;
  return 0;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t dt_imageio_open_ldr(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf) return DT_IMAGEIO_OK;
  dt_imageio_retval_t ret;

  ret = dt_imageio_open_jpeg(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB; // jpeg is always RGB
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_JPEG;
    return ret;
  }

  ret = dt_imageio_open_tiff(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    // cst is set by dt_imageio_open_tiff()
    img->buf_dsc.filters = 0u;
    // TIFF can be HDR or LDR. corresponding flags are set in dt_imageio_open_tiff()
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->loader = LOADER_TIFF;
    return ret;
  }

#ifdef HAVE_WEBP
  ret = dt_imageio_open_webp(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB;
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_WEBP;
    return ret;
  }
#endif

  ret = dt_imageio_open_png(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB; // png is always RGB
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_PNG;
    return ret;
  }

#ifdef HAVE_OPENJPEG
  ret = dt_imageio_open_j2k(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB; // j2k is always RGB
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_J2K;
    return ret;
  }
#endif

  ret = dt_imageio_open_pnm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB; // pnm is always RGB
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_PNM;
    return ret;
  }

  return DT_IMAGEIO_LOAD_FAILED;
}

void dt_imageio_to_fractional(float in, uint32_t *num, uint32_t *den)
{
  if(!(in >= 0))
  {
    *num = *den = 0;
    return;
  }
  *den = 1;
  *num = (int)(in * *den + .5f);
  while(fabsf(*num / (float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in * *den + .5f);
  }
}

int dt_imageio_export(const int32_t imgid, const char *filename, dt_imageio_module_format_t *format,
                      dt_imageio_module_data_t *format_params, const gboolean high_quality, const gboolean upscale,
                      const gboolean copy_metadata, const gboolean export_masks,
                      dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                      dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                      dt_imageio_module_data_t *storage_params, int num, int total, dt_export_metadata_t *metadata)
{
  if(strcmp(format->mime(format_params), "x-copy") == 0)
    /* This is a just a copy, skip process and just export */
    return format->write_image(format_params, filename, NULL, icc_type, icc_filename, NULL, 0, imgid, num, total, NULL,
                               export_masks);
  else
  {
    const gboolean is_scaling =
      dt_conf_is_equal("plugins/lighttable/export/resizing", "scaling");

    return dt_imageio_export_with_flags(imgid, filename, format, format_params, FALSE, FALSE, high_quality, upscale, is_scaling,
                                        FALSE, NULL, copy_metadata, export_masks, icc_type, icc_filename, icc_intent,
                                        storage, storage_params, num, total, metadata, -1);
  }
}

// internal function: to avoid exif blob reading + 8-bit byteorder flag + high-quality override
int dt_imageio_export_with_flags(const int32_t imgid, const char *filename,
                                 dt_imageio_module_format_t *format, dt_imageio_module_data_t *format_params,
                                 const gboolean ignore_exif, const gboolean display_byteorder,
                                 const gboolean high_quality, const gboolean upscale, gboolean is_scaling, const gboolean thumbnail_export,
                                 const char *filter, const gboolean copy_metadata, const gboolean export_masks,
                                 dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                                 dt_iop_color_intent_t icc_intent, dt_imageio_module_storage_t *storage,
                                 dt_imageio_module_data_t *storage_params, int num, int total,
                                 dt_export_metadata_t *metadata, const int history_end)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  if(history_end != -1)
    dt_dev_pop_history_items_ext(&dev, history_end);

  const gboolean buf_is_downscaled = (thumbnail_export && dt_conf_get_bool("ui/performance"));
  dt_mipmap_buffer_t buf;
  if(buf_is_downscaled)
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_F, DT_MIPMAP_BLOCKING, 'r');
  else
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  const dt_image_t *img = &dev.image_storage;

  if(!buf.buf || !buf.width || !buf.height)
  {
    fprintf(stderr, "[dt_imageio_export_with_flags] mipmap allocation for `%s' failed\n", filename);
    dt_control_log(_("image `%s' is not available!"), img->filename);
    goto error_early;
  }

  const int wd = img->width;
  const int ht = img->height;

  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_t pipe;
  gboolean res = thumbnail_export
    ? dt_dev_pixelpipe_init_thumbnail(&pipe, wd, ht)
    : dt_dev_pixelpipe_init_export(&pipe, wd, ht, format->levels(format_params), export_masks);
  if(!res)
  {
    dt_control_log(
      _("failed to allocate memory for %s, please lower the threads used for export or buy more memory."),
      thumbnail_export ? C_("noun", "thumbnail export") : C_("noun", "export"));
    goto error;
  }

  const int final_history_end = history_end == -1 ? dev.history_end : history_end;
  const gboolean use_style = !thumbnail_export && format_params->style[0] != '\0';
  const gboolean appending = format_params->style_append != FALSE;
  //  If a style is to be applied during export, add the iop params into the history
  if(use_style)
  {
    GList *style_items = dt_styles_get_item_list(format_params->style, TRUE, -1, TRUE);
    if(!style_items)
    {
      dt_control_log(_("cannot find the style '%s' to apply during export."), format_params->style);
      goto error;
    }

    GList *modules_used = NULL;

    if(!appending) dt_dev_pop_history_items_ext(&dev, 0);

    dt_ioppr_update_for_style_items(&dev, style_items, appending);

    for(GList *st_items = style_items; st_items; st_items = g_list_next(st_items))
    {
      dt_style_item_t *st_item = (dt_style_item_t *)st_items->data;
      dt_styles_apply_style_item(&dev, st_item, &modules_used, appending);
    }

    g_list_free(modules_used);
    g_list_free_full(style_items, dt_style_item_free);
  }
  else if(history_end != -1)
    dt_dev_pop_history_items_ext(&dev, final_history_end);

  dt_ioppr_resync_modules_order(&dev);

  dt_dev_pixelpipe_set_icc(&pipe, icc_type, icc_filename, icc_intent);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf, buf.width, buf.height, buf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    fprintf(stderr,"[dt_imageio_export_with_flags] ");
    if(use_style)
    {
      if(appending) fprintf(stderr,"appending style `%s'\n", format_params->style);
      else          fprintf(stderr,"overwrite style `%s'\n", format_params->style);
    }
    else fprintf(stderr,"\n");
    int cnt = 0;
    for(GList *nodes = pipe.nodes; nodes; nodes = g_list_next(nodes))
    {
      dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
      if(piece->enabled)
      {
        cnt++;
        fprintf(stderr," %s", piece->module->op);
      }
    }
    fprintf(stderr," (%i)\n", cnt);
  }

  if(filter)
  {
    if(!strncmp(filter, "pre:", 4)) dt_dev_pixelpipe_disable_after(&pipe, filter + 4);
    if(!strncmp(filter, "post:", 5)) dt_dev_pixelpipe_disable_before(&pipe, filter + 5);
  }

  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                                  &pipe.processed_height);

  dt_show_times(&start, "[export] creating pixelpipe");

  // find output color profile for this image:
  int sRGB = 1;
  if(icc_type == DT_COLORSPACE_SRGB)
  {
    sRGB = 1;
  }
  else if(icc_type == DT_COLORSPACE_NONE)
  {
    dt_iop_module_t *colorout = NULL;
    for(GList *modules = dev.iop; modules; modules = g_list_next(modules))
    {
      colorout = (dt_iop_module_t *)modules->data;
      if(colorout->get_p && strcmp(colorout->op, "colorout") == 0)
      {
        const dt_colorspaces_color_profile_type_t *type = colorout->get_p(colorout->params, "type");
        sRGB = (!type || *type == DT_COLORSPACE_SRGB);
        break; // colorout can't have > 1 instance
      }
    }
  }
  else
  {
    sRGB = 0;
  }

  // if is_scaling is used don't override high_quality
  // get only once at the beginning, in case the user changes it on the way:
  const gboolean high_quality_processing
      = ((format_params->max_width == 0 || format_params->max_width >= pipe.processed_width)
         && (format_params->max_height == 0 || format_params->max_height >= pipe.processed_height)
         && !is_scaling)
            ? FALSE
            : high_quality;

  /* The pipeline might have out-of-bounds problems at the right and lower borders leading to
     artifacts or mem access errors if ignored. (#3646)
     It's very difficult to prepare the pipeline avoiding this **and** not introducing artifacts.
     But we can test for that situation and if there is an out-of-bounds problem we
     have basically two options:
     a) reduce the output image size by one for width & height.
     b) increase the scale while keeping the output size. In theory this marginally reduces quality.

     These are the rules for export:
     1. If we have the **full image** (defined by dt_image_t width, height and crops) we look for upscale.
        If this is off use a), if on use b)
     2. If we have defined format_params->max_width or/and height we use b)
     3. Thumbnails are defined as in 2 so use b)
     4. Cropped images are detected and use b)
     5. Upscaled images use b)
     6. Rotating by +-90° does not change the output size.
     7. Never generate images larger than requested.
  */

  const gboolean iscropped =
    (   (pipe.processed_width < (wd - img->crop_x - img->crop_width))
     || (pipe.processed_height < (ht - img->crop_y - img->crop_height)));

  const gboolean exact_size =
         iscropped
      || upscale
      || (format_params->max_width != 0)
      || (format_params->max_height != 0)
      || thumbnail_export;

  int width = format_params->max_width > 0 ? format_params->max_width : 0;
  int height = format_params->max_height > 0 ? format_params->max_height : 0;

  if(iscropped && !thumbnail_export && width == 0 && height == 0)
  {
    width = pipe.processed_width;
    height = pipe.processed_height;
  }

  const double max_scale = (upscale && (width > 0 || height > 0)) ? 100.0 : 1.0;

  const double scalex = width > 0 ? fmin((double)width / (double)pipe.processed_width, max_scale) : max_scale;
  const double scaley = height > 0 ? fmin((double)height / (double)pipe.processed_height, max_scale) : max_scale;
  double scale = fmin(scalex, scaley);
  double corrscale = 1.0f;

  int processed_width = 0;
  int processed_height = 0;

  gboolean corrected = FALSE;
  float origin[] = { 0.0f, 0.0f };

  if(dt_dev_distort_backtransform_plus(&dev, &pipe, 0.f, DT_DEV_TRANSFORM_DIR_ALL, origin, 1))
  {
    if((width == 0) && exact_size)
      width = pipe.processed_width;
    if((height == 0) && exact_size)
      height = pipe.processed_height;

    scale = fmin(width >  0 ? fmin((double)width / (double)pipe.processed_width, max_scale) : max_scale,
                 height > 0 ? fmin((double)height / (double)pipe.processed_height, max_scale) : max_scale);

    if(is_scaling)
    {
      // scaling
      double _num, _denum;
      dt_imageio_resizing_factor_get_and_parsing(&_num, &_denum);

      const double scale_factor = _num / _denum;

      if(!thumbnail_export)
      {
        scale = fmin(scale_factor, max_scale);
      }
    }

    processed_width = scale * pipe.processed_width + 0.8f;
    processed_height = scale * pipe.processed_height + 0.8f;

    if((ceil((double)processed_width / scale) + origin[0] > pipe.iwidth) ||
       (ceil((double)processed_height / scale) + origin[1] > pipe.iheight))
    {
      corrected = TRUE;
     /* Here the scale is too **small** so while reading data from the right or low borders we are out-of-bounds.
        We can either just decrease output width & height or
        have to find a scale that takes data from within the origin data, so we have to increase scale to a size
        that fits both width & height.
     */
      if(exact_size)
      {
        corrscale = fmax( ((double)(pipe.processed_width + 1) / (double)(pipe.processed_width)),
                           ((double)(pipe.processed_height +1) / (double)(pipe.processed_height)) );
        scale = scale * corrscale;
      }
      else
      {
        processed_width--;
        processed_height--;
      }
    }

    dt_print(DT_DEBUG_IMAGEIO,"[dt_imageio_export] imgid %d, pipe %ix%i, range %ix%i --> exact %i, upscale %i, hq %i, corrected %i, scale %.7f, corr %.6f, size %ix%i\n",
             imgid, pipe.processed_width, pipe.processed_height, format_params->max_width, format_params->max_height,
             exact_size, upscale, high_quality_processing, corrected, scale, corrscale, processed_width, processed_height);
  }
  else
  {
    processed_width = floor(scale * pipe.processed_width);
    processed_height = floor(scale * pipe.processed_height);
    dt_print(DT_DEBUG_IMAGEIO,"[dt_imageio_export] (direct) imgid %d, hq %i, pipe %ix%i, range %ix%i --> size %ix%i / %ix%i\n",
             imgid, high_quality_processing, pipe.processed_width, pipe.processed_height, format_params->max_width, format_params->max_height,
             processed_width, processed_height, width, height);
  }

  const int bpp = format->bpp(format_params);

  dt_get_times(&start);
  if(high_quality_processing)
  {
    /*
     * if high quality processing was requested, downsampling will be done
     * at the very end of the pipe (just before border and watermark)
     */
    dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
  }
  else
  {
    // else, downsampling will be right after demosaic

    // so we need to turn temporarily disable in-pipe late downsampling iop.

    // find the finalscale module
    dt_dev_pixelpipe_iop_t *finalscale = NULL;
    {
      for(const GList *nodes = g_list_last(pipe.nodes); nodes; nodes = g_list_previous(nodes))
      {
        dt_dev_pixelpipe_iop_t *node = (dt_dev_pixelpipe_iop_t *)(nodes->data);
        if(!strcmp(node->module->op, "finalscale"))
        {
          finalscale = node;
          break;
        }
      }
    }

    if(finalscale) finalscale->enabled = 0;

    // do the processing (8-bit with special treatment, to make sure we can use openmp further down):
    if(bpp == 8)
      dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
    else
      dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);

    if(finalscale) finalscale->enabled = 1;
  }
  dt_show_times(&start, thumbnail_export ? "[dev_process_thumbnail] pixel pipeline processing"
                                         : "[dev_process_export] pixel pipeline processing");

  uint8_t *outbuf = pipe.backbuf;
  if(outbuf == NULL)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dt_imageio_export_with_flags] no valid output buffer\n");
    goto error;
  }

  // downconversion to low-precision formats:
  if(bpp == 8)
  {
    if(display_byteorder)
    {
      if(high_quality_processing)
      {
        const float *const inbuf = (float *)outbuf;
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          // convert in place, this is unfortunately very serial..
          const uint8_t r = roundf(CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff));
          const uint8_t g = roundf(CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff));
          const uint8_t b = roundf(CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff));
          outbuf[4 * k + 0] = r;
          outbuf[4 * k + 1] = g;
          outbuf[4 * k + 2] = b;
        }
      }
      // else processing output was 8-bit already, and no need to swap order
    }
    else // need to flip
    {
      // ldr output: char
      if(high_quality_processing)
      {
        const float *const inbuf = (float *)outbuf;
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          // convert in place, this is unfortunately very serial..
          const uint8_t r = roundf(CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff));
          const uint8_t g = roundf(CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff));
          const uint8_t b = roundf(CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff));
          outbuf[4 * k + 0] = r;
          outbuf[4 * k + 1] = g;
          outbuf[4 * k + 2] = b;
        }
      }
      else
      { // !display_byteorder, need to swap:
        uint8_t *const buf8 = pipe.backbuf;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(processed_width, processed_height, buf8) \
  schedule(static)
#endif
        // just flip byte order
        for(size_t k = 0; k < (size_t)processed_width * processed_height; k++)
        {
          uint8_t tmp = buf8[4 * k + 0];
          buf8[4 * k + 0] = buf8[4 * k + 2];
          buf8[4 * k + 2] = tmp;
        }
      }
    }
  }
  else if(bpp == 16)
  {
    // uint16_t per color channel
    float *buff = (float *)outbuf;
    uint16_t *buf16 = (uint16_t *)outbuf;
    for(int y = 0; y < processed_height; y++)
      for(int x = 0; x < processed_width; x++)
      {
        // convert in place
        const size_t k = (size_t)processed_width * y + x;
        for(int i = 0; i < 3; i++) buf16[4 * k + i] = roundf(CLAMP(buff[4 * k + i] * 0xffff, 0, 0xffff));
      }
  }
  // else output float, no further harm done to the pixels :)

  format_params->width = processed_width;
  format_params->height = processed_height;

  if(!ignore_exif)
  {
    uint8_t *exif_profile = NULL; // Exif data should be 65536 bytes max, but if original size is close to that,
                                  // adding new tags could make it go over that... so let it be and see what
                                  // happens when we write the image
    char pathname[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);
    // last param is dng mode, it's false here
    const int length = dt_exif_read_blob(&exif_profile, pathname, imgid, sRGB, processed_width, processed_height, 0);

    res = format->write_image(format_params, filename, outbuf, icc_type, icc_filename, exif_profile, length, imgid,
                              num, total, &pipe, export_masks);

    free(exif_profile);
  }
  else
  {
    res = format->write_image(format_params, filename, outbuf, icc_type, icc_filename, NULL, 0, imgid, num, total,
                              &pipe, export_masks);
  }

  if(res)
    goto error;

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  /* now write xmp into that container, if possible */
  if(copy_metadata && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP))
  {
    dt_exif_xmp_attach_export(imgid, filename, metadata);
    // no need to cancel the export if this fail
  }

  if(!thumbnail_export && strcmp(format->mime(format_params), "memory")
    && !(format->flags(format_params) & FORMAT_FLAGS_NO_TMPFILE))
  {
#ifdef USE_LUA
    //Synchronous calling of lua intermediate-export-image events
    dt_lua_lock();

    lua_State *L = darktable.lua_state.state;

    luaA_push(L, dt_lua_image_t, &imgid);

    lua_pushstring(L, filename);

    luaA_push_type(L, format->parameter_lua_type, format_params);

    if(storage)
      luaA_push_type(L, storage->parameter_lua_type, storage_params);
    else
      lua_pushnil(L);

    dt_lua_event_trigger(L, "intermediate-export-image", 4);

    dt_lua_unlock();
#endif

    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGE_EXPORT_TMPFILE, imgid, filename, format,
                            format_params, storage, storage_params);
  }

  return 0; // success

error:
  dt_dev_pixelpipe_cleanup(&pipe);
error_early:
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  return 1;
}


// fallback read method in case file could not be opened yet.
// use GraphicsMagick (if supported) to read exotic LDRs
dt_imageio_retval_t dt_imageio_open_exotic(dt_image_t *img, const char *filename,
                                           dt_mipmap_buffer_t *buf)
{
  // if buf is NULL, don't proceed
  if(!buf) return DT_IMAGEIO_OK;
#ifdef HAVE_GRAPHICSMAGICK
  dt_imageio_retval_t ret = dt_imageio_open_gm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.cst = IOP_CS_RGB;
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_S_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_GM;
    return ret;
  }
#elif HAVE_IMAGEMAGICK
  dt_imageio_retval_t ret = dt_imageio_open_im(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_IM;
    return ret;
  }
#endif

  return DT_IMAGEIO_LOAD_FAILED;
}

void dt_imageio_update_monochrome_workflow_tag(int32_t id, int mask)
{
  if(mask & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_PREVIEW | DT_IMAGE_MONOCHROME_BAYER))
  {
    guint tagid = 0;
    char tagname[64];
    snprintf(tagname, sizeof(tagname), "darktable|mode|monochrome");
    dt_tag_new(tagname, &tagid);
    dt_tag_attach(tagid, id, FALSE, FALSE);
  }
  else
    dt_tag_detach_by_string("darktable|mode|monochrome", id, FALSE, FALSE);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

void dt_imageio_set_hdr_tag(dt_image_t *img)
{
  guint tagid = 0;
  char tagname[64];
  snprintf(tagname, sizeof(tagname), "darktable|mode|hdr");
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, img->id, FALSE, FALSE);
  img->flags |= DT_IMAGE_HDR;
  img->flags &= ~DT_IMAGE_LDR;
}

// =================================================
//   combined reading
// =================================================

dt_imageio_retval_t dt_imageio_open(dt_image_t *img,               // non-const * means you hold a write lock!
                                    const char *filename,          // full path
                                    dt_mipmap_buffer_t *buf)
{
  /* first of all, check if file exists, don't bother to test loading if not exists */
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR)) return !DT_IMAGEIO_OK;
  const int32_t was_hdr = (img->flags & DT_IMAGE_HDR);
  const int32_t was_bw = dt_image_monochrome_flags(img);

  dt_imageio_retval_t ret = DT_IMAGEIO_LOAD_FAILED;
  img->loader = LOADER_UNKNOWN;

  /* check if file is ldr using magic's */
  if(dt_imageio_is_ldr(filename)) ret = dt_imageio_open_ldr(img, filename, buf);

#ifdef HAVE_LIBJXL
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_jpegxl(img, filename, buf);
#endif

#ifdef HAVE_LIBAVIF
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_avif(img, filename, buf);
#endif

#ifdef HAVE_LIBHEIF
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_heif(img, filename, buf);
#endif

  /* silly check using file extensions: */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL && dt_imageio_is_hdr(filename))
    ret = dt_imageio_open_hdr(img, filename, buf);

  /* use rawspeed to load the raw */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
  {
    ret = dt_imageio_open_rawspeed(img, filename, buf);
  }

  /* fallback that tries to open file via LibRAW to support Canon CR3 */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_libraw(img, filename, buf);

  /* fallback that tries to open file via GraphicsMagick */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_exotic(img, filename, buf);

  if((ret == DT_IMAGEIO_OK) && !was_hdr && (img->flags & DT_IMAGE_HDR))
    dt_imageio_set_hdr_tag(img);

  if((ret == DT_IMAGEIO_OK) && (was_bw != dt_image_monochrome_flags(img)))
    dt_imageio_update_monochrome_workflow_tag(img->id, dt_image_monochrome_flags(img));

  img->p_width = img->width - img->crop_x - img->crop_width;
  img->p_height = img->height - img->crop_y - img->crop_height;

  return ret;
}

gboolean dt_imageio_lookup_makermodel(const char *maker, const char *model,
                                      char *mk, int mk_len, char *md, int md_len,
                                      char *al, int al_len)
{
  // At this stage, we can't tell which loader is used to open the image.
  gboolean found = dt_rawspeed_lookup_makermodel(maker, model,
                                                 mk, mk_len,
                                                 md, md_len,
                                                 al, al_len);
  if(found == FALSE)
  {
    // Special handling for CR3 raw files via libraw
    found = dt_libraw_lookup_makermodel(maker, model,
                                        mk, mk_len,
                                        md, md_len,
                                        al, al_len);
  }
  return found;
}

typedef struct _imageio_preview_t
{
  dt_imageio_module_data_t head;
  int bpp;
  uint8_t *buf;
  uint32_t width, height;
} _imageio_preview_t;

static int _preview_write_image(dt_imageio_module_data_t *data,
                                const char *filename, const void *in,
                                dt_colorspaces_color_profile_type_t over_type,
                                const char *over_filename,
                                void *exif, int exif_len, int imgid, int num, int total,
                                dt_dev_pixelpipe_t*pipe,
                                const gboolean export_masks)
{
  _imageio_preview_t *d = (_imageio_preview_t *)data;

  memcpy(d->buf, in, sizeof(uint32_t) * data->width * data->height);
  d->width = data->width;
  d->height = data->height;

  return 0;
}

static int _preview_bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int _preview_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *_preview_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

cairo_surface_t *dt_imageio_preview(const int32_t imgid,
                                    const size_t width,
                                    const size_t height,
                                    const int history_end,
                                    const char *style_name)
{
  dt_imageio_module_format_t buf;
  buf.mime = _preview_mime;
  buf.levels = _preview_levels;
  buf.bpp = _preview_bpp;
  buf.write_image = _preview_write_image;

  _imageio_preview_t dat;
  dat.head.max_width = width;
  dat.head.max_height = height;
  dat.head.width = width;
  dat.head.height = height;
  dat.head.style_append = TRUE;
  dat.bpp = 8;
  dat.buf = (uint8_t *)dt_alloc_align(64, sizeof(uint32_t) * width * height);

  g_strlcpy(dat.head.style, style_name, sizeof(dat.head.style));

  const gboolean high_quality = FALSE;
  const gboolean upscale = TRUE;
  const gboolean export_masks = FALSE;
  const gboolean is_scaling = FALSE;

  dt_imageio_export_with_flags
    (imgid, "preview", &buf, (dt_imageio_module_data_t *)&dat, TRUE, TRUE,
     high_quality, upscale, is_scaling, FALSE, NULL, FALSE, export_masks,
     DT_COLORSPACE_DISPLAY, NULL, DT_INTENT_LAST, NULL, NULL, 1, 1, NULL,
     history_end);

  const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, dat.head.width);
  cairo_surface_t *surface = dt_cairo_image_surface_create_for_data
        (dat.buf, CAIRO_FORMAT_RGB24, dat.head.width, dat.head.height, stride);

  return surface;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
