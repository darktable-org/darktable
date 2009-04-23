#ifdef HAVE_CONFIG_H
  #include "../config.h"
#endif
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/image_compression.h"
#include "common/darktable.h"
#include "library/library.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "libraw/libraw.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#ifdef HAVE_MAGICK
  #include <magick/MagickCore.h>
#endif
#include <libexif/exif-tag.h>
#include <libexif/exif-content.h>
#include <libexif/exif-data.h>
// #include <exif-log.h>
#include <string.h>

int dt_imageio_preview_write(dt_image_t *img, dt_image_buffer_t mip)
{
  if(mip == DT_IMAGE_NONE || mip == DT_IMAGE_FULL) return 1;
  if(mip == DT_IMAGE_MIPF)
  {
    sqlite3_stmt *stmt;
    int rc, wd, ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIPF, &wd, &ht);
    dt_image_check_buffer(img, mip, 3*wd*ht*sizeof(float));
    dt_image_alloc(img, DT_IMAGE_MIP4);
    dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*wd*ht*sizeof(uint8_t));
    dt_image_compress(img->mipf, img->mip[DT_IMAGE_MIP4], wd, ht);
    rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
    rc = sqlite3_bind_blob(stmt, 1, img->mip[DT_IMAGE_MIP4], sizeof(uint8_t)*wd*ht, SQLITE_STATIC);
    rc = sqlite3_bind_int (stmt, 2, img->id);
    rc = sqlite3_bind_int (stmt, 3, mip);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    dt_image_release(img, DT_IMAGE_MIP4, 'w');
    return 0;
  }

  sqlite3_stmt *stmt;
  int rc;
  int wd, ht;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
#ifdef HAVE_MAGICK
  ExceptionInfo *exception = AcquireExceptionInfo();
  ImageInfo *image_info = CloneImageInfo((ImageInfo *) NULL);
  Image *image = ConstituteImage(wd, ht, "RGBA", CharPixel, img->mip[mip], exception);
  if (image == (Image *) NULL)
  {
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    fprintf(stderr, "[preview_write] could not constitute magick image!\n");
    return 1;
  }
  (void)strncpy(image_info->magick, "jpeg", 4);
  image_info->quality = 95;
  size_t length;
  uint8_t *blob = ImageToBlob(image_info, image, &length, exception);
  rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_blob(stmt, 1, blob, sizeof(uint8_t)*length, (void (*)(void *))RelinquishMagickMemory);
#else
  uint8_t *blob = img->mip[mip];
  size_t length = 4*wd*ht*sizeof(uint8_t);
  rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_blob(stmt, 1, blob, sizeof(uint8_t)*length, SQLITE_STATIC);
#endif
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_int (stmt, 2, img->id);
  rc = sqlite3_bind_int (stmt, 3, mip);
  rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "[preview_write] update mipmap failed: %s\n", sqlite3_errmsg(darktable.db));
  rc = sqlite3_finalize(stmt);

#ifdef HAVE_MAGICK
  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);
#endif
  return 0;
}

