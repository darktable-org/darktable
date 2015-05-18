/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#include <memory>

#include "rawspeed/RawSpeed/RawSpeed-API.h"

#define __STDC_LIMIT_MACROS

extern "C" {
#include "imageio.h"
#include "common/imageio_rawspeed.h"
#include "common/exif.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/file_location.h"
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

using namespace RawSpeed;

static dt_imageio_retval_t dt_imageio_open_rawspeed_sraw (dt_image_t *img, RawImage r, dt_mipmap_buffer_t *buf);
static CameraMetaData *meta = NULL;

static void dt_rawspeed_load_meta() {
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

void dt_rawspeed_lookup_makermodel(const char *maker, const char *model,
                                   char *mk, int mk_len, char *md, int md_len,
                                   char *al, int al_len)
{
  int got_it_done = FALSE;
  try {
    dt_rawspeed_load_meta();
    Camera *cam = meta->getCamera(maker, model, "");
    // Also look for dng cameras
    if (!cam)
      cam = meta->getCamera(maker, model, "dng");
    if (cam)
    {
      g_strlcpy(mk, cam->canonical_make.c_str(), mk_len);
      g_strlcpy(md, cam->canonical_model.c_str(), md_len);
      g_strlcpy(al, cam->canonical_alias.c_str(), al_len);
      got_it_done = TRUE;
    }
  }
  catch(const std::exception &exc)
  {
    printf("[rawspeed] %s\n", exc.what());
  }

  if (!got_it_done)
  {
    // We couldn't find the camera or caught some exception, just punt and pass
    // through the same values
    g_strlcpy(mk, maker, mk_len);
    g_strlcpy(md, model, md_len);
    g_strlcpy(al, model, al_len);
  }
}

dt_imageio_retval_t dt_imageio_open_rawspeed(dt_image_t *img, const char *filename,
                                             dt_mipmap_buffer_t *mbuf)
{
  if(!img->exif_inited) (void)dt_exif_read(img, filename);

#ifdef __WIN32__
  const size_t len = strlen(filename) + 1;
  wchar_t filen[len];
  mbstowcs(filen, filename, len);
  FileReader f(filen);
#else
  char filen[PATH_MAX] = { 0 };
  snprintf(filen, sizeof(filen), "%s", filename);
  FileReader f(filen);
#endif

#ifdef __APPLE__
  std::auto_ptr<RawDecoder> d;
  std::auto_ptr<FileMap> m;
#else
  std::unique_ptr<RawDecoder> d;
  std::unique_ptr<FileMap> m;
#endif

  try
  {
    dt_rawspeed_load_meta();

#ifdef __APPLE__
    m = auto_ptr<FileMap>(f.readFile());
#else
    m = unique_ptr<FileMap>(f.readFile());
#endif

    RawParser t(m.get());
#ifdef __APPLE__
    d = auto_ptr<RawDecoder>(t.getDecoder(meta));
#else
    d = unique_ptr<RawDecoder>(t.getDecoder(meta));
#endif

    if(!d.get()) return DT_IMAGEIO_FILE_CORRUPTED;

    d->failOnUnknown = true;
    d->checkSupport(meta);
    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;

    for (uint32 i=0; i<r->errors.size(); i++)
      fprintf(stderr, "[rawspeed] %s\n", r->errors[i]);

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

    for (uint32 i=0; i<(sizeof(legacy_aliases)/sizeof(legacy_aliases[1])); i++)
      if (!strcmp(legacy_aliases[i].origname, r->metadata.model.c_str())) {
        g_strlcpy(img->camera_legacy_makermodel, legacy_aliases[i].mungedname, sizeof(img->camera_legacy_makermodel));
        break;
      }

    img->raw_black_level = r->blackLevel;
    img->raw_white_point = r->whitePoint;

    if(r->blackLevelSeparate[0] == -1 || r->blackLevelSeparate[1] == -1 || r->blackLevelSeparate[2] == -1
       || r->blackLevelSeparate[3] == -1)
    {
      r->calculateBlackAreas();
    }

    for(uint8_t i = 0; i < 4; i++) img->raw_black_level_separate[i] = r->blackLevelSeparate[i];

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
    for(int i = 0; i < 3; i++) img->wb_coeffs[i] = r->metadata.wbCoeffs[i];

    img->filters = 0u;
    if(!r->isCFA)
    {
      dt_imageio_retval_t ret = dt_imageio_open_rawspeed_sraw(img, r, mbuf);
      return ret;
    }

    img->bpp = r->getBpp();
    img->filters = r->cfa.getDcrawFilter();
    if(img->filters)
    {
      img->flags &= ~DT_IMAGE_LDR;
      img->flags |= DT_IMAGE_RAW;
      if(r->getDataType() == TYPE_FLOAT32) img->flags |= DT_IMAGE_HDR;
      // special handling for x-trans sensors
      if(img->filters == 9u)
      {
        // get 6x6 CFA offset from top left of cropped image
        // NOTE: This is different from how things are done with Bayer
        // sensors. For these, the CFA in cameras.xml is pre-offset
        // depending on the distance modulo 2 between raw and usable
        // image data. For X-Trans, the CFA in cameras.xml is
        // (currently) aligned with the top left of the raw data, and
        // hence it is shifted here to align with the top left of the
        // cropped image.
        iPoint2D tl_margin = r->getCropOffset();
        for(int i = 0; i < 6; ++i)
          for(int j = 0; j < 6; ++j)
          {
            img->xtrans_uncropped[j][i] = r->cfa.getColorAt(i % 6, j % 6);
            img->xtrans[j][i] = r->cfa.getColorAt((i + tl_margin.x) % 6, (j + tl_margin.y) % 6);
          }
      }
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
    const size_t bufSize_mipmap = (size_t)img->width * img->height * img->bpp;
    const size_t bufSize_rawspeed = (size_t)r->pitch * dimUncropped.y;
    if(bufSize_mipmap == bufSize_rawspeed)
    {
      memcpy(buf, r->getDataUncropped(0, 0), bufSize_mipmap);
    }
    else
    {
      dt_imageio_flip_buffers((char *)buf, (char *)r->getDataUncropped(0, 0), r->getBpp(), dimUncropped.x,
                              dimUncropped.y, dimUncropped.x, dimUncropped.y, r->pitch, ORIENTATION_NONE);
    }
  }
  catch(const std::exception &exc)
  {
    printf("[rawspeed] %s\n", exc.what());

    /* if an exception is raised lets not retry or handle the
     specific ones, consider the file as corrupted */
    return DT_IMAGEIO_FILE_CORRUPTED;
  }
  catch(...)
  {
    printf("Unhandled exception in imageio_rawspeed\n");
    return DT_IMAGEIO_FILE_CORRUPTED;
  }

  return DT_IMAGEIO_OK;
}

dt_imageio_retval_t dt_imageio_open_rawspeed_sraw(dt_image_t *img, RawImage r, dt_mipmap_buffer_t *mbuf)
{
  // sraw aren't real raw, but not ldr either (need white balance and stuff)
  img->flags &= ~DT_IMAGE_LDR;
  img->flags &= ~DT_IMAGE_RAW;

  img->width = r->dim.x;
  img->height = r->dim.y;

  iPoint2D dimUncropped = r->getUncroppedDim();
  iPoint2D cropTL = r->getCropOffset();

  // actually we want to store full floats here:
  img->bpp = 4 * sizeof(float);
  img->cpp = r->getCpp();

  if(r->getDataType() != TYPE_USHORT16) return DT_IMAGEIO_FILE_CORRUPTED;

  if(img->cpp != 1 && img->cpp != 3) return DT_IMAGEIO_FILE_CORRUPTED;

  void *buf = dt_mipmap_cache_alloc(mbuf, img);
  if(!buf) return DT_IMAGEIO_CACHE_FULL;

  uint16_t *raw_img = (uint16_t *)r->getDataUncropped(0, 0);

  if(img->cpp == 1)
  {
/*
 * monochrome image (e.g. Leica M9 monochrom),
 * we need to copy data from only channel to each of 3 channels
 */

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(raw_img, img, dimUncropped, cropTL, buf)
#endif
    for(int j = 0; j < img->height; j++)
    {
      const uint16_t *in = ((uint16_t *)raw_img)
                           + (size_t)(img->cpp * (dimUncropped.x * (j + cropTL.y) + cropTL.x));
      float *out = ((float *)buf) + (size_t)4 * j * img->width;

      for(int i = 0; i < img->width; i++, in += img->cpp, out += 4)
      {
        for(int k = 0; k < 3; k++)
        {
          out[k] = (float)*in / (float)UINT16_MAX;
        }
      }
    }
  }
  else if(img->cpp == 3)
  {
/*
 * standard 3-ch image
 * just copy 3 ch to 3 ch
 */

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(raw_img, img, dimUncropped, cropTL, buf)
#endif
    for(int j = 0; j < img->height; j++)
    {
      const uint16_t *in = ((uint16_t *)raw_img)
                           + (size_t)(img->cpp * (dimUncropped.x * (j + cropTL.y) + cropTL.x));
      float *out = ((float *)buf) + (size_t)4 * j * img->width;

      for(int i = 0; i < img->width; i++, in += img->cpp, out += 4)
      {
        for(int k = 0; k < 3; k++)
        {
          out[k] = (float)in[k] / (float)UINT16_MAX;
        }
      }
    }
  }

  return DT_IMAGEIO_OK;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
