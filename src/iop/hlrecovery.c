/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

/*
Highlights recovery

** Overview **

The new highlight recovery algorithm only works for standard bayer sensors.
It has been developed in collaboration by Iain and garagecoder from the gmic team and Hanno Schwalm from dt.

The original idea was presented by Iain in pixls.us in: https://discuss.pixls.us/t/highlight-recovery-teaser/17670

and has been extensively discussed over the last months, a number of different approaches had been evaluated.

Prototyping and testing ideas has been done by Iain using gmic, Hanno and garagecoder did the implementation and integration
into dt’s codebase.
No other external modules (like gmic …) are used, the current code has been tuned for performance using omp,
no OpenCL codepath yet.

** Main ideas **

The algorithm follows these basic ideas:
1. We understand the bayer data as superpixels, each having one red, one blue and two green photosites
2. We analyse all data on the channels independently so resulting in 4 color-planes
3. We want to keep details as much as possible
4. In all 4 color planes we look for isolated areas being clipped (segments).
   These segments also include the unclipped photosites at the borders.
   Inside these segments we look for a candidate to represent the value we take for restoration.
   Choosing the candidate is done at all non-clipped locations of a segment, the best candidate is selected via a weighing
   function - the weight is derived from
   - the local standard deviation in a 5x5 area and
   - the median value of unclipped positions also in a 5x5 area.
   The best candidate points to the location in the color plane holding the reference value.
   If there is no good candidate we use an averaging approximation over the whole segment.
5. A core principle is to inpaint pseudo-chromacity, calculated by subtracting opponent channel means rather than luminance.
6. Cube root is used instead of logarithm for better stability, which suffices for an estimate.

The chosen segmentation algorithm works like this:
1. Doing the segmentation in every color plane.
2. Remove single clipped photosites for stability
3. To combine small segments for a shared candidate we use a morphological closing operation, the radius of that UI op
   can be chosen interactively between 0 and 8.
4. The segmentation algorithm uses a modified floodfill, it also takes care of the surrounding rectangle of every segment
   and marks the segment borders.
5. After segmentation we check every segment for
   - the segment's best candidate via the weighing function
   - the candidates location
*/

/* Ideas to be checked
   1. can we check for close segments after combining in other ways to find a common best candidate?
   2. the "best candidate" for bad segments is just so-so. Can we look for close segments here too and take that candidate?
   3. Should the weighing function also take the other planes in account? 
*/

#define HL_SENSOR_PLANES 4
#define HL_REF_PLANES 4
#define HL_FLOAT_PLANES (HL_SENSOR_PLANES + HL_REF_PLANES)
#define HLMAXSEGMENTS 0x4000
#define HLBORDER 8

#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("finite-math-only")
#endif

static size_t plane_size(size_t width, size_t height)
{
  return (size_t) dt_round_size((width + 4) * (height + 4), 16);
}

typedef enum dt_iop_highlights_plane_t
{
  DT_IO_PLANE_RED = 0,          // $DESCRIPTION: "red"
  DT_IO_PLANE_GREEN1 = 1,       // $DESCRIPTION: "green1"
  DT_IO_PLANE_GREEN2 = 2,       // $DESCRIPTION: "green2"
  DT_IO_PLANE_BLUE = 3,         // $DESCRIPTION: "blue"
} dt_iop_highlights_plane_t;

#include "iop/segmentation.h"

static inline float sqrf(const float a)
{
  return a*a;
}

