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
#include "iop/colorout.h"
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/imageio_exr.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_tiff.h"
#include "common/imageio_pfm.h"
#include "common/imageio_rgbe.h"
#include "common/imageio_rawspeed.h"
#include "common/image_compression.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "libraw/libraw.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <glib/gstdio.h>

void dt_imageio_preview_f_to_8(int32_t p_wd, int32_t p_ht, const float *f, uint8_t *p8)
{
  for(int idx=0; idx < p_wd*p_ht; idx++)
    for(int k=0; k<3; k++) p8[4*idx+2-k] = dt_dev_default_gamma[(int)CLAMPS(0xffff*f[4*idx+k], 0, 0xffff)];
}

void dt_imageio_preview_8_to_f(int32_t p_wd, int32_t p_ht, const uint8_t *p8, float *f)
{
  for(int idx=0; idx < p_wd*p_ht; idx++)
    for(int k=0; k<3; k++) f[4*idx+2-k] = dt_dev_de_gamma[p8[4*idx+k]];
}


// =================================================
//   begin libraw wrapper functions:
// =================================================

#define HANDLE_ERRORS(ret, verb) {                                 \
  if(ret)                                                     \
  {                                                       \
    if(verb) fprintf(stderr,"[imageio] %s: %s\n", filename, libraw_strerror(ret)); \
    libraw_close(raw);                         \
    raw = NULL; \
    return DT_IMAGEIO_FILE_CORRUPTED;                                   \
  }                                                       \
}

void
dt_imageio_flip_buffers(char *out, const char *in, const size_t bpp, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation)
{
  if(!orientation)
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
    for(int j=0; j<ht; j++) memcpy(out+j*bpp*wd, in+j*stride, bpp*wd);
    return;
  }
  int ii = 0, jj = 0;
  int si = bpp, sj = wd*bpp;
  if(orientation & 4)
  {
    sj = bpp;
    si = ht*bpp;
  }
  if(orientation & 2)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & 1)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si)
#endif
  for(int j=0; j<ht; j++)
  {
    char *out2 = out + labs(sj)*jj + labs(si)*ii + sj*j;
    const char *in2  = in + stride*j;
    for(int i=0; i<wd; i++)
    {
      memcpy(out2, in2, bpp);
      in2  += bpp;
      out2 += si;
    }
  }
}

void
dt_imageio_flip_buffers_ui16_to_float(float *out, const uint16_t *in, const float black, const float white, const int ch, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation)
{
  const float scale = 1.0f/(white - black);
  if(!orientation)
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
    for(int j=0; j<ht; j++) for(int i=0; i<wd; i++) for(int k=0; k<ch; k++) out[4*(j*wd + i)+k] = (in[ch*(j*stride + i)+k]-black)*scale;
    return;
  }
  int ii = 0, jj = 0;
  int si = 4, sj = wd*4;
  if(orientation & 4)
  {
    sj = 4;
    si = ht*4;
  }
  if(orientation & 2)
  {
    jj = (int)fht - jj - 1;
    sj = -sj;
  }
  if(orientation & 1)
  {
    ii = (int)fwd - ii - 1;
    si = -si;
  }
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(in, out, jj, ii, sj, si)
#endif
  for(int j=0; j<ht; j++)
  {
    float *out2 = out + labs(sj)*jj + labs(si)*ii + sj*j;
    const uint16_t *in2  = in + stride*j;
    for(int i=0; i<wd; i++)
    {
      for(int k=0; k<ch; k++) out2[k] = (in2[k] - black)*scale;
      in2  += ch;
      out2 += si;
    }
  }
}

int dt_imageio_write_pos(int i, int j, int wd, int ht, float fwd, float fht, int orientation)
{
  int ii = i, jj = j, w = wd, fw = fwd, fh = fht;
  if(orientation & 4)
  {
    w = ht;
    ii = j;
    jj = i;
    fw = fht;
    fh = fwd;
  }
  if(orientation & 2) ii = (int)fw - ii - 1;
  if(orientation & 1) jj = (int)fh - jj - 1;
  return jj*w + ii;
}


dt_imageio_retval_t dt_imageio_open_hdr_preview(dt_image_t *img, const char *filename)
{
  int p_wd, p_ht;
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_exr_preview(img, filename);
  if(ret == DT_IMAGEIO_OK) goto all_good;
  if(ret == DT_IMAGEIO_CACHE_FULL) return ret;
  ret = dt_imageio_open_rgbe_preview(img, filename);
  if(ret == DT_IMAGEIO_OK) goto all_good;
  if(ret == DT_IMAGEIO_CACHE_FULL) return ret;
  ret = dt_imageio_open_pfm_preview(img, filename);
  if(ret == DT_IMAGEIO_OK) goto all_good;
  if(ret == DT_IMAGEIO_CACHE_FULL) return ret;

  // no hdr file:
  if(ret == DT_IMAGEIO_FILE_CORRUPTED) return ret;
all_good:
  img->filters = 0;
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags |=  DT_IMAGE_HDR;

  // this updates mipf/mip4..0 from raw pixels.
  dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
  if(dt_image_alloc(img, DT_IMAGE_MIP4)) return DT_IMAGEIO_CACHE_FULL;
  if(dt_image_get(img, DT_IMAGE_MIPF, 'r') != DT_IMAGE_MIPF)
  {
    dt_image_release(img, DT_IMAGE_MIP4, 'w');
    dt_image_release(img, DT_IMAGE_MIP4, 'r');
    return DT_IMAGEIO_CACHE_FULL;
  }
  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  dt_image_check_buffer(img, DT_IMAGE_MIPF, 4*p_wd*p_ht*sizeof(float));
  dt_imageio_preview_f_to_8(p_wd, p_ht, img->mipf, img->mip[DT_IMAGE_MIP4]);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  ret = dt_image_update_mipmaps(img);
  dt_image_release(img, DT_IMAGE_MIPF, 'r');
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return ret;
}

