/*
    This file is part of darktable,
    Copyright (C) 2012-2024 darktable developers.

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
#include "common/exif.h"
#include "imageio/imageio_j2k.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <openjpeg.h>

#define J2K_CFMT 0
#define JP2_CFMT 1
#define JPT_CFMT 2

static char JP2_HEAD[] = { 0x0, 0x0, 0x0, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A };
static char JP2_MAGIC[] = { 0x0d, 0x0a, 0x87, 0x0a };
static char J2K_HEAD[] = { 0xFF, 0x4F, 0xFF, 0x51 };
// there seems to be no JPIP/JPT magic string, so we can't load it ...

static void color_sycc_to_rgb(opj_image_t *img);

static void error_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[j2k_open] Error: %s", msg);
}

static int get_file_format(const char *filename)
{
  static const char *extension[] = { "j2k", "jp2", "jpt", "j2c", "jpc", "jpf", "jpx" };
  static const int format[] = { J2K_CFMT, JP2_CFMT, JPT_CFMT, J2K_CFMT, J2K_CFMT, JP2_CFMT, JP2_CFMT };
  char *ext = strrchr(filename, '.');
  if(ext == NULL) return -1;
  ext++;
  if(*ext)
  {
    for(unsigned int i = 0; i < sizeof(format) / sizeof(*format); i++)
    {
      if(strncasecmp(ext, extension[i], 3) == 0)
      {
        return format[i];
      }
    }
  }

  return -1;
}

dt_imageio_retval_t dt_imageio_open_j2k(dt_image_t *img,
                                        const char *filename,
                                        dt_mipmap_buffer_t *mbuf)
{
  opj_dparameters_t parameters; /* decompression parameters */
  opj_image_t *image = NULL;
  FILE *fsrc = NULL;
  unsigned char src_header[12] = { 0 };
  opj_codec_t *d_codec = NULL;
  OPJ_CODEC_FORMAT codec;
  opj_stream_t *d_stream = NULL; /* Stream */
  int ret = DT_IMAGEIO_LOAD_FAILED;

  /* set decoding parameters to default values */
  opj_set_default_decoder_parameters(&parameters);

  g_strlcpy(parameters.infile, filename, sizeof(parameters.infile));

  parameters.decod_format = get_file_format(filename);
  if(parameters.decod_format == -1)
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  fsrc = g_fopen(filename, "rb");
  if(!fsrc)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: failed to open '%s' for reading", filename);
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }
  if(fread(src_header, 1, 12, fsrc) != 12)
  {
    fclose(fsrc);
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: fread returned a number of elements different from the expected.");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  fclose(fsrc);

  if(memcmp(JP2_HEAD, src_header, sizeof(JP2_HEAD)) == 0 ||
     memcmp(JP2_MAGIC, src_header, sizeof(JP2_MAGIC)) == 0)
  {
    parameters.decod_format = JP2_CFMT; // just in case someone used the wrong extension
  }
  else if(memcmp(J2K_HEAD, src_header, sizeof(J2K_HEAD)) == 0)
  {
    parameters.decod_format = J2K_CFMT; // just in case someone used the wrong extension
  }
  else // this will also reject jpt files.
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: '%s' has unsupported file format", filename);
    return DT_IMAGEIO_UNSUPPORTED_FORMAT;
  }


  /* decode the code-stream */
  /* ---------------------- */
  if(parameters.decod_format == J2K_CFMT) /* JPEG-2000 codestream */
    codec = OPJ_CODEC_J2K;
  else if(parameters.decod_format == JP2_CFMT) /* JPEG 2000 compressed image data */
    codec = OPJ_CODEC_JP2;
  else if(parameters.decod_format == JPT_CFMT) /* JPEG 2000, JPIP */
    codec = OPJ_CODEC_JPT;
  else
  {
    return DT_IMAGEIO_UNSUPPORTED_FEATURE; // can't happen
  }

  d_codec = opj_create_decompress(codec);
  if(!d_codec)
  {
    dt_print(DT_DEBUG_ALWAYS, "[j2k_open] Error: failed to create the decoder");
    return DT_IMAGEIO_LOAD_FAILED;
  }

  /* catch events using our callbacks and give a local context */
  opj_set_error_handler(d_codec, error_callback, stderr);
  // opj_set_warning_handler(d_codec, error_callback, stderr);
  // opj_set_info_handler(d_codec, error_callback, stderr);

  /* Decode JPEG-2000 with using multiple threads */
  if(!opj_codec_set_threads(d_codec, dt_get_num_threads()))
  {
    // This may not seem like a critical error but failure to initialize
    // the threads is a sign of major resource exhaustion, better to fail
    // as soon as possible to save overall stability.
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: failed to setup the threads for decoder %s",
             parameters.infile);
    opj_destroy_codec(d_codec);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  /* setup the decoder decoding parameters using user parameters */
  if(!opj_setup_decoder(d_codec, &parameters))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: failed to setup the decoder %s",
             parameters.infile);
    opj_destroy_codec(d_codec);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  d_stream = opj_stream_create_default_file_stream(parameters.infile, 1);
  if(!d_stream)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: failed to create the stream from the file %s",
             parameters.infile);
    opj_destroy_codec(d_codec);
    return DT_IMAGEIO_LOAD_FAILED;
  }

  /* Read the main header of the codestream and if necessary the JP2 boxes*/
  if(!opj_read_header(d_stream, d_codec, &image))
  {
    dt_print(DT_DEBUG_ALWAYS, "[j2k_open] Error: failed to read the header");
    opj_stream_destroy(d_stream);
    opj_destroy_codec(d_codec);
    opj_image_destroy(image);
    return DT_IMAGEIO_IOERROR;
  }

  /* Get the decoded image */
  if(!(opj_decode(d_codec, d_stream, image) && opj_end_decompress(d_codec, d_stream)))
  {
    dt_print(DT_DEBUG_ALWAYS, "[j2k_open] Error: failed to decode image!");
    opj_destroy_codec(d_codec);
    opj_stream_destroy(d_stream);
    opj_image_destroy(image);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* Close the byte stream */
  opj_stream_destroy(d_stream);

  if(!image)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: failed to decode image '%s'",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto end_of_the_world;
  }

  if(image->color_space == OPJ_CLRSPC_SYCC)
  {
    color_sycc_to_rgb(image);
  }

  /* Get the ICC profile if available */
  if(image->icc_profile_len > 0 && image->icc_profile_buf)
  {
    uint8_t *profile = g_try_malloc0(image->icc_profile_len);
    if(profile)
    {
      img->profile = profile;
      memcpy(img->profile, image->icc_profile_buf, image->icc_profile_len);
      img->profile_size = image->icc_profile_len;
    }
  }

  /* create output image */
  /* ------------------- */
  long signed_offsets[4] = { 0, 0, 0, 0 };
  int float_divs[4] = { 1, 1, 1, 1 };

  // some sanity checks
  if(image->numcomps == 0 || image->x1 == 0 || image->y1 == 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[j2k_open] Error: invalid raw image parameters in '%s'",
             filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto end_of_the_world;
  }

  for(int i = 0; i < image->numcomps; i++)
  {
    if(image->comps[i].w != image->x1 || image->comps[i].h != image->y1)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[j2k_open] Error: some component has different size in '%s'",
               filename);
      ret = DT_IMAGEIO_FILE_CORRUPTED;
      goto end_of_the_world;
    }
    if(image->comps[i].prec > 16)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[j2k_open] Error: precision %d is larger than 16 in '%s'",
               image->comps[1].prec,
               filename);
      ret = DT_IMAGEIO_UNSUPPORTED_FEATURE;
      goto end_of_the_world;
    }
  }

  img->width = image->x1;
  img->height = image->y1;

  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    ret = DT_IMAGEIO_CACHE_FULL;
    goto end_of_the_world;
  }

  image->numcomps = MIN(image->numcomps, 4);

  for(int i = 0; i < image->numcomps; i++)
  {
    if(image->comps[i].sgnd) signed_offsets[i] = 1 << (image->comps[i].prec - 1);

    float_divs[i] = (1 << image->comps[i].prec) - 1;
  }

  // numcomps == 1 : grey  -> r = grey, g = grey, b = grey
  // numcomps == 2 : grey, alpha -> r = grey, g = grey, b = grey. put alpha into the mix?
  // numcomps == 3 : rgb -> rgb
  // numcomps == 4 : rgb, alpha -> rgb. put alpha into the mix?

  // first try: ignore alpha.

  const size_t npixels = (size_t)img->width * img->height;

  if(image->numcomps < 3) // 1, 2 => grayscale
  {
    DT_OMP_FOR()
    for(size_t index = 0; index < npixels; index++)
      buf[index * 4] = buf[index * 4 + 1] = buf[index * 4 + 2] =
      (float)(image->comps[0].data[index] + signed_offsets[0]) / float_divs[0];
  }
  else // 3, 4 => rgb
  {
    DT_OMP_FOR()
    for(size_t index = 0; index < npixels; index++)
    {
      buf[index * 4]     = (float)(image->comps[0].data[index] + signed_offsets[0]) / float_divs[0];
      buf[index * 4 + 1] = (float)(image->comps[1].data[index] + signed_offsets[1]) / float_divs[1];
      buf[index * 4 + 2] = (float)(image->comps[2].data[index] + signed_offsets[2]) / float_divs[2];
    }
  }

  img->buf_dsc.cst = IOP_CS_RGB; // j2k is always RGB
  img->buf_dsc.filters = 0u;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags &= ~DT_IMAGE_S_RAW;
  img->flags |= DT_IMAGE_LDR;
  img->loader = LOADER_J2K;

  ret = DT_IMAGEIO_OK;

