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
Segmentation based highlight reconstruction Version 2

** Overview **

V2 of the segmentation based highlight reconstruction algorithm works for bayer and xtrans sensors.
It has been developed in collaboration by Iain and garagecoder from the gmic team and Hanno Schwalm from dt.

The original idea was presented by Iain in pixls.us in: https://discuss.pixls.us/t/highlight-recovery-teaser/17670
and has been extensively discussed over the last year, a number of different approaches have been evaluated.

No other external modules (like gmic …) are used, the current code has been tuned for performance using omp,
no OpenCL codepath yet.

** Main ideas **

The algorithm follows these basic ideas:
1. We approximate each of the red, green and blue color channels from sensor data in a 3x3 photosite region.
2. We analyse all data on the channels independently.
3. We want to keep details as much as possible
4. In all 3 color planes we look for isolated areas being clipped (segments).
   These segments also include the unclipped photosites at the borders, we also use these locations for estimating the global chrominance.
   Inside these segments we look for a candidate to represent the value we take for restoration.
   Choosing the candidate is done at all non-clipped locations of a segment, the best candidate is selected via a weighing
   function - the weight is derived from
   - the local standard deviation in a 5x5 area and
   - the median value of unclipped positions also in a 5x5 area.
   The best candidate points to the location in the color plane holding the reference value.
   If there is no good candidate we use an averaging approximation over the whole segment with correction of chrominance.
5. A core principle is to inpaint pseudo-chromacity, calculated by subtracting opponent channel means rather than luminance.
6. Cube root is used instead of logarithm for better stability, which suffices for an estimate.

The chosen segmentation algorithm works like this:
1. Doing the segmentation in every color plane.
2. To combine small segments for a shared candidate we use a morphological closing operation, the radius of that UI op
   can be chosen interactively between 0 and 8.
3. The segmentation algorithm uses a modified floodfill, it also takes care of the surrounding rectangle of every segment
   and marks the segment borders.
4. After segmentation we check every segment for
   - the segment's best candidate via the weighing function
   - the candidates location
*/

/* Recovery algorithm
  In areas with all planes clipped we try to reconstruct (hopefully a good guess) data based on the border gradients and the
  segment's size - here we use a distance transformation.
  What do we need to do so?
  1. We need a "luminance" plane, we use Y0 for this.
  2. We have an additional cmask holding information about all-channels-clipped
  3. Based on the Y0 data and all-clipped info we prepare a gradient plane.
  4. We also do a segmentation for the all-clipped data.

  After this preparation steps we reconstruct data for every segment.
  1. Calculate average gradients in an iterative loop for every distance value.
     The new gradients calculation uses the distance and averaged gradients of the last iterative step.
     By doing so we avoid direction problems.
  2. Do a box-blur to suppress ridges, the radius depends on segment size.
  3. Possibly add some noise.
  4. Do a sigmoid correction supressing artefacts at the borders.
     and write back data from this segment to the gradients plane

  The UI offers
  1. A drop down menu defining the recovery mode
  2. strength slider - this also has a mask button
  3. noise slider
*/

#define HL_RGB_PLANES 3
#define HL_SEGMENT_PLANES 4
#define HL_FLOAT_PLANES 8
#define HL_BORDER 8

#include "iop/segmentation.h"
#include "common/distance_transform.h"

