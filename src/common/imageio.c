/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2010--2011 henrik andersson.

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
#include "common/image_compression.h"
#include "common/imageio_gm.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_pfm.h"
#include "common/imageio_png.h"
#include "common/imageio_pnm.h"
#include "common/imageio_rawspeed.h"
#include "common/imageio_rgbe.h"
#include "common/imageio_tiff.h"
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
    *buffer = (uint8_t *)malloc((size_t)sizeof(uint8_t) * jpg.width * jpg.height * 4);
    if(!*buffer) goto error;

    *width = jpg.width;
    *height = jpg.height;
    // TODO: check if the embedded thumbs have a color space set! currently we assume that it's always sRGB
    *color_space = DT_COLORSPACE_SRGB;
    if(dt_imageio_jpeg_decompress(&jpg, *buffer))
    {
      free(*buffer);
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

    *buffer = (uint8_t *)malloc((size_t)sizeof(uint8_t) * image->columns * image->rows * 4);
    if(!*buffer) goto error_gm;

    for(uint32_t row = 0; row < image->rows; row++)
    {
      uint8_t *bufprt = *buffer + (size_t)4 * row * image->columns;
      int gm_ret = DispatchImage(image, 0, row, image->columns, 1, "RGBP", CharPixel, bufprt, &exception);

      if(exception.severity != UndefinedException) CatchException(&exception);

      if(gm_ret != MagickPass)
      {
        fprintf(stderr, "[dt_imageio_large_thumbnail GM] error_gm reading thumbnail\n");
        free(*buffer);
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
#else
    fprintf(stderr, "[dt_imageio_large_thumbnail] error: The thumbnail image is not in JPEG format, but DT "
                    "was built without GraphicsMagick. Please rebuild DT with GraphicsMagick support "
                    "enabled.\n");
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

void dt_imageio_flip_buffers(char *out, const char *in, const size_t bpp, const int wd, const int ht,
                             const int fwd, const int fht, const int stride,
                             const dt_image_orientation_t orientation)
{
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
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
  if(orientation & ORIENTATION_FLIP_X)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si)
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

void dt_imageio_flip_buffers_ui16_to_float(float *out, const uint16_t *in, const float black,
                                           const float white, const int ch, const int wd, const int ht,
                                           const int fwd, const int fht, const int stride,
                                           const dt_image_orientation_t orientation)
{
  const float scale = 1.0f / (white - black);
  if(!orientation)
  {
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
    for(int j = 0; j < ht; j++)
      for(int i = 0; i < wd; i++)
        for(int k = 0; k < ch; k++)
          out[4 * ((size_t)j * wd + i) + k] = (in[ch * ((size_t)j * stride + i) + k] - black) * scale;
    return;
  }
  int ii = 0, jj = 0;
  int si = 4, sj = wd * 4;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = 4;
    si = ht * 4;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *out2 = out + (size_t)labs(sj) * jj + (size_t)labs(si) * ii + (size_t)sj * j;
    const uint16_t *in2 = in + stride * j;
    for(int i = 0; i < wd; i++)
    {
      for(int k = 0; k < ch; k++) out2[k] = (in2[k] - black) * scale;
      in2 += ch;
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
#pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
    for(int j = 0; j < ht; j++)
      for(int i = 0; i < wd; i++)
        for(int k = 0; k < ch; k++)
          out[4 * ((size_t)j * wd + i) + k] = (in[(size_t)j * stride + ch * i + k] - black) * scale;
    return;
  }
  int ii = 0, jj = 0;
  int si = 4, sj = wd * 4;
  if(orientation & ORIENTATION_SWAP_XY)
  {
    sj = 4;
    si = ht * 4;
  }
  if(orientation & ORIENTATION_FLIP_X)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & ORIENTATION_FLIP_Y)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si)
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
  // needed to alloc correct buffer size:
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;
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
    img->flags |= DT_IMAGE_HDR;
    img->loader = loader;
  }
  return ret;
}