end_of_the_world:
  /* free remaining structures */
  opj_destroy_codec(d_codec);

  /* free image data structure */
  opj_image_destroy(image);

  return ret;
}


// stolen from openjpeg
/*--------------------------------------------------------
Matrix for sYCC, Amendment 1 to IEC 61966-2-1

Y :   0.299   0.587    0.114   :R
Cb:  -0.1687 -0.3312   0.5     :G
Cr:   0.5    -0.4187  -0.0812  :B

Inverse:

R: 1        -3.68213e-05    1.40199      :Y
G: 1.00003  -0.344125      -0.714128     :Cb - 2^(prec - 1)
B: 0.999823  1.77204       -8.04142e-06  :Cr - 2^(prec - 1)

-----------------------------------------------------------*/
static void sycc_to_rgb(const int offset,
                        const int upb,
                        const int y,
                        int cb,
                        int cr,
                        int *out_r,
                        int *out_g,
                        int *out_b)
{
  cb -= offset;
  cr -= offset;
  int r = y + (int)(1.402 * (float)cr);
  *out_r = CLAMP(r, 0, upb);

  int g = y - (int)(0.344 * (float)cb + 0.714 * (float)cr);
  *out_g = CLAMP(g, 0, upb);

  int b = y + (int)(1.772 * (float)cb);
  *out_b = CLAMP(b, 0, upb);
}