dt_imageio_retval_t dt_imageio_open_hdr(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_exr(img, filename);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  ret = dt_imageio_open_rgbe(img, filename);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  ret = dt_imageio_open_pfm(img, filename);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
return_label:
  if(ret == DT_IMAGEIO_OK)
  {
    img->filters = 0;
    img->bpp = 4*sizeof(float);
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags |=  DT_IMAGE_HDR;
  }
  return ret;
}

// only set mip4..0 and mipf
dt_imageio_retval_t dt_imageio_open_raw_preview(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);
  // init libraw stuff
  dt_imageio_retval_t retval = DT_IMAGEIO_OK;
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = 0; /* dcraw -h */
  raw->params.use_camera_wb = 0;
  raw->params.use_auto_wb = 0;
  raw->params.med_passes = 0;//img->raw_params.med_passes;
  raw->params.no_auto_bright = 1;
  raw->params.output_color = 0;
  raw->params.output_bps = 16;
  raw->params.user_flip = img->raw_params.user_flip;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
  raw->params.user_qual = 0; // linear
  raw->params.four_color_rgb = 0;//img->raw_params.four_color_rgb;
  raw->params.use_camera_matrix = 0;
  raw->params.green_matching = 0;// img->raw_params.greeneq;
  raw->params.highlight = 1;//img->raw_params.highlight; //0 clip, 1 unclip, 2 blend, 3+ rebuild
  raw->params.threshold = 0;//img->raw_denoise_threshold;
  raw->params.auto_bright_thr = img->raw_auto_bright_threshold;

  // are we going to need the image as input for a pixel pipe?
  dt_ctl_gui_mode_t mode = dt_conf_get_int("ui_last/view");
  const int altered = dt_image_altered(img) || (img == darktable.develop->image && mode == DT_DEVELOP);

  // if we have a history stack, don't load preview buffer!
  if(!altered && !dt_conf_get_bool("never_use_embedded_thumb"))
  {
    // no history stack: get thumbnail
    ret = libraw_open_file(raw, filename);
    HANDLE_ERRORS(ret, 0);
    ret = libraw_unpack_thumb(raw);
    if(ret) goto try_full_raw;
    ret = libraw_adjust_sizes_info_only(raw);
    ret = 0;
    img->orientation = raw->sizes.flip;
    img->width  = raw->sizes.iwidth;
    img->height = raw->sizes.iheight;
    img->exif_iso = raw->other.iso_speed;
    img->exif_exposure = raw->other.shutter;
    img->exif_aperture = raw->other.aperture;
    img->exif_focal_length = raw->other.focal_len;
    g_strlcpy(img->exif_maker, raw->idata.make, sizeof(img->exif_maker));
    img->exif_maker[sizeof(img->exif_maker) - 1] = 0x0;
    g_strlcpy(img->exif_model, raw->idata.model, sizeof(img->exif_model));
    img->exif_model[sizeof(img->exif_model) - 1] = 0x0;
    dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);
    image = libraw_dcraw_make_mem_thumb(raw, &ret);
    if(!image) goto try_full_raw;
    int p_wd, p_ht;
    float f_wd, f_ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
    dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);
    if(image && image->type == LIBRAW_IMAGE_JPEG)
    {
      // JPEG: decode (directly rescaled to mip4)
      const int orientation = dt_image_orientation(img);
      dt_imageio_jpeg_t jpg;
      if(dt_imageio_jpeg_decompress_header(image->data, image->data_size, &jpg)) goto error_raw_corrupted;
      if(orientation & 4)
      {
        image->width  = jpg.height;
        image->height = jpg.width;
      }
      else
      {
        image->width  = jpg.width;
        image->height = jpg.height;
      }
      uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
      if(dt_imageio_jpeg_decompress(&jpg, tmp))
      {
        free(tmp);
        goto error_raw_corrupted;
      }
      if(dt_image_alloc(img, DT_IMAGE_MIP4))
      {
        free(tmp);
        fprintf(stderr, "[raw_preview] could not alloc mip4 for img `%s'!\n", img->filename);
        goto error_raw_cache_full;
      }
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int p_ht2 = orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = orientation & 4 ? p_ht : p_wd;
      const int f_ht2 = MIN(p_ht2, (orientation & 4 ? f_wd : f_ht) + 1.0);
      const int f_wd2 = MIN(p_wd2, (orientation & 4 ? f_ht : f_wd) + 1.0);

      if(image->width == p_wd && image->height == p_ht)
      {
        // use 1:1
        for (int j=0; j < jpg.height; j++)
          for (int i=0; i < jpg.width; i++)
            for(int k=0; k<3; k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
      }
      else
      {
        // scale to fit
        memset(img->mip[DT_IMAGE_MIP4], 0, 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0; j<p_ht2 && scale*j<jpg.height; j++) for(int i=0; i<p_wd2 && scale*i < jpg.width; i++)
          {
            uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
            for(int k=0; k<3; k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = cam[k];
          }
      }
      free(tmp);
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      retval = dt_image_update_mipmaps(img);

      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      img->flags &= ~DT_IMAGE_LDR;
      img->flags &= ~DT_IMAGE_HDR;
      img->flags |= DT_IMAGE_RAW;
      return retval;
    }
    else
    {
      // BMP: directly to mip4
      if (dt_image_alloc(img, DT_IMAGE_MIP4)) goto error_raw_cache_full;
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int orientation = dt_image_orientation(img);
      const int p_ht2 = orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = orientation & 4 ? p_ht : p_wd;
      const int f_ht2 = MIN(p_ht2, (orientation & 4 ? f_wd : f_ht) + 1.0);
      const int f_wd2 = MIN(p_wd2, (orientation & 4 ? f_ht : f_wd) + 1.0);
      if(image->width == p_wd2 && image->height == p_ht2)
      {
        // use 1:1
        for(int j=0; j<image->height; j++) for(int i=0; i<image->width; i++)
          {
            uint8_t *cam = image->data + 3*(j*image->width + i);
            for(int k=0; k<3; k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation) + 2-k] = cam[k];
          }
      }
      else
      {
        // scale to fit
        memset(img->mip[DT_IMAGE_MIP4], 0, 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0; j<p_ht2 && scale*j<image->height; j++) for(int i=0; i<p_wd2 && scale*i < image->width; i++)
          {
            uint8_t *cam = image->data + 3*((int)(scale*j)*image->width + (int)(scale*i));
            for(int k=0; k<3; k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation) + 2-k] = cam[k];
          }
      }
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      if(retval == DT_IMAGEIO_OK)
        retval = dt_image_update_mipmaps(img);
      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      img->flags &= ~DT_IMAGE_LDR;
      img->flags &= ~DT_IMAGE_HDR;
      img->flags |= DT_IMAGE_RAW;
      return retval;
    }
  }
  else
  {
try_full_raw:
    raw->params.half_size = 1; /* dcraw -h */
    // raw->params.user_qual = 2;
    raw->params.document_mode = 2;
    ret = libraw_open_file(raw, filename);
    HANDLE_ERRORS(ret, 0);
    ret = libraw_unpack(raw);
    img->black   = raw->color.black/65535.0;
    img->maximum = raw->color.maximum/65535.0;
    HANDLE_ERRORS(ret, 1);
    ret = libraw_dcraw_process(raw);
    // ret = libraw_dcraw_document_mode_processing(raw);
    HANDLE_ERRORS(ret, 1);
    image = libraw_dcraw_make_mem_image(raw, &ret);
    HANDLE_ERRORS(ret, 1);

    img->orientation = raw->sizes.flip;
    img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
    img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
    img->exif_iso = raw->other.iso_speed;
    img->exif_exposure = raw->other.shutter;
    img->exif_aperture = raw->other.aperture;
    img->exif_focal_length = raw->other.focal_len;
    g_strlcpy(img->exif_maker, raw->idata.make, sizeof(img->exif_maker));
    img->exif_maker[sizeof(img->exif_maker) - 1] = 0x0;
    g_strlcpy(img->exif_model, raw->idata.model, sizeof(img->exif_model));
    img->exif_model[sizeof(img->exif_model) - 1] = 0x0;
    dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);

    const float m = 1.0f/(raw->color.maximum - raw->color.black);
    const uint16_t (*rawpx)[3] = (const uint16_t (*)[3])image->data;
    const int raw_wd = img->width;
    const int raw_ht = img->height;
    img->width  <<= 1;
    img->height <<= 1;
    int p_wd, p_ht;
    float f_wd, f_ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIPF, &p_wd, &p_ht);
    dt_image_get_exact_mip_size(img, DT_IMAGE_MIPF, &f_wd, &f_ht);

    if(dt_image_alloc(img, DT_IMAGE_MIPF)) goto error_raw_cache_full;
    dt_image_check_buffer(img, DT_IMAGE_MIPF, 4*p_wd*p_ht*sizeof(float));

    if(raw_wd == p_wd && raw_ht == p_ht)
    {
      // use 1:1
#ifdef _OPENMP
      #pragma omp parallel for default(none) schedule(static) shared(img, rawpx, raw, p_wd)
#endif
      for(int j=0; j<raw_ht; j++) for(int i=0; i<raw_wd; i++)
        {
          for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = fmaxf(0.0f, (rawpx[j*raw_wd + i][k] - raw->color.black)*m);
        }
    }
    else
    {
      // scale to fit
      memset(img->mipf, 0, 4*p_wd*p_ht*sizeof(float));
      const float scale = fmaxf(raw_wd/f_wd, raw_ht/f_ht);
      for(int j=0; j<p_ht && (int)(scale*j)<raw_ht; j++) for(int i=0; i<p_wd && (int)(scale*i) < raw_wd; i++)
        {
          for(int k=0; k<3; k++) img->mipf[4*(j*p_wd + i) + k] = fmaxf(0.0f, (rawpx[(int)(scale*j)*raw_wd + (int)(scale*i)][k] - raw->color.black)*m);
        }
    }

    // don't write mip4, it's shitty anyways (altered image has to be processed)

    dt_image_release(img, DT_IMAGE_MIPF, 'w');
    dt_image_release(img, DT_IMAGE_MIPF, 'r');
    // clean up raw stuff.
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    raw = NULL;
    image = NULL;
    // not a thumbnail!
    img->flags &= ~DT_IMAGE_THUMBNAIL;
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_RAW;
    return DT_IMAGEIO_OK;
  }