static inline float _local_std_deviation(const float *p, const int w)
{
  const int w2 = 2*w;
  const float av = 0.04f *
      (p[-w2-2] + p[-w2-1] + p[-w2] + p[-w2+1] + p[-w2+2] +
       p[-w-2]  + p[-w-1]  + p[-w]  + p[-w+1]  + p[-w+2] +
       p[-2]    + p[-1]    + p[0]   + p[1]     + p[2] +
       p[w-2]   + p[w-1]   + p[w]   + p[w+1]   + p[w+2] +
       p[w2-2]  + p[w2-1]  + p[w2]  + p[w2+1]  + p[w2+2]);
  return sqrtf(0.04f *
      (sqf(p[-w2-2]-av) + sqf(p[-w2-1]-av) + sqf(p[-w2]-av) + sqf(p[-w2+1]-av) + sqf(p[-w2+2]-av) +
       sqf(p[-w-2]-av)  + sqf(p[-w-1]-av)  + sqf(p[-w]-av)  + sqf(p[-w+1]-av)  + sqf(p[-w+2]-av) +
       sqf(p[-2]-av)    + sqf(p[-1]-av)    + sqf(p[0]-av)   + sqf(p[1]-av)     + sqf(p[2]-av) +
       sqf(p[w-2]-av)   + sqf(p[w-1]-av)   + sqf(p[w]-av)   + sqf(p[w+1]-av)   + sqf(p[w+2]-av) +
       sqf(p[w2-2]-av)  + sqf(p[w2-1]-av)  + sqf(p[w2]-av)  + sqf(p[w2+1]-av)  + sqf(p[w2+2]-av)));
}

static float _calc_weight(const float *s, const size_t loc, const int w, const float clipval)
{
  const float smoothness = fmaxf(0.0f, 1.0f - 10.0f * powf(_local_std_deviation(&s[loc], w), 0.5f));
  float val = 0.0f;
  for(int y = -1; y < 2; y++)
  {
    for(int x = -1; x < 2; x++)
      val += s[loc + y*w + x] * 0.11111f;
  }
  const float sval = fmaxf(1.0f, powf(fminf(clipval, val) / clipval, 2.0f));
  return sval * smoothness;
}

static void _calc_plane_candidates(const float * restrict plane, const float * restrict refavg, dt_iop_segmentation_t *seg, const float clipval, const float badlevel)
{
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg, seg) \
  dt_omp_sharedconst(clipval, badlevel) \
  schedule(dynamic)
#endif
  for(int id = 2; id < seg->nr + 2; id++)
  {
    seg->val1[id] = 0.0f;
    seg->val2[id] = 0.0f;
    seg->ref[id] = 0;
    // avoid very small segments
    if((seg->ymax[id] - seg->ymin[id] > 2) && (seg->xmax[id] - seg->xmin[id] > 2))
    {
      size_t testref = 0;
      float testweight = 0.0f;
      for(int row = seg->ymin[id] -2 ; row < seg->ymax[id] + 3; row++)
      {
        for(int col = seg->xmin[id] -2; col < seg->xmax[id] + 3; col++)
        {
          const size_t pos = row * seg->width + col;
          const int sid = _get_segment_id(seg, pos);
          if((sid == id) && (plane[pos] < clipval))
          {
            const float wht = _calc_weight(plane, pos, seg->width, clipval);
            if(wht > testweight)
            {
              testweight = wht;
              testref = pos;
            }
          }
        }
      }
      if(testref && (testweight > 1.0f - badlevel)) // We have found a reference location
      {
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
            const size_t pos = testref + y*seg->width + x;
            const gboolean unclipped = plane[pos] < clipval;
            sum += (unclipped) ? plane[pos] * weights[y+2][x+2] : 0.0f;
            pix += (unclipped) ? weights[y+2][x+2] : 0.0f;
          }
        }
        const float av = sum / fmaxf(1.0f, pix);
        if(av > 0.25f * clipval)
        {
          seg->val1[id] = fminf(clipval, sum / fmaxf(1.0f, pix));
          seg->val2[id] = refavg[testref];
          seg->ref[id] = testref;
        }
      }
    }
  }
}