int dt_imageio_preview_read(dt_image_t *img, dt_image_buffer_t mip)
{
  if(mip == DT_IMAGE_NONE || mip == DT_IMAGE_FULL) return 1;
  if(img->mip[mip])
  { // already loaded?
    dt_image_buffer_t mip2 = dt_image_get(img, mip, 'r');
    if(mip2 != mip) dt_image_release(img, mip2, 'r');
    else return 0;
  }
  sqlite3_stmt *stmt;
  int rc, wd, ht;
  size_t length = 0;
  const void *blob = NULL;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  rc = sqlite3_prepare_v2(darktable.db, "select data from mipmaps where imgid = ?1 and level = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  rc = sqlite3_bind_int (stmt, 2, mip);
  HANDLE_SQLITE_ERR(rc);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    blob = sqlite3_column_blob(stmt, 0);
    length = sqlite3_column_bytes(stmt, 0);
  }
  if(!blob) 
  {
    fprintf(stderr, "[preview_read] could not get mipmap from database: %s, trying recovery.\n", sqlite3_errmsg(darktable.db));
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from images where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    (void)sqlite3_step(stmt);
    char filename[512];
    strncpy(filename, img->filename, 512);
    int film_id = img->film_id;
    dt_image_cache_release(img, 'r');
    // TODO: need to preserve img id!
    return dt_image_import(film_id, filename);
  }
  if(dt_image_alloc(img, mip))
  {
    rc = sqlite3_finalize(stmt);
    return 1;
  }

  if(mip == DT_IMAGE_MIPF)
  {
    assert(length==sizeof(uint8_t)*wd*ht);
    dt_image_check_buffer(img, mip, 3*wd*ht*sizeof(float));
    dt_image_uncompress((uint8_t *)blob, img->mipf, wd, ht);
  }
  else
  {
    dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
#ifdef HAVE_MAGICK
    ExceptionInfo *exception = AcquireExceptionInfo();
    ImageInfo *image_info = CloneImageInfo((ImageInfo *) NULL);
    Image *image = BlobToImage(image_info, blob, length, exception);
    if (image == (Image *) NULL)
    {
      CatchException(exception);
      image_info = DestroyImageInfo(image_info);
      exception = DestroyExceptionInfo(exception);
      rc = sqlite3_finalize(stmt);
      dt_image_release(img, mip, 'w');
      dt_image_release(img, mip, 'r');
      fprintf(stderr, "[preview_read] could not get image from blob!\n");
      return 1;
    }
    const PixelPacket *p;
    assert(image->rows == ht && image->columns == wd);
    for (int y=0; y < image->rows; y++)
    {
      p = AcquireImagePixels(image,0,y,image->columns,1,exception);
      if (p == (const PixelPacket *) NULL) 
      {
        dt_image_release(img, mip, 'w');
        dt_image_release(img, mip, 'r');
        fprintf(stderr, "[preview_read] pixel read failed!\n");
        return 1;
      }
      for (int x=0; x < image->columns; x++)
      {
        if(QuantumDepth == 16)
        {
          img->mip[mip][4*wd*y + 4*x + 0] = (int)p->red>>8;
          img->mip[mip][4*wd*y + 4*x + 1] = (int)p->green>>8;
          img->mip[mip][4*wd*y + 4*x + 2] = (int)p->blue>>8;
        }
        else
        {
          img->mip[mip][4*wd*y + 4*x + 0] = (int)p->red;
          img->mip[mip][4*wd*y + 4*x + 1] = (int)p->green;
          img->mip[mip][4*wd*y + 4*x + 2] = (int)p->blue;
        }
        p++;
      }
    }
    image = DestroyImage(image);
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
#else
    assert(length==sizeof(uint8_t)*4*wd*ht);
    for(int k=0;k<length;k++) img->mip[mip][k] = ((uint8_t *)blob)[k];
#endif
  }
  rc = sqlite3_finalize(stmt);
  dt_image_release(img, mip, 'w');
  return 0;
}

void dt_imageio_preview_f_to_8(int32_t p_wd, int32_t p_ht, const float *f, uint8_t *p8)
{
  for(int idx=0;idx < p_wd*p_ht; idx++)
    for(int k=0;k<3;k++) p8[4*idx+2-k] = dt_dev_default_gamma[(int)CLAMP(0xffff*f[3*idx+k], 0, 0xffff)];
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
    return -1;                                   \
  }                                                       \
}