/* magic data: exclusion,offset,length, xx, yy, ...
    just add magic bytes to match to this struct
    to extend mathc on ldr formats.
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
  size_t offset = 0;
  uint8_t block[32] = { 0 }; // keep this big enough for whatever magic size we want to compare to!
  FILE *fin = g_fopen(filename, "rb");
  if(fin)
  {
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
  dt_imageio_retval_t ret;

  ret = dt_imageio_open_jpeg(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_JPEG;
    return ret;
  }

  ret = dt_imageio_open_tiff(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_TIFF;
    return ret;
  }

  ret = dt_imageio_open_png(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_PNG;
    return ret;
  }

#ifdef HAVE_OPENJPEG
  ret = dt_imageio_open_j2k(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_J2K;
    return ret;
  }
#endif

  ret = dt_imageio_open_pnm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_PNM;
    return ret;
  }

  return DT_IMAGEIO_FILE_CORRUPTED;
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

int dt_imageio_export(const uint32_t imgid, const char *filename, dt_imageio_module_format_t *format,
                      dt_imageio_module_data_t *format_params, const gboolean high_quality, const gboolean upscale,
                      const gboolean copy_metadata, dt_colorspaces_color_profile_type_t icc_type,
                      const gchar *icc_filename, dt_iop_color_intent_t icc_intent,
                      dt_imageio_module_storage_t *storage, dt_imageio_module_data_t *storage_params, int num,
                      int total)
{
  if(strcmp(format->mime(format_params), "x-copy") == 0)
    /* This is a just a copy, skip process and just export */
    return format->write_image(format_params, filename, NULL, icc_type, icc_filename, NULL, 0, imgid, num, total);
  else
    return dt_imageio_export_with_flags(imgid, filename, format, format_params, 0, 0, high_quality, upscale,
                                        0, NULL, copy_metadata, icc_type, icc_filename, icc_intent, storage,
                                        storage_params, num, total);
}

