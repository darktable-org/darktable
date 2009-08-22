#ifdef HAVE_CONFIG_H
  #include "../config.h"
#endif
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_png.h"
#include "common/image_compression.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "library/library.h"
#include "control/control.h"
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


int dt_imageio_preview_write(dt_image_t *img, dt_image_buffer_t mip)
{
  if(mip == DT_IMAGE_NONE || mip == DT_IMAGE_FULL) return 1;
  if(mip == DT_IMAGE_MIPF)
  {
    sqlite3_stmt *stmt;
    int rc, wd, ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIPF, &wd, &ht);
    dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*wd*ht*sizeof(float));
    uint8_t *buf = (uint8_t *)malloc(sizeof(uint8_t)*wd*ht);
    dt_image_compress(img->mipf, buf, wd, ht);
    rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
    rc = sqlite3_bind_blob(stmt, 1, buf, sizeof(uint8_t)*wd*ht, free);
    rc = sqlite3_bind_int (stmt, 2, img->id);
    rc = sqlite3_bind_int (stmt, 3, mip);
    rc = sqlite3_step(stmt);
    rc = sqlite3_finalize(stmt);
    return 0;
  }

  sqlite3_stmt *stmt;
  int rc;
  int wd, ht;
  dt_image_get_mip_size(img, mip, &wd, &ht);
  dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
