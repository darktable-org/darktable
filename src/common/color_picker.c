/*
    This file is part of darktable,
    copyright (c) 2016 Roman Lebedev.

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

#include "common/color_picker.h"
#include "common/darktable.h"
#include "develop/format.h"
#include "develop/imageop.h"

static void color_picker_helper_4ch(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                                    const dt_iop_roi_t *roi, const int *const box, float *const picked_color,
                                    float *const picked_color_min, float *const picked_color_max)
{
  const int width = roi->width;

  const size_t size = ((box[3] - box[1]) * (box[2] - box[0]));

  const float w = 1.0f / (float)size;

  if(size > 100) // avoid inefficient multi-threading in case of small region size (arbitrary limit)
  {
    const int numthreads = dt_get_num_threads();

    float *mean = malloc((size_t)3 * numthreads * sizeof(float));
    float *mmin = malloc((size_t)3 * numthreads * sizeof(float));
    float *mmax = malloc((size_t)3 * numthreads * sizeof(float));

    for(int n = 0; n < 3 * numthreads; n++)
    {
      mean[n] = 0.0f;
      mmin[n] = INFINITY;
      mmax[n] = -INFINITY;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(mean, mmin, mmax) schedule(static) collapse(2)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      for(size_t i = box[0]; i < box[2]; i++)
      {
        const int tnum = dt_get_thread_num();
        float *tmean = mean + 3 * tnum;
        float *tmmin = mmin + 3 * tnum;
        float *tmmax = mmax + 3 * tnum;
        const size_t k = 4 * (width * j + i);
        const float L = pixel[k];
        const float a = pixel[k + 1];
        const float b = pixel[k + 2];
        tmean[0] += w * L;
        tmean[1] += w * a;
        tmean[2] += w * b;
        tmmin[0] = fminf(tmmin[0], L);
        tmmin[1] = fminf(tmmin[1], a);
        tmmin[2] = fminf(tmmin[2], b);
        tmmax[0] = fmaxf(tmmax[0], L);
        tmmax[1] = fmaxf(tmmax[1], a);
        tmmax[2] = fmaxf(tmmax[2], b);
      }
    }

    for(int n = 0; n < numthreads; n++)
    {
      for(int k = 0; k < 3; k++)
      {
        picked_color[k] += mean[3 * n + k];
        picked_color_min[k] = fminf(picked_color_min[k], mmin[3 * n + k]);
        picked_color_max[k] = fmaxf(picked_color_max[k], mmax[3 * n + k]);
      }
    }

    free(mmax);
    free(mmin);
    free(mean);
  }
  else
  {
    // code path for small region, especially for color picker point mode
    for(size_t j = box[1]; j < box[3]; j++)
    {
      for(size_t i = box[0]; i < box[2]; i++)
      {
        const size_t k = 4 * (width * j + i);
        const float L = pixel[k];
        const float a = pixel[k + 1];
        const float b = pixel[k + 2];
        picked_color[0] += w * L;
        picked_color[1] += w * a;
        picked_color[2] += w * b;
        picked_color_min[0] = fminf(picked_color_min[0], L);
        picked_color_min[1] = fminf(picked_color_min[1], a);
        picked_color_min[2] = fminf(picked_color_min[2], b);
        picked_color_max[0] = fmaxf(picked_color_max[0], L);
        picked_color_max[1] = fmaxf(picked_color_max[1], a);
        picked_color_max[2] = fmaxf(picked_color_max[2], b);
      }
    }
  }
}

void dt_color_picker_helper(const dt_iop_buffer_dsc_t *dsc, const float *const pixel, const dt_iop_roi_t *roi,
                            const int *const box, float *const picked_color, float *const picked_color_min,
                            float *const picked_color_max)
{
  if(dsc->channels == 4u)
    color_picker_helper_4ch(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
  else
    dt_unreachable_codepath();
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