// internal function: to avoid exif blob reading + 8-bit byteorder flag + high-quality override
int dt_imageio_export_with_flags(const uint32_t imgid, const char *filename,
                                 dt_imageio_module_format_t *format, dt_imageio_module_data_t *format_params,
                                 const int32_t ignore_exif, const int32_t display_byteorder,
                                 const gboolean high_quality, const gboolean upscale, const int32_t thumbnail_export,
                                 const char *filter, const gboolean copy_metadata,
                                 dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename,
                                 dt_iop_color_intent_t icc_intent,
                                 dt_imageio_module_storage_t *storage,
                                 dt_imageio_module_data_t *storage_params, int num, int total)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, imgid);

  const int buf_is_downscaled
      = (thumbnail_export && dt_conf_get_bool("plugins/lighttable/low_quality_thumbnails"));

  dt_mipmap_buffer_t buf;
  if(buf_is_downscaled)
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_F, DT_MIPMAP_BLOCKING, 'r');
  else
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  const dt_image_t *img = &dev.image_storage;

  if(!buf.buf || !buf.width || !buf.height)
  {
    fprintf(stderr, "allocation failed???\n");
    dt_control_log(_("image `%s' is not available!"), img->filename);
    goto error_early;
  }

  const int wd = img->width;
  const int ht = img->height;
  const float max_scale = upscale ? 100.0 : 1.0;

  int res = 0;

  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_t pipe;
  res = thumbnail_export ? dt_dev_pixelpipe_init_thumbnail(&pipe, wd, ht)
                         : dt_dev_pixelpipe_init_export(&pipe, wd, ht, format->levels(format_params));
  if(!res)
  {
    dt_control_log(
        _("failed to allocate memory for %s, please lower the threads used for export or buy more memory."),
        thumbnail_export ? C_("noun", "thumbnail export") : C_("noun", "export"));
    goto error;
  }

  //  If a style is to be applied during export, add the iop params into the history
  if(!thumbnail_export && format_params->style[0] != '\0')
  {
    GList *style_items = dt_styles_get_item_list(format_params->style, TRUE, -1);
    if(!style_items)
    {
      dt_control_log(_("cannot find the style '%s' to apply during export."), format_params->style);
      goto error;
    }

    // remove everything above history_end
    GList *history = g_list_nth(dev.history, dev.history_end);
    while(history)
    {
      GList *next = g_list_next(history);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      free(hist->params);
      free(hist->blend_params);
      free(history->data);
      dev.history = g_list_delete_link(dev.history, history);
      history = next;
    }

    // Add each params
    for(GList *iter = style_items; iter; iter = g_list_next(iter))
    {
      dt_style_item_t *s = (dt_style_item_t *)iter->data;

      for(GList *module = dev.iop; module; module = g_list_next(module))
      {
        dt_iop_module_t *m = (dt_iop_module_t *)module->data;

        if(!strcmp(m->op, s->operation))
        {
          dt_dev_history_item_t *h = malloc(sizeof(dt_dev_history_item_t));
          dt_iop_module_t *style_module = m;

          if((format_params->style_append && !(m->flags() & IOP_FLAGS_ONE_INSTANCE)) || m->multi_priority != s->multi_priority)
          {
            // dt_dev_module_duplicate() doesn't work here, it's trying too hard to be clever
            style_module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
            if(style_module && !dt_iop_load_module(style_module, m->so, m->dev))
            {
              style_module->instance = m->instance;
              style_module->multi_priority = s->multi_priority;
              snprintf(style_module->multi_name, sizeof(style_module->multi_name), "%s", s->name);
              dev.iop = g_list_insert_sorted(dev.iop, style_module, sort_plugins);
            }
            else
            {
              free(h);
              goto error;
            }
          }

          h->params = s->params;
          h->blend_params = s->blendop_params;
          h->enabled = s->enabled;
          h->module = style_module;
          h->multi_priority = s->multi_priority;
          g_strlcpy(h->multi_name, s->name, sizeof(h->multi_name));
          g_strlcpy(h->op_name, m->op, sizeof(h->op_name));

          if(m->legacy_params && (s->module_version != m->version()))
          {
            void *new_params = malloc(m->params_size);
            m->legacy_params(m, h->params, s->module_version, new_params, labs(m->version()));

            free(h->params);
            h->params = new_params;
          }

          dev.history_end++;
          dev.history = g_list_append(dev.history, h);

          // make sure that dt_style_item_free doesn't free data we still use
          s->params = NULL;
          s->blendop_params = NULL;

          break;
        }
      }
    }
    g_list_free_full(style_items, dt_style_item_free);
  }

  dt_dev_pixelpipe_set_icc(&pipe, icc_type, icc_filename, icc_intent);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf, buf.width, buf.height, buf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  if(filter)
  {
    if(!strncmp(filter, "pre:", 4)) dt_dev_pixelpipe_disable_after(&pipe, filter + 4);
    if(!strncmp(filter, "post:", 5)) dt_dev_pixelpipe_disable_before(&pipe, filter + 5);
  }

  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width,
                                  &pipe.processed_height);

  dt_show_times(&start, "[export] creating pixelpipe", NULL);

  // find output color profile for this image:
  int sRGB = 1;
  if(icc_type == DT_COLORSPACE_SRGB)
  {
    sRGB = 1;
  }
  else if(icc_type == DT_COLORSPACE_NONE)
  {
    GList *modules = dev.iop;
    dt_iop_module_t *colorout = NULL;
    while(modules)
    {
      colorout = (dt_iop_module_t *)modules->data;
      if(colorout->get_p && strcmp(colorout->op, "colorout") == 0)
      {
        const dt_colorspaces_color_profile_type_t *type = colorout->get_p(colorout->params, "type");
        sRGB = (!type || *type == DT_COLORSPACE_SRGB);
        break; // colorout can't have > 1 instance
      }
      modules = g_list_next(modules);
    }
  }
  else
  {
    sRGB = 0;
  }

  // get only once at the beginning, in case the user changes it on the way:
  const gboolean high_quality_processing
      = ((format_params->max_width == 0 || format_params->max_width >= pipe.processed_width)
         && (format_params->max_height == 0 || format_params->max_height >= pipe.processed_height))
            ? FALSE
            : high_quality;

  const int width = format_params->max_width;
  const int height = format_params->max_height;
  const double scalex = width > 0 ? fminf(width / (double)pipe.processed_width, max_scale) : 1.0;
  const double scaley = height > 0 ? fminf(height / (double)pipe.processed_height, max_scale) : 1.0;
  const double scale = fminf(scalex, scaley);

  const int processed_width = scale * pipe.processed_width + .5f;
  const int processed_height = scale * pipe.processed_height + .5f;

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
      GList *nodes = g_list_last(pipe.nodes);
      while(nodes)
      {
        dt_dev_pixelpipe_iop_t *node = (dt_dev_pixelpipe_iop_t *)(nodes->data);
        if(!strcmp(node->module->op, "finalscale"))
        {
          finalscale = node;
          break;
        }
        nodes = g_list_previous(nodes);
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
                                         : "[dev_process_export] pixel pipeline processing",
                NULL);

  uint8_t *outbuf = pipe.backbuf;

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
          const uint8_t r = CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff);
          const uint8_t g = CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff);
          const uint8_t b = CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff);
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
          const uint8_t r = CLAMP(inbuf[4 * k + 0] * 0xff, 0, 0xff);
          const uint8_t g = CLAMP(inbuf[4 * k + 1] * 0xff, 0, 0xff);
          const uint8_t b = CLAMP(inbuf[4 * k + 2] * 0xff, 0, 0xff);
          outbuf[4 * k + 0] = r;
          outbuf[4 * k + 1] = g;
          outbuf[4 * k + 2] = b;
        }
      }
      else
      { // !display_byteorder, need to swap:
        uint8_t *const buf8 = pipe.backbuf;
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
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
        for(int i = 0; i < 3; i++) buf16[4 * k + i] = CLAMP(buff[4 * k + i] * 0x10000, 0, 0xffff);
      }
  }
  // else output float, no further harm done to the pixels :)

  format_params->width = processed_width;
  format_params->height = processed_height;

  if(!ignore_exif)
  {
    int length;
    uint8_t *exif_profile = NULL; // Exif data should be 65536 bytes max, but if original size is close to that,
                                  // adding new tags could make it go over that... so let it be and see what
                                  // happens when we write the image
    char pathname[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, pathname, sizeof(pathname), &from_cache);
    // last param is dng mode, it's false here
    length = dt_exif_read_blob(&exif_profile, pathname, imgid, sRGB, processed_width, processed_height, 0);

    res = format->write_image(format_params, filename, outbuf, icc_type, icc_filename, exif_profile, length, imgid,
                              num, total);

    free(exif_profile);
  }
  else
  {
    res = format->write_image(format_params, filename, outbuf, icc_type, icc_filename, NULL, 0, imgid, num, total);
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  /* now write xmp into that container, if possible */
  if(copy_metadata && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP))
  {
    dt_exif_xmp_attach(imgid, filename);
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

    if (storage)
      luaA_push_type(L, storage->parameter_lua_type, storage_params);
    else
      lua_pushnil(L);

    dt_lua_event_trigger(L, "intermediate-export-image", 4);

    dt_lua_unlock();
#endif

    dt_control_signal_raise(darktable.signals, DT_SIGNAL_IMAGE_EXPORT_TMPFILE, imgid, filename, format,
                            format_params, storage, storage_params);
  }

  return res;

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
#ifdef HAVE_GRAPHICSMAGICK
  dt_imageio_retval_t ret = dt_imageio_open_gm(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->buf_dsc.filters = 0u;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    img->loader = LOADER_GM;
    return ret;
  }
#endif

  return DT_IMAGEIO_FILE_CORRUPTED;
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

  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  img->loader = LOADER_UNKNOWN;

  /* check if file is ldr using magic's */
  if(dt_imageio_is_ldr(filename)) ret = dt_imageio_open_ldr(img, filename, buf);

  /* silly check using file extensions: */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL && dt_imageio_is_hdr(filename))
    ret = dt_imageio_open_hdr(img, filename, buf);

  /* use rawspeed to load the raw */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
  {
    ret = dt_imageio_open_rawspeed(img, filename, buf);
    if(ret == DT_IMAGEIO_OK) img->loader = LOADER_RAWSPEED;
  }

  /* fallback that tries to open file via GraphicsMagick */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_exotic(img, filename, buf);

  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
