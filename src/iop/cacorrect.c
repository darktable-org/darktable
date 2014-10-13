/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2014 Stéphane Gimenez

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/linsolve.c"
#include "common/interpolation.h"
#include "develop/imageop.h"
#include "dtgtk/slider.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <stdlib.h>

#define CACORRECT_DEBUG 0

#define CA_SHIFT 8 // max allowed CA shift, must be even

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_cacorrect_params_t)

typedef struct dt_iop_cacorrect_params_t
{
  int type;
}
dt_iop_cacorrect_params_t;

typedef struct dt_iop_cacorrect_gui_data_t
{
  GtkWidget *tcombo;
}
dt_iop_cacorrect_gui_data_t;

typedef struct dt_iop_cacorrect_data_t
{
  int type;
  int degree;
  const struct dt_interpolation *ip;
  void *fitdata;
}
dt_iop_cacorrect_data_t;

typedef struct dt_iop_cacorrect_global_data_t
{
}
dt_iop_cacorrect_global_data_t;

// this returns a translatable name
const char *name()
{
  // make sure you put all your translatable strings into _() !
  return _("chromatic aberrations");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int flags ()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
           dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(float);
}

static int get_degree()
{
  return dt_conf_get_int("plugins/darkroom/cacorrect/degree");
}

static int get_radial_degree()
{
  return dt_conf_get_int("plugins/darkroom/cacorrect/radial_degree");
}

static int get_displayed()
{
  return dt_conf_get_int("plugins/darkroom/cacorrect/display");
}

static int get_visuals()
{
  return dt_conf_get_int("plugins/darkroom/cacorrect/visuals");
}

// not activated but the old dummy value was 50
int
legacy_params(dt_iop_module_t *self,
              const void *const old_params, const int old_version,
              dt_iop_cacorrect_params_t *new_p, const int new_version)
{
  if (old_version == 1 && new_version == 2)
  {
    new_p->type = 0;
    return 0;
  }
  return 1;
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_cacorrect_data_t *d = piece->data;
  if (1 || !d->fitdata)
  {
    // we want all of it
    roi_in->x = 0;
    roi_in->y = 0;
    roi_in->width = piece->pipe->image.width;
    roi_in->height = piece->pipe->image.height;
  }
  else { // decreases performance since the buffer has to be adapted
    const int margin = CA_SHIFT + d->ip->width*2;
    roi_in->width  = roi_out->width + 2*margin;
    roi_in->height = roi_out->height + 2*margin;
    roi_in->x = roi_out->x - margin;
    roi_in->y = roi_out->y - margin;
    if (roi_in->x < 0) { roi_in->width -= roi_in->x; roi_in->x = 0; }
    if (roi_in->y < 0) { roi_in->height -= roi_in->y; roi_in->y = 0; }
    roi_in->width = MIN(piece->pipe->image.width, roi_in->width);
    roi_in->height = MIN(piece->pipe->image.height, roi_in->height);
  }
}