static inline float _calc_refavg(const float *in, const uint8_t(*const xtrans)[6], const uint32_t filters, const int row, const int col, const dt_iop_roi_t *const roi, const gboolean linear)
{
  const int color = (filters == 9u) ? FCxtrans(row, col, roi, xtrans) : FC(row, col, filters);
  dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f };
  for(int dy = -1; dy < 2; dy++)
  {
    for(int dx = -1; dx < 2; dx++)
    {
      const float val = in[(ssize_t)dy * roi->width + dx];
      const int c = (filters == 9u) ? FCxtrans(row + dy, col + dx, roi, xtrans) : FC(row + dy, col + dx, filters);
      mean[c] += val;
      cnt[c] += 1.0f;
    }
  }
  for(int c = 0; c < 3; c++) mean[c] = powf(mean[c] / cnt[c], 1.0f / 3.0f);

  const dt_aligned_pixel_t croot_refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};
  return (linear) ? powf(croot_refavg[color], 3.0f) : croot_refavg[color];
}

static void _process_segmentation(dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                         const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         dt_iop_highlights_data_t *data, const int vmode)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;

  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;

  const float clipval = fmaxf(0.1f, 0.987f * data->clip);
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t cube_coeffs = { powf(clips[0], 1.0f / 3.0f), powf(clips[1], 1.0f / 3.0f), powf(clips[2], 1.0f / 3.0f)};

  const int combining = (int) data->combine;

  const int pwidth  = dt_round_size(roi_in->width / 3, 2) + 2 * HL_BORDER;
  const int pheight = dt_round_size(roi_in->height / 3, 2) + 2 * HL_BORDER;
  const size_t p_size = dt_round_size((size_t) (pwidth + 4) * (pheight + 4), 16);
  const size_t p_off  = (HL_BORDER * pwidth) + HL_BORDER;

  float *fbuffer = dt_alloc_align_float((HL_FLOAT_PLANES) * p_size);
  if(!fbuffer) return;

  float *plane[HL_FLOAT_PLANES];
  for(int i = 0; i < HL_FLOAT_PLANES; i++)
    plane[i] = fbuffer + i * p_size;

  float *refavg[HL_RGB_PLANES];
  for(int i = 0; i < HL_RGB_PLANES; i++)
    refavg[i] = plane[HL_SEGMENT_PLANES + i];

  dt_iop_segmentation_t isegments[HL_SEGMENT_PLANES];

  const int segmentation_limit = roi_out->width * roi_out->height / 4000; // segments per mpix

  for(int i = 0; i < HL_SEGMENT_PLANES; i++)
    dt_segmentation_init_struct(&isegments[i], pwidth, pheight, HL_BORDER, segmentation_limit);

#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(ivoid, ovoid, roi_in, roi_out, plane, isegments, cube_coeffs, refavg, xtrans) \
  dt_omp_sharedconst(p_off, pwidth, filters) \
  schedule(static)
#endif
  for(size_t row = 1; row < roi_out->height-1; row++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * row + 1;
    float *in = (float *)ivoid + (size_t)roi_in->width * row + 1;
    for(size_t col = 1; col < roi_out->width-1; col++)
    {
      // calc all color planes for the centre of a 3x3 area
      if((col % 3 == 1) && (row % 3 == 1))
      {
        dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
        dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f };
        for(int dy = -1; dy < 2; dy++)
        {
          for(int dx = -1; dx < 2; dx++)
          {
            const float val = in[(ssize_t)dy * roi_in->width + dx];
            const int c = (filters == 9u) ? FCxtrans(row + dy, col + dx, roi_in, xtrans) : FC(row + dy, col + dx, filters);
            mean[c] += val;
            cnt[c] += 1.0f;
          }
        }

        for(int c = 0; c < 3; c++) mean[c] = powf(mean[c] / cnt[c], 1.0f / 3.0f);
        const dt_aligned_pixel_t cube_refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};

        const size_t o = p_off + (row/3) * pwidth + (col/3);
        int allclipped = 0;
        for(int c = 0; c < 3; c++)
        {
          plane[c][o] = mean[c];
          refavg[c][o] = cube_refavg[c];
          if(mean[c] > cube_coeffs[c])
          { 
            allclipped += 1;
            isegments[c].data[o] = 1;
          }
        }
        isegments[3].data[o] = (allclipped == 3) ? 1 : 0;
      }
      out++;
      in++;
    }
  }

  for(int i = 0; i < HL_RGB_PLANES; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HL_BORDER);

  for(int p = 0; p < HL_RGB_PLANES; p++)
  {
    // We prefer to have slightly wider segment borders for a possibly better chosen candidate
    if(combining > 0)
    {
      dt_segments_transform_dilate(&isegments[p], combining);
      if(combining > 1)
        dt_segments_transform_erode(&isegments[p], combining-1);
    }
  }
  if(dt_get_num_threads() >= HL_RGB_PLANES)
  {
#ifdef _OPENMP
  #pragma omp parallel num_threads(HL_RGB_PLANES)
#endif
    {
      dt_segmentize_plane(&isegments[dt_get_thread_num()]);
    }
  }
  else
  {
    for(int p = 0; p < HL_RGB_PLANES; p++)
      dt_segmentize_plane(&isegments[p]);
  }

  for(int p = 0; p < HL_RGB_PLANES; p++)
    _calc_plane_candidates(plane[p], refavg[p], &isegments[p], cube_coeffs[p], data->candidating);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, ivoid, ovoid, roi_in, roi_out, xtrans, isegments) \
  dt_omp_sharedconst(filters, p_off, pwidth) \
  schedule(static)
