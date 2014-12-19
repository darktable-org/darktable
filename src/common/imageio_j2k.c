/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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
#include "common/imageio_j2k.h"
#include "common/exif.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <strings.h>

#include <openjpeg.h>

#define J2K_CFMT 0
#define JP2_CFMT 1
#define JPT_CFMT 2

static char JP2_HEAD[] = { 0x0, 0x0, 0x0, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A };
static char J2K_HEAD[] = { 0xFF, 0x4F, 0xFF, 0x51, 0x00 };
// there seems to be no JPIP/JPT magic string, so we can't load it ...

static void color_sycc_to_rgb(opj_image_t *img);

/**
sample error callback expecting a FILE* client object
*/
static void error_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE *)client_data;
  fprintf(stream, "[j2k_open] Error: %s", msg);
}
/**
sample warning callback expecting a FILE* client object
*/
// static void warning_callback(const char *msg, void *client_data)
// {
//   FILE *stream = (FILE*)client_data;
//   fprintf(stream, "[j2k_open] Warning: %s", msg);
// }
/**
sample debug callback expecting no client object
*/
// static void info_callback(const char *msg, void *client_data)
// {
//   (void)client_data;
//   fprintf(stdout, "[j2k_open] Info: %s", msg);
// }

static int get_file_format(const char *filename)
{
  unsigned int i;
  static const char *extension[] = { "j2k", "jp2", "jpt", "j2c", "jpc" };
  static const int format[] = { J2K_CFMT, JP2_CFMT, JPT_CFMT, J2K_CFMT, J2K_CFMT };
  char *ext = strrchr(filename, '.');
  if(ext == NULL) return -1;
  ext++;
  if(ext)
  {
    for(i = 0; i < sizeof(format) / sizeof(*format); i++)
    {
      if(strncasecmp(ext, extension[i], 3) == 0)
      {
        return format[i];
      }
    }
  }

  return -1;
}