static void
CA_analyse(dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece,
           const float *const in, float *const out,
           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_cacorrect_data_t *d = piece->data;

  const int iwidth  = roi_in->width;
  const int iheight = roi_in->height;
  const int isize = MIN(iwidth, iheight);
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  const int offv = roi_out->y - roi_in->y;
  const int offh = roi_out->x - roi_in->x;

  const uint32_t filters = dt_image_filter(&piece->pipe->image);
  const int ca = (filters & 3) != 1, fb = (filters >> (!ca << 2) & 3) == 2;

  const int sl = CA_SHIFT;
  const int radial = d->type == 1;
  const int deg = d->degree;
  const int degn = lin_size(radial ? 1 : 2, deg);

  const int srad = 22; // size of samples, must be even
  const int subs = srad; // spacing between samples, must be even

  const int TS = (iwidth > 2024 && iheight > 2024) ? 256 : 128;
  const int border = 2 + srad + sl;
  const int ncoeff = radial ? 1 : 2;

  // these small sizes aren't safe to run through the code below.
  if (iheight < 3 * border || iwidth < 3 * border) return;

  int visuals = get_visuals();

#if CACORRECT_DEBUG
  clock_t time_begin = clock();
  clock_t time_start = time_begin;
  clock_t time_end;
#endif

  // initialize fit coefficients
  float fitmat[2][ncoeff][degn*degn];
  float fitvec[2][ncoeff][degn];
  for (int color = 0; color < 2; color++)
    for (int coeff = 0; coeff < ncoeff; coeff++)
      lin_zero(degn, fitmat[color][coeff], fitvec[color][coeff]);

#if CACORRECT_DEBUG
  printf("cacorrect: pre-analysis\n");
#endif

  // pre-analysis (todo: allow a second pass to ditch out the outliers)

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int top = 0; top < iheight; top += TS - 2*border)
    for (int left = 0; left < iwidth; left += TS - 2*border)
    {
      const int rm = top + TS > iheight ? iheight - top : TS;
      const int cm = left + TS > iwidth ? iwidth - left : TS;

      float rgb[TS][TS] __attribute__((aligned(16)));
      // assume gamma = 2.0 (experimental)
      for (int r = 0; r < rm; r++)
      {
        int c = 0;
        const int inr = top + r;
        const int inc = left + c;
        const float *pi = in + (size_t)iwidth*inr + inc;
        float *po = &rgb[r][c];
#if __SSE2__
        for (; c + 3 < cm; pi += 4, po += 4, c += 4)
          _mm_store_ps(po, _mm_sqrt_ps(_mm_loadu_ps(pi)));
#endif
        for (; c < cm; pi += 1, po += 1, c += 1)
          *po = sqrt(*pi);
      }

      float la[TS][TS][2];
      // compute derivatives
      for (int r = 2; r < rm - 2; r++)
        for (int c = 2; c < cm - 2; c++)
          {
            la[r][c][0] = (rgb[r+2][c] - rgb[r-2][c]) /* /4 */;
            la[r][c][1] = (rgb[r][c+2] - rgb[r][c-2]) /* /4 */;
          }

      // thread-local components to be summed up
      float tfitmat[2][ncoeff][degn*degn];
      float tfitvec[2][ncoeff][degn];
      for (int color = 0; color < 2; color++)
        for (int coeff = 0; coeff < ncoeff; coeff++)
          lin_zero(degn, tfitmat[color][coeff], tfitvec[color][coeff]);

      // compute centered offsets for sampling areas
      int rsuboff = (iheight/4 - border/2) % subs < subs/2 ? 0 : subs/2;
      int csuboff = (iwidth/4 - border/2) % subs < subs/2 ? 0 : subs/2;
      int roff = (iheight/4 + rsuboff - (top + border)/2) % subs;
      int coff = (iwidth/4 + csuboff - (left + border)/2) % subs;
      if (roff < 0) roff += subs;
      if (coff < 0) coff += subs;

      for (int r = border + 2*roff; r < rm - border; r += 2*subs)
        for (int c = border + 2*coff; c < cm - border; c += 2*subs)
        {
          int inr = top + r;
          int inc = left + c;
          if (inr < border || inr > iheight - 1 - border ||
              inc < border || inc > iwidth - 1 - border)
            continue;

          const float y = (2*(inr + 0.5) - iheight)/isize;
          const float x = (2*(inc + 0.5) - iwidth)/isize;

          // compute variation map
          float t[2][2*sl+1][2*sl+1];
          for (int color = 0; color < 2; color++)
            for (int i = 0; i < 2*sl+1; i++)
              for (int j = 0; j < 2*sl+1; j++)
                t[color][i][j] = 0.;

          for (int gi = -srad; gi < srad; gi++)
            for (int gj = -srad+((gi+ca)&1); gj < srad; gj += 2)
            {
              const float dgv = la[r+gi][c+gj][0];
              const float dgh = la[r+gi][c+gj][1];
              const int B = (gi+fb)&1;
              const int R = !B;
              for (int di = -sl+1; di <= sl; di += 2)
                for (int dj = -sl; dj <= sl; dj += 2)
                  // this test is too costly if the loop is not unrolled
                  /* if (di*di + dj*dj < (int)((sl+0.5)*(sl+0.5))) */
                {
                  float kvR = la[r+gi+di][c+gj+dj][0] - dgv;
                  float khR = la[r+gi+di][c+gj+dj][1] - dgh;
                  float kvB = la[r+gi+dj][c+gj+di][0] - dgv;
                  float khB = la[r+gi+dj][c+gj+di][1] - dgh;
                  t[R][di+sl][dj+sl] += kvR * kvR + khR * khR;
                  t[B][dj+sl][di+sl] += kvB * kvB + khB * khB;
                }
            }

          float bc[2][2] = { };
          float bw[2][2] = { };
          for (int color = 0; color < 2; color++)
          {
            // compute star averages and locate indexed minimum
            int I = 0;
            int J = 0;
            float min = INFINITY;
            for (int di = -sl+1; di <= sl-1; di++)
              for (int dj = -sl+1+!(di&1); dj <= sl-1; dj += 2)
              {
                int d2 = di*di + dj*dj;
                if (d2 < (int)((sl-0.5)*(sl-0.5))) // mask
                {
                  float sum =
                    t[color][di+sl+1][dj+sl] + t[color][di+sl-1][dj+sl] +
                    t[color][di+sl][dj+sl+1] + t[color][di+sl][dj+sl-1];
                  t[color][di+sl][dj+sl] = sum /* /4 */;
                  if (sum < min)
                    if (d2 < (int)((sl-2.5)*(sl-2.5))) // mask
                    { min = sum; I = di; J = dj; }
                }
              }

            // find shift estimation and weights
            {
              int i = I + sl;
              int j = J + sl;
              float dv = (t[color][i+2][j] - t[color][i-2][j]) /* /4 */;
              float dh = (t[color][i][j+2] - t[color][i][j-2]) /* /4 */;
              float d2v = (t[color][i+2][j] + t[color][i-2][j]
                            - 2*t[color][i][j])/2 /* /4 */;
              float d2h = (t[color][i][j+2] + t[color][i][j-2]
                            - 2*t[color][i][j])/2 /* /4 */;
              float d2m = (t[color][i+1][j+1] - t[color][i-1][j+1] -
                            t[color][i+1][j-1] + t[color][i-1][j-1]) /* /4 */;
              float div = 4*d2v*d2h - d2m*d2m;
              if (div > 0)
              {
                float d2d = d2v - d2h;
                float sqrtD = sqrt(d2d*d2d + d2m*d2m);
                float lb = (d2v + d2h + sqrtD)/2;
                float ls = (d2v + d2h - sqrtD)/2;
                float lb2 = lb * lb;
                float ls2 = ls * ls;
                float pv = -(2*dv*d2h - dh*d2m) / div;
                float ph = -(2*dh*d2v - dv*d2m) / div;
                float sv = I + CLAMP(pv, -2.5, +2.5);
                float sh = J + CLAMP(ph, -2.5, +2.5);
                if (radial)
                {
                  float rad = sqrt(x*x + y*y);
                  if (rad != 0.0)
                  {
                    float theta = atan2(d2m, d2d)/2;
                    float delta = atan2(x, y);
                    float cosd = cos(theta - delta);
                    float sind = sin(theta - delta);
                    bc[color][0] = (y*sv + x*sh)/rad;
                    // weight (deviations are in 1/l^2)
                    bw[color][0] = 1/(sqrt(cosd*cosd/lb2 + sind*sind/ls2));
                  }
                }
                else
                {
                  /* float sin2th = d2m / sqrtD; */
                  float cos2th = d2d / sqrtD;
                  float sqsinth = (1 - cos2th)/2;
                  float sqcosth = (1 + cos2th)/2;
                  bc[color][0] = sv;
                  bc[color][1] = sh;
                  // weights (deviations are in 1/l^2)
                  bw[color][0] = 1/(sqrt(sqcosth/lb2 + sqsinth/ls2));
                  bw[color][1] = 1/(sqrt(sqsinth/lb2 + sqcosth/ls2));
                }
              }
            }
          }

          // show weights
          if (visuals)
          {
            const int rad = 6;
            const int outr = inr - offv;
            const int outc = inc - offh + !((outr+ca)&1);
            float *outp = out + (size_t)owidth*outr + outc;
            if (outr >= rad && outr < oheight - rad &&
                outc >= rad && outc < owidth - rad)
            {
              const float s =
                sqrt(bw[0][0]*bw[0][0] + bw[0][1]*bw[0][1])/4 +
                sqrt(bw[1][0]*bw[1][0] + bw[1][1]*bw[1][1])/4;
              for (int di = -rad+1; di <= rad; di += 2)
                for (int dj = -rad; dj <= rad; dj += 2)
                {
                  int d2 = di*di + dj*dj;
                  if (d2 < rad*rad)
                  {
                    outp[owidth*di+dj] += fmin(0.5, s/exp(1.5*d2));
                    outp[owidth*dj+di] += fmin(0.5, s/exp(1.5*d2));
                  }
                }
            }
          }

#if CACORRECT_DEBUG
          if (y > -0.05 && y < 0.05 && x > -0.05 && x < 0.05)
          {
            printf("x=%+.4lf y=%+.4lf g=%.2lf r  ", x, y, rgb[r][c]);
            printf("%+.5f (%8.4f)  %+.5f (%8.4f)\n",
                   bc[0][0], bw[0][0], bc[0][1], bw[0][1]);
            printf("x=%+.4lf y=%+.4lf g=%.2lf b  ", x, y, rgb[r][c]);
            printf("%+.5f (%8.4f)  %+.5f (%8.4f)\n",
                   bc[1][0], bw[1][0], bc[1][1], bw[1][1]);
          }
#endif

          // fill up parameters for the fitting
          for (int color = 0; color < 2; color++)
            for (int coeff = 0; coeff < ncoeff; coeff++)
              if (radial)
                lin_push1(
                  degn, deg, tfitmat[color][coeff], tfitvec[color][coeff],
                  sqrtf(x*x + y*y), bw[color][coeff], bc[color][coeff]);
              else
                lin_push2(
                  degn, deg, tfitmat[color][coeff], tfitvec[color][coeff],
                  x, y, bw[color][coeff], bc[color][coeff]);
        }

#ifdef _OPENMP
#pragma omp critical
#endif
      {
        for (int color = 0; color < 2; color++)
          for (int coeff = 0; coeff < ncoeff; coeff++)
            lin_add(
              degn,
              tfitmat[color][coeff], tfitvec[color][coeff],
              fitmat[color][coeff], fitvec[color][coeff]);
      }
    }

  // end of pre-analysis

