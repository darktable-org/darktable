/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2016 Pedro Côrte-Real

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

#include <exception>
#include <memory>

#include "rawspeed/RawSpeed/RawSpeed-API.h"

#define __STDC_LIMIT_MACROS

extern "C" {
#include "config.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
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


int main(int argc, const char* argv[])
{

  if(argc != 2)
  {
    fprintf(stderr, "Usage: %s <file>\n", argv[0]);
    return 2;
  }

  FileReader f((char *) argv[1]);

  std::unique_ptr<RawDecoder> d;
  std::unique_ptr<FileMap> m;

  char *datadir = NULL;
#if defined(__MACH__) || defined(__APPLE__)
  /*
   * FIXME: following function is only available in libdarktable,
   * but we can not link against it - #10913.
   */
  datadir = find_install_dir("/share/darktable");
#endif
  char camfile[PATH_MAX] = { 0 };
  if (datadir)
    snprintf(camfile, sizeof(camfile), "%s/rawspeed/cameras.xml", datadir);
  else
    snprintf(camfile, sizeof(camfile), "%s/rawspeed/cameras.xml", DARKTABLE_DATADIR);

  try
  {
    CameraMetaData *meta = new CameraMetaData(camfile);

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

    if(!d.get())
    {
      fprintf(stderr, "ERROR: Couldn't get a RawDecoder instance\n");
      return 2;
    }

    d->failOnUnknown = true;
    d->checkSupport(meta);
    d->decodeRaw();
    d->decodeMetaData(meta);
    RawImage r = d->mRaw;

    for (uint32 i=0; i<r->errors.size(); i++)
      fprintf(stderr, "WARNING: [rawspeed] %s\n", r->errors[i]);

    fprintf(stdout, "make: %s\n", r->metadata.make.c_str());
    fprintf(stdout, "model: %s\n", r->metadata.model.c_str());

    fprintf(stdout, "canonical_make: %s\n", r->metadata.canonical_make.c_str());
    fprintf(stdout, "canonical_model: %s\n", r->metadata.canonical_model.c_str());
    fprintf(stdout, "canonical_alias: %s\n", r->metadata.canonical_alias.c_str());
    fprintf(stdout, "blackLevel: %d\n", r->blackLevel);
    fprintf(stdout, "whitePoint: %d\n", r->whitePoint);

    fprintf(stdout, "blackLevelSeparate: %d %d %d %d\n",
                    r->blackLevelSeparate[0], r->blackLevelSeparate[1],
                    r->blackLevelSeparate[2], r->blackLevelSeparate[3]);

    fprintf(stdout, "wbCoeffs: %f %f %f %f\n",
                    r->metadata.wbCoeffs[0], r->metadata.wbCoeffs[1],
                    r->metadata.wbCoeffs[2], r->metadata.wbCoeffs[3]);

    fprintf(stdout, "isCFA: %d\n", r->isCFA);
    uint32_t filters = r->cfa.getDcrawFilter();
    fprintf(stdout, "filters: %d (0x%x)\n", filters, filters);
    uint32_t bpp = r->getBpp();
    fprintf(stdout, "bpp: %d\n", bpp);
    uint32_t cpp = r->getCpp();
    fprintf(stdout, "cpp: %d\n", cpp);
    fprintf(stdout, "dataType: %d\n", r->getDataType());

    // dimensions of uncropped image
    iPoint2D dimUncropped = r->getUncroppedDim();
    fprintf(stdout, "dimUncropped: %dx%d\n", dimUncropped.x, dimUncropped.y);

    // dimensions of cropped image
    iPoint2D dimCropped = r->dim;
    fprintf(stdout, "dimCropped: %dx%d\n", dimCropped.x, dimCropped.y);

    // crop - Top,Left corner
    iPoint2D cropTL = r->getCropOffset();
    fprintf(stdout, "cropOffset: %dx%d\n", cropTL.x, cropTL.y);

    fprintf(stdout, "fuji_rotation_pos: %d\n", r->metadata.fujiRotationPos);
    fprintf(stdout, "pixel_aspect_ratio: %f\n", r->metadata.pixelAspectRatio);

    double sum = 0.0f;
    for(uint32_t row = 0; row < ((uint32_t) dimUncropped.y); row++)
    {
      uint8_t *data = r->getDataUncropped(0, 0);
      for(uint32_t byte = 0; byte < ((uint32_t) dimUncropped.x*cpp*bpp) ; byte++)
        sum += (double) data[byte];
    }
    fprintf(stdout, "Image byte sum: %lf\n", sum);
    fprintf(stdout, "Image byte avg: %lf\n", sum/(dimUncropped.y*dimUncropped.x*cpp*bpp));
  }
  catch(const std::exception &exc)
  {
    printf("ERROR: [rawspeed] %s\n", exc.what());

    /* if an exception is raised lets not retry or handle the
     specific ones, consider the file as corrupted */
    return 2;
  }
  catch(...)
  {
    printf("Unhandled exception in rawspeed-identify\n");
    return 3;
  }

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