error_raw_cache_full:
  fprintf(stderr, "[imageio_open_raw_preview] could not get image from thumbnail!\n");
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  return DT_IMAGEIO_CACHE_FULL;

error_raw_corrupted:
  fprintf(stderr, "[imageio_open_raw_preview] could not get image from thumbnail!\n");
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  return DT_IMAGEIO_FILE_CORRUPTED;
}

dt_imageio_retval_t dt_imageio_open_raw(dt_image_t *img, const char *filename)
{
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = 0; /* dcraw -h */
  raw->params.use_camera_wb = 0;
  raw->params.use_auto_wb = 0;
  raw->params.med_passes = 0;//img->raw_params.med_passes;
  raw->params.no_auto_bright = 1;
  // raw->params.filtering_mode |= LIBRAW_FILTERING_NOBLACKS;
  // raw->params.document_mode = 2; // no color scaling, no black, no max, no wb..?
  raw->params.document_mode = 2; // color scaling (clip,wb,max) and black point, but no demosaic
  raw->params.output_color = 0;
  raw->params.output_bps = 16;
  raw->params.user_flip = img->raw_params.user_flip;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
  // raw->params.user_qual = img->raw_params.demosaic_method; // 3: AHD, 2: PPG, 1: VNG
  raw->params.user_qual = 0;
  // raw->params.four_color_rgb = img->raw_params.four_color_rgb;
  raw->params.four_color_rgb = 0;
  raw->params.use_camera_matrix = 0;
  raw->params.green_matching = 0;
  raw->params.highlight = 1;
  raw->params.threshold = 0;
  raw->params.auto_bright_thr = img->raw_auto_bright_threshold;

  // raw->params.amaze_ca_refine = 0;
  raw->params.fbdd_noiserd    = 0;

  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  raw->params.user_qual = 0;
  raw->params.half_size = 0;

  ret = libraw_unpack(raw);
  img->black   = raw->color.black/65535.0;
  img->maximum = raw->color.maximum/65535.0;
  img->bpp = sizeof(uint16_t);
  // printf("black, max: %d %d %f %f\n", raw->color.black, raw->color.maximum, img->black, img->maximum);
  HANDLE_ERRORS(ret, 1);
  ret = libraw_dcraw_process(raw);
  // ret = libraw_dcraw_document_mode_processing(raw);
  HANDLE_ERRORS(ret, 1);
  image = libraw_dcraw_make_mem_image(raw, &ret);
  HANDLE_ERRORS(ret, 1);

  // filters seem only ever to take a useful value after unpack/process
  img->filters = raw->idata.filters;
  img->orientation = raw->sizes.flip;
  img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
  img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
  img->exif_iso = raw->other.iso_speed;
  img->exif_exposure = raw->other.shutter;
  img->exif_aperture = raw->other.aperture;
  img->exif_focal_length = raw->other.focal_len;
  g_strlcpy(img->exif_maker, raw->idata.make, sizeof(img->exif_maker));
  img->exif_maker[sizeof(img->exif_maker) - 1] = 0x0;
  g_strlcpy(img->exif_model, raw->idata.model, sizeof(img->exif_model));
  img->exif_model[sizeof(img->exif_model) - 1] = 0x0;
  dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);

  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    return DT_IMAGEIO_CACHE_FULL;
  }
  dt_image_check_buffer(img, DT_IMAGE_FULL, (img->width)*(img->height)*sizeof(uint16_t));
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(img, image, raw)
#endif
  for(int k=0; k<img->width*img->height; k++)
    ((uint16_t *)img->pixels)[k] = CLAMPS((((uint16_t *)image->data)[k] - raw->color.black)*65535.0f/(float)(raw->color.maximum - raw->color.black), 0, 0xffff);
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;
  dt_image_release(img, DT_IMAGE_FULL, 'w');

  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_RAW;
  return DT_IMAGEIO_OK;
}

