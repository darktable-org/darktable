/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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
Highlights restoration II

** Overview **

The new highlight restoration II algorithm only works for standard bayer sensors.
It has been developed in collaboration by Iain from the gmic team and Hanno Schwalm from dt.

The original idea was presented by Iain in pixls.us in: https://discuss.pixls.us/t/highlight-recovery-teaser/17670

and has been extensively discussed over the last months.
Prototyping and testing ideas has been done by Iain using gmic, Hanno did the implementation and integration into dt’s
codebase. No other external modules (like gmic …) are used, the current code has been tuned for performance using omp,
no OpenCL codepath yet.

** Main ideas **

The algorithm follows these basic ideas:
1. We understand the bayer data as superpixels, each having one red, one blue and two green photosites
2. We analyse all data (without wb correction applied) on the channels independently so resulting in 4 color-planes
3. We want to keep details as much as possible; we assume that details are best represented in the color channel having
   the minimum value. So beside the 4 color planes we also have a plane holding the minimum values (pminimum)
4. In all 4 color planes we look for isolated areas being clipped (segments).
   Inside these segments including borders around we look for a candidate to represent the value we take for restoration.
   Choosing the candidate is done at all non-clipped locations of a segment, the best candidate is selected via a weighing
   function - the weight is derived from
   - the local standard deviation in a 5x5 area and
   - the median value of unclipped positions also in a 5x5 area.
   The best candidate points to the location in the color plane holding the reference value.
   If there is no good candidate we use an approximation.
5. We evaluated several ways to further reduce the pre-existing color cast, atm we calc linearly while using a correction
   coeff for every plane.
   We also tried using some gamma correction which helped in some cases but was unstable in others.
