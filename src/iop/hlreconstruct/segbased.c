/*
    This file is part of darktable,
    Copyright (C) 2022-23 darktable developers.

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

/* Rebuild algorithm
  In areas with all planes clipped we try to reconstruct (hopefully a good guess) data based on the border gradients and the
  segment's size - here we use a distance transformation.
  What do we need to do so?
  1. We need a "luminance" plane, we use Y0 for this.
  2. We have an additional mask holding information about all-channels-clipped
  3. Based on the Y0 data and all-clipped info we prepare a gradient plane.
  4. We also do a segmentation for the all-clipped data.

  After this preparation steps we reconstruct data for every segment.
  1. Calculate average gradients in an iterative loop for every distance value.
     The new gradients calculation uses the distance and averaged gradients of the last iterative step.
     By doing so we avoid direction problems.
  2. Do a box-blur to suppress ridges, the radius depends on segment size.
  3. Possibly add some noise.
  4. Do a sigmoid correction suppressing artefacts at the borders.
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

#define HL_POWERF 3.0f

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
      (sqrf(p[-w2-2]-av) + sqrf(p[-w2-1]-av) + sqrf(p[-w2]-av) + sqrf(p[-w2+1]-av) + sqrf(p[-w2+2]-av) +
       sqrf(p[-w-2]-av)  + sqrf(p[-w-1]-av)  + sqrf(p[-w]-av)  + sqrf(p[-w+1]-av)  + sqrf(p[-w+2]-av) +
       sqrf(p[-2]-av)    + sqrf(p[-1]-av)    + sqrf(p[0]-av)   + sqrf(p[1]-av)     + sqrf(p[2]-av) +
       sqrf(p[w-2]-av)   + sqrf(p[w-1]-av)   + sqrf(p[w]-av)   + sqrf(p[w+1]-av)   + sqrf(p[w+2]-av) +
       sqrf(p[w2-2]-av)  + sqrf(p[w2-1]-av)  + sqrf(p[w2]-av)  + sqrf(p[w2+1]-av)  + sqrf(p[w2+2]-av)));
}

static float _calc_weight(const float *s, const size_t loc, const int w, const float clipval)
{
  const float smoothness = fmaxf(0.0f, 1.0f - 10.0f * powf(_local_std_deviation(&s[loc], w), 0.5f));
  float val = 0.0f;
  for(int y = -1; y < 2; y++)
  {
    for(int x = -1; x < 2; x++)
      val += s[loc + y*w + x] / 9.0f;
  }
  const float sval = fmaxf(1.0f, powf(fminf(clipval, val) / clipval, 2.0f));
  return sval * smoothness;
}

static void _calc_plane_candidates(const float *plane,
                                   const float *refavg,
                                   dt_iop_segmentation_t *seg,
                                   const float clipval,
                                   const float badlevel)
{
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(plane, refavg, seg, clipval, badlevel) \
  schedule(dynamic)
#endif
  for(int id = 2; id < seg->nr; id++)
  {
    seg->val1[id] = 0.0f;
    seg->val2[id] = 0.0f;
    seg->ref[id] = 0;
    // avoid very small segments
    if((seg->ymax[id] - seg->ymin[id] > 2) && (seg->xmax[id] - seg->xmin[id] > 2))
    {
      size_t testref = 0;
      float testweight = 0.0f;
      // make sure we don't calc a candidate from duplicated border data
      for(int row = MAX(seg->border+2, seg->ymin[id]-2); row < MIN(seg->height - seg->border-2, seg->ymax[id]+3); row++)
      {
        for(int col = MAX(seg->border+2, seg->xmin[id]-2); col < MIN(seg->width - seg->border-2, seg->xmax[id]+3); col++)
        {
          const size_t pos = row * seg->width + col;
          const int sid = _get_segment_id(seg, pos);
          if((sid == id) && (plane[pos] < clipval))
          {
            const float wht = _calc_weight(plane, pos, seg->width, clipval) * ((seg->data[pos] & DT_SEG_ID_MASK) ? 1.0f : 0.75f);
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
        if(av > 0.125f * clipval)
        {
          seg->val1[id] = fminf(clipval, sum / fmaxf(1.0f, pix));
          seg->val2[id] = refavg[testref];
          seg->ref[id] = testref;
        }
      }
    }
  }
}

static inline float _calc_refavg(const float *in,
                                 const uint8_t(*const xtrans)[6],
                                 const uint32_t filters,
                                 const int row,
                                 const int col,
                                 const dt_iop_roi_t *const roi,
                                 const gboolean linear)
{
  const int color = (filters == 9u) ? FCxtrans(row, col, roi, xtrans) : FC(row, col, filters);
  dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };

  const int dymin = (row > 0) ? -1 : 0;
  const int dxmin = (col > 0) ? -1 : 0;
  const int dymax = (row < roi->height -1) ? 2 : 1;
  const int dxmax = (col < roi->width -1) ? 2 : 1;

  for(int dy = dymin; dy < dymax; dy++)
  {
    for(int dx = dxmin; dx < dxmax; dx++)
    {
      const float val = fmaxf(0.0f, in[(ssize_t)dy * roi->width + dx]);
      const int c = (filters == 9u) ? FCxtrans(row + dy, col + dx, roi, xtrans) : FC(row + dy, col + dx, filters);
      mean[c] += val;
      cnt[c] += 1.0f;
    }
  }
  for_each_channel(c)
    mean[c] = (cnt[c] > 0.0f) ? powf(mean[c] / cnt[c], 1.0f / HL_POWERF) : 0.0f;

  const dt_aligned_pixel_t croot_refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};
  return (linear) ? powf(croot_refavg[color], HL_POWERF) : croot_refavg[color];
}

static void _initial_gradients(const size_t w,
                               const size_t height,
                               float *luminance,
                               float *distance,
                               float *gradient)
{
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(luminance, gradient, distance, w, height) \
  schedule(static) collapse(2)
#endif
  for(size_t row = HL_BORDER + 2; row < height - HL_BORDER - 2; row++)
  {
    for(size_t col = HL_BORDER + 2; col < w - HL_BORDER - 2; col++)
    {
      const size_t v = row * w + col;
      float g = 0.0f;
      if((distance[v] > 0.0f) && (distance[v] < 2.0f))
      {
        // scharr operator
        const float gx = 47.0f * (luminance[v-w-1] - luminance[v-w+1])
                      + 162.0f * (luminance[v-1]   - luminance[v+1])
                       + 47.0f * (luminance[v+w-1] - luminance[v+w+1]);
        const float gy = 47.0f * (luminance[v-w-1] - luminance[v+w-1])
                      + 162.0f * (luminance[v-w]   - luminance[v+w])
                       + 47.0f * (luminance[v-w+1] - luminance[v+w+1]);
        g = 4.0f * sqrtf(sqrf(gx / 256.0f) + sqrf(gy / 256.0f));
      }
      gradient[v] = g;
    }
  }
}

static float _segment_maxdistance(const int width,
                                  const int height,
                                  float *distance,
                                  dt_iop_segmentation_t *seg,
                                  const int id)
{
  const int xmin = MAX(seg->xmin[id]-2, HL_BORDER);
  const int xmax = MIN(seg->xmax[id]+3, width - HL_BORDER);
  const int ymin = MAX(seg->ymin[id]-2, HL_BORDER);
  const int ymax = MIN(seg->ymax[id]+3, height - HL_BORDER);
  float max_distance = 0.0f;

#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction(max : max_distance) \
  dt_omp_firstprivate(distance, seg, width, xmin, xmax, ymin, ymax, id) \
  schedule(static) collapse(2)
#endif
  for(size_t row = ymin; row < ymax; row++)
  {
    for(size_t col = xmin; col < xmax; col++)
    {
      const size_t v = row * width + col;
      if(id == seg->data[v])
        max_distance = fmaxf(max_distance, distance[v]);
    }
  }
  return max_distance;
}

static float _segment_attenuation(dt_iop_segmentation_t *seg, const int id, const int mode)
{
  const float attenuate[NUM_RECOVERY_MODES] = { 0.0f, 1.7f, 1.0f, 1.7f, 1.0f, 1.0f, 1.0f};
  if(mode < DT_RECOVERY_MODE_ADAPT)
    return attenuate[mode];
  else
  {
    const float maxdist = fmaxf(1.0f, seg->val1[id]);
    return fminf(1.7f,  0.9f + (3.0f / maxdist));
  }
}

static float _segment_correction(dt_iop_segmentation_t *seg,
                                 const int id,
                                 const int mode,
                                 const int seg_border)
{
  const float correction = _segment_attenuation(seg, id, mode);
  return correction - 0.1f * (float)seg_border;
}

static void _calc_distance_ring(const int width,
                                const int xmin,
                                const int xmax,
                                const int ymin,
                                const int ymax,
                                float *gradient,
                                float *distance,
                                const float attenuate,
                                const float dist,
                                dt_iop_segmentation_t *seg,
                                const int id)
{
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(distance, gradient, seg, width, xmin, xmax, ymin, ymax, dist, id, attenuate) \
  schedule(static) collapse(2)
#endif
  for(size_t row = ymin; row < ymax; row++)
  {
    for(size_t col = xmin; col < xmax; col++)
    {
      const size_t v = row * width + col;
      const float dv = distance[v];
      if((dv >= dist) && (dv < dist + 1.5f) && (id == seg->data[v]))
      {
        float grd = 0.0f;
        float cnt = 0.0f;
        for(int y = -2; y < 3; y++)
        {
          for(int x = -2; x < 3; x++)
          {
            size_t p = v + x + (width * y);
            const float dd = distance[p];
            if((dd >= dist - 1.5f) && (dd < dist))
            {
              cnt += 1.0f;
              grd += gradient[p];
            }
          }
        }
        if(cnt > 0.0f)
          gradient[v] = fminf(1.5f, (grd / cnt) * (1.0f + 1.0f / powf(distance[v], attenuate)));
      }
    }
  }
}

static void _segment_gradients(const int width,
                               const int height,
                               float *distance,
                               float *gradient,
                               float *tmp,
                               const int mode,
                               dt_iop_segmentation_t *seg,
                               const int id,
                               const int seg_border)
{
  const int xmin = MAX(seg->xmin[id]-1, HL_BORDER);
  const int xmax = MIN(seg->xmax[id]+2, width - HL_BORDER);
  const int ymin = MAX(seg->ymin[id]-1, HL_BORDER);
  const int ymax = MIN(seg->ymax[id]+2, height - HL_BORDER);
  const float attenuate = _segment_attenuation(seg, id, mode);
  const float strength = _segment_correction(seg, id, mode, seg_border);

  float maxdist = 1.5f;
  while(maxdist < seg->val1[id])
  {
    _calc_distance_ring(width, xmin, xmax, ymin, ymax, gradient, distance, attenuate, maxdist, seg, id);
    maxdist += 1.5f;
  }

  if(maxdist > 4.0f)
  {
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(gradient, tmp, width, xmin, xmax, ymin, ymax) \
  schedule(static)
#endif
    for(size_t row = ymin; row < ymax; row++)
    {
      for(size_t col = xmin, s = row*width + col, d = (row-ymin)*(xmax-xmin); col < xmax; col++, s++, d++)
        tmp[d] = gradient[s];
    }

    dt_box_mean(tmp, ymax-ymin, xmax-xmin, 1, MIN((int)maxdist, 15), 2);
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(gradient, tmp, distance, seg, width, xmin, xmax, ymin, ymax, id) \
  schedule(static)
#endif
    for(size_t row = ymin; row < ymax; row++)
    {
      for(size_t col = xmin, v = row * width + col, s = (row-ymin)*(xmax-xmin); col < xmax; col++, v++, s++)
      {
        if(id == seg->data[v])
          gradient[v] = tmp[s];
      }
    }
  }
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(gradient, tmp, distance, seg, width, xmin, xmax, ymin, ymax, id, strength) \
  schedule(static) collapse(2)
#endif
  for(size_t row = ymin; row < ymax; row++)
  {
    for(size_t col = xmin; col < xmax; col++)
    {
      const size_t v = row * width + col;
      if(id == seg->data[v])
        gradient[v] *= strength;
    }
  }
}

static void _add_poisson_noise(const int width,
                               const int height,
                               float *lum,
                               dt_iop_segmentation_t *seg,
                               const int id,
                               const float noise_level)
{
  const int xmin = MAX(seg->xmin[id], HL_BORDER);
  const int xmax = MIN(seg->xmax[id]+1, width - HL_BORDER);
  const int ymin = MAX(seg->ymin[id], HL_BORDER);
  const int ymax = MIN(seg->ymax[id]+1, height - HL_BORDER);
  uint32_t DT_ALIGNED_ARRAY state[4] = { splitmix32(ymin), splitmix32(xmin), splitmix32(1337), splitmix32(666) };
  xoshiro128plus(state);
  xoshiro128plus(state);
  xoshiro128plus(state);
  xoshiro128plus(state);
  for(size_t row = ymin; row < ymax; row++)
  {
    for(size_t col = xmin; col < xmax; col++)
    {
      const size_t v = row * width + col;
      if(seg->data[v] == id)
      {
        const float pnoise = poisson_noise(lum[v] * noise_level, noise_level, col & 1, state);
        lum[v] += pnoise;
      }
    }
  }
}

static inline size_t _raw_to_plane(const int width, const int row, const int col)
{
  return (HL_BORDER + (row / 3)) * width + (col / 3) + HL_BORDER;
}

static void _process_segmentation(dt_dev_pixelpipe_iop_t *piece,
                                  const float *const input,
                                  float *const output,
                                  const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out,
                                  dt_iop_highlights_data_t *data,
                                  const int vmode,
                                  float *tmpout)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;
  const uint32_t filters = piece->pipe->dsc.filters;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const float clipval = fmaxf(0.1f, 0.987f * data->clip);
  const dt_aligned_pixel_t icoeffs = { piece->pipe->dsc.temperature.coeffs[0], piece->pipe->dsc.temperature.coeffs[1], piece->pipe->dsc.temperature.coeffs[2]};
  const dt_aligned_pixel_t clips = { clipval * icoeffs[0], clipval * icoeffs[1], clipval * icoeffs[2]}; 
  const dt_aligned_pixel_t cube_coeffs = { powf(clips[0], 1.0f / HL_POWERF), powf(clips[1], 1.0f / HL_POWERF), powf(clips[2], 1.0f / HL_POWERF)};

  const int combining = MAX(1, (int) data->combine);
  const int recovery_mode = data->recovery;
  const float strength = data->strength;

  const int recovery_closing[NUM_RECOVERY_MODES] = { 0, 0, 0, 2, 2, 0, 2};
  const int seg_border = recovery_closing[recovery_mode];
  const int segmentation_limit = (piece->pipe->iwidth * piece->pipe->iheight) * sqrf(piece->pipe->iscale) / 4000; // 250 segments per mpix

  const size_t pwidth  = dt_round_size(roi_in->width / 3, 2) + 2 * HL_BORDER;
  const size_t pheight = dt_round_size(roi_in->height / 3, 2) + 2 * HL_BORDER;
  const size_t p_size =  dt_round_size((size_t) pwidth * pheight, 64);

  float *fbuffer = dt_alloc_align_float((HL_FLOAT_PLANES) * p_size);
  if(!fbuffer) return;

  float *plane[HL_FLOAT_PLANES];
  for(int i = 0; i < HL_FLOAT_PLANES; i++)
    plane[i] = fbuffer + i * p_size;

  float *refavg[HL_RGB_PLANES];
  for(int i = 0; i < HL_RGB_PLANES; i++)
    refavg[i] = plane[HL_SEGMENT_PLANES + i];

  dt_iop_segmentation_t isegments[HL_SEGMENT_PLANES];
  for(int i = 0; i < HL_SEGMENT_PLANES; i++)
    dt_segmentation_init_struct(&isegments[i], pwidth, pheight, HL_BORDER +1, segmentation_limit);

  const int xshifter = ((filters != 9u) && (FC(0, 0, filters) == 1)) ? 1 : 2;

  size_t anyclipped = 0;
  gboolean has_allclipped = FALSE;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  reduction( | : has_allclipped) \
  reduction( + : anyclipped) \
  dt_omp_firstprivate(tmpout, roi_in, plane, isegments, cube_coeffs, refavg, xtrans, pwidth, filters, xshifter) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 1; row < roi_in->height-1; row++)
  {
    for(size_t col = 1; col < roi_in->width - 1; col++)
    {
      // calc all color planes in a 3x3 area. For chroma noise stability in bayer sensors we make sure
      // to align the box with a green photosite in centre so we always have a 5:2:2 ratio
      if((col % 3 == xshifter) && (row % 3 == 1))
      {
        dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f, 0.0f };
        dt_aligned_pixel_t cnt = { 0.0f, 0.0f, 0.0f, 0.0f };
        for(int dy = -1; dy < 2; dy++)
        {
          for(int dx = -1; dx < 2; dx++)
          {
            const size_t idx = (row + dy) * roi_in->width + col + dx;
            const float val = tmpout[idx];
            const int c = (filters == 9u) ? FCxtrans(row + dy, col + dx, roi_in, xtrans) : FC(row + dy, col + dx, filters);
            mean[c] += val;
            cnt[c] += 1.0f;
          }
        }

        for_each_channel(c)
          mean[c] = (cnt[c] > 0.0f) ? powf(mean[c] / cnt[c], 1.0f / HL_POWERF) : 0.0f;
        const dt_aligned_pixel_t cube_refavg = { 0.5f * (mean[1] + mean[2]), 0.5f * (mean[0] + mean[2]), 0.5f * (mean[0] + mean[1])};

        const size_t o = _raw_to_plane(pwidth, row, col);
        int allclipped = 0;
        for_three_channels(c)
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
        has_allclipped |= (allclipped == 3) ? TRUE : FALSE;
        anyclipped += allclipped;
      }
    }
  }

  if((anyclipped < 20) && vmode == DT_HIGHLIGHTS_MASK_OFF)
    goto finish; 

  for(int i = 0; i < HL_RGB_PLANES; i++)
    dt_masks_extend_border(plane[i], pwidth, pheight, HL_BORDER);

  for(int p = 0; p < HL_RGB_PLANES; p++)
  {
    // We prefer to have slightly wider segment borders for a possibly better chosen candidate
    dt_segments_transform_dilate(&isegments[p], combining);
    if(combining > 1)
      dt_segments_transform_erode(&isegments[p], combining-1);
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
  dt_omp_firstprivate(clips, input, tmpout, roi_in, xtrans, isegments, plane, filters, pwidth) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 1; row < roi_in->height-1; row++)
  {
    for(size_t col = 1; col < roi_in->width - 1; col++)
    {
      const size_t idx = row * roi_in->width + col;
      const float inval = fmaxf(0.0f, input[idx]);
      const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
      if(inval > clips[color])
      {
        const size_t o = _raw_to_plane(pwidth, row, col);
        const int pid = _get_segment_id(&isegments[color], o);
        if((pid > 1) && (pid < isegments[color].nr))
        {
          const float candidate = isegments[color].val1[pid];
          if(candidate != 0.0f)
          {
            const float cand_reference = isegments[color].val2[pid];
            const float refavg_here = _calc_refavg(&input[idx], xtrans, filters, row, col, roi_in, FALSE);
            const float oval = powf(refavg_here + candidate - cand_reference, HL_POWERF);
            tmpout[idx] = plane[color][o] = fmaxf(inval, oval);
          }
        }
      }
    }
  }

  float *restrict distance  = plane[HL_RGB_PLANES];
  float *restrict gradient  = plane[HL_RGB_PLANES + 1];
  float *restrict luminance = plane[HL_RGB_PLANES + 2];
  float *restrict recout    = plane[HL_RGB_PLANES + 3];
  float *restrict tmp       = plane[HL_RGB_PLANES + 4];

  const gboolean do_recovery = (recovery_mode != DT_RECOVERY_MODE_OFF) && has_allclipped && (strength > 0.0f);
  const gboolean do_masking = (vmode != DT_HIGHLIGHTS_MASK_OFF) && fullpipe;

  if(do_recovery || do_masking)
  {
    dt_segments_transform_closing(&isegments[3], seg_border);
    dt_iop_image_fill(gradient, fminf(1.0f, 5.0f * strength), pwidth, pheight, 1);
    dt_iop_image_fill(distance, 0.0f, pwidth, pheight, 1);
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
  dt_omp_firstprivate(tmp, plane, distance, isegments, icoeffs, pheight, pwidth) \
  schedule(static)
#endif
    for(size_t row = HL_BORDER + 1; row < pheight - HL_BORDER - 1; row++)
    {
      for(size_t col = HL_BORDER + 1; col < pwidth - HL_BORDER - 1; col++)
      {
        const size_t i = row * pwidth + col;
        // prepare the temporary luminance for later blurring and also prefill the distance plane
        tmp[i] = (plane[0][i] * icoeffs[0] + plane[1][i] * icoeffs[1] + plane[2][i] * icoeffs[2]) / 3.0f;
        distance[i] = (isegments[3].data[i] == 1) ? DT_DISTANCE_TRANSFORM_MAX : 0.0f;
      }
    }
    dt_masks_extend_border(tmp, pwidth, pheight, HL_BORDER+1);
    dt_masks_blur_fast(tmp, luminance, pwidth, pheight, 1.2f, 1.0f, 20.0f);
    dt_masks_extend_border(luminance, pwidth, pheight, HL_BORDER+1);
  }

  if(do_recovery)
  {
    const float max_distance = dt_image_distance_transform(NULL, distance, pwidth, pheight, 1.0f, DT_DISTANCE_TRANSFORM_NONE);
    if(max_distance > 3.0f)
    {
      dt_segmentize_plane(&isegments[3]);
      _initial_gradients(pwidth, pheight, luminance, distance, recout);
      dt_masks_extend_border(recout, pwidth, pheight, HL_BORDER+1);

      // now we check for significant all-clipped-segments and reconstruct data
      for(int id = 2; id < isegments[3].nr; id++)
      {
        const float seg_dist = _segment_maxdistance(pwidth, pheight, distance, &isegments[3], id);
        isegments[3].val1[id] = seg_dist;

        if(isegments[3].val1[id] > 2.0f)
          _segment_gradients(pwidth, pheight, distance, recout, tmp, recovery_mode, &isegments[3], id, seg_border);
      }

      dt_masks_blur_fast(recout, gradient, pwidth, pheight, 1.2f, 1.0f, 20.0f);
      // possibly add some noise
      const float noise_level = data->noise_level;
      if(noise_level > 0.0f)
      {
        for(int id = 2; id < isegments[3].nr; id++)
        {
          if(isegments[3].val1[id] > 3.0f)
            _add_poisson_noise(pwidth, pheight, gradient, &isegments[3], id, noise_level);
        }
      }
      const float dshift = 2.0f + (float)recovery_closing[recovery_mode];

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, input, tmpout, roi_in, xtrans, gradient, distance, filters, pwidth, dshift, strength) \
  schedule(static) collapse(2)
#endif
      for(size_t row = 1; row < roi_in->height-1; row++)
      {
        for(size_t col = 1; col < roi_in->width - 1; col++)
        {
          const size_t idx = row * roi_in->width + col;
          const int color = (filters == 9u) ? FCxtrans(row, col, roi_in, xtrans) : FC(row, col, filters);
          if(fmaxf(0.0f, input[idx]) > clips[color])
          {
            const size_t o = _raw_to_plane(pwidth, row, col);
            const float effect = strength / (1.0f + expf(-(distance[o] - dshift)));
            tmpout[idx]+= fmaxf(0.0f, gradient[o] * effect);
          }
        }
      }
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clips, output, tmpout, roi_in, roi_out, xtrans, isegments, gradient, filters, pwidth, vmode, strength, do_masking) \
  schedule(static) collapse(2)
#endif
  for(size_t row = 0; row < roi_out->height; row++)
  {
    for(size_t col = 0; col < roi_out->width; col++)
    {
      const size_t odx = row * roi_out->width + col;
      const size_t inrow = row + roi_out->y;
      const size_t incol = col + roi_out->x;
      const size_t idx = inrow * roi_in->width + incol;
      if((inrow < roi_in->height) && (incol < roi_in->width))
      {
        output[odx] = tmpout[idx];
        if(do_masking)
        {
          output[odx] = 0.1f * fmaxf(0.0f, output[odx]);
          const gboolean inrefs = (inrow > 0) && (incol > 0) && (inrow < roi_in->height-1) && (incol < roi_in->width-1);
          if(inrefs)
          {
            const int color = (filters == 9u) ? FCxtrans(inrow, incol, roi_in, xtrans) : FC(inrow, incol, filters);
            const size_t ppos = _raw_to_plane(pwidth, inrow, incol);
            const int pid = _get_segment_id(&isegments[color], ppos);
            const gboolean isegment = ((pid > 1) && (pid < isegments[color].nr));
            const gboolean goodseg = isegment && (isegments[color].val1[pid] != 0.0f);
            const int allid = _get_segment_id(&isegments[3], ppos);
            const gboolean allseg = ((allid > 1) && (allid < isegments[3].nr));
            if((vmode == DT_HIGHLIGHTS_MASK_COMBINE) && isegment)
              output[odx] = (isegments[color].data[ppos] & DT_SEG_ID_MASK) ? 1.0f : 0.6f;
            else if((vmode == DT_HIGHLIGHTS_MASK_CANDIDATING) && goodseg)
              output[odx] = (ppos == isegments[color].ref[pid]) ? 1.0f : 0.6f;
            else if((vmode == DT_HIGHLIGHTS_MASK_STRENGTH) && allseg)
              output[odx] += strength * gradient[ppos];
          }
        }
      }
    }
  }

  dt_print(DT_DEBUG_PERF, "[segmentation report %12s] %5.1fMpix, segments: %3i red, %3i green, %3i blue, %3i all, %4i allowed.\n",
      dt_dev_pixelpipe_type_to_str(piece->pipe->type),
      (float) (roi_in->width * roi_in->height) / 1.0e6f, isegments[0].nr -2, isegments[1].nr-2, isegments[2].nr-2, isegments[3].nr-2,
      segmentation_limit-2);

  finish:

  for(int i = 0; i < HL_SEGMENT_PLANES; i++)
    dt_segmentation_free_struct(&isegments[i]);
  dt_free_align(fbuffer);
}