/* magic data: offset,length, xx, yy, ... 
    just add magic bytes to match to this struct
    to extend mathc on ldr formats.
*/
static const uint8_t _imageio_ldr_magic[] =  {
    /* jpeg magics */
    0x00, 0x02, 0xff, 0xd8,                         // SOI marker
  
    /* png image */
    0x01, 0x03, 0x50, 0x4E, 0x47,                   // ASCII 'PNG'

    /* tiff image, intel */
    // 0x00, 0x04, 0x4d, 0x4d, 0x00, 0x2a,          // unfortunately fails because raw is similar

    /* tiff image, motorola */
    // 0x00, 0x04, 0x49, 0x49, 0x2a, 0x00
};

gboolean dt_imageio_is_ldr(const char *filename)
{
  int offset=0;
  uint8_t block[16]={0};
  FILE *fin = fopen(filename,"rb");
  if (fin)
  {
    /* read block from file */
    int s = fread(block,16,1,fin);
    fclose(fin);
    
    /* compare magic's */
    while (s)
    {
      if (memcmp(_imageio_ldr_magic+offset+2, block + _imageio_ldr_magic[offset], _imageio_ldr_magic[offset+1]) == 0)
          return TRUE;
      offset += 2 + (_imageio_ldr_magic+offset)[1];
      
      /* check if finished */
      if(offset >= sizeof(_imageio_ldr_magic))
        break;
    }
  }
  return FALSE;
}