6. The restored value at position 'i' is basically calculated as
     val = candidate + pminimum[i] - pminimum[candidate_location;
7. For locations with all planes clipped we might do a synthesis in pminimum, the value for every position is derived
   from the local gradient at the border and basically the distance.
   This code part has been surprisingly difficult to implement (avoiding ridges, good transition, … ),
   the existing code is working ok (with some minor issues) but is rather slow and not perfect.
   It has not been included in the first pr and will be re-evaluated.

For segmentation i implemented and tested several approaches including Felszenzwalb and a watershed algo,
both have problems with identifying the clipped segments in a plane. I ended up with this:

1. Doing the segmentation in every color plane.
2. The segmentation algorithm uses a modified floodfill, it also takes care of the surrounding rectangle of every segment
   and marks the segment borders.
3. After segmentation we check every segment for
   - the segment's best candidate via the weighing function
   - the candidates location
4. To combine small segments for a shared candidate we use a morphological closing operation, the radius of that op
   can be chosen interactively between 0 and 10.

Hanno & Ian 2021/12

Issues not solved yet:
1. possibly combine segments over larger distances, but how can we define a rule for that?
2. for segments with all color channels clipped the colors might be bad.
   Some preps to check for inter-channel work has been done already ... 
*/

#define HLFPLANES 5
#define HLSEGPLANES 4
#define HLMAXSEGMENTS 0x4000
#define HLBADWEIGHT 0.3f

#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("fast-math", "fp-contract=fast", "finite-math-only", "no-math-errno", "ivopts")
#endif

#include "iop/highlights_algos/segmentation.h"
#include "common/distance_transform.h"

#if HLMINSYNTHESIS
 #include "iop/highlights_algos/min_synthesis.c"
#endif

#define SQR(x) ((x) * (x))
static float calc_weight(float *p, const int w)
{
  if(p[0] >= 1.0f) return HLEPSILON;
  const int w2 = 2*w;
  const float av = 0.04f *
      (p[-w2-2] + p[-w2-1] + p[-w2] + p[-w2+1] + p[-w2+2] +
       p[-w-2]  + p[-w-1]  + p[-w]  + p[-w+1]  + p[-w+2] +
       p[-2]    + p[-1]    + p[0]   + p[1]     + p[2] +
       p[w-2]   + p[w-1]   + p[w]   + p[w+1]   + p[w+2] +
       p[w2-2]  + p[w2-1]  + p[w2]  + p[w2+1]  + p[w2+2]);
  const float sd = sqrtf(0.04f *
      (SQR(p[-w2-2]-av) + SQR(p[-w2-1]-av) + SQR(p[-w2]-av) + SQR(p[-w2+1]-av) + SQR(p[-w2+2]-av) +
       SQR(p[-w-2]-av)  + SQR(p[-w-1]-av)  + SQR(p[-w]-av)  + SQR(p[-w+1]-av)  + SQR(p[-w+2]-av) +
       SQR(p[-2]-av)    + SQR(p[-1]-av)    + SQR(p[0]-av)   + SQR(p[1]-av)     + SQR(p[2]-av) +
       SQR(p[w-2]-av)   + SQR(p[w-1]-av)   + SQR(p[w]-av)   + SQR(p[w+1]-av)   + SQR(p[w+2]-av) +
       SQR(p[w2-2]-av)  + SQR(p[w2-1]-av)  + SQR(p[w2]-av)  + SQR(p[w2+1]-av)  + SQR(p[w2+2]-av)));
  const float smoothness = (fmaxf(HLEPSILON, 1.0f - 4.0f * sd));

  float val = 0.0f;
  float cnt = 0.0f;
  for(int y = -2; y < 3; y++)
  {
    for(int x = -2; x < 3; x++)
    {
      const float t = p[y*w + x];
      val += (t < 1.0f) ? t    : 0.0f;
      cnt += (t < 1.0f) ? 1.0f : 0.0f;
    }
  }
  return fmaxf(HLEPSILON, smoothness * (val / fmaxf(1.0f, cnt)));
}
#undef SQR

static void calc_plane_candidates(float *s, float *pmin, dt_iop_segmentation_t *seg, const int width, const int height, const float maxval)
{
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(s, pmin, seg) \
  dt_omp_sharedconst(width, height) \
  schedule(dynamic)
#endif
  for(int id = 2; id < seg->nr + 2; id++)
  {
    int *segmap    = seg->data;
    size_t testref = 0;
    float testweight = 0.0f;
    for(int row = seg->ymin[id] -1 ; row < seg->ymax[id] + 2; row++)
    {
      for(int col = seg->xmin[id] -1; col < seg->xmax[id] + 2; col++)
      {
        const size_t pos = row * width + col;
        const int sid = segmap[pos] & (HLMAXSEGMENTS-1);
        if((sid == id) && (s[pos] < 1.0f)) // we test for a) being in segment and b) unclipped
        {
          const float wht = calc_weight(&s[pos], width);
          if(wht > testweight)
          {
            testweight = wht;
            testref = pos;
          }        
        }
      }
    }

    if(testref)
    {
      float sum  = 0.0f;
      float pix = 0.0f;
      float candidate = 0.0f;
      if(testweight > HLBADWEIGHT)
      {
        seg->ref[id] = testref;
        segmap[testref] = 2*HLMAXSEGMENTS + id;
        for(int y = -2; y < 3; y++)
        {
          for(int x = -2; x < 3; x++)
          {
            const float rr = s[testref + y*width + x];
            sum += (rr < 1.0f) ? rr   : 0.0f;
            pix += (rr < 1.0f) ? 1.0f : 0.0f;
          }
        }
        candidate = sum / fmaxf(1.0f, pix);
      }
      else
      {
        for(int row = seg->ymin[id]; row < seg->ymax[id] + 1; row++)
        {
          for(int col = seg->xmin[id]; col < seg->xmax[id] + 1; col++)
          {
            const size_t pos = row * width + col;
            const int sid = segmap[pos];
            if(segmap[pos] == sid)
            {
              sum += pmin[pos];
              pix += 1.0f;
            }
          }
        }
        candidate = 1.0f - sum / fmaxf(1.0f, pix);
      }
      seg->refcol[id] = fminf(1.0f - HLEPSILON, candidate);
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

static inline int plane2bitmask(const int p)
{
  const int masks[4] = { 0x01, 0x06, 0x06, 0x08 };
  return masks[p&3];
}

static void process_recovery(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const uint32_t filters, dt_iop_highlights_data_t *data, const gboolean debug)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  const float clip = fminf(1.0f, 0.97f * data->clip);
  const float reconstruct = data->reconstructing;
  const int combining = (int) data->combine;
  const dt_iop_highlights_maskmode_t maskmode = data->maskmode;
  const dt_iop_highlights_plane_t selplane = MAX( 0, MIN(HLSEGPLANES -1, data->plane));

  const int doption = data->option;
  const float foption = data->floatoption;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const int pwidth  = ((width + 1 ) / 2) + (2 * HLBORDER);
  const int pheight = ((height + 1) / 2) + (2 * HLBORDER);

  const size_t p_size = plane_size(pwidth, pheight);
  const int p_off  = (HLBORDER * pwidth) + HLBORDER;

  dt_iop_image_copy(out, in, width * height);

  const gboolean run_fast = (piece->pipe->type & DT_DEV_PIXELPIPE_FAST) == DT_DEV_PIXELPIPE_FAST;

  if((filters == 0) || (filters == 9u) || run_fast) return;

  float *fbuffer = dt_alloc_align_float(HLFPLANES * p_size);
  uint8_t *locmask  = dt_alloc_align(16, p_size * sizeof(uint8_t));

  if(!fbuffer || !locmask)
  {
    fprintf(stderr, "[highlights reconstruction in recovery mode] internal buffer allocation problem\n");
    dt_free_align(fbuffer);
    dt_free_align(locmask);
    return;
  }

  dt_times_t time0 = { 0 }, time1 = { 0 }, time2 = { 0 }, time3 = { 0 }, time4 = { 0 };
  const gboolean info = (((darktable.unmuted & DT_DEBUG_PERF) || HLDEVELOP) && (piece->pipe->type == DT_DEV_PIXELPIPE_FULL));
  if(info) dt_get_times(&time0);

  float icoeffs[4] = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2], 0.0f};  
  // make sure we have so wb coeffs
  if((icoeffs[0] < 0.1f) || (icoeffs[0] < 0.1f) || (icoeffs[0] < 0.1f))
  {
    fprintf(stderr, "[highlights reconstruction in recovery mode] no white balance coeffs found, choosing stupid defaults\n");
    icoeffs[0] = 2.0f;
    icoeffs[1] = 1.0f;
    icoeffs[2] = 1.5f;
  }
  const float coeffs[4] = { icoeffs[0], icoeffs[1], icoeffs[1], icoeffs[2]};
  float corr_coeff[4]   = { icoeffs[1] + icoeffs[2], icoeffs[0] + icoeffs[2], icoeffs[0] + icoeffs[2], icoeffs[0] + icoeffs[1]};
  const float mincoeff  = fminf(corr_coeff[0], fminf(corr_coeff[1], corr_coeff[3]));
  for(int c = 0; c < 4; c++) corr_coeff[c] /= mincoeff;

  float *plane[HLFPLANES];
  for(int i = 0; i < HLFPLANES; i++)
    plane[i] = fbuffer + i * p_size;

  float *const pminimum  = plane[4];

/* 
  We fill the planes [0-3] by the data from the photosites.
  These will be modified by the reconstruction algorithm and will at last be written to out.
  As we have temperature corrected values as input we revert the temperature coeffs here.
  The size of input rectangle can be odd meaning the planes might be not exactly of equal size
  so we possibly fill latest row/col by previous.
*/
  float maxval = 0.0f;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  reduction(max : maxval) \
  dt_omp_firstprivate(in, plane, coeffs) \
  dt_omp_sharedconst(width, p_off, height, pwidth, filters, clip) \
  schedule(simd:static) aligned(in : 64)
#endif
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row*width; col < width; col++, i++)
    {
      const int p = pos2plane(row, col, filters);
      const size_t o = (row/2)*pwidth + (col/2) + p_off;
      const float val = fminf(1.0f, (in[i] / coeffs[p]) / clip);
      plane[p][o] = val;
      maxval = fmaxf(maxval, val);          
      if(col >= width-2)      plane[p][o+1] = val;
      if(row >= height-2)     plane[p][o+pwidth] = val;
    }
  }
  if((maxval < 1.0f) && !debug)
  {
    if(info) fprintf(stderr, "[highlights reconstruction recovery mode] early exit because of no clipped data\n");
    dt_free_align(fbuffer);
    dt_free_align(locmask);
    return;
  }

  dt_iop_segmentation_t isegments[HLSEGPLANES];
  for(int i = 0; i < HLSEGPLANES; i++)
    dt_segmentation_init_struct(&isegments[i], pwidth, pheight, HLMAXSEGMENTS);

  for(int i = 0; i < 4; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HLBORDER);