int dt_imageio_open_raw(dt_image_t *img, const char *filename)
{
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image;
  raw->params.half_size = img->shrink; /* dcraw -h */
  raw->params.use_camera_wb = img->wb_cam;
  raw->params.use_auto_wb = img->wb_auto;
  raw->params.output_bps = 16;
  // TODO: make this user choosable.
  if(img->shrink) raw->params.user_qual = 0; // linear
  else            raw->params.user_qual = 3; // AHD
  // img->raw->params.output_color = 1;
  raw->params.use_camera_matrix = 1;
  // TODO: let this unclipped for develop, clip for preview.
  raw->params.highlight = 0; //0 clip, 1 unclip, 2 blend, 3+ rebuild
  // img->raw->params.user_flip = img->raw->sizes.flip;
  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  if(raw->idata.dng_version || (raw->sizes.iwidth <= 1200 && raw->sizes.iheight <= 800))
  { // FIXME: this is a temporary bugfix avoiding segfaults for dng images. (and to avoid shrinking on small images).
    raw->params.user_qual = 0;
    raw->params.half_size = img->shrink = 0;
  }
  ret = libraw_unpack(raw);
  HANDLE_ERRORS(ret, 1);
  ret = libraw_dcraw_process(raw);
  HANDLE_ERRORS(ret, 1);
  image = dcraw_make_mem_image(raw, &ret);
  HANDLE_ERRORS(ret, 1);

  img->shrink = raw->params.half_size;
  img->orientation = raw->sizes.flip;
  img->width  = (img->orientation & 4) ? raw->sizes.iheight : raw->sizes.iwidth;
  img->height = (img->orientation & 4) ? raw->sizes.iwidth  : raw->sizes.iheight;
  img->exif_iso = raw->other.iso_speed;
  img->exif_exposure = raw->other.shutter;
  img->exif_aperture = raw->other.aperture;
  img->exif_focal_length = raw->other.focal_len;
  strncpy(img->exif_maker, raw->idata.make, 20);
  strncpy(img->exif_model, raw->idata.model, 20);
  dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);
  
  img->width <<= img->shrink;
  img->height <<= img->shrink;
  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    return 1;
  }
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*(img->width>>img->shrink)*(img->height>>img->shrink)*sizeof(float));
  // img->pixels = (float *)malloc(sizeof(float)*3*img->width*img->height);
  const float m = 1./0xffff;
// #pragma omp parallel for schedule(static) shared(img, image)
  for(int k=0;k<3*(img->width>>img->shrink)*(img->height>>img->shrink);k++) img->pixels[k] = ((uint16_t *)(image->data))[k]*m;
  // TODO: wrap all exif data here:
  // img->dreggn = 
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return 0;
}

#if 0
static void MaxMidMin(int64_t p[3], int *maxc, int *midc, int *minc)
{
  if (p[0] > p[1] && p[0] > p[2]) {
    *maxc = 0;
    if (p[1] > p[2]) { *midc = 1; *minc = 2; }
    else { *midc = 2; *minc = 1; }
  } else if (p[1] > p[2]) {
    *maxc = 1;
    if (p[0] > p[2]) { *midc = 0; *minc = 2; }
    else { *midc = 2; *minc = 0; }
  } else {
    *maxc = 2;
    if (p[0] > p[1]) { *midc = 0; *minc = 1; }
    else { *midc = 1; *minc = 0; }
  }
}

// this is stolen from develop_linear (ufraw_developer.c), because it does a great job ;)
void dt_raw_develop(uint16_t *in, uint16_t *out, dt_image_t *img)
{
  int64_t tmppix[4];//, tmp;
  int64_t exposure = pow(2, img->exposure) * 0x10000;
  int clipped = 0;
  for(int c=0;c<img->raw->idata.colors;c++)
  {
    tmppix[c] = (uint64_t)in[c] * img->raw->color.cam_mul[c]/0x10000 - img->raw->color.black;
    if(tmppix[c] > img->raw->color.maximum) clipped = 1;
    tmppix[c] = tmppix[c] * exposure / img->raw->color.maximum;
    // tmppix[c] = (tmppix[c] * 0x10000) / img->raw.color.maximum;
  }
  if(clipped)
  {
    int64_t unclipped[3], clipped[3];
    for(int cc=0;cc<3;cc++)
    {
      // for(int c=0,tmp=0;c<img->raw->idata.colors;c++)
      //   tmp += tmppix[c] * img->raw->color.cmatrix[cc][c];
      // unclipped[cc] = MAX(tmp/0x10000, 0);
      unclipped[cc] = tmppix[cc];
    }
    for(int c=0; c<3; c++) tmppix[c] = MIN(tmppix[c], 0xFFFF);
    // for(int c=0; c<3; c++) tmppix[c] = MIN(tmppix[c], exposure);
    for(int cc=0; cc<3; cc++)
    {
      // for(int c=0, tmp=0; c<img->raw->idata.colors; c++)
      //   tmp += tmppix[c] * img->raw->color.cmatrix[cc][c];
      // clipped[cc] = MAX(tmp/0x10000, 0);
      clipped[cc] = tmppix[cc];
    }
    int maxc, midc, minc;
    MaxMidMin(unclipped, &maxc, &midc, &minc);
    int64_t unclippedLum = unclipped[maxc];
    int64_t clippedLum = clipped[maxc];
    int64_t clippedSat;
    if(clipped[maxc] < clipped[minc] || clipped[maxc] == 0)
      clippedSat = 0;
    else
      clippedSat = 0x10000 - clipped[minc] * 0x10000 / clipped[maxc];
    int64_t clippedHue;
    if(clipped[maxc] == clipped[minc]) clippedHue = 0;
    else clippedHue =
      (clipped[midc]-clipped[minc])*0x10000 /
        (clipped[maxc]-clipped[minc]);
    int64_t unclippedHue;
    if(unclipped[maxc] == unclipped[minc])
      unclippedHue = clippedHue;
    else
      unclippedHue =
        (unclipped[midc]-unclipped[minc])*0x10000 /
        (unclipped[maxc]-unclipped[minc]);
    int64_t lum = clippedLum + (unclippedLum - clippedLum) * 1/2;
    int64_t sat = clippedSat;
    int64_t hue = unclippedHue;

    tmppix[maxc] = lum;
    tmppix[minc] = lum * (0x10000-sat) / 0x10000;
    tmppix[midc] = lum * (0x10000-sat + sat*hue/0x10000) / 0x10000;
  }
  for(int c=0; c<3; c++)
    out[c] = MIN(MAX(tmppix[c], 0), 0xFFFF);
}
#endif