#if CACORRECT_DEBUG
  time_end = clock();
  printf("time: %f\n", (float)(time_end-time_start)/CLOCKS_PER_SEC);
  time_start = time_end;
  printf("cacorrect: solve\n");
#endif

  // resolve fit coefficients
  float fitsol[2][ncoeff][degn];
  for (int color = 0; color < 2; color++)
    for (int coeff = 0; coeff < ncoeff; coeff++)
    {
      int unk =
        lin_solve(
          degn, fitmat[color][coeff], fitvec[color][coeff],
          fitsol[color][coeff]);
      if (unk)
      {
        printf("cacorrect: unsolved unk=%d color=%d coeff=%d\n",
               unk, color, coeff);
        return;
      }

      if (radial)
      {
#if CACORRECT_DEBUG
        printf("cacorrect: radial color=%d offset=%+f\n",
               color, fitsol[color][coeff][0]);
#endif
        fitsol[color][coeff][0] = 0.0;
      }
    }

#if CACORRECT_DEBUG
  time_end = clock();
  printf("time: %f\n", (float)(time_end-time_start)/CLOCKS_PER_SEC);
  time_start = time_end;
  printf("cacorrect: analyse in %f seconds (cpu time)\n",
         (float)(time_end-time_begin)/CLOCKS_PER_SEC);
