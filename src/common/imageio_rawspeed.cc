/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#ifdef _OPENMP
#include <omp.h>
#endif

#include "RawSpeed-API.h"

#define TYPE_FLOAT32 RawImageType::F32
#define TYPE_USHORT16 RawImageType::UINT16

#include <memory>

#define __STDC_LIMIT_MACROS

extern "C" {
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/imageio_rawspeed.h"
#include "imageio.h"
#include "common/tags.h"
#include "develop/imageop.h"
#include <stdint.h>
}

// define this function, it is only declared in rawspeed:
int rawspeed_get_number_of_processor_cores()
{
#ifdef _OPENMP
  return omp_get_num_procs();
#else
  return 1;
#endif
}

using namespace rawspeed;

static dt_imageio_retval_t dt_imageio_open_rawspeed_sraw (dt_image_t *img, RawImage r, dt_mipmap_buffer_t *buf);
static CameraMetaData *meta = NULL;

static void dt_rawspeed_load_meta()
{
  /* Load rawspeed cameras.xml meta file once */
  if(meta == NULL)
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    if(meta == NULL)
    {
      char datadir[PATH_MAX] = { 0 }, camfile[PATH_MAX] = { 0 };
      dt_loc_get_datadir(datadir, sizeof(datadir));
      snprintf(camfile, sizeof(camfile), "%s/rawspeed/cameras.xml", datadir);
      // never cleaned up (only when dt closes)
      meta = new CameraMetaData(camfile);
    }
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
}

gboolean dt_rawspeed_lookup_makermodel(const char *maker, const char *model,
                                   char *mk, int mk_len, char *md, int md_len,
                                   char *al, int al_len)
{
  gboolean got_it_done = FALSE;
  try {
    dt_rawspeed_load_meta();
    const Camera *cam = meta->getCamera(maker, model, "");
    // Also look for dng cameras
    if(!cam)
      cam = meta->getCamera(maker, model, "dng");
    if(cam)
    {
      g_strlcpy(mk, cam->canonical_make.c_str(), mk_len);
      g_strlcpy(md, cam->canonical_model.c_str(), md_len);
      g_strlcpy(al, cam->canonical_alias.c_str(), al_len);
      got_it_done = TRUE;
    }
  }
  catch(const std::exception &exc)
  {
    fprintf(stderr, "[rawspeed] %s\n", exc.what());
  }

  if(!got_it_done)
  {
    // We couldn't find the camera or caught some exception, just punt and pass
    // through the same values
    g_strlcpy(mk, maker, mk_len);
    g_strlcpy(md, model, md_len);
    g_strlcpy(al, model, al_len);
  }
  return got_it_done;
}

uint32_t dt_rawspeed_crop_dcraw_filters(uint32_t filters, uint32_t crop_x, uint32_t crop_y)
{
  if(!filters || filters == 9u) return filters;

  return ColorFilterArray::shiftDcrawFilter(filters, crop_x, crop_y);
}

// CR3 files are for now handled by LibRaw, we do not want rawspeed to try to open them
// as this issues a lot of error messages on the console.
static gboolean _ignore_image(const gchar *filename)
{
  const char *extensions_whitelist[] = { "cr3", NULL };
  char *ext = g_strrstr(filename, ".");
  if(!ext) return FALSE;
  ext++;
  for(const char **i = extensions_whitelist; *i != NULL; i++)
    if(!g_ascii_strncasecmp(ext, *i, strlen(*i)))
    {
      return TRUE;
    }
  return FALSE;
}