dt_imageio_retval_t dt_imageio_open_j2k(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *mbuf)
{
  opj_dparameters_t parameters; /* decompression parameters */
  opj_event_mgr_t event_mgr;    /* event manager */
  opj_image_t *image = NULL;
  FILE *fsrc = NULL;
  unsigned char *src = NULL;
  size_t file_length;
  opj_dinfo_t *dinfo = NULL; /* handle to a decompressor */
  opj_cio_t *cio = NULL;
  OPJ_CODEC_FORMAT codec;
  int ret = DT_IMAGEIO_FILE_CORRUPTED;

  int file_format = get_file_format(filename);
  if(file_format == -1) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  /* read the input file and put it in memory */
  /* ---------------------------------------- */
  fsrc = fopen(filename, "rb");
  if(!fsrc)
  {
    fprintf(stderr, "[j2k_open] Error: failed to open `%s' for reading\n", filename);
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }
  fseek(fsrc, 0, SEEK_END);
  file_length = ftell(fsrc);
  fseek(fsrc, 0, SEEK_SET);
  src = (unsigned char *)malloc(file_length);
  if(fread(src, 1, file_length, fsrc) != file_length)
  {
    free(src);
    fclose(fsrc);
    fprintf(stderr, "[j2k_open] Error: fread returned a number of elements different from the expected.\n");
    return DT_IMAGEIO_FILE_NOT_FOUND;
  }
  fclose(fsrc);

  if(memcmp(JP2_HEAD, src, sizeof(JP2_HEAD)) == 0)
  {
    file_format = JP2_CFMT; // just in case someone used the wrong extension
  }
  else if(memcmp(J2K_HEAD, src, sizeof(J2K_HEAD)) == 0)
  {
    file_format = J2K_CFMT; // just in case someone used the wrong extension
  }
  else // this will also reject jpt files.
  {
    free(src);
    fprintf(stderr, "[j2k_open] Error: `%s' has unsupported file format.\n", filename);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  /* configure the event callbacks (not required) */
  memset(&event_mgr, 0, sizeof(opj_event_mgr_t));
  event_mgr.error_handler = error_callback;
  //   event_mgr.warning_handler = warning_callback;
  //   event_mgr.info_handler = info_callback;

  /* set decoding parameters to default values */
  opj_set_default_decoder_parameters(&parameters);

  /* decode the code-stream */
  /* ---------------------- */
  if(file_format == J2K_CFMT) /* JPEG-2000 codestream */
    codec = CODEC_J2K;
  else if(file_format == JP2_CFMT) /* JPEG 2000 compressed image data */
    codec = CODEC_JP2;
  else if(file_format == JPT_CFMT) /* JPEG 2000, JPIP */
    codec = CODEC_JPT;
  else
  {
    free(src);
    return DT_IMAGEIO_FILE_CORRUPTED; // can't happen
  }

  /* get a decoder handle */
  dinfo = opj_create_decompress(codec);

  /* catch events using our callbacks and give a local context */
  opj_set_event_mgr((opj_common_ptr)dinfo, &event_mgr, stderr);

  /* setup the decoder decoding parameters using user parameters */
  opj_setup_decoder(dinfo, &parameters);

  /* open a byte stream */
  cio = opj_cio_open((opj_common_ptr)dinfo, src, file_length);

  /* decode the stream and fill the image structure */
  image = opj_decode(dinfo, cio);

  /* close the byte stream */
  opj_cio_close(cio);

  /* free the memory containing the code-stream */
  free(src);

  if(!image)
  {
    fprintf(stderr, "[j2k_open] Error: failed to decode image `%s'\n", filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto end_of_the_world;
  }

  if(image->color_space == CLRSPC_SYCC)
  {
    color_sycc_to_rgb(image);
  }

// FIXME: openjpeg didn't have support for icc profiles before version 1.5
// this needs some #ifdef magic and proper implementation
#ifdef HAVE_OPENJPEG_ICC
  if(image->icc_profile_buf)
  {
#if defined(HAVE_LIBLCMS1) || defined(HAVE_LIBLCMS2)
    color_apply_icc_profile(image);
#endif

    free(image->icc_profile_buf);
    image->icc_profile_buf = NULL;
    image->icc_profile_len = 0;
  }
#endif

  /* create output image */
  /* ------------------- */
  long signed_offsets[4] = { 0, 0, 0, 0 };
  int float_divs[4] = { 1, 1, 1, 1 };

  // some sanity checks
  if(image->numcomps == 0 || image->x1 == 0 || image->y1 == 0)
  {
    fprintf(stderr, "[j2k_open] Error: invalid raw image parameters in `%s'\n", filename);
    ret = DT_IMAGEIO_FILE_CORRUPTED;
    goto end_of_the_world;
  }

  for(int i = 0; i < image->numcomps; i++)
  {
    if(image->comps[i].w != image->x1 || image->comps[i].h != image->y1)
    {
      fprintf(stderr, "[j2k_open] Error: some component has different size in `%s'\n", filename);
      ret = DT_IMAGEIO_FILE_CORRUPTED;
      goto end_of_the_world;
    }
    if(image->comps[i].prec > 16)
    {
      fprintf(stderr, "[j2k_open] Error: precision %d is larger than 16 in `%s'\n", image->comps[1].prec,
              filename);
      ret = DT_IMAGEIO_FILE_CORRUPTED;
      goto end_of_the_world;
    }
  }

  img->width = image->x1;
  img->height = image->y1;
  img->bpp = 4 * sizeof(float);

  float *buf = (float *)dt_mipmap_cache_alloc(mbuf, img);
  if(!buf)
  {
    ret = DT_IMAGEIO_CACHE_FULL;
    goto end_of_the_world;
  }

  int i = image->numcomps;
  if(i > 4) i = 4;

  while(i)
  {
    i--;

    if(image->comps[i].sgnd) signed_offsets[i] = 1 << (image->comps[i].prec - 1);

    float_divs[i] = (1 << image->comps[i].prec) - 1;
  }

  // numcomps == 1 : grey  -> r = grey, g = grey, b = grey
  // numcomps == 2 : grey, alpha -> r = grey, g = grey, b = grey. put alpha into the mix?
  // numcomps == 3 : rgb -> rgb
  // numcomps == 4 : rgb, alpha -> rgb. put alpha into the mix?

  // first try: ignore alpha.
  if(image->numcomps < 3) // 1, 2 => grayscale
  {
    for(size_t i = 0; i < (size_t)img->width * img->height; i++)
      buf[i * 4 + 0] = buf[i * 4 + 1] = buf[i * 4 + 2] = (float)(image->comps[0].data[i] + signed_offsets[0])
                                                         / float_divs[0];
  }
  else // 3, 4 => rgb
  {
    for(size_t i = 0; i < (size_t)img->width * img->height; i++)
      for(int k = 0; k < 3; k++)
        buf[i * 4 + k] = (float)(image->comps[k].data[i] + signed_offsets[k]) / float_divs[k];
  }

  ret = DT_IMAGEIO_OK;

end_of_the_world:
  /* free remaining structures */
  if(dinfo) opj_destroy_decompress(dinfo);

  /* free image data structure */
  opj_image_destroy(image);

  return ret;
}

int dt_imageio_j2k_read_profile(const char *filename, uint8_t **out)
{
#ifdef HAVE_OPENJPEG_ICC
  opj_dparameters_t parameters; /* decompression parameters */
  opj_image_t *image = NULL;
  FILE *fsrc = NULL;
  unsigned char *src = NULL;
  size_t file_length;
  opj_dinfo_t *dinfo = NULL; /* handle to a decompressor */
  opj_cio_t *cio = NULL;
  OPJ_CODEC_FORMAT codec;
  gboolean res = FALSE;
  unsigned int length = 0;
  *out = NULL;

  /* read the input file and put it in memory */
  /* ---------------------------------------- */
  fsrc = fopen(filename, "rb");
  if(!fsrc)
  {
    fprintf(stderr, "[j2k_open] Error: failed to open `%s' for reading\n", filename);
    goto another_end_of_the_world;
  }
  fseek(fsrc, 0, SEEK_END);
  file_length = ftell(fsrc);
  fseek(fsrc, 0, SEEK_SET);
  src = (unsigned char *)malloc(file_length);
  if(fread(src, 1, file_length, fsrc) != file_length)
  {
    free(src);
    fclose(fsrc);
    fprintf(stderr, "[j2k_open] Error: fread returned a number of elements different from the expected.\n");
    goto another_end_of_the_world;
  }
  fclose(fsrc);

  if(memcmp(JP2_HEAD, src, sizeof(JP2_HEAD)) == 0)
  {
    codec = CODEC_JP2;
  }
  else if(memcmp(J2K_HEAD, src, sizeof(J2K_HEAD)) == 0)
  {
    codec = CODEC_J2K;
  }
  else // this will also reject jpt files.
  {
    free(src);
    fprintf(stderr, "[j2k_open] Error: `%s' has unsupported file format.\n", filename);
    goto another_end_of_the_world;
  }

  /* set decoding parameters to default values */
  opj_set_default_decoder_parameters(&parameters);
  parameters.cp_limit_decoding = LIMIT_TO_MAIN_HEADER;

  /* decode the code-stream */
  /* ---------------------- */

  /* get a decoder handle */
  dinfo = opj_create_decompress(codec);

  /* setup the decoder decoding parameters using user parameters */
  opj_setup_decoder(dinfo, &parameters);

  /* open a byte stream */
  cio = opj_cio_open((opj_common_ptr)dinfo, src, file_length);

  /* decode the stream and fill the image structure */
  image = opj_decode(dinfo, cio);

  /* close the byte stream */
  opj_cio_close(cio);

  /* free the memory containing the code-stream */
  free(src);

  if(!image)
  {
    fprintf(stderr, "[j2k_open] Error: failed to decode image `%s'\n", filename);
    goto another_end_of_the_world;
  }

  if(image->icc_profile_buf)
  {
    res = TRUE;
    length = image->icc_profile_len;
    *out = image->icc_profile_buf;

    image->icc_profile_buf = NULL;
    image->icc_profile_len = 0;
  }

another_end_of_the_world:
  /* free remaining structures */
  if(dinfo) opj_destroy_decompress(dinfo);

  /* free image data structure */
  opj_image_destroy(image);

  return res ? length : 0;
#else
  return 0;
#endif
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
static void sycc_to_rgb(int offset, int upb, int y, int cb, int cr, int *out_r, int *out_g, int *out_b)
{
  int r, g, b;

  cb -= offset;
  cr -= offset;
  r = y + (int)(1.402 * (float)cr);
  if(r < 0)
    r = 0;
  else if(r > upb)
    r = upb;
  *out_r = r;

  g = y - (int)(0.344 * (float)cb + 0.714 * (float)cr);
  if(g < 0)
    g = 0;
  else if(g > upb)
    g = upb;
  *out_g = g;

  b = y + (int)(1.772 * (float)cb);
  if(b < 0)
    b = 0;
  else if(b > upb)
    b = upb;
  *out_b = b;
}

static void sycc444_to_rgb(opj_image_t *img)
{
  int *d0, *d1, *d2, *r, *g, *b;
  const int *y, *cb, *cr;
  size_t maxw, maxh, max;
  int i, offset, upb;

  i = img->comps[0].prec;
  offset = 1 << (i - 1);
  upb = (1 << i) - 1;

  maxw = img->comps[0].w;
  maxh = img->comps[0].h;
  max = maxw * maxh;

  y = img->comps[0].data;
  cb = img->comps[1].data;
  cr = img->comps[2].data;

  d0 = r = (int *)calloc(max, sizeof(int));
  d1 = g = (int *)calloc(max, sizeof(int));
  d2 = b = (int *)calloc(max, sizeof(int));

  for(i = 0; i < max; ++i)
  {
    sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
    ++y;
    ++cb;
    ++cr;
    ++r;
    ++g;
    ++b;
  }
  free(img->comps[0].data);
  img->comps[0].data = d0;
  free(img->comps[1].data);
  img->comps[1].data = d1;
  free(img->comps[2].data);
  img->comps[2].data = d2;
} /* sycc444_to_rgb() */

static void sycc422_to_rgb(opj_image_t *img)
{
  int *d0, *d1, *d2, *r, *g, *b;
  const int *y, *cb, *cr;
  size_t maxw, maxh, max;
  int offset, upb;
  int i, j;

  i = img->comps[0].prec;
  offset = 1 << (i - 1);
  upb = (1 << i) - 1;

  maxw = img->comps[0].w;
  maxh = img->comps[0].h;
  max = maxw * maxh;

  y = img->comps[0].data;
  cb = img->comps[1].data;
  cr = img->comps[2].data;

  d0 = r = (int *)calloc(max, sizeof(int));
  d1 = g = (int *)calloc(max, sizeof(int));
  d2 = b = (int *)calloc(max, sizeof(int));

  for(i = 0; i < maxh; ++i)
  {
    for(j = 0; j < maxw; j += 2)
    {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;

      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;
      ++cb;
      ++cr;
    }
  }
  free(img->comps[0].data);
  img->comps[0].data = d0;
  free(img->comps[1].data);
  img->comps[1].data = d1;
  free(img->comps[2].data);
  img->comps[2].data = d2;

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
  int *d0, *d1, *d2, *r, *g, *b, *nr, *ng, *nb;
  const int *y, *cb, *cr, *ny;
  size_t maxw, maxh, max;
  int offset, upb;
  int i, j;

  i = img->comps[0].prec;
  offset = 1 << (i - 1);
  upb = (1 << i) - 1;

  maxw = img->comps[0].w;
  maxh = img->comps[0].h;
  max = maxw * maxh;

  y = img->comps[0].data;
  cb = img->comps[1].data;
  cr = img->comps[2].data;

  d0 = r = (int *)calloc(max, sizeof(int));
  d1 = g = (int *)calloc(max, sizeof(int));
  d2 = b = (int *)calloc(max, sizeof(int));

  for(i = 0; i < maxh; i += 2)
  {
    ny = y + maxw;
    nr = r + maxw;
    ng = g + maxw;
    nb = b + maxw;

    for(j = 0; j < maxw; j += 2)
    {
      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;

      sycc_to_rgb(offset, upb, *y, *cb, *cr, r, g, b);
      ++y;
      ++r;
      ++g;
      ++b;

      sycc_to_rgb(offset, upb, *ny, *cb, *cr, nr, ng, nb);
      ++ny;
      ++nr;
      ++ng;
      ++nb;

      sycc_to_rgb(offset, upb, *ny, *cb, *cr, nr, ng, nb);
      ++ny;
      ++nr;
      ++ng;
      ++nb;
      ++cb;
      ++cr;
    }
    y += maxw;
    r += maxw;
    g += maxw;
    b += maxw;
  }
  free(img->comps[0].data);
  img->comps[0].data = d0;
  free(img->comps[1].data);
  img->comps[1].data = d1;
  free(img->comps[2].data);
  img->comps[2].data = d2;

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
    img->color_space = CLRSPC_GRAY;
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
    fprintf(stderr, "%s:%d:color_sycc_to_rgb\n\tCAN NOT CONVERT\n", __FILE__, __LINE__);
    return;
  }
  img->color_space = CLRSPC_SRGB;
} /* color_sycc_to_rgb() */

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