dt_imageio_retval_t dt_imageio_open_ldr_preview(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_tiff_preview(img, filename);
  if(ret == DT_IMAGEIO_OK) goto all_good;
  if(ret == DT_IMAGEIO_CACHE_FULL) return ret;

  // jpeg stuff here:
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);
  const int orientation = dt_image_orientation(img);

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(orientation & 4)
  {
    img->width  = jpg.height;
    img->height = jpg.width;
  }
  else
  {
    img->width  = jpg.width;
    img->height = jpg.height;
  }
  uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
  if(dt_imageio_jpeg_read(&jpg, tmp))
  {
    free(tmp);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  dt_image_buffer_t mip;
  // the mip4 loading seems to lead to race conditions, so it's disabled here:
  // dt_ctl_gui_mode_t mode = dt_conf_get_int("ui_last/view");
  // const int altered = dt_image_altered(img) || (img == darktable.develop->image && mode == DT_DEVELOP);
  if(1)//altered)
  {
    // the image has a history stack. we want mipf and process it!
    mip = DT_IMAGE_MIPF;
  }
  else
  {
    // the image has no history stack. we want mip4 directly.
    mip = DT_IMAGE_MIP4;
  }

  if(dt_image_alloc(img, mip))
  {
    free(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }

  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, mip, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, mip, &f_wd, &f_ht);

  // printf("mip sizes: %d %d -- %f %f\n", p_wd, p_ht, f_wd, f_ht);
  // FIXME: there is a black border on the left side of a portrait image!

  dt_image_check_buffer(img, mip, mip==DT_IMAGE_MIP4?4*p_wd*p_ht*sizeof(uint8_t):4*p_wd*p_ht*sizeof(float));
  const int p_ht2 = orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
  const int p_wd2 = orientation & 4 ? p_ht : p_wd;
  const int f_ht2 = MIN(p_ht2, (orientation & 4 ? f_wd : f_ht) + 1.0);
  const int f_wd2 = MIN(p_wd2, (orientation & 4 ? f_ht : f_wd) + 1.0);

  if(img->width == p_wd && img->height == p_ht)
  {
    // use 1:1
    if(mip == DT_IMAGE_MIP4)
      for (int j=0; j < jpg.height; j++) for (int i=0; i < jpg.width; i++) for(int k=0; k<3; k++)
            img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
    else
      for (int j=0; j < jpg.height; j++) for (int i=0; i < jpg.width; i++) for(int k=0; k<3; k++)
            img->mipf[4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+k] = tmp[4*jpg.width*j+4*i+k]*(1.0/255.0);
  }
  else
  {
    // scale to fit
    if(mip == DT_IMAGE_MIP4) memset(img->mip[mip], 0, 4*p_wd*p_ht*sizeof(uint8_t));
    else                     memset(img->mipf, 0,     4*p_wd*p_ht*sizeof(float));
    const float scale = fmaxf(img->width/f_wd, img->height/f_ht);
    if(mip == DT_IMAGE_MIP4)
      for(int j=0; j<p_ht2 && scale*j<jpg.height; j++) for(int i=0; i<p_wd2 && scale*i < jpg.width; i++)
        {
          uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
          for(int k=0; k<3; k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+2-k] = cam[k];
        }
    else
      for(int j=0; j<p_ht2 && scale*j<jpg.height; j++) for(int i=0; i<p_wd2 && scale*i < jpg.width; i++)
        {
          uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
          for(int k=0; k<3; k++) img->mipf[4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, f_wd2, f_ht2, orientation)+k] = cam[k]*(1.0/255.0);
        }
  }
  free(tmp);
  dt_image_release(img, mip, 'w');
  if(mip == DT_IMAGE_MIP4)
    dt_image_update_mipmaps(img);
  dt_image_release(img, mip, 'r');
all_good:
  img->filters = 0;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;
  return DT_IMAGEIO_OK;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t dt_imageio_open_ldr(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_tiff(img, filename);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    return ret;
  }

  // jpeg stuff here:
  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);
  const int orientation = dt_image_orientation(img);

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return DT_IMAGEIO_FILE_CORRUPTED;
  if(orientation & 4)
  {
    img->width  = jpg.height;
    img->height = jpg.width;
  }
  else
  {
    img->width  = jpg.width;
    img->height = jpg.height;
  }
  uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
  if(dt_imageio_jpeg_read(&jpg, tmp))
  {
    free(tmp);
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  img->bpp = 4*sizeof(float);
  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    free(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }

  const int ht2 = orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = orientation & 4 ? img->height : img->width;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 4*img->width*img->height*sizeof(float));

  for(int j=0; j < jpg.height; j++)
    for(int i=0; i < jpg.width; i++)
      for(int k=0; k<3; k++) img->pixels[4*dt_imageio_write_pos(i, j, wd2, ht2, wd2, ht2, orientation)+k] = (1.0/255.0)*tmp[4*jpg.width*j+4*i+k];

  free(tmp);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  // try to fill mipf
  dt_image_raw_to_preview(img, img->pixels);

  img->filters = 0;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;
  return DT_IMAGEIO_OK;
}