dt_imageio_retval_t dt_imageio_open_rawspeed(dt_image_t *img, const char *filename,
                                             dt_mipmap_buffer_t *mbuf)
{
  if(_ignore_image(filename)) return DT_IMAGEIO_FILE_CORRUPTED;

  if(!img->exif_inited) (void)dt_exif_read(img, filename);

  char filen[PATH_MAX] = { 0 };
  snprintf(filen, sizeof(filen), "%s", filename);
  FileReader f(filen);

  std::unique_ptr<RawDecoder> d;
  std::unique_ptr<const Buffer> m;

  try
  {
    dt_rawspeed_load_meta();

    dt_pthread_mutex_lock(&darktable.readFile_mutex);
    m = f.readFile();
    dt_pthread_mutex_unlock(&darktable.readFile_mutex);

    RawParser t(*m.get());
    d = t.getDecoder(meta);

    if(!d.get()) return DT_IMAGEIO_FILE_CORRUPTED;

    d->failOnUnknown = true;
    d->checkSupport(meta);
    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;

    const auto errors = r->getErrors();
    for(const auto &error : errors)
      fprintf(stderr, "[rawspeed] (%s) %s\n", img->filename, error.c_str());

    g_strlcpy(img->camera_maker, r->metadata.canonical_make.c_str(), sizeof(img->camera_maker));
    g_strlcpy(img->camera_model, r->metadata.canonical_model.c_str(), sizeof(img->camera_model));
    g_strlcpy(img->camera_alias, r->metadata.canonical_alias.c_str(), sizeof(img->camera_alias));
    dt_image_refresh_makermodel(img);

    // We used to partial match the Canon local rebrandings so lets pass on
    // the value just in those cases to be able to fix old history stacks
    static const struct {
      const char *mungedname;
      const char *origname;
    } legacy_aliases[] = {
      {"Canon EOS","Canon EOS REBEL SL1"},
      {"Canon EOS","Canon EOS Kiss X7"},
      {"Canon EOS","Canon EOS DIGITAL REBEL XT"},
      {"Canon EOS","Canon EOS Kiss Digital N"},
      {"Canon EOS","Canon EOS 350D"},
      {"Canon EOS","Canon EOS DIGITAL REBEL XSi"},
      {"Canon EOS","Canon EOS Kiss Digital X2"},
      {"Canon EOS","Canon EOS Kiss X2"},
      {"Canon EOS","Canon EOS REBEL T5i"},
      {"Canon EOS","Canon EOS Kiss X7i"},
      {"Canon EOS","Canon EOS Rebel T6i"},
      {"Canon EOS","Canon EOS Kiss X8i"},
      {"Canon EOS","Canon EOS Rebel T6s"},
      {"Canon EOS","Canon EOS 8000D"},
      {"Canon EOS","Canon EOS REBEL T1i"},
      {"Canon EOS","Canon EOS Kiss X3"},
      {"Canon EOS","Canon EOS REBEL T2i"},
      {"Canon EOS","Canon EOS Kiss X4"},
      {"Canon EOS REBEL T3","Canon EOS REBEL T3i"},
      {"Canon EOS","Canon EOS Kiss X5"},
      {"Canon EOS","Canon EOS REBEL T4i"},
      {"Canon EOS","Canon EOS Kiss X6i"},
      {"Canon EOS","Canon EOS DIGITAL REBEL XS"},
      {"Canon EOS","Canon EOS Kiss Digital F"},
      {"Canon EOS","Canon EOS REBEL T5"},
      {"Canon EOS","Canon EOS Kiss X70"},
      {"Canon EOS","Canon EOS DIGITAL REBEL XTi"},
      {"Canon EOS","Canon EOS Kiss Digital X"},
    };

    for(uint32_t i = 0; i < (sizeof(legacy_aliases) / sizeof(legacy_aliases[1])); i++)
      if(!strcmp(legacy_aliases[i].origname, r->metadata.model.c_str()))
      {
        g_strlcpy(img->camera_legacy_makermodel, legacy_aliases[i].mungedname, sizeof(img->camera_legacy_makermodel));
        break;
      }

    img->raw_black_level = r->blackLevel;
    img->raw_white_point = r->whitePoint;

    if(r->blackLevelSeparate[0] == -1
       || r->blackLevelSeparate[1] == -1
       || r->blackLevelSeparate[2] == -1
       || r->blackLevelSeparate[3] == -1)
    {
      r->calculateBlackAreas();
    }

    for(uint8_t i = 0; i < 4; i++)
      img->raw_black_level_separate[i] = r->blackLevelSeparate[i];

    if(r->blackLevel == -1)
    {
      float black = 0.0f;
      for(uint8_t i = 0; i < 4; i++)
      {
        black += img->raw_black_level_separate[i];
      }
      black /= 4.0f;

      img->raw_black_level = CLAMP(black, 0, UINT16_MAX);
    }

    /*
     * FIXME
     * if(r->whitePoint == 65536)
     *   ???
     */

    /* free auto pointers on spot */
    d.reset();
    m.reset();

    // Grab the WB
    for(int i = 0; i < 4; i++)
      img->wb_coeffs[i] = r->metadata.wbCoeffs[i];

    // Grab the Adobe coeffs
    const int msize = r->metadata.colorMatrix.size();
    for(int k = 0; k < 4; k++)
      for(int i = 0; i < 3; i++)
      {
        const int idx = k*3 + i;
        if(idx < msize)
          img->adobe_XYZ_to_CAM[k][i] = float(r->metadata.colorMatrix[idx]);
        else
          img->adobe_XYZ_to_CAM[k][i] = 0.0f;
      }

    // Get additional exif tags that are not cached in the database
    dt_exif_img_check_additional_tags(img, filename);

    if(r->getDataType() == TYPE_FLOAT32)
    {
      img->flags |= DT_IMAGE_HDR;

      // we assume that image is normalized before.
      // FIXME: not true for hdrmerge DNG's.
      for(int k = 0; k < 4; k++) img->buf_dsc.processed_maximum[k] = 1.0f;
    }

    img->buf_dsc.filters = 0u;
    if(!r->isCFA)
    {
      const dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw(img, r, mbuf);
      return ret;
    }

    if((r->getDataType() != TYPE_USHORT16) && (r->getDataType() != TYPE_FLOAT32))
      return DT_IMAGEIO_FILE_CORRUPTED;

    if((r->getBpp() != sizeof(uint16_t)) && (r->getBpp() != sizeof(float)))
      return DT_IMAGEIO_FILE_CORRUPTED;

    if((r->getDataType() == TYPE_USHORT16) && (r->getBpp() != sizeof(uint16_t)))
      return DT_IMAGEIO_FILE_CORRUPTED;

    if((r->getDataType() == TYPE_FLOAT32) && (r->getBpp() != sizeof(float)))
      return DT_IMAGEIO_FILE_CORRUPTED;

    const float cpp = r->getCpp();
    if(cpp != 1) return DT_IMAGEIO_FILE_CORRUPTED;

    img->buf_dsc.channels = 1;

    switch(r->getBpp())
    {
      case sizeof(uint16_t):
        img->buf_dsc.datatype = TYPE_UINT16;
        break;
      case sizeof(float):
        img->buf_dsc.datatype = TYPE_FLOAT;
        break;
      default:
        return DT_IMAGEIO_FILE_CORRUPTED;
    }

    // dimensions of uncropped image
    iPoint2D dimUncropped = r->getUncroppedDim();
    img->width = dimUncropped.x;
    img->height = dimUncropped.y;

    // dimensions of cropped image
    iPoint2D dimCropped = r->dim;

    // crop - Top,Left corner
    iPoint2D cropTL = r->getCropOffset();
    img->crop_x = cropTL.x;
    img->crop_y = cropTL.y;

    // crop - Bottom,Right corner
    iPoint2D cropBR = dimUncropped - dimCropped - cropTL;
    img->crop_width = cropBR.x;
    img->crop_height = cropBR.y;

    img->fuji_rotation_pos = r->metadata.fujiRotationPos;
    img->pixel_aspect_ratio = (float)r->metadata.pixelAspectRatio;

    // as the X-Trans filters comments later on states, these are for
    // cropped image, so we need to uncrop them.
    img->buf_dsc.filters = dt_rawspeed_crop_dcraw_filters(r->cfa.getDcrawFilter(), cropTL.x, cropTL.y);

    if(FILTERS_ARE_4BAYER(img->buf_dsc.filters)) img->flags |= DT_IMAGE_4BAYER;

    if(img->buf_dsc.filters)
    {
      img->flags &= ~DT_IMAGE_LDR;
      img->flags |= DT_IMAGE_RAW;

      // special handling for x-trans sensors
      if(img->buf_dsc.filters == 9u)
      {
        // get 6x6 CFA offset from top left of cropped image
        // NOTE: This is different from how things are done with Bayer
        // sensors. For these, the CFA in cameras.xml is pre-offset
        // depending on the distance modulo 2 between raw and usable
        // image data. For X-Trans, the CFA in cameras.xml is
        // (currently) aligned with the top left of the raw data.
        for(int i = 0; i < 6; ++i)
          for(int j = 0; j < 6; ++j)
          {
            img->buf_dsc.xtrans[j][i] = (uint8_t)r->cfa.getColorAt(i % 6, j % 6);
          }
      }
    }
    // if buf is NULL, we quit the fct here
    if(!mbuf)
    {
      img->buf_dsc.cst = IOP_CS_RAW;
      img->loader = LOADER_RAWSPEED;
      return DT_IMAGEIO_OK;
    }

    void *buf = dt_mipmap_cache_alloc(mbuf, img);
    if(!buf) return DT_IMAGEIO_CACHE_FULL;

    /*
     * since we do not want to crop black borders at this stage,
     * and we do not want to rotate image, we can just use memcpy,
     * as it is faster than dt_imageio_flip_buffers, but only if
     * buffer sizes are equal,
     * (from Klaus: r->pitch may differ from DT pitch (line to line spacing))
     * else fallback to generic dt_imageio_flip_buffers()
     */
    const size_t bufSize_mipmap = (size_t)img->width * img->height * r->getBpp();
    const size_t bufSize_rawspeed = (size_t)r->pitch * dimUncropped.y;
    if(bufSize_mipmap == bufSize_rawspeed)
    {
      memcpy(buf, (char *)(&(r->getByteDataAsUncroppedArray2DRef()(0, 0))), bufSize_mipmap);
    }
    else
    {
      dt_imageio_flip_buffers((char *)buf, (char *)(&(r->getByteDataAsUncroppedArray2DRef()(0, 0))), r->getBpp(),
                              dimUncropped.x, dimUncropped.y, dimUncropped.x, dimUncropped.y, r->pitch,
                              ORIENTATION_NONE);
    }

    //  Check if the camera is missing samples
    const Camera *cam = meta->getCamera(r->metadata.make.c_str(),
                                        r->metadata.model.c_str(),
                                        r->metadata.mode.c_str());

    if(cam && cam->supportStatus == Camera::SupportStatus::NoSamples)
      img->camera_missing_sample = TRUE;
  }
  catch(const std::exception &exc)
  {
    fprintf(stderr, "[rawspeed] (%s) %s\n", img->filename, exc.what());

    /* if an exception is raised lets not retry or handle the
     specific ones, consider the file as corrupted */
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch(...)
  {
    fprintf(stderr, "[rawspeed] unhandled exception in imageio_rawspeed\n");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  img->buf_dsc.cst = IOP_CS_RAW;
  img->loader = LOADER_RAWSPEED;

  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_buffer_t *mbuf)
{
  // sraw aren't real raw, but not ldr either (need white balance and stuff)
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags |= DT_IMAGE_S_RAW;
  img->width = r->dim.x;
  img->height = r->dim.y;

  // actually we want to store full floats here:
  img->buf_dsc.channels = 4;
  img->buf_dsc.datatype = TYPE_FLOAT;

  if(r->getDataType() != TYPE_USHORT16 && r->getDataType() != TYPE_FLOAT32)
    return DT_IMAGEIO_FILE_CORRUPTED;

  const uint32_t cpp = r->getCpp();
  if(cpp != 1 && cpp != 3 && cpp != 4) return DT_IMAGEIO_FILE_CORRUPTED;

  // if buf is NULL, we quit the fct here
  if(!mbuf)
  {
    img->buf_dsc.cst = IOP_CS_RAW;
    img->loader = LOADER_RAWSPEED;
    return DT_IMAGEIO_OK;
  }

  if(cpp == 1) img->flags |= DT_IMAGE_MONOCHROME;

  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) return DT_IMAGEIO_CACHE_FULL;

  if(cpp == 1)
  {
    /*
     * monochrome image (e.g. Leica M9 monochrom),
     * we need to copy data from only channel to each of 3 channels
     */

    if(r->getDataType() == TYPE_USHORT16)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(cpp) shared(r, img, buf)
#endif
      for(int j = 0; j < img->height; j++)
      {
        const Array2DRef<uint16_t> in = r->getU16DataAsUncroppedArray2DRef();
        float *out = ((float *)buf) + (size_t)4 * j * img->width;

        for(int i = 0; i < img->width; i++, out += 4)
        {
          for(int k = 0; k < 3; k++)
          {
            out[k] = (float)in(j, cpp * i + k) / (float)UINT16_MAX;
          }
        }
      }
    }
    else // r->getDataType() == TYPE_FLOAT32
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(cpp) shared(r, img, buf)
#endif
      for(int j = 0; j < img->height; j++)
      {
        const Array2DRef<float> in = r->getF32DataAsUncroppedArray2DRef();
        float *out = ((float *)buf) + (size_t)4 * j * img->width;

        for(int i = 0; i < img->width; i++, out += 4)
        {
          for(int k = 0; k < 3; k++)
          {
            out[k] = in(j, cpp * i + k);
          }
        }
      }
    }
  }
  else // case cpp == 3 or 4
  {
    /*
     * standard 3-ch image
     * just copy 3 ch to 3 ch
     */

    if(r->getDataType() == TYPE_USHORT16)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(cpp) shared(r, img, buf)
#endif
      for(int j = 0; j < img->height; j++)
      {
        const Array2DRef<uint16_t> in = r->getU16DataAsUncroppedArray2DRef();
        float *out = ((float *)buf) + (size_t)4 * j * img->width;

        for(int i = 0; i < img->width; i++, out += 4)
        {
          for(int k = 0; k < 3; k++)
          {
            out[k] = (float)in(j, cpp * i + k) / (float)UINT16_MAX;
          }
        }
      }
    }
    else // r->getDataType() == TYPE_FLOAT32
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) dt_omp_firstprivate(cpp) shared(r, img, buf)
#endif
      for(int j = 0; j < img->height; j++)
      {
        const Array2DRef<float> in = r->getF32DataAsUncroppedArray2DRef();
        float *out = ((float *)buf) + (size_t)4 * j * img->width;

        for(int i = 0; i < img->width; i++, out += 4)
        {
          for(int k = 0; k < 3; k++)
          {
            out[k] = in(j, cpp * i + k);
          }
        }
      }
    }
  }

  img->buf_dsc.cst = IOP_CS_RAW;
  img->loader = LOADER_RAWSPEED;

  //  Check if the camera is missing samples
  const Camera *cam = meta->getCamera(r->metadata.make.c_str(),
                                      r->metadata.model.c_str(),
                                      r->metadata.mode.c_str());

  if(cam && cam->supportStatus == Camera::SupportStatus::NoSamples)
    img->camera_missing_sample = TRUE;

  return DT_IMAGEIO_OK;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
