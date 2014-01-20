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
#include "common/darktable.h"
#include "common/colorlabels.h"
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
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/imageio_tiff.h"
#include "common/imageio_pfm.h"
#include "common/imageio_rgbe.h"
#include "common/imageio_gm.h"
#include "common/imageio_rawspeed.h"
#include "common/image_compression.h"
#include "common/mipmap_cache.h"
#include "common/styles.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "iop/colorout.h"
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


// load a full-res thumbnail:
int dt_imageio_large_thumbnail(const char *filename, uint8_t **buffer, int32_t *width, int32_t *height, int32_t *orientation)
{
  int ret = 0;
  int res = 1;
  // raw image thumbnail
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  ret = libraw_open_file(raw, filename);
  if(ret) goto libraw_fail;
  ret = libraw_unpack_thumb(raw);
  if(ret) goto libraw_fail;
  ret = libraw_adjust_sizes_info_only(raw);
  if(ret) goto libraw_fail;

  image = libraw_dcraw_make_mem_thumb(raw, &ret);
  if(!image || ret) goto libraw_fail;
  *orientation = raw->sizes.flip;
  if(image->type == LIBRAW_IMAGE_JPEG)
  {
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(image->data, image->data_size, &jpg)) goto libraw_fail;
    *buffer = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
    if(!buffer) goto libraw_fail;
    *width = jpg.width;
    *height = jpg.height;
    if(dt_imageio_jpeg_decompress(&jpg, *buffer))
    {
      free(*buffer);
      *buffer = 0;
      goto libraw_fail;
    }
    res = 0;
  }

  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  if(0)
  {
libraw_fail:
    // fprintf(stderr,"[imageio] %s: %s\n", filename, libraw_strerror(ret));
    libraw_close(raw);
    res = 1;
  }
  return res;
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

void
dt_imageio_flip_buffers_ui8_to_float(float *out, const uint8_t *in, const float black, const float white, const int ch, const int wd, const int ht, const int fwd, const int fht, const int stride, const int orientation)
{
  const float scale = 1.0f/(white - black);
  if(!orientation)
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(in, out)
#endif
    for(int j=0; j<ht; j++) for(int i=0; i<wd; i++) for(int k=0; k<ch; k++) out[4*(j*wd + i)+k] = (in[j*stride + ch*i +k]-black)*scale;
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
    const uint8_t *in2  = in + stride*j;
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

dt_imageio_retval_t
dt_imageio_open_hdr(
  dt_image_t  *img,
  const char  *filename,
  dt_mipmap_cache_allocator_t a)
{
  // needed to alloc correct buffer size:
  img->bpp = 4*sizeof(float);
  dt_imageio_retval_t ret;
#ifdef HAVE_OPENEXR
  ret = dt_imageio_open_exr(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
#endif
  ret = dt_imageio_open_rgbe(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
  ret = dt_imageio_open_pfm(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL) goto return_label;
return_label:
  if(ret == DT_IMAGEIO_OK)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags |=  DT_IMAGE_HDR;
  }
  return ret;
}


// most common modern raw files are handled by rawspeed, which accidentally
// may go through LibRaw, since it may break history stacks because of different
// blackpoint handling. in addition guarding LibRaw from these
// extensions slightly reduces our security surface
static gboolean
_blacklisted_ext(const gchar *filename)
{
  const char *extensions_blacklist[] = { "dng", "cr2", "nef", "nrw", "orf", "rw2", "pef", "srw", "arw", NULL };
  gboolean supported = TRUE;
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_blacklist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i,strlen(*i)))
    {
      supported = FALSE;
      break;
    }
  return supported;
}

// we do not support non-Bayer raw images; make sure we skip those in order
// to prevent LibRaw from crashing
static gboolean 
_blacklisted_raw(const gchar *maker, const gchar *model)
{
  typedef struct blacklist_t {
    const gchar *maker;
    const gchar *model;
  } blacklist_t;

  blacklist_t blacklist[] = { { "fujifilm",                        "x-pro1" },
                              { "fujifilm",                        "x-e1"   },
                              { "fujifilm",                        "x-e2"   },
                              { "fujifilm",                        "x-m1"   },
                              { NULL,                              NULL     } };

  gboolean blacklisted = FALSE;

  for(blacklist_t *i = blacklist; i->maker != NULL; i++)
    if(!g_ascii_strncasecmp(maker, i->maker, strlen(i->maker)) &&
       !g_ascii_strncasecmp(model, i->model, strlen(i->model)))
    {
      blacklisted = TRUE;
      break;
    }
  return blacklisted;
}