// =================================================
//   begin magickcore wrapper functions:
// =================================================

// transparent read method to load ldr/raw image to dt_raw_image_t with exif and so on.
int dt_imageio_open_ldr(dt_image_t *img, const char *filename)
{
#ifdef HAVE_MAGICK
  // TODO: shrink!!
  // TODO: orientation!!
  img->shrink = 0;
  img->orientation = 0;

  ImageInfo *image_info;
  ExceptionInfo *exception;
  Image *image;
  exception = AcquireExceptionInfo();
  image_info = CloneImageInfo((ImageInfo *) NULL);

  (void) strcpy(image_info->filename, filename);
  image = ReadImage(image_info, exception);
  if (image == (Image *) NULL)
  {
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    return 1;
  }

  img->width = image->columns;
  img->height = image->rows;
  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    image = DestroyImage(image);
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    return 2;
  }
  // img->pixels = (float *)malloc(sizeof(float)*3*img->width*img->height);

  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));
  const PixelPacket *p;
  for (int y=0; y < image->rows; y++)
  {
    p = AcquireImagePixels(image,0,y,image->columns,1,exception);
    if (p == (const PixelPacket *) NULL) 
    {
      image = DestroyImage(image);
      image_info = DestroyImageInfo(image_info);
      exception = DestroyExceptionInfo(exception);
      dt_image_release(img, DT_IMAGE_FULL, 'w');
      dt_image_release(img, DT_IMAGE_FULL, 'r');
      return 1;
    }
    for (int x=0; x < image->columns; x++)
    {
      if(QuantumDepth == 16)
      {
        img->pixels[3*img->width*y + 3*x + 0] = dt_dev_de_gamma[(int)p->red>>8];
        img->pixels[3*img->width*y + 3*x + 1] = dt_dev_de_gamma[(int)p->green>>8];
        img->pixels[3*img->width*y + 3*x + 2] = dt_dev_de_gamma[(int)p->blue>>8];
      }
      else
      {
        img->pixels[3*img->width*y + 3*x + 0] = dt_dev_de_gamma[(int)p->red];
        img->pixels[3*img->width*y + 3*x + 1] = dt_dev_de_gamma[(int)p->green];
        img->pixels[3*img->width*y + 3*x + 2] = dt_dev_de_gamma[(int)p->blue];
      }
      // p->opacity
      p++;
    }
  }

    // exif:Flash: 9
    // exif:FlashPixVersion: 0100
  const char *value = NULL;
  float num, den;
  value = GetImageProperty(image, "exif:DateTimeOriginal");
  if (value != (const char *) NULL) strncpy(img->exif_datetime_taken, value, 20);
  value = GetImageProperty(image, "exif:ExposureTime");
  if (value != (const char *) NULL) { num = g_ascii_strtod(value, (gchar**)&value); den = g_ascii_strtod(value+1, NULL); img->exif_exposure = num/den; }
  value = GetImageProperty(image, "exif:FNumber");
  if (value != (const char *) NULL) { num = g_ascii_strtod(value, (gchar**)&value); den = g_ascii_strtod(value+1, NULL); img->exif_aperture = num/den; }
  value = GetImageProperty(image, "exif:FocalLength");
  if (value != (const char *) NULL) { num = g_ascii_strtod(value, (gchar**)&value); den = g_ascii_strtod(value+1, NULL); img->exif_focal_length = num/den; }
  value = GetImageProperty(image, "exif:ISOSpeedRatings");
  if (value != (const char *) NULL) img->exif_iso = g_ascii_strtod(value, NULL);
  value = GetImageProperty(image, "exif:Make");
  if (value != (const char *) NULL) strncpy(img->exif_maker, value, 20);
  value = GetImageProperty(image, "exif:Model");
  if (value != (const char *) NULL) strncpy(img->exif_model, value, 20);
    // TODO:
    // exif:Orientation: 1
    // http://sylvana.net/jpegcrop/exif_orientation.html
  value = GetImageProperty(image, "exif:Orientation");
  if (value != (const char *) NULL) img->orientation = atol(value);
  