#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(plane, pminimum, locmask) \
  dt_omp_sharedconst(p_size) \
  schedule(simd:static) aligned(pminimum, plane, : 64)
#endif
  for(int i = 0; i < p_size; i++)
  {
    const float minval = fminf(plane[0][i], fminf(plane[1][i], fminf(plane[2][i], plane[3][i])));
    pminimum[i] = minval;
    uint8_t mask = 0;
    for(int p = 0; p < 4; p++)
    {
      if(plane[p][i] >= 1.0f)   mask |= (0x01 << p); // mark as clipped
      if(plane[p][i] == minval) mask |= (0x10 << p); // mark as minimum defined by this plane
    }
    locmask[i] = mask;   
  }

  if(info) dt_get_times(&time1);
#if HLMINSYNTHESIS
  if((data->allclipped > 0.0f) && (maxval >= 1.0f))
    maxval = reconstruct_minimum_plane(pminimum, pwidth, pheight, data->allclipped);
#endif
  if(info) dt_get_times(&time2);

  // prepare the segmentation process by writing the int mask; 1 for condition on, otherwise 0
  for(int p = 0; p < 4; p++)
  {
    dt_iop_segmentation_t *seg = &isegments[p];
    float *src = plane[p];
    int *map  = seg->data; 
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, map) \
  dt_omp_sharedconst(pwidth, pheight) \
  schedule(static)
