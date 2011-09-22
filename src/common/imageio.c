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

dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white, const int ch, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation)
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
void

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

dt_imageio_retval_t
dt_imageio_open_hdr(
    dt_image_t  *img,
    const char  *filename,
    void       **buf)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_exr(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  ret = dt_imageio_open_rgbe(img, filename, buf);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  ret = dt_imageio_open_pfm(img, filename, buf);
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

// open a raw file, libraw path:
dt_imageio_retval_t
dt_imageio_open_raw(
    dt_image_t  *img,
    const char  *filename,
    void       **buf)
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

  *buf = dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL);
  if(!*buf)
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    return DT_IMAGEIO_CACHE_FULL;
  }
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(img, image, raw)
#endif
  for(int k=0; k<img->width*img->height; k++)
    ((uint16_t *)*buf)[k] = CLAMPS((((uint16_t *)image->data)[k] - raw->color.black)*65535.0f/(float)(raw->color.maximum - raw->color.black), 0, 0xffff);
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;

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

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t
dt_imageio_open_ldr(
    dt_image_t  *img,
    const char  *filename,
    void       **buf)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_tiff(img, filename, buf);
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

  *buf = dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL);
  if(!*buf)
  {
    free(tmp);
    return DT_IMAGEIO_CACHE_FULL;
  }

  const int ht2 = orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = orientation & 4 ? img->height : img->width;

  dt_imageio_flip_buffers_ui8_to_float((float *)*buf, tmp, 0.0f, 255.0f, 4, wd2, ht2, wd2, ht2, wd2, orientation);

  free(tmp);

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

int dt_imageio_export(const dt_image_t *img, const char *filename, dt_imageio_module_format_t *format, dt_imageio_module_data_t *format_params)
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

// TODO: interface has to change! something like:
dt_imageio_retval_t
dt_imageio_open(
    dt_image_t  *img,              // non-const * means you hold a write lock!
    const char  *filename,         // full path
    void       **buf)              // fill this buffer, allocate it via dt_mipmap_cache_alloc
{
  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;
  
  /* check if file is ldr using magic's */
  if (dt_imageio_is_ldr(filename))
    ret = dt_imageio_open_ldr(img, filename, buf);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
#ifdef HAVE_RAWSPEED
    ret = dt_imageio_open_rawspeed(img, filename, buf);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
#endif
    ret = dt_imageio_open_raw(img, filename, buf);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_hdr(img, filename, buf);
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)      // Failsafing, if ldr magic test fails..
      ret = dt_imageio_open_ldr(img, filename, buf);

  // FIXME: flushing is done in image_cache_write_release(), which has to follow this function,
  //        since we get a non-const image pointer!
  // if(ret == DT_IMAGEIO_OK) dt_image_cache_flush_no_sidecars(img);
  img->flags &= ~DT_IMAGE_THUMBNAIL;
  img->dirty = 1;
  return ret;
}



// =======================================================
//   dt-file synching (legacy functions, replaced by xmp)
// =======================================================

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
// tags synching (legacy function, replaced by xmp)
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