#if 0//def HAVE_MAGICK
  ExceptionInfo *exception = AcquireExceptionInfo();
  ImageInfo *image_info = CloneImageInfo((ImageInfo *) NULL);
  Image *image = ConstituteImage(wd, ht, "RGBA", CharPixel, img->mip[mip], exception);
  if (image == (Image *) NULL)
  {
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    fprintf(stderr, "[preview_write] could not constitute magick image (%dx%d for mip %d)!\n", wd, ht, mip);
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
  /*uint8_t *blob = img->mip[mip];
  size_t length = 4*wd*ht*sizeof(uint8_t);*/
  uint8_t *blob = (uint8_t *)malloc(4*sizeof(uint8_t)*wd*ht);
  int length = dt_imageio_jpeg_compress(img->mip[mip], blob, wd, ht, 97);
  rc = sqlite3_prepare_v2(darktable.db, "update mipmaps set data = ?1 where imgid = ?2 and level = ?3", -1, &stmt, NULL);
  HANDLE_SQLITE_ERR(rc);
  //rc = sqlite3_bind_blob(stmt, 1, blob, sizeof(uint8_t)*length, SQLITE_STATIC);
  rc = sqlite3_bind_blob(stmt, 1, blob, length, free);
#endif
  HANDLE_SQLITE_ERR(rc);
  rc = sqlite3_bind_int (stmt, 2, img->id);
  rc = sqlite3_bind_int (stmt, 3, mip);
  rc = sqlite3_step(stmt);
  if(rc != SQLITE_DONE) fprintf(stderr, "[preview_write] update mipmap failed: %s\n", sqlite3_errmsg(darktable.db));
  rc = sqlite3_finalize(stmt);

#if 0//def HAVE_MAGICK
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
  if(!blob) return 1; // not there. will be handled by caller (load to db)
  /*{
    fprintf(stderr, "[preview_read] could not get mipmap from database: %s, removing image %s.\n", sqlite3_errmsg(darktable.db), img->filename);
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from images where id = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, img->id);
    (void)sqlite3_step(stmt);
    // char filename[512];
    // strncpy(filename, img->filename, 512);
    // int film_id = img->film_id;
    dt_image_cache_release(img, 'r');
    // TODO: need to preserve img id!
    return 1;//dt_image_import(film_id, filename);
  }*/
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
#if 0//def HAVE_MAGICK
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
    dt_imageio_jpeg_t jpg;
    if(dt_imageio_jpeg_decompress_header(blob, length, &jpg) || 
       dt_imageio_jpeg_decompress(&jpg, img->mip[mip]))
    {
      assert(jpg.width == wd && jpg.height == ht);
      rc = sqlite3_finalize(stmt);
      dt_image_release(img, mip, 'w');
      dt_image_release(img, mip, 'r');
      fprintf(stderr, "[preview_read] could not get image from blob!\n");
      return 1;
    }
    // assert(length==sizeof(uint8_t)*4*wd*ht);
    // for(int k=0;k<length;k++) img->mip[mip][k] = ((uint8_t *)blob)[k];
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

void dt_imageio_preview_8_to_f(int32_t p_wd, int32_t p_ht, const uint8_t *p8, float *f)
{
  for(int idx=0;idx < p_wd*p_ht; idx++)
    for(int k=0;k<3;k++) f[3*idx+2-k] = dt_dev_de_gamma[p8[4*idx+k]];
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

int dt_imageio_write_pos(int i, int j, int wd, int ht, int orientation)
{
  int ii = i, jj = j, w = wd, h = ht;
  if(orientation & 4)
  {
    w = ht; h = wd;
    ii = j; jj = i;
  }
  if(orientation & 2) ii = w - ii - 1;
  if(orientation & 1) jj = h - jj - 1;
  return jj*w + ii;
}

// only set mip4..0.
int dt_imageio_open_raw_preview(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  // printf("datum: %s\n", img->exif_datetime_taken);
  // printf("lens: %s\n", img->exif_lens);
  // printf("lensptr : %lu\n", (long int)(img->exif_lens));
  // printf("imgptr  : %lu\n", (long int)(img));
  // init libraw stuff
  // img = dt_image_cache_use(img->id, 'r');
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = img->shrink = 1; /* dcraw -h */
  raw->params.use_camera_wb = img->wb_cam;
  raw->params.use_auto_wb = img->wb_auto;
  // raw->params.med_passes; // median filter TODO
  // raw->params.no_auto_bright = 1;
  raw->params.output_bps = 16;
  raw->params.user_flip = -1;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
  raw->params.user_qual = 0; // linear
  // img->raw->params.output_color = 1;
  raw->params.use_camera_matrix = 1;
  // TODO: let this unclipped for develop, clip for preview.
  raw->params.highlight = 0; //0 clip, 1 unclip, 2 blend, 3+ rebuild
  ret = libraw_open_file(raw, filename);
  HANDLE_ERRORS(ret, 0);
  if(raw->idata.dng_version || (raw->sizes.width <= 1200 && raw->sizes.height <= 800))
  { // FIXME: this is a temporary bugfix avoiding segfaults for dng images. (and to avoid shrinking on small images).
    raw->params.user_qual = 0;
    raw->params.half_size = img->shrink = 0;
  }

  // get thumbnail
  ret = libraw_unpack_thumb(raw);
  if(!ret)
  {
    ret = 0;
    img->shrink = raw->params.half_size;
    img->orientation = raw->sizes.flip;
    img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
    img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
    // printf("size: %dx%d\n", img->width, img->height);
    img->exif_iso = raw->other.iso_speed;
    img->exif_exposure = raw->other.shutter;
    img->exif_aperture = raw->other.aperture;
    img->exif_focal_length = raw->other.focal_len;
    strncpy(img->exif_maker, raw->idata.make, 20);
    strncpy(img->exif_model, raw->idata.model, 20);
    dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);
    image = dcraw_make_mem_thumb(raw, &ret);
    int p_wd, p_ht;
    float f_wd, f_ht;
    dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
    dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);
    if(image && image->type == LIBRAW_IMAGE_JPEG)
    {
      // JPEG: decode with magick (directly rescaled to mip4)
#if 0//def HAVE_MAGICK
      ExceptionInfo *exception = AcquireExceptionInfo();
      ImageInfo *image_info = CloneImageInfo((ImageInfo *) NULL);
      Image *imimage = BlobToImage(image_info, image->data, image->data_size, exception);
      if(raw->sizes.flip & 4)
      {
        image->width  = imimage->rows;
        image->height = imimage->columns;
      }
      else
      {
        image->width  = imimage->columns;
        image->height = imimage->rows;
      }
      if (imimage == (Image *) NULL || dt_image_alloc(img, DT_IMAGE_MIP4)) goto error_raw_magick;

      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      assert(QuantumDepth == 16);
      const PixelPacket *p, *p1;
      const int p_ht2 = raw->sizes.flip & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = raw->sizes.flip & 4 ? p_ht : p_wd;

      if(image->width == p_wd && image->height == p_ht)
      { // use 1:1
        for (int j=0; j < imimage->rows; j++)
        {
          p = AcquireImagePixels(imimage,0,j,imimage->columns,1,exception);
          if (p == (const PixelPacket *) NULL) goto error_raw_magick_mip4;
          for (int i=0; i < imimage->columns; i++)
          {
            uint8_t cam[3] = {(int)p->red>>8, (int)p->green>>8, (int)p->blue>>8};
            for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, raw->sizes.flip)+2-k] = cam[k];
            p++;
          }
        }
      }
      else
      { // scale to fit
        bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        p1 = AcquireImagePixels(imimage,0,0,imimage->columns,imimage->rows,exception);
        if (p1 == (const PixelPacket *) NULL) goto error_raw_magick_mip4;
        for(int j=0;j<p_ht2 && scale*j<imimage->rows;j++) for(int i=0;i<p_wd2 && scale*i < imimage->columns;i++)
        {
          p = p1 + ((int)(scale*j)*imimage->columns + (int)(scale*i));
          uint8_t cam[3] = {(int)p->red>>8, (int)p->green>>8, (int)p->blue>>8};
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, raw->sizes.flip)+2-k] = cam[k];
        }
      }
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      // store in db.
      dt_imageio_preview_write(img, DT_IMAGE_MIP4);
      if(dt_image_update_mipmaps(img)) ret = 1;

      imimage = DestroyImage(imimage);
      image_info = DestroyImageInfo(image_info);
      exception = DestroyExceptionInfo(exception);
      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      return ret;