#endif
    for(size_t i = 0; i < pwidth * pheight; i++)
      map[i]  = (src[i] >= 1.0f) ? 1 : 0;
  }

  for(int p = 0; p < 4; p++)
  {
    if(combining)
    {
      dt_image_transform_erode(isegments[p].data, pwidth, pheight, 0, HLBORDER);
      dt_image_transform_dilate(isegments[p].data, pwidth, pheight, 1, HLBORDER);
      dt_image_transform_closing(isegments[p].data, pwidth, pheight, combining, HLBORDER);
    }
  }
  if(dt_get_num_threads() >= HLSEGPLANES)
  {
#ifdef _OPENMP
  #pragma omp parallel num_threads(HLSEGPLANES)
#endif
    {
      segmentize_plane(&isegments[dt_get_thread_num()], pwidth, pheight);
    }
  }
  else
  {
    for(int p = 0; p < HLSEGPLANES; p++)
      segmentize_plane(&isegments[p], pwidth, pheight);
  }

  for(int p = 0; p < HLSEGPLANES; p++)
    calc_plane_candidates(plane[p], pminimum, &isegments[p], pwidth, pheight, maxval);

  if(info) dt_get_times(&time3);

  float max_correction = 1.0f;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  reduction(max : max_correction) \
  dt_omp_firstprivate(out, plane, pminimum, isegments, coeffs, corr_coeff, locmask) \
  dt_omp_sharedconst(width, height, pheight, pwidth, p_size, clip, reconstruct, p_off, filters, debug, maskmode, doption, foption) \
  schedule(simd:static) aligned(out, plane, pminimum : 64)