#endif

  // store the fit
  d->fitdata = malloc(sizeof(fitsol));
  memcpy(d->fitdata, fitsol, sizeof(fitsol));
}

static void
CA_correct(dt_iop_module_t *const self, dt_dev_pixelpipe_iop_t *const piece,
           const float *const in, float *const out,
           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_cacorrect_data_t *d = piece->data;

  const int iwidth  = roi_in->width;
  const int iheight = roi_in->height;
  const int isize = MIN(iwidth, iheight);
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  const int offv = roi_out->y - roi_in->y;
  const int offh = roi_out->x - roi_in->x;

#if CACORRECT_DEBUG
  printf("cacorrect: i.w=%d i.h=%d i.x=%d i.y=%d s=%f\n",
         iwidth, iheight, roi_in->x, roi_in->y, roi_in->scale);
  printf("cacorrect: o.w=%d o.h=%d o.x=%d o.y=%d s=%f\n",
         owidth, oheight, roi_out->x, roi_out->y, roi_out->scale);
#endif

  const uint32_t filters = dt_image_filter(&piece->pipe->image);
  const int ca = (filters & 3) != 1, fb = (filters >> (!ca << 2) & 3) == 2;

  const int sl = CA_SHIFT;
  const int radial = d->type == 1;
  const int deg = d->degree;
  const int degn = lin_size(radial ? 1 : 2, deg);

  const int TS = (iwidth > 2024 && iheight > 2024) ? 256 : 128;
  const int ncoeff = radial ? 1 : 2;

  int visuals = get_visuals();

  // do the fitting if not already computed
  if (!d->fitdata)
    CA_analyse(self, piece, in, out, roi_in, roi_out);

  if (!d->fitdata)
    return;

  // retrieve fit coefficients
  float fitsol[2][ncoeff][degn];
  memcpy(fitsol, d->fitdata, sizeof(fitsol));

#if CACORRECT_DEBUG
  clock_t time_start = clock();
  clock_t time_end;
#endif

  const int margin = sl + d->ip->width*2;

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for (int top = -margin; top < oheight + margin; top += TS - 2*margin)
    for (int left = -margin; left < owidth + margin; left += TS - 2*margin)
    {
      const int rm = top + TS > oheight + margin ? oheight + margin - top : TS;
      const int cm = left + TS > owidth + margin ? owidth + margin - left : TS;

      for (int color = 0; color < 2; color++)
      {
        float vc[TS/2][TS/2] __attribute__((aligned(16)));
        for (int r = 0; r < rm/2; r++)
          for (int c = 0; c < cm/2; c++)
          {
            const int inr = top + 2*r + offv + ((color+fb)&1);
            const int inc = left + 2*c + offh + !((color+fb+ca)&1);
            if (inr >= 0 && inr < iheight && inc >= 0 && inc < iwidth)
              vc[r][c] = in[(size_t)iwidth*inr+inc];
            else
              vc[r][c] = 0./0.; // nan values will be extrapolated
          }

        // gamma = 2.0
        for (int r = 0; r < rm/2; r++)
        {
          int c = 0;
          float *p0 = &vc[r][c];
#if __SSE2__
          for (; c + 3 < cm/2; p0 += 4, c += 4)
            _mm_store_ps(p0, _mm_sqrt_ps(_mm_load_ps(p0)));
#endif
          for (; c < cm/2; p0 += 1, c += 1)
            *p0 = sqrt(*p0);
        }

        for (int r = margin+((color+fb)&1); r < rm - margin; r += 2)
          for (int c = margin+!((color+fb+ca)&1); c < cm - margin; c += 2)
          {
            const int outr = top + r;
            const int outc = left + c;
            const int inr = outr + offv;
            const int inc = outc + offh;

            float bc[2] = { };
            const float y = (2*(inr + 0.5) - iheight)/isize;
            const float x = (2*(inc + 0.5) - iwidth)/isize;

            // compute the polynomial
            if (radial)
            {
              const float rad = sqrtf(x*x + y*y);
              if (rad != 0.0)
              {
                float v = lin_get1(deg, fitsol[color][0], rad);
                bc[0] = v*y/rad;
                bc[1] = v*x/rad;
              }
            }
            else
            {
              for (int coeff = 0; coeff < ncoeff; coeff++)
              {
                bc[coeff] = lin_get2(deg, fitsol[color][coeff], x, y);
                bc[coeff] = lin_get2(deg, fitsol[color][coeff], x, y);
              }
            }

            const float bv = CLAMPS(bc[0], -sl, sl);
            const float bh = CLAMPS(bc[1], -sl, sl);

            // shift by interpolation
            const int xmin = MAX(-inc, -margin);
            const int ymin = MAX(-inr, -margin);
            const int xmax = MIN(iwidth-inc, +margin);
            const int ymax = MIN(iheight-inr, +margin);
            float v =
              dt_interpolation_compute_extrapolated_sample(d->ip,
                &vc[r/2][c/2], bh/2, bv/2,
                xmin/2, ymin/2, xmax/2-1, ymax/2-1, 1, TS/2);
            v = fmax(0., v);

            // show shift norms as isos
            if (visuals)
            {
              const float norm = sqrt(bv*bv + bh*bh);
              for (float l = 0; l <= sl; l += 0.5)
              {
                const float d = norm - l;
                if ((-0.01 < d && d < -0.005) || (0 < d && d < 0.01))
                {
                  v *= d >= 0 ? 1.75 : 1.5;
                  v += d > 0 ? 0.2 : 0.075;
                }
              }
            }

            out[(size_t)owidth*outr + outc] = v*v; // gamma = 2.0
          }
      }
    }

#if CACORRECT_DEBUG
  time_end = clock();
  printf("cacorrect: correct in %f seconds (cpu time)\n",
         (float)(time_end-time_start)/CLOCKS_PER_SEC);
#endif

}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const float *const in, float *const out,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int iwidth  = roi_in->width;
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  const int offv = roi_out->y - roi_in->y;
  const int offh = roi_out->x - roi_in->x;
  for (int r = 0; r < oheight; r++)
    memcpy(out + (size_t)owidth*r, in + (size_t)iwidth*(r+offv)+offh,
           sizeof(float)*owidth);

  if (piece->pipe->type != DT_DEV_PIXELPIPE_EXPORT && !get_displayed())
    if (!dt_control_get_dev_zoom()) return;

  CA_correct(self, piece, in, out, roi_in, roi_out);
}