error_raw_magick_mip4:// clean up libraw and magick only
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
error_raw_magick:// clean up libraw and magick only
      CatchException(exception);
      image_info = DestroyImageInfo(image_info);
      exception = DestroyExceptionInfo(exception);
      goto error_raw;
#else
      dt_imageio_jpeg_t jpg;
      if(dt_imageio_jpeg_decompress_header(image->data, image->data_size, &jpg)) goto error_raw;
      if(img->orientation & 4)
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
      if(dt_imageio_jpeg_decompress(&jpg, tmp) || dt_image_alloc(img, DT_IMAGE_MIP4))
      {
        free(tmp);
        goto error_raw;
      }
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int p_ht2 = img->orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = img->orientation & 4 ? p_ht : p_wd;

      if(image->width == p_wd && image->height == p_ht)
      { // use 1:1
        for (int j=0; j < jpg.height; j++)
          for (int i=0; i < jpg.width; i++)
            for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
      }
      else
      { // scale to fit
        bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0;j<p_ht2 && scale*j<jpg.height;j++) for(int i=0;i<p_wd2 && scale*i < jpg.width;i++)
        {
          uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = cam[k];
        }
      }
      free(tmp);
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      // store in db.
      dt_imageio_preview_write(img, DT_IMAGE_MIP4);
      if(dt_image_update_mipmaps(img)) ret = 1;

      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      return ret;
#endif
    }
    else
    {
      // BMP: directly to mip4
      if (dt_image_alloc(img, DT_IMAGE_MIP4)) goto error_raw;
      dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
      const int p_ht2 = raw->sizes.flip & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
      const int p_wd2 = raw->sizes.flip & 4 ? p_ht : p_wd;
      if(image->width == p_wd2 && image->height == p_ht2)
      { // use 1:1
        for(int j=0;j<image->height;j++) for(int i=0;i<image->width;i++)
        {
          uint8_t *cam = image->data + 3*(j*image->width + i);
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, raw->sizes.flip) + 2-k] = cam[k];
        }
      }
      else
      { // scale to fit
        bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
        const float scale = fmaxf(image->width/f_wd, image->height/f_ht);
        for(int j=0;j<p_ht2 && scale*j<image->height;j++) for(int i=0;i<p_wd2 && scale*i < image->width;i++)
        {
          uint8_t *cam = image->data + 3*((int)(scale*j)*image->width + (int)(scale*i));
          for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, raw->sizes.flip) + 2-k] = cam[k];
        }
      }
      // store in db.
      dt_image_release(img, DT_IMAGE_MIP4, 'w');
      dt_imageio_preview_write(img, DT_IMAGE_MIP4);
      if(dt_image_update_mipmaps(img)) ret = 1;
      // clean up raw stuff.
      libraw_recycle(raw);
      libraw_close(raw);
      free(image);
      dt_image_release(img, DT_IMAGE_MIP4, 'r');
      // dt_image_cache_release(img, 'r');
      return ret;
    }
  }

  // if no thumbnail: load shrinked raw to tmp buffer (use dt_imageio_load_raw)
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  ret = dt_imageio_open_raw(img, filename);
  ret +=  dt_image_raw_to_preview(img);       // this updates mipf/mip4..0 from raw pixels.
  dt_image_release(img, DT_IMAGE_FULL, 'r');  // drop open_raw lock on full buffer.
  return ret;