#if 0
  const StringInfo *str = GetImageProfile(image, "exif");
  for(int k=0;k<str->length;k++) putchar(str->path[k]);

  (void) GetImageProperty(image,"exif:*");
  ResetImagePropertyIterator(image);
  char *property=GetNextImageProperty(image);
  if (property != (const char *) NULL)
  {
    (void) printf("  Properties:\n");
    while (property != (const char *) NULL)
    {
      (void) printf("    %c",*property);
      if (strlen(property) > 1)
        (void) printf("%s: ",property+1);
      if (strlen(property) > 80)
        (void) printf("\n");
      const char *value=GetImageProperty(image,property);
      if (value != (const char *) NULL)
        (void) printf("%s\n",value);
      property=GetNextImageProperty(image);
    }
  }
#endif
  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return 0;
#else
  fprintf(stderr, "[open_ldr] compiled without Magick support!\n");
  return 1;
#endif
}

// batch-processing enabled write method: history stack, raw image, custom copy of gamma/tonecurves
int dt_imageio_export_f(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  // go through stack, exec stuff (like dt_dev_load_small_cache, only operate on full buf this time)
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));
  for(int k=1;k<dev.history_top;k++)
  {
    dt_dev_image_t *hist = dev.history + k;
    // TODO: need to change iop_execute signature: full window info (param struct?)
    dt_iop_execute(dev.image->pixels, dev.image->pixels, wd, ht, wd, ht, hist->operation, &(hist->op_params));
  }

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", wd, ht);
    float tmp[3];
    for(int i=0;i<wd*ht;i++) for(int k=0;k<3;k++)
    {
      tmp[k] = dev.tonecurve[(int)CLAMP(0xffff*dev.image->pixels[3*i+k], 0, 0xffff)]/(float)0x10000;
      (void)fwrite(tmp, sizeof(float)*3, 1, f);
    }
    fclose(f);
    status = 0;
  }

  dt_dev_cleanup(&dev);
  return status;
}

int dt_imageio_export_16(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  // go through stack, exec stuff (like dt_dev_load_small_cache, only operate on full buf this time)
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));
  uint16_t *buf16 = (uint16_t *)malloc(sizeof(uint16_t)*3*wd*ht);
  for(int k=1;k<dev.history_top;k++)
  {
    dt_dev_image_t *hist = dev.history + k;
    // TODO: need to change iop_execute signature: full window info (param struct?)
    dt_iop_execute(dev.image->pixels, dev.image->pixels, wd, ht, wd, ht, hist->operation, &(hist->op_params));
  }
  for(int i=0;i<wd*ht;i++) for(int k=0;k<3;k++)
  {
    buf16[3*i+k] = dev.tonecurve[(int)CLAMP(0xffff*dev.image->pixels[3*i+k], 0, 0xffff)];
    buf16[3*i+k] = (0xff00 & (buf16[3*i+k]<<8))|(buf16[3*i+k]>>8);
  }

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "P6\n%d %d\n65535\n", wd, ht);
    (void)fwrite(buf16, sizeof(uint16_t)*3, wd*ht, f);
    fclose(f);
    status = 0;
  }

  dt_dev_cleanup(&dev);
  free(buf16);
  return status;
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