static void sycc444_to_rgb(opj_image_t *img)
{
  const int prec = img->comps[0].prec;
  const int offset = 1 << (prec - 1);
  const int upb = (1 << prec) - 1;

  const size_t maxw = img->comps[0].w;
  const size_t maxh = img->comps[0].h;
  const size_t max = maxw * maxh;

  const int *y = img->comps[0].data;
  const int *cb = img->comps[1].data;
  const int *cr = img->comps[2].data;

  int *const r = (int *)calloc(max, sizeof(int));
  int *const g = (int *)calloc(max, sizeof(int));
  int *const b = (int *)calloc(max, sizeof(int));

  if(!r || !g || !b)
  {
    free(r);
    free(g);
    free(b);
    return;
  }

  DT_OMP_FOR()
  for(size_t k = 0; k < max; ++k)
  {
    sycc_to_rgb(offset, upb, y[k], cb[k], cr[k], r+k, g+k, b+k);
  }
  free(img->comps[0].data);
  img->comps[0].data = r;
  free(img->comps[1].data);
  img->comps[1].data = g;
  free(img->comps[2].data);
  img->comps[2].data = b;
} /* sycc444_to_rgb() */

static void sycc422_to_rgb(opj_image_t *img)
{
  const int prec = img->comps[0].prec;
  const int offset = 1 << (prec - 1);
  const int upb = (1 << prec) - 1;

  const size_t maxw = img->comps[0].w;
  const size_t maxh = img->comps[0].h;
  const size_t max = maxw * maxh;

  const int *y = img->comps[0].data;
  const int *cb = img->comps[1].data;
  const int *cr = img->comps[2].data;

  int *const r = (int *)calloc(max, sizeof(int));
  int *const g = (int *)calloc(max, sizeof(int));
  int *const b = (int *)calloc(max, sizeof(int));

  if(!r || !g || !b)
  {
    free(r);
    free(g);
    free(b);
    return;
  }

  DT_OMP_FOR()
  for(size_t i = 0; i < maxh; ++i)
  {
    size_t rowstart = i * maxw;
    for(size_t j = 0; j < maxw; j += 2)
    {
      const int curr_cb = cb[rowstart + j/2];
      const int curr_cr = cr[rowstart + j/2];
      sycc_to_rgb(offset, upb, y[rowstart+j], curr_cb, curr_cr, r+rowstart+j, g+rowstart+j, b+rowstart+j);
      sycc_to_rgb(offset, upb, y[rowstart+j+1], curr_cb, curr_cr, r+rowstart+j+1, g+rowstart+j+1, b+rowstart+j+1);
    }
  }
  free(img->comps[0].data);
  img->comps[0].data = r;
  free(img->comps[1].data);
  img->comps[1].data = g;
  free(img->comps[2].data);
  img->comps[2].data = b;

  img->comps[1].w = maxw;
  img->comps[1].h = maxh;
  img->comps[2].w = maxw;
  img->comps[2].h = maxh;
  img->comps[1].dx = img->comps[0].dx;
  img->comps[2].dx = img->comps[0].dx;
  img->comps[1].dy = img->comps[0].dy;
  img->comps[2].dy = img->comps[0].dy;
} /* sycc422_to_rgb() */