static inline float local_std_deviation(const float *p, const int w)
{
  const int w2 = 2*w;
  const float av = 0.04f *
      (p[-w2-2] + p[-w2-1] + p[-w2] + p[-w2+1] + p[-w2+2] +
       p[-w-2]  + p[-w-1]  + p[-w]  + p[-w+1]  + p[-w+2] +
       p[-2]    + p[-1]    + p[0]   + p[1]     + p[2] +
       p[w-2]   + p[w-1]   + p[w]   + p[w+1]   + p[w+2] +
       p[w2-2]  + p[w2-1]  + p[w2]  + p[w2+1]  + p[w2+2]);
  return sqrtf(0.04f *
      (sqrf(p[-w2-2]-av) + sqrf(p[-w2-1]-av) + sqrf(p[-w2]-av) + sqrf(p[-w2+1]-av) + sqrf(p[-w2+2]-av) +
       sqrf(p[-w-2]-av)  + sqrf(p[-w-1]-av)  + sqrf(p[-w]-av)  + sqrf(p[-w+1]-av)  + sqrf(p[-w+2]-av) +
       sqrf(p[-2]-av)    + sqrf(p[-1]-av)    + sqrf(p[0]-av)   + sqrf(p[1]-av)     + sqrf(p[2]-av) +
       sqrf(p[w-2]-av)   + sqrf(p[w-1]-av)   + sqrf(p[w]-av)   + sqrf(p[w+1]-av)   + sqrf(p[w+2]-av) +
       sqrf(p[w2-2]-av)  + sqrf(p[w2-1]-av)  + sqrf(p[w2]-av)  + sqrf(p[w2+1]-av)  + sqrf(p[w2+2]-av)));
}

static inline float local_smoothness(const float *p, const int w)
{
  return sqrf(1.0f - fmaxf(0.0f, fminf(1.0f, 2.0f * local_std_deviation(p, w))));
}

static float calc_weight(const float *s, const char *lmask, const size_t loc, const int w, const float clipval)
{
  const float smoothness = local_smoothness(&s[loc], w);
  float val = 0.0f;
  for(int y = -1; y < 2; y++)
  {
    for(int x = -1; x < 2; x++)
      val += s[loc + y*w + x] * 0.11111f;
  }
  const float sval = fminf(clipval, val) / clipval;
  return smoothness * sval;
}

static void prepare_smooth_singles(char *lmask, float *src, const float *ref, const size_t width, const size_t height, const float clipval)
{
  const size_t p_size = plane_size(width, height);
  float *tmp = dt_alloc_align_float(p_size);
  char *mtmp = dt_alloc_align(16, p_size * sizeof(char));
  if(tmp == NULL || mtmp == NULL) return;

  dt_iop_image_copy(tmp, src, width * height);
  memcpy(mtmp, lmask, width * height * sizeof (char));   

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(src, ref, tmp, lmask, mtmp) \
  dt_omp_sharedconst(width, height, clipval) \
  schedule(static)
#endif
  for(size_t row = HLBORDER; row < height - HLBORDER; row++)
  {
    for(size_t col = HLBORDER, ix = row * width + col; col < width - HLBORDER; col++, ix++)
    {
      if(lmask[ix] == 1) // we only take care of clipped locations
      {
        float sum = 0.0f;
        float cnt = 0.0f;
        for(int y = -1; y < 2; y++) // we look for surrounding unclipped in 3x3 area
        {
          for(int x = -1; x < 2; x++)
          {
            const int pos = ix + y*width + x;
            const gboolean unclipped = (lmask[pos] == 0);
            cnt += (unclipped) ? 1.0f : 0.0f;
            sum += (unclipped) ? src[pos] - ref[pos] : 0.0f;
          }
        }
        if(cnt > 4.0f)
        {
          if(local_std_deviation(&ref[ix], width) < 0.005f) // arbitrary from tests
          {
            tmp[ix] = fmaxf(clipval, ref[ix] + (sum / cnt));
            mtmp[ix] = 0;
          }
        }
      }
    }
  }
  memcpy(lmask, mtmp, width * height * sizeof(char));   
  dt_iop_image_copy(src, tmp, width * height);

  dt_masks_extend_border(src, width, height, HLBORDER);
  dt_free_align(tmp);
  dt_free_align(mtmp);
}