error_raw:
  fprintf(stderr, "[imageio_open_raw_preview] could not get image from thumbnail!\n");
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  // dt_image_cache_release(img, 'r');
  return 1;
}

int dt_imageio_open_raw(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
  // img = dt_image_cache_use(img->id, 'r');
  int ret;
  libraw_data_t *raw = libraw_init(0);
  libraw_processed_image_t *image = NULL;
  raw->params.half_size = img->shrink; /* dcraw -h */
  raw->params.use_camera_wb = img->wb_cam;
  raw->params.use_auto_wb = img->wb_auto;
  // raw->params.no_auto_bright = 1;
  raw->params.output_bps = 16;
  raw->params.user_flip = -1;
  raw->params.gamm[0] = 1.0;
  raw->params.gamm[1] = 1.0;
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
  if(raw->idata.dng_version || (raw->sizes.width <= 1200 && raw->sizes.height <= 800))
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
  img->width  = (img->orientation & 4) ? raw->sizes.height : raw->sizes.width;
  img->height = (img->orientation & 4) ? raw->sizes.width  : raw->sizes.height;
  img->width <<= img->shrink;
  img->height <<= img->shrink;
  img->exif_iso = raw->other.iso_speed;
  img->exif_exposure = raw->other.shutter;
  img->exif_aperture = raw->other.aperture;
  img->exif_focal_length = raw->other.focal_len;
  strncpy(img->exif_maker, raw->idata.make, 20);
  strncpy(img->exif_model, raw->idata.model, 20);
  dt_gettime_t(img->exif_datetime_taken, raw->other.timestamp);

  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    libraw_recycle(raw);
    libraw_close(raw);
    free(image);
    // dt_image_cache_release(img, 'r');
    return 1;
  }
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*(img->width>>img->shrink)*(img->height>>img->shrink)*sizeof(float));
  const float m = 1./0xffff;
// #pragma omp parallel for schedule(static) shared(img, image)
  for(int k=0;k<3*(img->width>>img->shrink)*(img->height>>img->shrink);k++) img->pixels[k] = ((uint16_t *)(image->data))[k]*m;
  // clean up raw stuff.
  libraw_recycle(raw);
  libraw_close(raw);
  free(image);
  raw = NULL;
  image = NULL;
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  // dt_image_cache_release(img, 'r');
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

int dt_imageio_open_ldr_preview(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
#if 0//def HAVE_MAGICK
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
  value = GetImageProperty(image, "exif:Orientation");
  if (value != (const char *) NULL) img->orientation = atol(value);

  if(img->orientation & 4)
  {
    img->width  = image->rows;
    img->height = image->columns;
  }
  else
  {
    img->width  = image->columns;
    img->height = image->rows;
  }
  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);

  if(dt_image_alloc(img, DT_IMAGE_MIP4))
  {
    image = DestroyImage(image);
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    return 2;
  }

  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  assert(QuantumDepth == 16);
  const PixelPacket *p, *p1;
  const int p_ht2 = img->orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
  const int p_wd2 = img->orientation & 4 ? p_ht : p_wd;

  if(img->width == p_wd && img->height == p_ht)
  { // use 1:1
    for (int j=0; j < image->rows; j++)
    {
      p = AcquireImagePixels(image,0,j,image->columns,1,exception);
      if (p == (const PixelPacket *) NULL) goto error_magick_mip4;
      for (int i=0; i < image->columns; i++)
      {
        uint8_t cam[3] = {(int)p->red>>8, (int)p->green>>8, (int)p->blue>>8};
        for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = cam[k];
        p++;
      }
    }
  }
  else
  { // scale to fit
    bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
    const float scale = fmaxf(img->width/f_wd, img->height/f_ht);
    p1 = AcquireImagePixels(image,0,0,image->columns,image->rows,exception);
    if (p1 == (const PixelPacket *) NULL) goto error_magick_mip4;
    for(int j=0;j<p_ht2 && scale*j<image->rows;j++) for(int i=0;i<p_wd2 && scale*i < image->columns;i++)
    {
      p = p1 + ((int)(scale*j)*image->columns + (int)(scale*i));
      uint8_t cam[3] = {(int)p->red>>8, (int)p->green>>8, (int)p->blue>>8};
      for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = cam[k];
    }
  }
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  // store in db.
  int ret = 0;
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  if(dt_image_update_mipmaps(img)) ret = 1;
  
  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);
  return ret;