#endif
  for(int row = 0; row < height; row++)
  {
    for(int col = 0, o = row * width; col < width; col++, o++)
    {
      const int i = (row/2)*pwidth + (col/2) + p_off;
      const int p = pos2plane(row, col, filters);
      if(locmask[i] & (0x01 << p))
      {
        float candidates_minimum = 0.0f;
        float candidate = 0.0f;
        gboolean restore = FALSE;

        int segid[4]          = { 0, 0, 0, 0 };
        float candidates[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
        float cand_minimum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        // Prepare data for interchannel corrections, so far only used by greens mixing
        for(int pp = 0; pp < 4; pp++)
        {
          const int pid = isegments[pp].data[i] & (HLMAXSEGMENTS-1);
          if((pid > 1) && (pid < isegments[pp].nr+2))
          {
            segid[pp]         = pid;          
            candidates[pp]    = isegments[pp].refcol[pid];   
            const size_t loc  = isegments[pp].ref[pid];
            if(loc) cand_minimum[pp] = pminimum[loc];
          }
          else if(pid == 0)
          {
            segid[pp] = 1;          
            float sum = 0.0f;
            for(int y = -2; y < 3; y++)
              for(int x = -2; x < 3; x++) sum += pminimum[i + y*pwidth +x];
            candidates[pp]    = 1.0f - (0.04f * sum);   
          }
        }     

        if((p == DT_IO_PLANE_GREEN1 || p == DT_IO_PLANE_GREEN2) && segid[DT_IO_PLANE_GREEN1] && segid[DT_IO_PLANE_GREEN2])
        {
          // we take the median of greens candidates.          
          if((candidates[DT_IO_PLANE_GREEN1] >= 0.0f) && (cand_minimum[DT_IO_PLANE_GREEN1] >= 0.0f) &&
             (candidates[DT_IO_PLANE_GREEN2] >= 0.0f) && (cand_minimum[DT_IO_PLANE_GREEN2] >= 0.0f))                  
          {
            candidate = 0.5f * (candidates[DT_IO_PLANE_GREEN1] + candidates[DT_IO_PLANE_GREEN2]);
            candidates_minimum = 0.5f * (cand_minimum[DT_IO_PLANE_GREEN1] + cand_minimum[DT_IO_PLANE_GREEN2]);
            restore = TRUE;
          }
        }

        if(segid[p] && !restore)
        {
          candidate = candidates[p];
          candidates_minimum = cand_minimum[p];
          restore = TRUE;
        }

        if(restore)
        {
          const float correction = corr_coeff[p] * (0.7 + 1.5f * reconstruct);
          float val = candidate + pminimum[i] - candidates_minimum;
          val = 1.0f + (val - 1.0f) * correction;
          val = clip * fmaxf(1.0f, val);
          out[o] = val * coeffs[p];
          max_correction = fmaxf(max_correction, val);
          if(debug && ((maskmode == DT_IO_HIGHLIGHTSMASK_PLANELATE) || (maskmode == DT_IO_HIGHLIGHTSMASK_PLANEABOVE))) plane[p][i] = val;
        }
      }
    }
  }

  for(int k = 0; k < 4; k++)
    piece->pipe->dsc.processed_maximum[k] *= max_correction;

  if(info) 
  {
    dt_get_times(&time4);
    fprintf(stderr, "Highlight recovery: %4.1fMpix, maxval=%.2f, maxcorr=%.2f, option=%i, foption=%.3f", (float) (width * height) / 1.0e6f, maxval, max_correction, doption, foption);
    fprintf(stderr, "\n Segments(combine %i): ", combining);
    for(int i=0; i<HLSEGPLANES; i++)  { fprintf(stderr, " %6i", isegments[i].nr); }
    fprintf(stderr, "\n Performance (all)   %.3f",   time4.clock - time0.clock);
    fprintf(stderr, "\n    initialize       %.3f",   time1.clock - time0.clock);
    fprintf(stderr, "\n    synth minimum    %.3f",   time2.clock - time1.clock);
    fprintf(stderr, "\n    segmentation     %.3f",   time3.clock - time2.clock);
    fprintf(stderr, "\n    output           %.3f\n", time4.clock - time3.clock);
  }

  if(debug && maskmode)
  {
    dt_iop_image_fill(out, 0.0f, width, height, 1);
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(out, plane, pminimum, isegments, locmask, coeffs) \
  dt_omp_sharedconst(maskmode, width, height, pwidth, p_off, selplane, filters, p_size) \
  schedule(static) aligned(out, plane, pminimum : 64)
#endif
    for(size_t row = 0; row < height; row++)
    {
      for(size_t col = 0, o = row*width; col < width; col++, o++)
      {
        const size_t i = (row/2)*pwidth + (col/2) + p_off;
        const int p = pos2plane(row, col, filters);
        dt_iop_segmentation_t *seg = &isegments[selplane];            
        const int c = seg->data[i];
        const int cc = c & (HLMAXSEGMENTS - 1);
        float val = 0.0f;
        switch(maskmode)
        {
          case DT_IO_HIGHLIGHTSMASK_RECONSTR_NO:
            break;
          case DT_IO_HIGHLIGHTSMASK_CLIPPED_PLANES:
            if(locmask[i] & (1 << p)) val = 1.0f;
            break;
          case DT_IO_HIGHLIGHTSMASK_MINIMUM_PLANE:
            if(((locmask[i] >> 4) & plane2bitmask(p)) != 0) val = 1.0f;
            break;
          case DT_IO_HIGHLIGHTSMASK_SEGMENTS:
            if((c>1) && (c < HLMAXSEGMENTS)) val = 0.1f + (0.1f * (float) (c & 7));
            break;
          case DT_IO_HIGHLIGHTSMASK_SEGMENTWEIGHT:
            if((c>1) && (c < HLMAXSEGMENTS))
            {
             const size_t cref = seg->ref[cc];
             const float wht = (cref) ? calc_weight(&plane[selplane][cref], pwidth) : 0.0f;            
             val = (wht < HLBADWEIGHT) ? 0.1f : 0.4f + 0.6f * wht;
            }
            break;
          case DT_IO_HIGHLIGHTSMASK_CANDIDATE:
            if((c>1) && (c < HLMAXSEGMENTS)) val = seg->refcol[cc];
            break;
          case DT_IO_HIGHLIGHTSMASK_CANDIDATE_LOC:
            if((c > 1) && (c < HLMAXSEGMENTS))  val = 0.1f;
            else if(c > 2*HLMAXSEGMENTS+1)              // mark the reference point
            {
              val = 1.0f;
              if((row > 0) && (row < height-1) && (col > 0) && (col < width-1))
                out[o-1] = out[o+1] = out[o-width] = out[o+width] = 1.0f; 
            }
            break;
          case DT_IO_HIGHLIGHTSMASK_PLANE:
            val = 0.5f * plane[selplane][i];
            break;
          case DT_IO_HIGHLIGHTSMASK_PLANELATE:
            val = 0.5f * plane[selplane][i];
            break;
          case DT_IO_HIGHLIGHTSMASK_PLANEABOVE:
            val = 0.5f * (plane[selplane][i] - 1.0f);
            break;
          case DT_IO_HIGHLIGHTSMASK_PLANEWEIGHT:
            val = calc_weight(&plane[selplane][i], pwidth);
            break;
          case DT_IO_HIGHLIGHTSMASK_MINIMUMPLANE:
            val = 0.5f * pminimum[i];
            break;
        }
        if(maskmode == DT_IO_HIGHLIGHTSMASK_CANDIDATE_LOC)
          out[o] = fmaxf(out[o], fminf(1.0f, fmaxf(0.0f, val)));
        else
          out[o] = fminf(1.0f, fmaxf(0.0f, val));
      }
    }
  }

  for(int i = 0; i < HLSEGPLANES; i++)
    dt_segmentation_free_struct(&isegments[i]);

  dt_free_align(fbuffer);
  dt_free_align(locmask);
}

// revert specific aggressive optimizing
#ifdef __GNUC__
  #pragma GCC pop_options
#endif

#undef HLFPLANES
#undef HLMAXSEGMENTS
#undef HLBADWEIGHT
#undef HLSEGPLANES