static void calc_plane_candidates(const float *s, char *lmask, const float *ref, dt_iop_segmentation_t *seg, const int width, const int height, const float clipval, const float refval)
{
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(s, lmask, ref, seg) \
  dt_omp_sharedconst(width, height, clipval, refval) \
  schedule(dynamic)
#endif
  for(int id = 2; id < seg->nr + 2; id++)
  {
    int *segmap    = seg->data;
    size_t testref = 0;
    float testweight = 0.0f;
    for(int row = seg->ymin[id] -2 ; row < seg->ymax[id] + 3; row++)
    {
      for(int col = seg->xmin[id] -2; col < seg->xmax[id] + 3; col++)
      {
        const size_t pos = row * width + col;
        const int sid = segmap[pos] & (HLMAXSEGMENTS-1);
        if((sid == id) && (lmask[pos] == 0)) // we test for a) being in segment and b) unclipped
        {
          const float wht = calc_weight(s, lmask, pos, width, clipval);
          if(wht > testweight)
          {
            testweight = wht;
            testref = pos;
          }        
        }
      }
    }

    if(testref && (testweight > refval)) // We have found a reference location
    {
      seg->ref[id] = testref;
      segmap[testref] = 2*HLMAXSEGMENTS + id;
      float sum  = 0.0f;
      float pix = 0.0f;
      const float weights[5][5] = {
        { 1.0f,  4.0f,  6.0f,  4.0f, 1.0f },
        { 4.0f, 16.0f, 24.0f, 16.0f, 4.0f },
        { 6.0f, 24.0f, 36.0f, 24.0f, 6.0f },
        { 4.0f, 16.0f, 24.0f, 16.0f, 4.0f },
        { 1.0f,  4.0f,  6.0f,  4.0f, 1.0f }};
      for(int y = -2; y < 3; y++)
      {
        for(int x = -2; x < 3; x++)
        {
          const size_t pos = testref + y*width + x;
          const gboolean unclipped = (lmask[pos] == 0);
          sum += (unclipped) ? s[pos] * weights[y+2][x+2] : 0.0f;
          pix += (unclipped) ? weights[y+2][x+2] : 0.0f;
        }
      }
      seg->val1[id] = fminf(clipval, sum / fmaxf(1.0f, pix));
      seg->val2[id] = ref[testref];
    }
    else
    {
#if FALSE
      float maxval = 0.0f;
      float msum = 0.0f;
      float pix  = 0.0f;
      for(int row = seg->ymin[id]; row < seg->ymax[id] + 1; row++)
      {
        for(int col = seg->xmin[id]; col < seg->xmax[id] + 1; col++)
        {
          const size_t pos = row * width + col;
          const int sid = segmap[pos] & (HLMAXSEGMENTS-1);
          if(id == sid) // we look for all in-segment locations
          {
            msum += ref[pos];
            maxval = fmaxf(maxval, s[pos]);
            pix  += 1.0;
          }
        }
      }
      seg->val1[id] = maxval;
      seg->val2[id] = msum / fmaxf(1.0f, pix);
#else
      seg->val1[id] = clipval;
      seg->val2[id] = clipval;
#endif
    }
  }
}

static inline int pos2plane(const int row, const int col, const uint32_t filters)
{
  const int c = FC(row, col, filters);
  if(c == 0)      return 0;
  else if(c == 2) return 3;
  else return 1 + (row&1);
}