error_magick_mip4:
  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  return 1;
#else
  img->shrink = 0;

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return 1;
  if(img->orientation & 4)
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
  if(dt_imageio_jpeg_read(&jpg, tmp) || dt_image_alloc(img, DT_IMAGE_MIP4))
  {
    free(tmp);
    return 1;
  }

  int p_wd, p_ht;
  float f_wd, f_ht;
  dt_image_get_mip_size(img, DT_IMAGE_MIP4, &p_wd, &p_ht);
  dt_image_get_exact_mip_size(img, DT_IMAGE_MIP4, &f_wd, &f_ht);

  // printf("mip sizes: %d %d -- %f %f\n", p_wd, p_ht, f_wd, f_ht);
  // FIXME: there is a black border on the left side of a portrait image!

  dt_image_check_buffer(img, DT_IMAGE_MIP4, 4*p_wd*p_ht*sizeof(uint8_t));
  const int p_ht2 = img->orientation & 4 ? p_wd : p_ht; // pretend unrotated preview, rotate in write_pos
  const int p_wd2 = img->orientation & 4 ? p_ht : p_wd;

  if(img->width == p_wd && img->height == p_ht)
  { // use 1:1
    for (int j=0; j < jpg.height; j++)
      for (int i=0; i < jpg.width; i++)
        for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = tmp[4*jpg.width*j+4*i+k];
  }
  else
  { // scale to fit
    bzero(img->mip[DT_IMAGE_MIP4], 4*p_wd*p_ht*sizeof(uint8_t));
    const float scale = fmaxf(img->width/f_wd, img->height/f_ht);
    for(int j=0;j<p_ht2 && scale*j<jpg.height;j++) for(int i=0;i<p_wd2 && scale*i < jpg.width;i++)
    {
      uint8_t *cam = tmp + 4*((int)(scale*j)*jpg.width + (int)(scale*i));
      for(int k=0;k<3;k++) img->mip[DT_IMAGE_MIP4][4*dt_imageio_write_pos(i, j, p_wd2, p_ht2, img->orientation)+2-k] = cam[k];
    }
  }
  free(tmp);
  dt_image_release(img, DT_IMAGE_MIP4, 'w');
  // store in db.
  int ret = 0;
  dt_imageio_preview_write(img, DT_IMAGE_MIP4);
  if(dt_image_update_mipmaps(img)) ret = 1;
  dt_image_release(img, DT_IMAGE_MIP4, 'r');
  
  return ret;
#endif
}