void dt_imageio_to_fractional(float in, uint32_t *num, uint32_t *den)
{
  if(!(in >= 0))
  {
    *num = *den = 0;
    return;
  }
  *den = 1;
  *num = (int)(in**den + .5f);
  while(fabsf(*num/(float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in**den + .5f);
  }
}

int dt_imageio_export(dt_image_t *img, const char *filename, dt_imageio_module_format_t *format, dt_imageio_module_data_t *format_params)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;

  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_t pipe;
  if(!dt_dev_pixelpipe_init_export(&pipe, wd, ht))
  {
    dt_control_log(_("failed to allocate memory for export, please lower the threads used for export or buy more memory."));
    dt_dev_cleanup(&dev);
    return 0;
  }

  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);
  dt_show_times(&start, "[export] creating pixelpipe", NULL);

  // find output color profile for this image:
  int sRGB = 1;
  gchar *overprofile = dt_conf_get_string("plugins/lighttable/export/iccprofile");
  if(overprofile && !strcmp(overprofile, "sRGB"))
  {
    sRGB = 1;
  }
  else if(!overprofile || !strcmp(overprofile, "image"))
  {
    GList *modules = dev.iop;
    dt_iop_module_t *colorout = NULL;
    while (modules)
    {
      colorout = (dt_iop_module_t *)modules->data;
      if (strcmp(colorout->op, "colorout") == 0)
      {
        dt_iop_colorout_params_t *p = (dt_iop_colorout_params_t *)colorout->params;
        if(!strcmp(p->iccprofile, "sRGB")) sRGB = 1;
        else sRGB = 0;
      }
      modules = g_list_next(modules);
    }
  }
  else
  {
    sRGB = 0;
  }
  g_free(overprofile);

  // get only once at the beginning, in case the user changes it on the way:
  const int high_quality_processing = ((format_params->max_width  == 0 || format_params->max_width  >= pipe.processed_width ) &&
                                       (format_params->max_height == 0 || format_params->max_height >= pipe.processed_height)) ? 0 :
                                        dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  const int width  = high_quality_processing ? 0 : format_params->max_width;
  const int height = high_quality_processing ? 0 : format_params->max_height;
  const float scalex = width  > 0 ? fminf(width /(float)pipe.processed_width,  1.0) : 1.0;
  const float scaley = height > 0 ? fminf(height/(float)pipe.processed_height, 1.0) : 1.0;
  const float scale = fminf(scalex, scaley);
  int processed_width  = scale*pipe.processed_width;
  int processed_height = scale*pipe.processed_height;
  const int bpp = format->bpp(format_params);

  // downsampling done last, if high quality processing was requested:
  uint8_t *outbuf = pipe.backbuf;
  uint8_t *moutbuf = NULL; // keep track of alloc'ed memory
  if(high_quality_processing)
  {
    dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
    const float scalex = format_params->max_width  > 0 ? fminf(format_params->max_width /(float)pipe.processed_width,  1.0) : 1.0;
    const float scaley = format_params->max_height > 0 ? fminf(format_params->max_height/(float)pipe.processed_height, 1.0) : 1.0;
    const float scale = fminf(scalex, scaley);
    processed_width  = scale*pipe.processed_width  + .5f;
    processed_height = scale*pipe.processed_height + .5f;
    moutbuf = (uint8_t *)dt_alloc_align(64, sizeof(float)*processed_width*processed_height*4);
    outbuf = moutbuf;
    // now downscale into the new buffer:
    dt_iop_roi_t roi_in, roi_out;
    roi_in.x = roi_in.y = roi_out.x = roi_out.y = 0;
    roi_in.scale = 1.0;
    roi_out.scale = scale;
    roi_in.width = pipe.processed_width;
    roi_in.height = pipe.processed_height;
    roi_out.width = processed_width;
    roi_out.height = processed_height;
    dt_iop_clip_and_zoom((float *)outbuf, (float *)pipe.backbuf, &roi_out, &roi_in, processed_width, pipe.processed_width);
  }
  else
  {
    // do the processing (8-bit with special treatment, to make sure we can use openmp further down):
    if(bpp == 8)
      dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
    else
      dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
    outbuf = pipe.backbuf;
  }

  // downconversion to low-precision formats:
  if(bpp == 8)
  {
    // ldr output: char
    if(high_quality_processing)
    {
      const float *const inbuf = (float *)outbuf;
      for(int k=0; k<processed_width*processed_height; k++)
      {
        // convert in place, this is unfortunately very serial.. 
        const uint8_t r = CLAMP(inbuf[4*k+0]*0xff, 0, 0xff);
        const uint8_t g = CLAMP(inbuf[4*k+1]*0xff, 0, 0xff);
        const uint8_t b = CLAMP(inbuf[4*k+2]*0xff, 0, 0xff);
        outbuf[4*k+0] = r;
        outbuf[4*k+1] = g;
        outbuf[4*k+2] = b;
      }
    }
    else
    {
      uint8_t *const buf8 = pipe.backbuf;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(processed_width, processed_height) schedule(static)
#endif
      // just flip byte order
      for(int k=0; k<processed_width*processed_height; k++)
      {
        uint8_t tmp = buf8[4*k+0];
        buf8[4*k+0] = buf8[4*k+2];
        buf8[4*k+2] = tmp;
      }
    }
  }
  else if(bpp == 16)
  {
    // uint16_t per color channel
    float    *buff  = (float *)   outbuf;
    uint16_t *buf16 = (uint16_t *)outbuf;
    for(int y=0; y<processed_height; y++) for(int x=0; x<processed_width ; x++)
    {
      // convert in place
      const int k = x + processed_width*y;
      for(int i=0; i<3; i++) buf16[4*k+i] = CLAMP(buff[4*k+i]*0x10000, 0, 0xffff);
    }
  }
  // else output float, no further harm done to the pixels :)

  int length;
  uint8_t exif_profile[65535]; // C++ alloc'ed buffer is uncool, so we waste some bits here.
  char pathname[1024];
  dt_image_full_path(img->id, pathname, 1024);
  length = dt_exif_read_blob(exif_profile, pathname, sRGB, img->id);

  format_params->width  = processed_width;
  format_params->height = processed_height;
  const int res = format->write_image (format_params, filename, outbuf, exif_profile, length, img->id);

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  free(moutbuf);
  return res;
}