static void process_recovery(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const uint32_t filters, dt_iop_highlights_data_t *data, const int vmode)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float clipval = 0.987f * data->clip;
  const int combining = (int) data->combine;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const int pwidth  = ((width + 1 ) / 2) + (2 * HLBORDER);
  const int pheight = ((height + 1) / 2) + (2 * HLBORDER);

  const size_t p_size = plane_size(pwidth, pheight);

  const int p_off  = (HLBORDER * pwidth) + HLBORDER;

  dt_iop_image_copy(out, in, width * height);

  if(filters == 0 || filters == 9u) return;

  float *fbuffer = dt_alloc_align_float(HL_FLOAT_PLANES * p_size);
  char *mbuffer = dt_alloc_align(16, HL_SENSOR_PLANES * p_size * sizeof(char));

  if(!fbuffer || !mbuffer)
  {
    fprintf(stderr, "[highlights reconstruction in recovery mode] internal buffer allocation problem\n");
    dt_free_align(fbuffer);
    dt_free_align(mbuffer);
    return;
  }

  dt_times_t time0 = { 0 }, time1 = { 0 }, time2 = { 0 }, time3 = { 0 };
  dt_get_times(&time0);

  dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2], 0.0f};  
  // make sure we have wb coeffs
  if((icoeffs[0] < 0.1f) || (icoeffs[1] < 0.1f) || (icoeffs[2] < 0.1f))
  {
    fprintf(stderr, "[highlights reconstruction in recovery mode] no white balance coeffs found, choosing stupid defaults\n");
    icoeffs[0] = 2.0f;
    icoeffs[1] = 1.0f;
    icoeffs[2] = 1.5f;
  }
  const dt_aligned_pixel_t coeffs = { powf(clipval * icoeffs[0], 1.0f / 3.0f), powf(clipval * icoeffs[1], 1.0f / 3.0f), powf(clipval * icoeffs[1], 1.0f / 3.0f), powf(clipval * icoeffs[2], 1.0f / 3.0f)};

  float *plane[HL_FLOAT_PLANES];
  char  *cmask[HL_SENSOR_PLANES];
  for(int i = 0; i < HL_FLOAT_PLANES; i++)
  {
    plane[i] = fbuffer + i * p_size;
    if(i < HL_SENSOR_PLANES) cmask[i] = mbuffer + i * p_size;
  }

  float *refavg[HL_REF_PLANES];
  for(int i = 0; i < HL_REF_PLANES; i++)
  {
    refavg[i] = plane[HL_SENSOR_PLANES + i];
  }