// transparent read method to load ldr image to dt_raw_image_t with exif and so on.
int dt_imageio_open_ldr(dt_image_t *img, const char *filename)
{
  (void) dt_exif_read(img, filename);
#if 0//def HAVE_MAGICK
  // TODO: shrink!!
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
  value = GetImageProperty(image, "exif:Orientation");
  if (value != (const char *) NULL) img->orientation = atol(value);

  if(img->orientation & 4)
  {
    img->width  = image->rows;
    img->height = image->columns;
  }
  else
  {
    img->width  = image->columns;
    img->height = image->rows;
  }

  if(dt_image_alloc(img, DT_IMAGE_FULL))
  {
    image = DestroyImage(image);
    image_info = DestroyImageInfo(image_info);
    exception = DestroyExceptionInfo(exception);
    return 2;
  }
  // img->pixels = (float *)malloc(sizeof(float)*3*img->width*img->height);

  const int ht2 = img->orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = img->orientation & 4 ? img->height : img->width;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));
  const PixelPacket *p;
  assert(QuantumDepth == 16);
  for (int j=0; j < image->rows; j++)
  {
    p = AcquireImagePixels(image,0,j,image->columns,1,exception);
    if (p == (const PixelPacket *) NULL)
    {
      image = DestroyImage(image);
      image_info = DestroyImageInfo(image_info);
      exception = DestroyExceptionInfo(exception);
      dt_image_release(img, DT_IMAGE_FULL, 'w');
      dt_image_release(img, DT_IMAGE_FULL, 'r');
      return 1;
    }
    for (int i=0; i < image->columns; i++)
    {
      float cam[3] = {dt_dev_de_gamma[(int)p->red>>8], dt_dev_de_gamma[(int)p->green>>8], dt_dev_de_gamma[(int)p->blue>>8]};
      for(int k=0;k<3;k++) img->pixels[3*dt_imageio_write_pos(i, j, wd2, ht2, img->orientation)+k] = cam[k];
      p++;
    }
  }

#if 0
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
#endif

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
  img->shrink = 0;

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_read_header(filename, &jpg)) return 1;
  if(img->orientation & 4)
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
  if(dt_imageio_jpeg_read(&jpg, tmp) || dt_image_alloc(img, DT_IMAGE_FULL))
  {
    free(tmp);
    return 1;
  }
 
  const int ht2 = img->orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = img->orientation & 4 ? img->height : img->width;
  dt_image_check_buffer(img, DT_IMAGE_FULL, 3*img->width*img->height*sizeof(uint8_t));

  for(int j=0; j < jpg.height; j++)
    for(int i=0; i < jpg.width; i++)
      for(int k=0;k<3;k++) img->pixels[3*dt_imageio_write_pos(i, j, wd2, ht2, img->orientation)+k] = dt_dev_de_gamma[tmp[4*jpg.width*j+4*i+k]];

  free(tmp);
  dt_image_release(img, DT_IMAGE_FULL, 'w');
  return 0;
#endif
}

// batch-processing enabled write method: history stack, raw image, custom copy of gamma/tonecurves
int dt_imageio_export_f(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_process_16(&pipe, &dev, 0, 0, wd, ht, 1.0);
  uint16_t *buf = (uint16_t *)pipe.backbuf;

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "PF\n%d %d\n-1.0\n", wd, ht);
    for(int k=0;k<wd*ht;k++)
    {
      float tmp[3];
      for(int i=0;i<3;i++) tmp[i] = buf[3*k+i]*(1.0/0x10000);
      int cnt = fwrite(tmp, sizeof(float)*3, 1, f);
      if(cnt != sizeof(float)*3) break;
    }
    fclose(f);
    status = 0;
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return status;
}