#if 0
static void
log_func (ExifLog *log, ExifLogCode code, const char *domain,
	  const char *format, va_list args, void *data)
{

		vfprintf (stderr, format, args);
		fprintf (stderr, "\n");
}
#endif

int dt_imageio_export_8(dt_image_t *img, const char *filename)
{
#ifdef HAVE_MAGICK
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  // go through stack, exec stuff (like dt_dev_load_small_cache, only operate on full buf this time)
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));
  uint8_t *buf8 = (uint8_t *)malloc(sizeof(uint8_t)*3*wd*ht);
  for(int k=1;k<dev.history_top;k++)
  {
    dt_dev_image_t *hist = dev.history + k;
    // TODO: need to change iop_execute signature: full window info (param struct?)
    dt_iop_execute(dev.image->pixels, dev.image->pixels, wd, ht, wd, ht, hist->operation, &(hist->op_params));
  }
  for(int i=0;i<wd*ht;i++) for(int k=0;k<3;k++)
    buf8[3*i+k] = dev.gamma[dev.tonecurve[(int)CLAMP(0xffff*dev.image->pixels[3*i+k], 0, 0xffff)]];

  // MagickCore write
  ImageInfo *image_info;
  ExceptionInfo *exception;
  Image *image;
  exception = AcquireExceptionInfo();
  image_info = CloneImageInfo((ImageInfo *) NULL);
  image = ConstituteImage(wd, ht, "RGB", CharPixel, buf8, exception);
  if (image == (Image *) NULL)
  {
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    dt_dev_cleanup(&dev);
    free(buf8);
    return 1;
  }

#if 1
  	// ExifLog *log = NULL;

  	// log = exif_log_new ();
	// exif_log_set_func (log, log_func, NULL);

  // FIXME: this is not working at all :(
  // set image properties!
  ExifRational rat;
  int length;
  uint8_t *exif_profile;
  ExifData *exif_data = exif_data_new();
  exif_data_set_byte_order(exif_data, EXIF_BYTE_ORDER_INTEL);
  ExifContent *exif_content = exif_data->ifd[0];
  // exif_data_log(exif_data, log);
  ExifEntry *entry;

  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_EXIF]->count, exif_content->count);