/* 
  We fill the planes [0-3] by the data from the photosites.
  These will be modified by the reconstruction algorithm and eventually written to out.
  The size of input rectangle can be odd meaning the planes might be not exactly of equal size
  so we possibly fill latest row/col by previous.
*/
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in, plane) \
  dt_omp_sharedconst(width, p_off, height, pwidth, filters) \
  schedule(simd:static) aligned(in, plane : 64)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int p = pos2plane(row, col, filters);
      const size_t o = (row/2)*pwidth + (col/2) + p_off;
      const float val = powf(fmaxf(0.0f, in[i]), 1.0f / 3.0f);
      plane[p][o] = val;

      if(col >= width-2)      plane[p][o+1] = val;
      if(row >= height-2)     plane[p][o+pwidth] = val;
    }
  }

  for(int i = 0; i < HL_SENSOR_PLANES; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HLBORDER);

  dt_iop_segmentation_t isegments[HL_SENSOR_PLANES];
  for(int i = 0; i < HL_SENSOR_PLANES; i++)
    dt_segmentation_init_struct(&isegments[i], pwidth, pheight, HLMAXSEGMENTS);

  gboolean has_clipped = FALSE;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction( | : has_clipped) \
  dt_omp_firstprivate(plane, cmask, isegments, coeffs) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < pwidth * pheight; i++)
  {
    for(int p = 0; p < HL_SENSOR_PLANES; p++)
    {
      const gboolean clipped = (plane[p][i] >= coeffs[p]);
      cmask[p][i]          = (clipped) ? 1 : 0;
      isegments[p].data[i] = (clipped) ? 1 : 0;
      has_clipped |= clipped;
    }
  }

  if(!has_clipped) goto finish;

  // Calculate opponent channel weighted means
  const float weights[4][4] = {
    { 0.0f, 0.25f, 0.25f, 0.5f },
    { 0.5f,  0.0f,  0.0f, 0.5f },
    { 0.5f,  0.0f,  0.0f, 0.5f },
    { 0.5f, 0.25f, 0.25f, 0.0f },
  };
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg, weights) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static) collapse(2)
#endif
  for(int row = HLBORDER; row < pheight - HLBORDER; row++)
  {
    for(int col = HLBORDER; col < pwidth - HLBORDER; col++)
    {
      const size_t i = row * pwidth + col;
      for(int p = 0; p < HL_REF_PLANES; p++)
      {
        refavg[p][i] = 0.125f * (
          weights[p][0] * (4.0f * plane[0][i] + plane[0][i-1] + plane[0][i+1] + plane[0][i-pwidth] + plane[0][i+pwidth]) +
          weights[p][1] * (4.0f * plane[1][i] + plane[1][i-1] + plane[1][i+1] + plane[1][i-pwidth] + plane[1][i+pwidth]) +
          weights[p][2] * (4.0f * plane[2][i] + plane[2][i-1] + plane[2][i+1] + plane[2][i-pwidth] + plane[2][i+pwidth]) +
          weights[p][3] * (4.0f * plane[3][i] + plane[3][i-1] + plane[3][i+1] + plane[3][i-pwidth] + plane[3][i+pwidth]));
      }
    }
  }

  dt_get_times(&time1);

  for(int p = 0; p < HL_SENSOR_PLANES; p++)
  {
    prepare_smooth_singles(cmask[p], plane[p], refavg[p], pwidth, pheight, coeffs[p]);

    // We prefer to have slightly wider segment borders for a possibly better chosen candidate
#if TRUE
    if(combining > 0)
    {
      dt_image_transform_dilate(isegments[p].data, pwidth, pheight, combining, HLBORDER);
      if(combining > 1)
        dt_image_transform_erode(isegments[p].data, pwidth, pheight, combining-1, HLBORDER);
    }
#else
    dt_image_transform_closing(isegments[p].data, pwidth, pheight, combining, HLBORDER);
#endif
  }
  if(dt_get_num_threads() >= HL_SENSOR_PLANES)
  {
#ifdef _OPENMP
  #pragma omp parallel num_threads(HL_SENSOR_PLANES)
#endif
    {
      segmentize_plane(&isegments[dt_get_thread_num()], pwidth, pheight);
    }
  }
  else
  {
    for(int p = 0; p < HL_SENSOR_PLANES; p++)
      segmentize_plane(&isegments[p], pwidth, pheight);
  }

  for(int p = 0; p < HL_SENSOR_PLANES; p++)
    calc_plane_candidates(plane[p], cmask[p], refavg[p], &isegments[p], pwidth, pheight, coeffs[p], 1.0f - sqrf(data->reconstructing));

  dt_get_times(&time2);

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg, isegments, cmask, coeffs) \
  dt_omp_sharedconst(pheight, pwidth, p_size, vmode) \
  schedule(static)