static void sycc420_to_rgb(opj_image_t *img)
{
  const int offset = 1 << (img->comps[0].prec - 1);
  const int upb = (1 << img->comps[0].prec) - 1;

  size_t maxw = img->comps[0].w;
  size_t maxh = img->comps[0].h;
  size_t max = maxw * maxh;

  const int *y = img->comps[0].data;
  const int *cb = img->comps[1].data;
  const int *cr = img->comps[2].data;

  int *const r = (int *)calloc(max, sizeof(int));
  int *const g = (int *)calloc(max, sizeof(int));
  int *const b = (int *)calloc(max, sizeof(int));

  if(!r || !g || !b)
  {
    free(r);
    free(g);
    free(b);
    return;
  }

  DT_OMP_FOR()
  for(size_t i = 0; i < maxh; i += 2)
  {
    const size_t subrow = (i/2) * maxw;
    const size_t rowstart = i * maxw;
    const size_t nextrow = (i+1) * maxw;

    for(size_t j = 0; j < maxw; j += 2)
    {
      const int curr_cb = cb[subrow + j/2];
      const int curr_cr = cr[subrow + j/2];
      sycc_to_rgb(offset, upb, y[rowstart+j], curr_cb, curr_cr, r+rowstart+j, g+rowstart+j, b+rowstart+j);
      sycc_to_rgb(offset, upb, y[rowstart+j+1], curr_cb, curr_cr, r+rowstart+j+1, g+rowstart+j+1, b+rowstart+j+1);
      sycc_to_rgb(offset, upb, y[nextrow+j], curr_cb, curr_cr, r+nextrow+j, g+nextrow+j, b+nextrow+j);
      sycc_to_rgb(offset, upb, y[nextrow+j+1], curr_cb, *cr, r+nextrow+j+1, g+nextrow+j+1, b+nextrow+j+1);
    }
  }
  free(img->comps[0].data);
  img->comps[0].data = r;
  free(img->comps[1].data);
  img->comps[1].data = g;
  free(img->comps[2].data);
  img->comps[2].data = b;

  img->comps[1].w = maxw;
  img->comps[1].h = maxh;
  img->comps[2].w = maxw;
  img->comps[2].h = maxh;
  img->comps[1].dx = img->comps[0].dx;
  img->comps[2].dx = img->comps[0].dx;
  img->comps[1].dy = img->comps[0].dy;
  img->comps[2].dy = img->comps[0].dy;
} /* sycc420_to_rgb() */

static void color_sycc_to_rgb(opj_image_t *img)
{
  if(img->numcomps < 3)
  {
    img->color_space = OPJ_CLRSPC_GRAY;
    return;
  }

  if((img->comps[0].dx == 1) && (img->comps[1].dx == 2) && (img->comps[2].dx == 2) && (img->comps[0].dy == 1)
     && (img->comps[1].dy == 2) && (img->comps[2].dy == 2)) /* horizontal and vertical sub-sample */
  {
    sycc420_to_rgb(img);
  }
  else if((img->comps[0].dx == 1) && (img->comps[1].dx == 2) && (img->comps[2].dx == 2)
          && (img->comps[0].dy == 1) && (img->comps[1].dy == 1)
          && (img->comps[2].dy == 1)) /* horizontal sub-sample only */
  {
    sycc422_to_rgb(img);
  }
  else if((img->comps[0].dx == 1) && (img->comps[1].dx == 1) && (img->comps[2].dx == 1)
          && (img->comps[0].dy == 1) && (img->comps[1].dy == 1)
          && (img->comps[2].dy == 1)) /* no sub-sample */
  {
    sycc444_to_rgb(img);
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS,
             "%s:%d:color_sycc_to_rgb\n\tCAN NOT CONVERT", __FILE__, __LINE__);
    return;
  }
  img->color_space = OPJ_CLRSPC_SRGB;
} /* color_sycc_to_rgb() */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
