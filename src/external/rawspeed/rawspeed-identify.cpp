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

#include "rawspeed/RawSpeed/RawSpeed-API.h"  // IWYU pragma: keep
#include <cstddef>                           // for size_t
#include <cstdio>                            // for fprintf, stdout, stderr
#include <exception>                         // for exception
#include <memory>                            // for unique_ptr, allocator
#include <stdint.h>                          // for uint16_t
#include <string>                            // for string, operator+, basic...
#include <sys/stat.h>                        // for stat
#include <vector>                            // for vector

#ifdef _OPENMP
#include <omp.h>
#endif

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

std::string find_cameras_xml(const char *argv0)
{
  struct stat statbuf;

#ifdef RS_CAMERAS_XML_PATH
  static const char set_camfile[] = RS_CAMERAS_XML_PATH;
  if (stat(set_camfile, &statbuf))
  {
    fprintf(stderr, "WARNING: Couldn't find cameras.xml in '%s'\n", set_camfile);
  }
  else
  {
    return set_camfile;
  }
#endif

  std::string self(argv0);

  // If we haven't been provided with a valid cameras.xml path on compile try relative to argv[0]
  std::size_t lastslash = self.find_last_of("/\\");
  std::string bindir(self.substr(0, lastslash));

  std::string found_camfile(bindir + "/../share/darktable/rawspeed/cameras.xml");

  if (stat(found_camfile.c_str(), &statbuf))
  {
    fprintf(stderr, "ERROR: Couldn't find cameras.xml in '%s'\n", found_camfile.c_str());
    return NULL;
  }

  return found_camfile;
}

int main(int argc, const char* argv[])
{

  if(argc != 2)
  {
    fprintf(stderr, "Usage: darktable-rs-identify <file>\n");
    return 2;
  }

  const std::string camfile = find_cameras_xml(argv[0]);
  if (camfile.empty())
  {
    //fprintf(stderr, "ERROR: Couldn't find cameras.xml\n");
    return 2;
  }
  //fprintf(stderr, "Using cameras.xml from '%s'\n", camfile);

  try
  {
    std::unique_ptr<CameraMetaData> meta(new CameraMetaData(camfile.c_str()));

    if(!meta.get())
    {
      fprintf(stderr, "ERROR: Couldn't get a CameraMetaData instance\n");
      return 2;
    }

#ifdef __AFL_HAVE_MANUAL_CONTROL
    __AFL_INIT();
#endif

    fprintf(stderr, "Loading file: \"%s\"\n", argv[1]);

    FileReader f((char *) argv[1]);

    std::unique_ptr<FileMap> m(f.readFile());

    RawParser t(m.get());

    std::unique_ptr<RawDecoder> d(t.getDecoder(meta.get()));

    if(!d.get())
    {
      fprintf(stderr, "ERROR: Couldn't get a RawDecoder instance\n");
      return 2;
    }

    d->applyCrop = false;
    d->failOnUnknown = true;
    RawImage r = d->mRaw;

    d->decodeMetaData(meta.get());

    fprintf(stdout, "make: %s\n", r->metadata.make.c_str());
    fprintf(stdout, "model: %s\n", r->metadata.model.c_str());

    fprintf(stdout, "canonical_make: %s\n", r->metadata.canonical_make.c_str());
    fprintf(stdout, "canonical_model: %s\n", r->metadata.canonical_model.c_str());
    fprintf(stdout, "canonical_alias: %s\n", r->metadata.canonical_alias.c_str());

    d->checkSupport(meta.get());
    d->decodeRaw();
    d->decodeMetaData(meta.get());
    r = d->mRaw;
    for (uint32 i=0; i<r->errors.size(); i++)
      fprintf(stderr, "WARNING: [rawspeed] %s\n", r->errors[i]);

    fprintf(stdout, "blackLevel: %d\n", r->blackLevel);
    fprintf(stdout, "whitePoint: %d\n", r->whitePoint);

    fprintf(stdout, "blackLevelSeparate: %d %d %d %d\n",
                    r->blackLevelSeparate[0], r->blackLevelSeparate[1],
                    r->blackLevelSeparate[2], r->blackLevelSeparate[3]);

    fprintf(stdout, "wbCoeffs: %f %f %f %f\n",
                    r->metadata.wbCoeffs[0], r->metadata.wbCoeffs[1],
                    r->metadata.wbCoeffs[2], r->metadata.wbCoeffs[3]);

    fprintf(stdout, "isCFA: %d\n", r->isCFA);
    uint32 filters = r->cfa.getDcrawFilter();
    fprintf(stdout, "filters: %d (0x%x)\n", filters, filters);
    const uint32 bpp = r->getBpp();
    fprintf(stdout, "bpp: %d\n", bpp);
    uint32 cpp = r->getCpp();
    fprintf(stdout, "cpp: %d\n", cpp);
    fprintf(stdout, "dataType: %d\n", r->getDataType());

    // dimensions of uncropped image
    const iPoint2D dimUncropped = r->getUncroppedDim();
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
    {
      uchar8 *const data = r->getDataUncropped(0, 0);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) reduction(+:sum)
#endif
      for(size_t k = 0; k < ((size_t) dimUncropped.y*dimUncropped.x*bpp); k++)
      {
        sum += (double) data[k];
      }

    }
    fprintf(stdout, "Image byte sum: %lf\n", sum);
    fprintf(stdout, "Image byte avg: %lf\n", sum/(double)(dimUncropped.y*dimUncropped.x*bpp));

    if(r->getDataType() == TYPE_FLOAT32)
    {
      sum = 0.0f;
      float *const data = (float *)r->getDataUncropped(0, 0);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) reduction(+:sum)
#endif
      for(size_t k = 0; k < ((size_t) dimUncropped.y*dimUncropped.x); k++)
      {
        sum += (double) data[k];
      }

      fprintf(stdout, "Image float sum: %lf\n", sum);
      fprintf(stdout, "Image float avg: %lf\n", sum/(double)(dimUncropped.y*dimUncropped.x));
    }
    else if(r->getDataType() == TYPE_USHORT16)
    {
      sum = 0.0f;
      uint16_t *const data = (uint16_t *)r->getDataUncropped(0, 0);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) reduction(+:sum)
#endif
      for(size_t k = 0; k < ((size_t) dimUncropped.y*dimUncropped.x); k++)
      {
        sum += (double) data[k];
      }

      fprintf(stdout, "Image uint16_t sum: %lf\n", sum);
      fprintf(stdout, "Image uint16_t avg: %lf\n", sum/(double)(dimUncropped.y*dimUncropped.x));
    }
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