#endif
  for(int row = HLBORDER; row < pheight - HLBORDER; row++)
  {
    for(int col = HLBORDER; col < pwidth - HLBORDER; col++)
    {
      const size_t ix = row * pwidth + col;

      float candidates[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
      float cand_reference[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

      for(int p = 0; p < HL_SENSOR_PLANES; p++)
      {
        if(cmask[p][ix] == 1)
        {
          const int pid = isegments[p].data[ix] & (HLMAXSEGMENTS-1);
          if((pid > 1) && (pid < isegments[p].nr+2)) // segmented
          {
            candidates[p]   = isegments[p].val1[pid];
            cand_reference[p] = isegments[p].val2[pid];
          }
          else if(pid == 0)
          {
            float mval = 0.0f;
            float msum = 0.0f;
            float pix  = 0.0f;
            for(int y = -2; y < 3; y++)
            {
              for(int x = -2; x < 3; x++)
              {
                const size_t pos = ix + y*pwidth +x;
                if(cmask[p][pos] == 0)
                {
                  mval  = fmaxf(mval, plane[p][pos]);
                  msum += refavg[p][pos];
                  pix  += 1.0f;
                }
              }
            }
            if(pix > 0.0f)
            {
              candidates[p]   = mval;
              cand_reference[p] = fminf(coeffs[p], msum / pix);
            }
            else
            {
              candidates[p]   = coeffs[p];
              cand_reference[p] = fminf(coeffs[p], refavg[p][ix]);
            }
          }
        }
      }

      for(int p = 0; p < HL_SENSOR_PLANES; p++)
      {
        if((cmask[p][ix] != 0) && (vmode == 0))
        {
          float current_reference = 0.0f;
          float candidate = 0.0f;

          if((p == DT_IO_PLANE_GREEN1 || p == DT_IO_PLANE_GREEN2) && (cmask[DT_IO_PLANE_GREEN1][ix] == 1) && (cmask[DT_IO_PLANE_GREEN2][ix] == 1))
          {
            // we take the median of greens candidates.          
            if((candidates[DT_IO_PLANE_GREEN1] >= 0.0f) && (cand_reference[DT_IO_PLANE_GREEN1] >= 0.0f) &&
               (candidates[DT_IO_PLANE_GREEN2] >= 0.0f) && (cand_reference[DT_IO_PLANE_GREEN2] >= 0.0f))                  
            {
              candidate = 0.5f * (candidates[DT_IO_PLANE_GREEN1] + candidates[DT_IO_PLANE_GREEN2]);
              current_reference = 0.5f * (cand_reference[DT_IO_PLANE_GREEN1] + cand_reference[DT_IO_PLANE_GREEN2]);
            }
          }
          else
          {
            candidate = candidates[p];
            current_reference = cand_reference[p];
          }
          const float val = candidate + refavg[p][ix] - current_reference;
          plane[p][ix] = fmaxf(coeffs[p], val);
        }
      }
    }
  }

  for(int i = 0; i < HL_SENSOR_PLANES; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HLBORDER);

  float max_correction = 1.0f;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction(max : max_correction) \
  dt_omp_firstprivate(out, plane, isegments, cmask) \
  dt_omp_sharedconst(width, height, pwidth, p_off, filters, vmode) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, o = row * width; col < width; col++, o++)
    {
      const size_t i = (row/2)*pwidth + (col/2) + p_off;
      const int p = pos2plane(row, col, filters);

      const float val = powf(plane[p][i], 3.0f);
      const float ratio = val / fmaxf(1.0f, out[o]);
      out[o] = val;
      max_correction = fmaxf(max_correction, ratio);

      if(vmode)
      {
        const int pid = isegments[p].data[i] & (HLMAXSEGMENTS-1);
        const gboolean iclipped = (cmask[p][i] == 1);
        const gboolean isegmented = ((pid > 1) && (pid < isegments[p].nr+2));
        const gboolean badseg = isegmented && (isegments[p].ref[pid] == 0);

        out[o] *= 0.2f;
        if((vmode & 1) && isegmented && !iclipped) out[o] = 1.0f;
        if((vmode & 2) && isegmented && badseg)    out[o] = 1.0f;     
      }
    }
  }

  for(int k = 0; k < 3; k++)
    piece->pipe->dsc.processed_maximum[k] *= max_correction;

  dt_get_times(&time3);
  dt_print(DT_DEBUG_PERF, "[Highlight recovery] %.1fMpix, max=%1.2f, combine=%i, segs %ir %ig %ig %ib. Times: init %.3fs, segmentize %.3fs, paint %.3fs\n",
       (float) (width * height) / 1.0e6f, max_correction, combining,
       isegments[0].nr, isegments[1].nr, isegments[2].nr, isegments[3].nr, 
       time1.clock - time0.clock, time2.clock - time1.clock, time3.clock - time2.clock);

  finish:
  for(int i = 0; i < HL_SENSOR_PLANES; i++)
    dt_segmentation_free_struct(&isegments[i]);

  dt_free_align(fbuffer);
  dt_free_align(mbuffer);
}

#ifdef __GNUC__
  #pragma GCC pop_options
#endif

#undef HL_SENSOR_PLANES
#undef HL_REF_PLANES
#undef HL_FLOAT_PLANES
#undef HLFPLANES
#undef HLMAXSEGMENTS
#undef HLBORDER