// open a raw file, libraw path:
dt_imageio_retval_t
dt_imageio_open_raw(
  dt_image_t  *img,
  const char  *filename,
  dt_mipmap_cache_allocator_t a)
{
  if(!_blacklisted_ext(filename)) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  if(_blacklisted_raw(img->exif_maker, img->exif_model)) return DT_IMAGEIO_FILE_CORRUPTED;

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
  raw->params.user_flip = -1; // -1 means: use orientation from raw
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
  // raw->params.auto_bright_thr = img->raw_auto_bright_threshold;

  // raw->params.amaze_ca_refine = 0;
  raw->params.fbdd_noiserd    = 0;

  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  raw->params.user_qual = 0;
  raw->params.half_size = 0;

  ret = libraw_unpack(raw);
  img->raw_black_level = raw->color.black;
  img->raw_white_point = raw->color.maximum;
  HANDLE_ERRORS(ret, 1);
  ret = libraw_dcraw_process(raw);
  // ret = libraw_dcraw_document_mode_processing(raw);
  HANDLE_ERRORS(ret, 1);
  image = libraw_dcraw_make_mem_image(raw, &ret);
  HANDLE_ERRORS(ret, 1);

  // fallback for broken exif read in case of phase one H25
  if(!strncmp(img->exif_maker, "Phase One", 9))
    img->orientation = raw->sizes.flip;
  // filters seem only ever to take a useful value after unpack/process
  img->filters = raw->idata.filters;
  img->bpp = img->filters ? sizeof(uint16_t) : 4*sizeof(float);
  img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
  img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
#if 0 // disabled libraw exif data. it's inconsistent with exiv2, we don't want that.
  img->exif_iso = raw->other.iso_speed;
  img->exif_exposure = raw->other.shutter;
  img->exif_aperture = raw->other.aperture;
  img->exif_focal_length = raw->other.focal_len;
  g_strlcpy(img->exif_maker, raw->idata.make, sizeof(img->exif_maker));
  img->exif_maker[sizeof(img->exif_maker) - 1] = 0x0;
  g_strlcpy(img->exif_model, raw->idata.model, sizeof(img->exif_model));
  img->exif_model[sizeof(img->exif_model) - 1] = 0x0;
  dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);
#endif

  void *buf = dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!buf)
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    return DT_IMAGEIO_CACHE_FULL;
  }
  if(img->filters)
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(img, image, raw, buf)
#endif
    for(int k=0; k<img->width*img->height; k++)
      ((uint16_t *)buf)[k] = CLAMPS((((uint16_t *)image->data)[k] - raw->color.black)*65535.0f/(float)(raw->color.maximum - raw->color.black), 0, 0xffff);
  }
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;

  if(img->filters)
  {
    img->flags &= ~DT_IMAGE_LDR;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_RAW;
  }
  else
  {
    // ldr dng. it exists :(
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
  }
  return DT_IMAGEIO_OK;
}

/* magic data: exclusion,offset,length, xx, yy, ...
    just add magic bytes to match to this struct
    to extend mathc on ldr formats.
*/
static const uint8_t _imageio_ldr_magic[] =
{
  /* jpeg magics */
  0x00, 0x00, 0x02, 0xff, 0xd8,                         // SOI marker

#ifdef HAVE_OPENJPEG
  /* jpeg 2000, jp2 format */
  0x00, 0x00, 0x0c, 0x0, 0x0, 0x0, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A,

  /* jpeg 2000, j2k format */
  0x00, 0x00, 0x05, 0xFF, 0x4F, 0xFF, 0x51, 0x00,
#endif

  /* png image */
  0x00, 0x01, 0x03, 0x50, 0x4E, 0x47,                   // ASCII 'PNG'

  /* canon CR2 */
  0x01, 0x00, 0x0a, 0x49, 0x49, 0x2a, 0x00, 0x10, 0x00, 0x00, 0x00, 0x43, 0x52,  // Canon CR2 is like TIFF with additional magic number. must come before tiff as an exclusion

  /* tiff image, intel */
  0x00, 0x00, 0x04, 0x4d, 0x4d, 0x00, 0x2a,

  /* tiff image, motorola */
  0x00, 0x00, 0x04, 0x49, 0x49, 0x2a, 0x00
};