void reload_defaults(dt_iop_module_t *module)
{
  // init defaults:
  dt_iop_cacorrect_params_t p;
  p.type = 0;
  memcpy(module->params, &p, sizeof(dt_iop_cacorrect_params_t));
  memcpy(module->default_params, &p, sizeof(dt_iop_cacorrect_params_t));

  if (!module->dev) return;

  // can't be switched on for non-raw images:
  module->hide_enable_button =
    !dt_image_is_raw(&module->dev->image_storage)
    || (module->dev->image_storage.filters == 9u);
}

void init(dt_iop_module_t *module)
{
  module->params_size = sizeof(dt_iop_cacorrect_params_t);
  module->params = malloc(sizeof(dt_iop_cacorrect_params_t));
  module->default_params = malloc(sizeof(dt_iop_cacorrect_params_t));
  // our module is disabled by default
  module->default_enabled = 0;

  // we come just before demosaicing.
  module->priority = 87; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
}

void commit_params(dt_iop_module_t *self, dt_iop_cacorrect_params_t *p,
                   dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_cacorrect_data_t *d = piece->data;

  // non raw and preview pipe do not have mosaiced data:
  if (!(pipe->image.flags & DT_IMAGE_RAW)
     || dt_dev_pixelpipe_uses_downsampled_input(pipe))
  {
    piece->enabled = 0;
    return;
  }

  // drop the current fit
  if (d->fitdata)
  {
    free(d->fitdata);
    d->fitdata = NULL;
  }

  // set up parameters
  d->type = p->type;
  d->ip = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  if (p->type == 1)
  {
    int rdegree = get_radial_degree();
    if (rdegree <= 0) rdegree = 4; // default value
    d->degree = rdegree; // order of the radial polynomial fit
  }
  else
  {
    int gdegree = get_degree();
    if (gdegree < 0) gdegree = 4; // default value
    d->degree = gdegree; // order of the 2-dimentional polynomial fit
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_cacorrect_data_t));
  dt_iop_cacorrect_data_t *d = piece->data;
  d->fitdata = NULL;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_cacorrect_data_t *d = piece->data;
  if (d->fitdata)
    free(d->fitdata);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_cacorrect_gui_data_t *g = self->gui_data;
  dt_iop_cacorrect_params_t *p = (void *)self->params;
  if (p->type < 0 || p->type > 2) p->type = 0; // compat
  dt_bauhaus_combobox_set(g->tcombo, p->type);
}

static void type_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_cacorrect_gui_data_t *g = self->gui_data;
  dt_iop_cacorrect_params_t *p = (void *)self->params;
  if(self->dt->gui->reset) return;
  p->type = dt_bauhaus_combobox_get(g->tcombo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_cacorrect_gui_data_t));
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  dt_iop_cacorrect_gui_data_t *g = self->gui_data;

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g->tcombo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->tcombo, NULL, _("type"));
  dt_bauhaus_combobox_clear(g->tcombo);
  dt_bauhaus_combobox_add(g->tcombo, _("generic"));
  dt_bauhaus_combobox_add(g->tcombo, _("radial"));
  g_signal_connect(G_OBJECT (g->tcombo), "value-changed",
                   G_CALLBACK (type_changed), (gpointer)self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->tcombo, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
