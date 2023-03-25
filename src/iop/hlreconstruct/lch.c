/*
   This file is part of darktable,
   Copyright (C) 2010-2023 darktable developers.

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
 * these 2 constants were computed using following Sage code:
 *
 * sqrt3 = sqrt(3)
 * sqrt12 = sqrt(12) # 2*sqrt(3)
 *
 * print 'sqrt3 = ', sqrt3, ' ~= ', RealField(128)(sqrt3)
 * print 'sqrt12 = ', sqrt12, ' ~= ', RealField(128)(sqrt12)
 */
#define SQRT3 1.7320508075688772935274463415058723669L
#define SQRT12 3.4641016151377545870548926830117447339L // 2*SQRT3

static void process_lch_bayer(dt_iop_module_t *self,
                              dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid,
                              void *const ovoid,
                              const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out,
                              const float clip)
{
  const uint32_t filters = piece->pipe->dsc.filters;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, filters, ivoid, ovoid, roi_out) \
  schedule(static) collapse(2)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      float *const out = (float *)ovoid + (size_t)roi_out->width * j + i;
      const float *const in = (float *)ivoid + (size_t)roi_out->width * j + i;

      if(i == roi_out->width - 1 || j == roi_out->height - 1)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        int clipped = 0;

        // sample 1 bayer block. thus we will have 2 green values.
        float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
        for(int jj = 0; jj <= 1; jj++)
        {
          for(int ii = 0; ii <= 1; ii++)
          {
            const float val = in[(size_t)jj * roi_out->width + ii];

            clipped = (clipped || (val > clip));

            const int c = FC(j + jj + roi_out->y, i + ii + roi_out->x, filters);
            switch(c)
            {
              case 0:
                R = val;
                break;
              case 1:
                Gmin = MIN(Gmin, val);
                Gmax = MAX(Gmax, val);
                break;
              case 2:
                B = val;
                break;
            }
          }
        }

        if(clipped)
        {
          const float Ro = MIN(R, clip);
          const float Go = MIN(Gmin, clip);
          const float Bo = MIN(B, clip);

          const float L = (R + Gmax + B) / 3.0f;

          float C = SQRT3 * (R - Gmax);
          float H = 2.0f * B - Gmax - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != Gmax && Gmax != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          /*
           * backtransform proof, sage:
           *
           * R,G,B,L,C,H = var('R,G,B,L,C,H')
           * solve([L==(R+G+B)/3, C==sqrt(3)*(R-G), H==2*B-G-R], R, G, B)
           *
           * result:
           * [[R == 1/6*sqrt(3)*C - 1/6*H + L, G == -1/6*sqrt(3)*C - 1/6*H + L, B == 1/3*H + L]]
           */
          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FC(j + roi_out->y, i + roi_out->x, filters)];
        }
        else
        {
          out[0] = in[0];
        }
      }
    }
  }
}

static void process_lch_xtrans(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               const void *const ivoid,
                               void *const ovoid,
                               const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out,
                               const float clip)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(clip, ivoid, ovoid, roi_in, roi_out, xtrans) \
  schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = (float *)ovoid + (size_t)roi_out->width * j;
    float *in = (float *)ivoid + (size_t)roi_in->width * j;

    // bit vector used as ring buffer to remember clipping of current
    // and last two columns, checking current pixel and its vertical
    // neighbors
    int cl = 0;

    for(int i = 0; i < roi_out->width; i++)
    {
      // update clipping ring buffer
      cl = (cl << 1) & 6;
      if(j >= 2 && j <= roi_out->height - 3)
      {
        cl |= (in[-roi_in->width] > clip) | (in[0] > clip) | (in[roi_in->width] > clip);
      }

      if(i < 2 || i > roi_out->width - 3 || j < 2 || j > roi_out->height - 3)
      {
        // fast path for border
        out[0] = MIN(clip, in[0]);
      }
      else
      {
        // if current pixel is clipped, always reconstruct
        int clipped = (in[0] > clip);
        if(!clipped)
        {
          clipped = cl;
          if(clipped)
          {
            // If the ring buffer can't show we are in an obviously
            // unclipped region, this is the slow case: check if there
            // is any 3x3 block touching the current pixel which has
            // no clipping, as then don't need to reconstruct the
            // current pixel. This avoids zippering in edge
            // transitions from clipped to unclipped areas. The
            // X-Trans sensor seems prone to this, unlike Bayer, due
            // to its irregular pattern.
            for(int offset_j = -2; offset_j <= 0; offset_j++)
            {
              for(int offset_i = -2; offset_i <= 0; offset_i++)
              {
                if(clipped)
                {
                  clipped = 0;
                  for(int jj = offset_j; jj <= offset_j + 2; jj++)
                  {
                    for(int ii = offset_i; ii <= offset_i + 2; ii++)
                    {
                      const float val = in[(ssize_t)jj * roi_in->width + ii];
                      clipped = (clipped || (val > clip));
                    }
                  }
                }
              }
            }
          }
        }

        if(clipped)
        {
          dt_aligned_pixel_t mean = { 0.0f, 0.0f, 0.0f };
          dt_aligned_pixel_t RGBmax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
          int cnt[3] = { 0, 0, 0 };

          for(int jj = -1; jj <= 1; jj++)
          {
            for(int ii = -1; ii <= 1; ii++)
            {
              const float val = in[(ssize_t)jj * roi_in->width + ii];
              const int c = FCxtrans(j+jj, i+ii, roi_in, xtrans);
              mean[c] += val;
              cnt[c]++;
              RGBmax[c] = MAX(RGBmax[c], val);
            }
          }

          const float Ro = MIN(mean[0]/cnt[0], clip);
          const float Go = MIN(mean[1]/cnt[1], clip);
          const float Bo = MIN(mean[2]/cnt[2], clip);

          const float R = RGBmax[0];
          const float G = RGBmax[1];
          const float B = RGBmax[2];

          const float L = (R + G + B) / 3.0f;

          float C = SQRT3 * (R - G);
          float H = 2.0f * B - G - R;

          const float Co = SQRT3 * (Ro - Go);
          const float Ho = 2.0f * Bo - Go - Ro;

          if(R != G && G != B)
          {
            const float ratio = sqrtf((Co * Co + Ho * Ho) / (C * C + H * H));
            C *= ratio;
            H *= ratio;
          }

          dt_aligned_pixel_t RGB = { 0.0f, 0.0f, 0.0f };

          RGB[0] = L - H / 6.0f + C / SQRT12;
          RGB[1] = L - H / 6.0f - C / SQRT12;
          RGB[2] = L + H / 3.0f;

          out[0] = RGB[FCxtrans(j, i, roi_out, xtrans)];
        }
        else
          out[0] = in[0];
      }
      out++;
      in++;
    }
  }
}

#undef SQRT3
#undef SQRT12

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