gboolean dt_imageio_is_ldr(const char *filename)
{
  int offset=0;
  uint8_t block[16]= {0};
  FILE *fin = fopen(filename,"rb");
  if (fin)
  {
    /* read block from file */
    int s = fread(block,16,1,fin);
    fclose(fin);

    /* compare magic's */
    while (s)
    {
      if (memcmp(_imageio_ldr_magic+offset+3, block + _imageio_ldr_magic[offset+1], _imageio_ldr_magic[offset+2]) == 0)
      {
        if(_imageio_ldr_magic[offset] == 0x01)
          return FALSE;
        else
          return TRUE;
      }
      offset += 3 + (_imageio_ldr_magic+offset)[2];

      /* check if finished */
      if(offset >= sizeof(_imageio_ldr_magic))
        break;
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
    ) return 1;
  return 0;
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
dt_imageio_retval_t
dt_imageio_open_ldr(
  dt_image_t *img,
  const char *filename,
  dt_mipmap_cache_allocator_t a)
{
  dt_imageio_retval_t ret;
  ret = dt_imageio_open_tiff(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    return ret;
  }

  ret = dt_imageio_open_png(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    return ret;
  }

#ifdef HAVE_OPENJPEG
  ret = dt_imageio_open_j2k(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
    return ret;
  }
#endif

  ret = dt_imageio_open_jpeg(img, filename, a);
  if(ret == DT_IMAGEIO_OK || ret == DT_IMAGEIO_CACHE_FULL)
  {
    img->filters = 0;
    img->flags &= ~DT_IMAGE_RAW;
    img->flags &= ~DT_IMAGE_HDR;
    img->flags |= DT_IMAGE_LDR;
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
  *num = (int)(in**den + .5f);
  while(fabsf(*num/(float)*den - in) > 0.001f)
  {
    *den *= 10;
    *num = (int)(in**den + .5f);
  }
}

int dt_imageio_export(
  const uint32_t              imgid,
  const char                 *filename,
  dt_imageio_module_format_t *format,
  dt_imageio_module_data_t   *format_params,
  const gboolean              high_quality,
  const gboolean              copy_metadata,
  dt_imageio_module_storage_t *storage,
  dt_imageio_module_data_t   *storage_params)
{
  if (strcmp(format->mime(format_params),"x-copy")==0)
    /* This is a just a copy, skip process and just export */
    return format->write_image(format_params, filename, NULL, NULL, 0, imgid);
  else
    return dt_imageio_export_with_flags(imgid, filename, format, format_params,
                                        0, 0, high_quality, 0, NULL,copy_metadata,storage,storage_params);
}

// internal function: to avoid exif blob reading + 8-bit byteorder flag + high-quality override
int dt_imageio_export_with_flags(
  const uint32_t              imgid,
  const char                 *filename,
  dt_imageio_module_format_t *format,
  dt_imageio_module_data_t   *format_params,
  const int32_t               ignore_exif,
  const int32_t               display_byteorder,
  const gboolean              high_quality,
  const int32_t               thumbnail_export,
  const char                 *filter,
  const gboolean              copy_metadata,
  dt_imageio_module_storage_t *storage,
  dt_imageio_module_data_t   *storage_params)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_mipmap_buffer_t buf;
  if(thumbnail_export && dt_conf_get_bool("plugins/lighttable/low_quality_thumbnails"))
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_F, DT_MIPMAP_BLOCKING);
  else
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING);
  dt_dev_load_image(&dev, imgid);
  const dt_image_t *img = &dev.image_storage;
  const int wd = img->width;
  const int ht = img->height;

  int res = 0;

  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_t pipe;
  res = thumbnail_export ? dt_dev_pixelpipe_init_thumbnail(&pipe, wd, ht) : dt_dev_pixelpipe_init_export(&pipe, wd, ht, format->levels(format_params));
  if(!res)
  {
    dt_control_log(_("failed to allocate memory for export, please lower the threads used for export or buy more memory."));
    dt_dev_cleanup(&dev);
    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
    return 1;
  }

  if(!buf.buf)
  {
    dt_control_log(_("image `%s' is not available!"), img->filename);
    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
    dt_dev_cleanup(&dev);
    return 1;
  }

  //  If a style is to be applied during export, add the iop params into the history
  if (!thumbnail_export && format_params->style[0] != '\0')
  {
    GList *stls;

    GList *modules = dev.iop;
    dt_iop_module_t *m = NULL;

    if ((stls=dt_styles_get_item_list(format_params->style, TRUE, -1)) == 0)
    {
      dt_control_log(_("cannot find the style '%s' to apply during export."), format_params->style);
      dt_dev_cleanup(&dev);
      dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
      return 1;
    }

    //  Add each params
    while (stls)
    {
      dt_style_item_t *s = (dt_style_item_t *) stls->data;

      modules = dev.iop;
      while (modules)
      {
        m = (dt_iop_module_t *)modules->data;

        //  since the name in the style is returned with a possible multi-name, just check the start of the name
        if (strncmp(m->op, s->name, strlen(m->op)) == 0)
        {
          dt_dev_history_item_t *h = malloc(sizeof(dt_dev_history_item_t));

          h->params = s->params;
          h->blend_params = s->blendop_params;
          h->enabled = s->enabled;
          h->module = m;
          h->multi_priority = 1;
          strcpy(h->multi_name, "");

          dev.history_end++;
          dev.history = g_list_append(dev.history, h);
          break;
        }
        modules = g_list_next(modules);
      }
      stls = g_list_next(stls);
    }
  }

  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf, buf.width, buf.height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight, &pipe.processed_width, &pipe.processed_height);
  if(filter)
  {
    if(!strncmp(filter, "pre:", 4))
      dt_dev_pixelpipe_disable_after(&pipe, filter+4);
    if(!strncmp(filter, "post:", 5))
      dt_dev_pixelpipe_disable_before(&pipe, filter+5);
  }
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
  const gboolean high_quality_processing = ((format_params->max_width  == 0 || format_params->max_width  >= pipe.processed_width ) &&
      (format_params->max_height == 0 || format_params->max_height >= pipe.processed_height)) ? FALSE :
      high_quality;
  const int width  = high_quality_processing ? 0 : format_params->max_width;
  const int height = high_quality_processing ? 0 : format_params->max_height;
  const double scalex = width  > 0 ? fminf(width /(double)pipe.processed_width,  1.0) : 1.0;
  const double scaley = height > 0 ? fminf(height/(double)pipe.processed_height, 1.0) : 1.0;
  const double scale = fminf(scalex, scaley);
  int processed_width  = scale*pipe.processed_width  + .5f;
  int processed_height = scale*pipe.processed_height + .5f;
  const int bpp = format->bpp(format_params);

  // downsampling done last, if high quality processing was requested:
  uint8_t *outbuf = pipe.backbuf;
  uint8_t *moutbuf = NULL; // keep track of alloc'ed memory
  dt_get_times(&start);
  if(high_quality_processing)
  {
    dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, processed_width, processed_height, scale);
    const double scalex = format_params->max_width  > 0 ? fminf(format_params->max_width /(double)pipe.processed_width,  1.0) : 1.0;
    const double scaley = format_params->max_height > 0 ? fminf(format_params->max_height/(double)pipe.processed_height, 1.0) : 1.0;
    const double scale = fminf(scalex, scaley);
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
  dt_show_times(&start, thumbnail_export ? "[dev_process_thumbnail] pixel pipeline processing" : "[dev_process_export] pixel pipeline processing", NULL);

  // downconversion to low-precision formats:
  if(bpp == 8 && !display_byteorder)
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

  format_params->width  = processed_width;
  format_params->height = processed_height;

  if(!ignore_exif)
  {
    int length;
    uint8_t exif_profile[65535]; // C++ alloc'ed buffer is uncool, so we waste some bits here.
    char pathname[1024];
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid, pathname, 1024, &from_cache);
    // last param is dng mode, it's false here
    length = dt_exif_read_blob(exif_profile, pathname, imgid, sRGB, processed_width, processed_height, 0);

    res = format->write_image (format_params, filename, outbuf, exif_profile, length, imgid);
  }
  else
  {
    res = format->write_image (format_params, filename, outbuf, NULL, 0, imgid);
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
  dt_free_align(moutbuf);
  /* now write xmp into that container, if possible */
  if(copy_metadata && (format->flags(format_params) & FORMAT_FLAGS_SUPPORT_XMP)) {
    dt_exif_xmp_attach(imgid, filename);
    // no need to cancel the export if this fail
  }


  if(!thumbnail_export)
  {
    dt_control_signal_raise(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_TMPFILE,imgid,filename,format,format_params,storage,storage_params);
  }
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

dt_imageio_retval_t
dt_imageio_open(
  dt_image_t  *img,               // non-const * means you hold a write lock!
  const char  *filename,          // full path
  dt_mipmap_cache_allocator_t a)  // allocate via dt_mipmap_cache_alloc
{
  /* first of all, check if file exists, don't bother to test loading if not exists */
  if(!g_file_test(filename, G_FILE_TEST_IS_REGULAR))
    return !DT_IMAGEIO_OK;

  dt_imageio_retval_t ret = DT_IMAGEIO_FILE_CORRUPTED;

  /* check if file is ldr using magic's */
  if (dt_imageio_is_ldr(filename))
    ret = dt_imageio_open_ldr(img, filename, a);

  /* silly check using file extensions: */
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL && dt_imageio_is_hdr(filename))
    ret = dt_imageio_open_hdr(img, filename, a);

#ifdef HAVE_RAWSPEED
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_rawspeed(img, filename, a);
#endif

  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_raw(img, filename, a);

#ifdef HAVE_GRAPHICSMAGICK
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_gm(img, filename, a);
#else
  if(ret != DT_IMAGEIO_OK && ret != DT_IMAGEIO_CACHE_FULL)
    ret = dt_imageio_open_ldr(img, filename, a);
#endif

  return ret;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