// =================================================
//   combined reading
// =================================================

static inline int
has_ldr_extension(const char *filename)
{
  // TODO: this is a bad, lazy hack to avoid me coding a true libmagic fix here!
  int ret = 0;
  const char *cc = filename + strlen(filename);
  for(; *cc!='.'&&cc>filename; cc--);
  gchar *ext = g_ascii_strdown(cc+1, -1);
  if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg") ||
     !strcmp(ext, "tif") || !strcmp(ext, "tiff"))
     ret = 1;
  g_free(ext);
  return ret;
}

dt_imageio_retval_t dt_imageio_open(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  
  /* check if file is ldr using magic's */
  if (dt_imageio_is_ldr(filename))
    ret = dt_imageio_open_ldr(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
#ifdef HAVE_RAWSPEED
    ret = dt_imageio_open_rawspeed(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
#endif
    ret = dt_imageio_open_raw(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_hdr(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)      // Failsafing, if ldr magic test fails..
      ret = dt_imageio_open_ldr(img, filename);
  if(ret == DT_IMAGEIO_OK) dt_image_cache_flush_no_sidecars(img);
  img->flags &= ~DT_IMAGE_THUMBNAIL;
  img->dirty = 1;
  return ret;
}

dt_imageio_retval_t dt_imageio_open_preview(dt_image_t *img, const char *filename)
{
  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  
  /* check if file is ldr using magic's */
  if (dt_imageio_is_ldr(filename))
    ret = dt_imageio_open_ldr_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
 #ifdef HAVE_RAWSPEED
    ret = dt_imageio_open_rawspeed_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
#endif
    ret = dt_imageio_open_raw_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_hdr_preview(img, filename);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)      // Failsafing, if ldr magic test fails..
    ret = dt_imageio_open_ldr_preview(img, filename);
  if(ret == DT_IMAGEIO_OK) dt_image_cache_flush_no_sidecars(img);
  img->dirty = 1;
  return ret;
}

// =================================================
//   dt-file synching
// =================================================

int dt_imageio_dt_read (const int imgid, const char *filename)
{
  FILE *f = fopen(filename, "rb");
  if(!f) return 1;

  sqlite3_stmt *stmt;
  int num = 0;
  size_t rd;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize (stmt);

  uint32_t magic = 0;
  rd = fread(&magic, sizeof(int32_t), 1, f);
  if(rd != 1 || magic != 0xd731337) goto delete_old_config;

  sqlite3_stmt *stmt_sel_num, *stmt_ins_hist, *stmt_upd_hist;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num from history where imgid = ?1 and num = ?2", -1, &stmt_sel_num, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into history (imgid, num) values (?1, ?2)", -1, &stmt_ins_hist, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update history set operation = ?1, op_params = ?2, module = ?3, enabled = ?4 where imgid = ?5 and num = ?6", -1, &stmt_upd_hist, NULL);
  while(!feof(f))
  {
    int32_t enabled, len, modversion;
    dt_dev_operation_t op;
    rd = fread(&enabled, sizeof(int32_t), 1, f);
    if(feof(f)) break;
    if(rd < 1) goto delete_old_config;
    rd = fread(op, sizeof(dt_dev_operation_t), 1, f);
    if(rd < 1) goto delete_old_config;
    rd = fread(&modversion, sizeof(int32_t), 1, f);
    if(rd < 1) goto delete_old_config;
    rd = fread(&len, sizeof(int32_t), 1, f);
    if(rd < 1) goto delete_old_config;
    char *params = (char *)malloc(len);
    rd = fread(params, 1, len, f);
    if(rd < len)
    {
      free(params);
      goto delete_old_config;
    }
    DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt_sel_num, 2, num);
    if(sqlite3_step(stmt_sel_num) != SQLITE_ROW)
    {
      DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 1, imgid);
      DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_hist, 2, num);
      sqlite3_step (stmt_ins_hist);
      sqlite3_reset(stmt_ins_hist);
      sqlite3_clear_bindings(stmt_ins_hist);
    }
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt_upd_hist, 1, op, strlen(op), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt_upd_hist, 2, params, len, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 3, modversion);
    DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 4, enabled);
    DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 5, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_hist, 6, num);
    sqlite3_step (stmt_upd_hist);
    free(params);
    num ++;
    sqlite3_reset(stmt_sel_num);
    sqlite3_clear_bindings(stmt_sel_num);
    sqlite3_reset(stmt_upd_hist);
    sqlite3_clear_bindings(stmt_upd_hist);
  }
  sqlite3_finalize(stmt_sel_num);
  sqlite3_finalize(stmt_ins_hist);
  sqlite3_finalize(stmt_upd_hist);
  fclose(f);
  return 0;
delete_old_config:
  fclose(f);
  return g_unlink(filename);
}


// =================================================
// tags synching
// =================================================