#endif
  for(int row = 1; row < roi_out->height-1; row++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * row + 1;
    float *in = (float *)ivoid + (size_t)roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width-1; col++)
    {
      const float inval = fmaxf(0.0f, in[0]);
      const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
      if(inval > clips[color])
      {
        const size_t o = p_off + (row/3) * pwidth + (col/3);
        const int pid = _get_segment_id(&isegments[color], o);
        const float candidate = isegments[color].val1[pid];
        if((pid > 1) && (pid < isegments[color].nr+2) && (candidate != 0.0f))
        {
          const float cand_reference = isegments[color].val2[pid];
          const float refavg_here = _calc_refavg(&in[0], xtrans, filters, row, col, roi_in, FALSE);
          const float oval = powf(refavg_here + candidate - cand_reference, 3.0f);
          out[0] = fmaxf(inval, oval);
        }
      }
      out++;
      in++;
    }
  }

  if((vmode != DT_SEGMENTS_MASK_OFF) && fullpipe)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, ivoid, ovoid, roi_in, roi_out, xtrans, isegments) \
  dt_omp_sharedconst(filters, p_off, pwidth, vmode) \
  schedule(static)
#endif
    for(int row = 1; row < roi_out->height-1; row++)
    {
      float *out = (float *)ovoid + (size_t)roi_out->width * row + 1;
      float *in = (float *)ivoid + (size_t)roi_in->width * row + 1;
      for(int col = 1; col < roi_out->width-1; col++)
      {
        const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
        const size_t ppos = p_off + (row/3) * pwidth + (col/3);

        const int pid = _get_segment_id(&isegments[color], ppos);
        const gboolean iclipped = (in[0] >= clips[color]);
        const gboolean isegment = ((pid > 1) && (pid <= isegments[color].nr));
        const gboolean badseg = isegment && (isegments[color].ref[pid] == 0);

        out[0] = 0.1f * in[0];
        if((vmode == DT_SEGMENTS_MASK_COMBINE) && isegment && !iclipped)        out[0] = 1.0f;
        else if((vmode == DT_SEGMENTS_MASK_CANDIDATING) && isegment && !badseg) out[0] = 1.0f;
//        else if(vmode == DT_SEGMENTS_MASK_STRENGTH)                            out[o] += gradient[i];
        out++;
        in++;
      }
    }
  }

//  fprintf(stderr, "[segmentation report]%6.1fMpix, segments: %4i red, %4i green, %4i blue, %4i all.\n",
//       (float) (roi_in->width * roi_in->height) / 1.0e6f, isegments[0].nr, isegments[1].nr, isegments[2].nr, isegments[3].nr);

  for(int i = 0; i < HL_SEGMENT_PLANES; i++)
    dt_segmentation_free_struct(&isegments[i]);
  dt_free_align(fbuffer);
}