#if 1
  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MAKE);
  entry->components = strlen (img->exif_maker) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  // entry->data = exif_mem_realloc (entry, entry->data, entry->size);
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_maker, entry->components);
  // exif_mem_free((ExifEntryPrivate*)(entry->priv)->mem, entry->data);
  // entry->data = img->exif_maker;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MODEL);
  entry->components = strlen (img->exif_model) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  // exif_mem_free((ExifEntryPrivate*)(entry->priv)->mem, entry->data);
  // entry->data = img->exif_model;
  // entry->data = exif_mem_realloc (entry, entry->data, entry->size);
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_model, entry->components);
#endif

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_DATE_TIME_ORIGINAL);
  // strncpy((char *)entry->data, img->exif_datetime_taken, 20);
  // entry->data = img->exif_datetime_taken;
  entry->components = 20;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)(entry->data), img->exif_datetime_taken, entry->components);
  // printf("size: %d %d\n", entry->size, entry->components);
  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_0]->count, exif_content->count);

  entry = exif_entry_new();// entry->tag = EXIF_TAG_ISO_SPEED_RATINGS;
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_ISO_SPEED_RATINGS);
  exif_set_short(entry->data, EXIF_BYTE_ORDER_INTEL, (int16_t)(img->exif_iso));
  entry->size = 2;
  entry->components = 1;
  // printf("size: %d\n", entry->size);
  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_EXIF]->count, exif_content->count);

  entry = exif_entry_new(); //entry->tag = EXIF_TAG_FNUMBER;
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FNUMBER);
  dt_imageio_to_fractional(img->exif_aperture, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;
  // printf("size: %d %d\n", entry->size, entry->components);

  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_EXIF]->count, exif_content->count);

  entry = exif_entry_new(); //entry->tag = EXIF_TAG_EXPOSURE_TIME;
  // printf("prereq: %d %d %d %d %d\n", exif_content , exif_content->priv, entry, entry->parent, exif_content->entries);
  exif_content_add_entry(exif_content, entry);
  // printf("prereq: %d %d %d %d %d\n", exif_content , exif_content->priv, entry, entry->parent, exif_content->entries);
  exif_entry_initialize(entry, EXIF_TAG_EXPOSURE_TIME);
  dt_imageio_to_fractional(img->exif_exposure, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;
  // printf("size: %d %d\n", entry->size, entry->components);

  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_EXIF]->count, exif_content->count);

  entry = exif_entry_new(); //entry->tag = EXIF_TAG_FOCAL_LENGTH;
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FOCAL_LENGTH);
  dt_imageio_to_fractional(img->exif_focal_length, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;
  // printf("size: %d %d\n", entry->size, entry->components);

  // printf("count : %d %d\n", exif_data->ifd[EXIF_IFD_EXIF]->count, exif_content->count);

  // exif_data_fix(exif_data);
  exif_data_save_data(exif_data, &exif_profile, (uint32_t *)&length);
  // printf("exif header size %u\n", length);
  // TODO: try to free al the alloc from above!
  // exif_content_free(exif_content);
  exif_data_free(exif_data);
  StringInfo *profile = AcquireStringInfo(length);
  SetStringInfoDatum(profile, exif_profile);
  (void)SetImageProfile(image, "exif", profile);
  profile = DestroyStringInfo(profile);
  free(exif_profile);
#endif

#if 0
  char value[100];
  uint32_t num, den;
  // FIXME: this refuses to work :(
  //(void)DefineImageProperty(image, "exif:DateTimeOriginal");
  (void)SetImageProperty   (image, "exif:DateTimeOriginal", img->exif_datetime_taken);
  //(void)DefineImageProperty(image, "exif:Make");
  (void)SetImageProperty   (image, "exif:Make", img->exif_maker);
  //(void)DefineImageProperty(image, "exif:Model");
  (void)SetImageProperty   (image, "exif:Model", img->exif_model);
  snprintf(value, 100, "%.0f", img->exif_iso);
  //(void)DefineImageProperty(image, "exif:ISOSpeedRatings");
  (void)SetImageProperty   (image, "exif:ISOSpeedRatings", value);
  dt_imageio_to_fractional(img->exif_exposure, &num, &den);
  snprintf(value, 100, "%d/%d", num, den);
  //(void)DefineImageProperty(image, "exif:ExposureTime");
  (void)SetImageProperty   (image, "exif:ExposureTime", value);
  dt_imageio_to_fractional(img->exif_aperture, &num, &den);
  snprintf(value, 100, "%d/%d", num, den);
  //(void)DefineImageProperty(image, "exif:FNumber");
  (void)SetImageProperty   (image, "exif:FNumber", value);
  dt_imageio_to_fractional(img->exif_exposure, &num, &den);
  snprintf(value, 100, "%d/%d", num, den);
  //(void)DefineImageProperty(image, "exif:FocalLength");
  (void)SetImageProperty   (image, "exif:FocalLength", value);
  // printf("properties: %d\n", image->properties);
#endif

  image_info->quality = 97;
  (void) strcpy(image->filename, filename);
  WriteImage(image_info, image);

  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);
  free(buf8);
  dt_dev_cleanup(&dev);
  return 0;
#else
  fprintf(stderr, "[export_8] compiled without Magick support!\n");
  return 1;
#endif
}

// =================================================
//   combined reading
// =================================================

int dt_imageio_open(dt_image_t *img, const char *filename)
{ // first try raw loading
  if(!dt_imageio_open_raw(img, filename)) return 0;
  if(!dt_imageio_open_ldr(img, filename)) return 0;
  return 1;
}