int dt_imageio_dttags_read (dt_image_t *img, const char *filename)
{
  int stars = 1;
  char line[512]= {0};
  FILE *f = fopen(filename, "rb");
  if(!f) return 1;

  // dt_image_t *img = dt_image_cache_get(imgid, 'w');
  sqlite3_stmt *stmt_upd_tagxtag, *stmt_del_tagged, *stmt_sel_id, *stmt_ins_tagxtag, *stmt_upd_tagxtag2, *stmt_ins_tags, *stmt_ins_tagged, *stmt_upd_tagxtag3;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update tagxtag set count = count - 1 where "
                              "(id2 in (select tagid from tagged_images where imgid = ?2)) or "
                              "(id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt_upd_tagxtag, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from tagged_images where imgid = ?1", -1, &stmt_del_tagged, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from tags where name = ?1", -1, &stmt_sel_id, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into tagxtag select id, ?1, 0 from tags", -1, &stmt_ins_tagxtag, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update tagxtag set count = 1000000 where id1 = ?1 and id2 = ?1", -1, &stmt_upd_tagxtag2, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into tags (id, name) values (null, ?1)", -1, &stmt_ins_tags, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "insert into tagged_images (tagid, imgid) values (?1, ?2)", -1, &stmt_ins_tagged, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update tagxtag set count = count + 1 where "
                              "(id1 = ?1 and id2 in (select tagid from tagged_images where imgid = ?2)) or "
                              "(id2 = ?1 and id1 in (select tagid from tagged_images where imgid = ?2))", -1, &stmt_upd_tagxtag3, NULL);

  while( fgets( line, 512, f ) )
  {
    if( strncmp( line, "stars:", 6) == 0)
    {
      if( sscanf( line, "stars: %d\n", &stars) == 1 )
        img->flags = (img->flags & ~0x7) | (0x7 & stars);
    }
    else if( strncmp( line, "rawimport:",10) == 0)
    {
      sscanf( line, "rawimport: %f %f %d\n", &img->raw_denoise_threshold, &img->raw_auto_bright_threshold, (int32_t *)&img->raw_params);
    }
    else if( strncmp( line, "colorlabels:",12) == 0)
    {
      // Remove associated color labels
      dt_colorlabels_remove_labels( img->id );

      if( strlen(line+12) > 1 )
      {
        char *colors=line+12;
        char *p=colors+1;
        while( *p!=0)
        {
          if(*p==' ') *p='\0';
          p++;
        }
        p=colors;
        while( *p != '\0' )
        {
          dt_colorlabels_set_label( img->id, atoi(p) );
          p+=strlen(p)+1;
        }

      }
    }
    else if( strncmp( line, "tags:",5) == 0)
    {
      // Special, tags should always be placed at end of dttags file....

      // consistency: strip all tags from image (tagged_image, tagxtag)
      DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag, 1, img->id);
      sqlite3_step(stmt_upd_tagxtag);
      sqlite3_reset(stmt_upd_tagxtag);
      sqlite3_clear_bindings(stmt_upd_tagxtag);

      // remove from tagged_images
      DT_DEBUG_SQLITE3_BIND_INT(stmt_del_tagged, 1, img->id);
      sqlite3_step(stmt_del_tagged);
      sqlite3_reset(stmt_del_tagged);
      sqlite3_clear_bindings(stmt_del_tagged);

      // while read line, add tag to db.
      while(fscanf(f, "%[^\n]\n", line) != EOF)
      {
        int tagid = -1;
        // check if tag is available, get its id:
        for(int k=0; k<2; k++)
        {
          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_sel_id, 1, line, strlen(line), SQLITE_TRANSIENT);
          if(sqlite3_step(stmt_sel_id) == SQLITE_ROW)
            tagid = sqlite3_column_int(stmt_sel_id, 0);
          sqlite3_reset(stmt_sel_id);
          sqlite3_clear_bindings(stmt_sel_id);
          if(tagid > 0)
          {
            if(k == 1)
            {
              DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagxtag, 1, tagid);
              sqlite3_step(stmt_ins_tagxtag);
              sqlite3_reset(stmt_ins_tagxtag);
              sqlite3_clear_bindings(stmt_ins_tagxtag);
              DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag2, 1, tagid);
              sqlite3_step(stmt_upd_tagxtag2);
              sqlite3_reset(stmt_upd_tagxtag2);
              sqlite3_clear_bindings(stmt_upd_tagxtag2);
            }
            break;
          }
          // create this tag (increment id, leave icon empty), retry.
          DT_DEBUG_SQLITE3_BIND_TEXT(stmt_ins_tags, 1, line, strlen(line), SQLITE_TRANSIENT);
          sqlite3_step(stmt_ins_tags);
          sqlite3_reset(stmt_ins_tags);
          sqlite3_clear_bindings(stmt_ins_tags);
        }
        // associate image and tag.
        DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagged, 1, tagid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt_ins_tagged, 2, img->id);
        sqlite3_step(stmt_ins_tagged);
        sqlite3_reset(stmt_ins_tagged);
        sqlite3_clear_bindings(stmt_ins_tagged);
        DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag3, 1, tagid);
        DT_DEBUG_SQLITE3_BIND_INT(stmt_upd_tagxtag3, 2, img->id);
        sqlite3_step(stmt_upd_tagxtag3);
        sqlite3_reset(stmt_upd_tagxtag3);
        sqlite3_clear_bindings(stmt_upd_tagxtag3);
      }

    }
    memset( line,0,512);
  }
  sqlite3_finalize(stmt_upd_tagxtag);
  sqlite3_finalize(stmt_del_tagged);
  sqlite3_finalize(stmt_sel_id);
  sqlite3_finalize(stmt_ins_tagxtag);
  sqlite3_finalize(stmt_upd_tagxtag2);
  sqlite3_finalize(stmt_ins_tags);
  sqlite3_finalize(stmt_ins_tagged);
  sqlite3_finalize(stmt_upd_tagxtag3);

  fclose(f);
  dt_image_cache_flush_no_sidecars(img);
  // dt_image_cache_release(img, 'w');
  return 0;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