int dt_imageio_export_16(dt_image_t *img, const char *filename)
{
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_process_16(&pipe, &dev, 0, 0, wd, ht, 1.0);
  uint16_t *buf16 = (uint16_t *)pipe.backbuf;

  int status = 1;
  FILE *f = fopen(filename, "wb");
  if(f)
  {
    (void)fprintf(f, "P6\n%d %d\n65535\n", wd, ht);
    for(int k=0;k<wd*ht;k++)
    {
      uint16_t tmp[3];
      for(int i=0;i<3;i++) tmp[i] = (0xff00 & (buf16[3*k+i]<<8))|(buf16[3*k+i]>>8);
      int cnt = fwrite(tmp, sizeof(uint16_t)*3, 1, f);
      if(cnt != sizeof(float)*3) break;
    }
    fclose(f);
    status = 0;
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
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

int dt_imageio_export_8(dt_image_t *img, const char *filename)
{
#ifdef HAVE_MAGICK
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, wd, ht, 1.0);
  uint8_t *buf8 = pipe.backbuf;
  for(int k=0;k<wd*ht;k++) for(int i=0;i<3;i++) buf8[3*k+i] = buf8[4*k+2-i];

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

  // set image properties!
  ExifRational rat;
  int length;
  uint8_t *exif_profile;
  ExifData *exif_data = exif_data_new();
  exif_data_set_byte_order(exif_data, EXIF_BYTE_ORDER_INTEL);
  ExifContent *exif_content = exif_data->ifd[0];
  ExifEntry *entry;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MAKE);
  entry->components = strlen (img->exif_maker) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_maker, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_MODEL);
  entry->components = strlen (img->exif_model) + 1;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)entry->data, img->exif_model, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_DATE_TIME_ORIGINAL);
  entry->components = 20;
  entry->format = EXIF_FORMAT_ASCII;
  entry->size = exif_format_get_size (entry->format) * entry->components;
  entry->data = realloc(entry->data, entry->size);
  strncpy((char *)(entry->data), img->exif_datetime_taken, entry->components);

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_ISO_SPEED_RATINGS);
  exif_set_short(entry->data, EXIF_BYTE_ORDER_INTEL, (int16_t)(img->exif_iso));
  entry->size = 2;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FNUMBER);
  dt_imageio_to_fractional(img->exif_aperture, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_EXPOSURE_TIME);
  dt_imageio_to_fractional(img->exif_exposure, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  entry = exif_entry_new();
  exif_content_add_entry(exif_content, entry);
  exif_entry_initialize(entry, EXIF_TAG_FOCAL_LENGTH);
  dt_imageio_to_fractional(img->exif_focal_length, &rat.numerator, &rat.denominator);
  exif_set_rational(entry->data, EXIF_BYTE_ORDER_INTEL, rat);
  entry->size = 8;
  entry->components = 1;

  exif_data_save_data(exif_data, &exif_profile, (uint32_t *)&length);

  exif_data_free(exif_data);
  StringInfo *profile = AcquireStringInfo(length);
  SetStringInfoDatum(profile, exif_profile);
  (void)SetImageProfile(image, "exif", profile);
  profile = DestroyStringInfo(profile);
  free(exif_profile);

  image_info->quality = 97;
  (void) strcpy(image->filename, filename);
  WriteImage(image_info, image);

  image = DestroyImage(image);
  image_info = DestroyImageInfo(image_info);
  exception = DestroyExceptionInfo(exception);

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return 0;
#else
  dt_develop_t dev;
  dt_dev_init(&dev, 0);
  dt_dev_load_image(&dev, img);
  const int wd = dev.image->width;
  const int ht = dev.image->height;
  dt_image_check_buffer(dev.image, DT_IMAGE_FULL, 3*wd*ht*sizeof(float));

  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_init_export(&pipe, wd, ht);
  dt_dev_pixelpipe_set_input(&pipe, &dev, dev.image->pixels, dev.image->width, dev.image->height, 1.0);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);
  dt_dev_pixelpipe_process(&pipe, &dev, 0, 0, wd, ht, 1.0);
  char pathname[1024];
  dt_image_full_path(img, pathname, 1024);
  const char *suffix = pathname + strlen(pathname) - 3;
  int export_png = 0;
  if(suffix > pathname && strncmp(suffix, "png", 3) == 0) export_png = 1;
  uint8_t *buf8 = pipe.backbuf;
  for(int k=0;k<wd*ht;k++)
  {
    uint8_t tmp = buf8[4*k+0];
    buf8[4*k+0] = buf8[4*k+2];
    buf8[4*k+2] = tmp;
  }

  int length;
  uint8_t exif_profile[65535]; // C++ alloc'ed buffer is uncool, so we waste some bits here.
  length = dt_exif_read_blob(exif_profile, pathname);

  int quality = 100;
  DT_CTL_GET_GLOBAL(quality, dev_export_quality);
  if((!export_png && dt_imageio_jpeg_write(filename, buf8, wd, ht, quality, exif_profile, length)) ||
     ( export_png && dt_imageio_png_write (filename, buf8, wd, ht)))
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_dev_cleanup(&dev);
    return 1;
  }
  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);
  return 0;
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

int dt_imageio_open_preview(dt_image_t *img, const char *filename)
{ // first try raw loading
  if(!dt_imageio_open_raw_preview(img, filename)) return 0;
  if(!dt_imageio_open_ldr_preview(img, filename)) return 0;
  return 1;
}
