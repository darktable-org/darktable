/*
  This file is part of darktable,
  copyright (c) 2012 ulrich pegelow.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "dtgtk/togglebutton.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <xmmintrin.h>

#define CLAMPF(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))
#define CLAMP_RANGE(x,y,z) (CLAMP(x,y,z))
#define MMCLAMPPS(a, mn, mx) (_mm_min_ps((mx), _mm_max_ps((a), (mn))))

#define BLOCKSIZE 64		/* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */

DT_MODULE(2)

typedef enum dt_iop_gaussian_order_t
{
  DT_IOP_GAUSSIAN_ZERO = 0,
  DT_IOP_GAUSSIAN_ONE = 1,
  DT_IOP_GAUSSIAN_TWO = 2
}
dt_iop_gaussian_order_t;

/* legacy version 1 params */
typedef struct dt_iop_shadhi_params1_t
{
  dt_iop_gaussian_order_t order;
  float radius;
  float shadows;
  float reserved1;
  float highlights;
  float reserved2;
  float compress;
}
dt_iop_shadhi_params1_t;

typedef struct dt_iop_shadhi_params_t
{
  dt_iop_gaussian_order_t order;
  float radius;
  float shadows;
  float reserved1;
  float highlights;
  float reserved2;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
}
dt_iop_shadhi_params_t;


typedef struct dt_iop_shadhi_gui_data_t
{
  GtkWidget *scale1,*scale2,*scale3,*scale4,*scale5,*scale6;       // shadows, highlights, radius, compress, shadows_ccorrect, highlights_ccorrect
}
dt_iop_shadhi_gui_data_t;

typedef struct dt_iop_shadhi_data_t
{
  dt_iop_gaussian_order_t order;
  float radius;
  float shadows;
  float highlights;
  float compress;
  float shadows_ccorrect;
  float highlights_ccorrect;
}
dt_iop_shadhi_data_t;

typedef struct dt_iop_shadhi_global_data_t
{
  int kernel_gaussian_column;
  // int kernel_gaussian_row;
  int kernel_gaussian_transpose;
  int kernel_shadows_highlights_mix;
  int kernel_gaussian_copy_alpha;
}
dt_iop_shadhi_global_data_t;


const char *name()
{
  return _("shadows and highlights");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if (old_version == 1 && new_version == 2)
  {
    const dt_iop_shadhi_params1_t *old = old_params;
    dt_iop_shadhi_params_t *new = new_params;
    new->order = old->order;
    new->radius = old->radius;
    new->shadows = 0.5f*old->shadows;
    new->reserved1 = old->reserved1;
    new->highlights = -0.5f*old->highlights;
    new->reserved2 = old->reserved2;
    new->compress = old->compress;
    new->shadows_ccorrect = 100.0f;
    new->highlights_ccorrect = 0.0f;

    return 0;
  }
  return 1;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "highlights"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "compress"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "shadows color correction"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "highlights color correction"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "shadows", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "highlights", GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->scale3));
  dt_accel_connect_slider_iop(self, "compress", GTK_WIDGET(g->scale4));
  dt_accel_connect_slider_iop(self, "shadows color correction", GTK_WIDGET(g->scale5));
  dt_accel_connect_slider_iop(self, "highlights color correction", GTK_WIDGET(g->scale6));
}


static 
void compute_gauss_params(const float sigma, dt_iop_gaussian_order_t order, float *a0, float *a1, float *a2, float *a3, 
                          float *b1, float *b2, float *coefp, float *coefn)
{
  const float alpha = 1.695f / sigma;
  const float ema = exp(-alpha);
  const float ema2 = exp(-2.0f * alpha);
  *b1 = -2.0f * ema;
  *b2 = ema2;
  *a0 = 0.0f;
  *a1 = 0.0f;
  *a2 = 0.0f;
  *a3 = 0.0f;
  *coefp = 0.0f;
  *coefn = 0.0f;

  switch(order)
  {
    default:
    case DT_IOP_GAUSSIAN_ZERO:
    {
      const float k = (1.0f - ema)*(1.0f - ema)/(1.0f + (2.0f * alpha * ema) - ema2);
      *a0 = k;
      *a1 = k * (alpha - 1.0f) * ema;
      *a2 = k * (alpha + 1.0f) * ema;
      *a3 = -k * ema2;
    }
    break;

    case DT_IOP_GAUSSIAN_ONE:
    {
      *a0 = (1.0f - ema)*(1.0f - ema);
      *a1 = 0.0f;
      *a2 = -*a0;
      *a3 = 0.0f;
    }
    break;

    case DT_IOP_GAUSSIAN_TWO:
    {
      const float k = -(ema2 - 1.0f) / (2.0f * alpha * ema);
      float kn = -2.0f * (-1.0f + (3.0f * ema) - (3.0f * ema * ema) + (ema * ema * ema));
      kn /= ((3.0f * ema) + 1.0f + (3.0f * ema * ema) + (ema * ema * ema));
      *a0 = kn;
      *a1 = -kn * (1.0f + (k * alpha)) * ema;
      *a2 = kn * (1.0f - (k * alpha)) * ema;
      *a3 = -kn * ema2;
    }
  }

  *coefp = (*a0 + *a1)/(1.0f + *b1 + *b2);
  *coefn = (*a2 + *a3)/(1.0f + *b1 + *b2);
}


static inline void _Lab_scale(const float *i, float *o)
{
  o[0] = i[0]/100.0f;
  o[1] = i[1]/128.0f;
  o[2] = i[2]/128.0f;
}


static inline void _Lab_rescale(const float *i, float *o)
{
  o[0] = i[0]*100.0f;
  o[1] = i[1]*128.0f;
  o[2] = i[2]*128.0f;
}

static inline float sign(float x)
{
  return (x < 0 ? -1.0f : 1.0f);
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_shadhi_data_t *data = (dt_iop_shadhi_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  float a0, a1, a2, a3, b1, b2, coefp, coefn;


  const float radius = fmax(0.1f, data->radius);
  const float sigma = radius * roi_in->scale / piece ->iscale;
  const float shadows = 2.0*fmin(fmax(-1.0,(data->shadows/100.0)), 1.0f);
  const float highlights = 2.0*fmin(fmax(-1.0,(data->highlights/100.0)), 1.0f);
  const float compress = fmin(fmax(0,(data->compress/100.0)), 0.99f);   // upper limit 0.99f to avoid division by zero later
  const float shadows_ccorrect = (fmin(fmax(0,(data->shadows_ccorrect/100.0)), 1.0f) - 0.5f) * sign(shadows) + 0.5f;
  const float highlights_ccorrect = (fmin(fmax(0,(data->highlights_ccorrect/100.0)), 1.0f) - 0.5f) * sign(-highlights) + 0.5f;

  // as the function name implies
  compute_gauss_params(sigma, data->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  float *temp = dt_alloc_align(64, roi_out->width*roi_out->height*ch*sizeof(float));
  if(temp==NULL) return;

#if 0
  const float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  const float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };

  // vertical blur column by column
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int i=0; i<roi_out->width; i++)
  {
    float xp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xa[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ya[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // forward filter
    for(int k=0; k<3; k++)
    {
      xp[k] = CLAMPF(in[i*ch+k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }
 
    for(int j=0; j<roi_out->height; j++)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k<4; k++)
      {
        xc[k] = CLAMPF(in[offset+k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        temp[offset+k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k=0; k<3; k++)
    {
      xn[k] = CLAMPF(in[((roi_out->height - 1) * roi_out->width + i)*ch+k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int j=roi_out->height - 1; j > -1; j--)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k<3; k++)
      {      
        xc[k] = CLAMPF(in[offset+k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k]; 
        xn[k] = xc[k]; 
        ya[k] = yn[k]; 
        yn[k] = yc[k];

        temp[offset+k] += yc[k];
      }
    }
  }

  // horizontal blur line by line
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float xp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yb[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float xa[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float yn[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float ya[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    // forward filter
    for(int k=0; k<3; k++)
    {
      xp[k] = CLAMPF(temp[j*roi_out->width*ch+k], Labmin[k], Labmax[k]);
      yb[k] = xp[k] * coefp;
      yp[k] = yb[k];
    }
 
    for(int i=0; i<roi_out->width; i++)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k<3; k++)
      {
        xc[k] = CLAMPF(temp[offset+k], Labmin[k], Labmax[k]);
        yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

        out[offset+k] = yc[k];

        xp[k] = xc[k];
        yb[k] = yp[k];
        yp[k] = yc[k];
      }
    }

    // backward filter
    for(int k=0; k<3; k++)
    {
      xn[k] = CLAMPF(temp[((j + 1)*roi_out->width - 1)*ch + k], Labmin[k], Labmax[k]);
      xa[k] = xn[k];
      yn[k] = xn[k] * coefn;
      ya[k] = yn[k];
    }

    for(int i=roi_out->width - 1; i > -1; i--)
    {
      int offset = (i + j * roi_out->width)*ch;

      for(int k=0; k<3; k++)
      {      
        xc[k] = CLAMPF(temp[offset+k], Labmin[k], Labmax[k]);

        yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

        xa[k] = xn[k]; 
        xn[k] = xc[k]; 
        ya[k] = yn[k]; 
        yn[k] = yc[k];

        out[offset+k] += yc[k];
      }
    }
  }
#else

  const __m128 Labmax = _mm_set_ps(1.0f, 128.0f, 128.0f, 100.0f);
  const __m128 Labmin = _mm_set_ps(0.0f, -128.0f, -128.0f, 0.0f);

  // vertical blur column by column
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int i=0; i<roi_out->width; i++)
  {
    __m128 xp = _mm_setzero_ps();
    __m128 yb = _mm_setzero_ps();
    __m128 yp = _mm_setzero_ps();
    __m128 xc = _mm_setzero_ps();
    __m128 yc = _mm_setzero_ps();
    __m128 xn = _mm_setzero_ps();
    __m128 xa = _mm_setzero_ps();
    __m128 yn = _mm_setzero_ps();
    __m128 ya = _mm_setzero_ps();

    // forward filter
    xp = MMCLAMPPS(_mm_load_ps(in+i*ch), Labmin, Labmax);
    yb = _mm_mul_ps(_mm_set_ps1(coefp), xp);
    yp = yb;

 
    for(int j=0; j<roi_out->height; j++)
    {
      int offset = (i + j * roi_out->width)*ch;

      xc = MMCLAMPPS(_mm_load_ps(in+offset), Labmin, Labmax);

      //yc = (a0 * xc[k]) + (a1 * xp) - (b1 * yp) - (b2 * yb);
      //yc = (a0 * xc[k]) + ((a1 * xp) - ((b1 * yp) + (b2 * yb)));

      yc = _mm_add_ps(_mm_mul_ps(xc, _mm_set_ps1(a0)),
           _mm_sub_ps(_mm_mul_ps(xp, _mm_set_ps1(a1)),
           _mm_add_ps(_mm_mul_ps(yp, _mm_set_ps1(b1)), _mm_mul_ps(yb, _mm_set_ps1(b2)))));

      _mm_store_ps(temp+offset, yc);

      xp = xc;
      yb = yp;
      yp = yc;

    }

    // backward filter
    xn = MMCLAMPPS(_mm_load_ps(in+((roi_out->height - 1) * roi_out->width + i)*ch), Labmin, Labmax);
    xa = xn;
    yn = _mm_mul_ps(_mm_set_ps1(coefn), xn);
    ya = yn;

    for(int j=roi_out->height - 1; j > -1; j--)
    {
      int offset = (i + j * roi_out->width)*ch;

      xc = MMCLAMPPS(_mm_load_ps(in+offset), Labmin, Labmax);

      //yc = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);
      //yc = (a2 * xn) + ((a3 * xa) - ((b1 * yn) + (b2 * ya)));

      yc = _mm_add_ps(_mm_mul_ps(xn, _mm_set_ps1(a2)),
           _mm_sub_ps(_mm_mul_ps(xa, _mm_set_ps1(a3)),
           _mm_add_ps(_mm_mul_ps(yn, _mm_set_ps1(b1)), _mm_mul_ps(ya, _mm_set_ps1(b2)))));


      xa = xn; 
      xn = xc; 
      ya = yn; 
      yn = yc;

      _mm_store_ps(temp+offset, _mm_add_ps(_mm_load_ps(temp+offset), yc));
    }
  }

  // horizontal blur line by line
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out,temp,roi_out,data,a0,a1,a2,a3,b1,b2,coefp,coefn) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    __m128 xp = _mm_setzero_ps();
    __m128 yb = _mm_setzero_ps();
    __m128 yp = _mm_setzero_ps();
    __m128 xc = _mm_setzero_ps();
    __m128 yc = _mm_setzero_ps();
    __m128 xn = _mm_setzero_ps();
    __m128 xa = _mm_setzero_ps();
    __m128 yn = _mm_setzero_ps();
    __m128 ya = _mm_setzero_ps();

    // forward filter
    xp = MMCLAMPPS(_mm_load_ps(temp+j*roi_out->width*ch), Labmin, Labmax);
    yb = _mm_mul_ps(_mm_set_ps1(coefp), xp);
    yp = yb;

 
    for(int i=0; i<roi_out->width; i++)
    {
      int offset = (i + j * roi_out->width)*ch;

      xc = MMCLAMPPS(_mm_load_ps(temp+offset), Labmin, Labmax);

      // yc[k] = (a0 * xc[k]) + (a1 * xp[k]) - (b1 * yp[k]) - (b2 * yb[k]);

      yc = _mm_add_ps(_mm_mul_ps(xc, _mm_set_ps1(a0)),
           _mm_sub_ps(_mm_mul_ps(xp, _mm_set_ps1(a1)),
           _mm_add_ps(_mm_mul_ps(yp, _mm_set_ps1(b1)), _mm_mul_ps(yb, _mm_set_ps1(b2)))));

      _mm_store_ps(out+offset, yc);

      xp = xc;
      yb = yp;
      yp = yc;
    }

    // backward filter
    xn = MMCLAMPPS(_mm_load_ps(temp+((j + 1)*roi_out->width - 1)*ch), Labmin, Labmax);
    xa = xn;
    yn = _mm_mul_ps(_mm_set_ps1(coefn), xn);
    ya = yn;


    for(int i=roi_out->width - 1; i > -1; i--)
    {
      int offset = (i + j * roi_out->width)*ch;

      xc = MMCLAMPPS(_mm_load_ps(temp+offset), Labmin, Labmax);

      //yc[k] = (a2 * xn[k]) + (a3 * xa[k]) - (b1 * yn[k]) - (b2 * ya[k]);

      yc = _mm_add_ps(_mm_mul_ps(xn, _mm_set_ps1(a2)),
           _mm_sub_ps(_mm_mul_ps(xa, _mm_set_ps1(a3)),
           _mm_add_ps(_mm_mul_ps(yn, _mm_set_ps1(b1)), _mm_mul_ps(ya, _mm_set_ps1(b2)))));


      xa = xn; 
      xn = xc; 
      ya = yn; 
      yn = yc;

      _mm_store_ps(out+offset, _mm_add_ps(_mm_load_ps(out+offset), yc));
    }
  }
#endif

  // invert and desaturate
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(out, roi_out) schedule(static)
#endif
  for(int j=0; j<roi_out->width*roi_out->height*4; j+=4)
  {
    out[j+0] = 100.0f - out[j+0];
    out[j+1] = 0.0f;
    out[j+2] = 0.0f;
  }

  const float max[4] = {1.0f, 1.0f, 1.0f, 1.0f };
  const float min[4] = {0.0f, -1.0f, -1.0f, 0.0f };
  const float lmin = 0.0f;
  const float lmax = max[0] + fabs(min[0]);
  const float halfmax = lmax/2.0;
  const float doublemax = lmax*2.0;

  // overlay highlights
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in, out, temp, roi_out) schedule(static)
#endif
  for(int j=0; j<roi_out->width*roi_out->height*4; j+=4)
  {
    float ta[3], tb[3];
    _Lab_scale(&in[j], ta); _Lab_scale(&out[j], tb);

    float lb = CLAMP_RANGE((tb[0] - halfmax) * sign(-highlights) + halfmax, lmin, lmax);
    float opacity = highlights*highlights;
    float xform = CLAMP_RANGE(1.0f - tb[0]/(1.0f-compress), 0.0f, 1.0f);

    while(opacity > 0.0f)
    {
      float lref = ta[0] > 0.01f ? ta[0] : 0.01f;
      float href = ta[0] < 0.99f ? ta[0] : 0.99f;
      float la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);


      float chunk = opacity > 1.0f ? 1.0f : opacity;
      float optrans = chunk * xform;
      opacity -= 1.0f;
      
      ta[0] = CLAMP_RANGE( la * (1.0 - optrans) + 
                          ( la>halfmax  ?  lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) : doublemax*la*lb ) * optrans, lmin, lmax) - 
                          fabs(min[0]);

      ta[1] = CLAMP_RANGE(ta[1] * (1.0f - optrans) + (ta[1] + tb[1]) * (ta[0]/lref * (1.0f - highlights_ccorrect) + 
                           (1.0f - ta[0])/(1.0f - href) * highlights_ccorrect) * optrans, min[1], max[1]);

      ta[2] = CLAMP_RANGE(ta[2] * (1.0f - optrans) + (ta[2] + tb[2]) * (ta[0]/lref * (1.0f - highlights_ccorrect) + 
                           (1.0f - ta[0])/(1.0f - href) * highlights_ccorrect) * optrans, min[2], max[2]);

    }

    _Lab_rescale(ta, &temp[j]);
  }


  // overlay shadows
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in, out, temp, roi_out) schedule(static)
#endif
  for(int j=0; j<roi_out->width*roi_out->height*4; j+=4)
  {
    float ta[3], tb[3];
    _Lab_scale(&temp[j], ta); _Lab_scale(&out[j], tb);

    float lb = CLAMP_RANGE((tb[0] - halfmax) * sign(shadows) + halfmax, lmin, lmax);
    float opacity = shadows*shadows;
    float xform = CLAMP_RANGE(tb[0]/(1.0f-compress) - compress/(1.0f-compress), 0.0f, 1.0f);

    while(opacity > 0.0f)
    {
      float lref = ta[0] > 0.01f ? ta[0] : 0.01f;
      float href = ta[0] < 0.99f ? ta[0] : 0.99f;
      float la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);


      float chunk = opacity > 1.0f ? 1.0f : opacity;
      float optrans = chunk * xform;
      opacity -= 1.0f;
      
      ta[0] = CLAMP_RANGE( la * (1.0 - optrans) + 
                          ( la>halfmax  ?  lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) : doublemax*la*lb ) * optrans, lmin, lmax) - 
                          fabs(min[0]);

      ta[1] = CLAMP_RANGE(ta[1] * (1.0f - optrans) + (ta[1] + tb[1]) * (ta[0]/lref * shadows_ccorrect + 
                           (1.0f - ta[0])/(1.0f - href) * (1.0f - shadows_ccorrect)) * optrans, min[1], max[1]);

      ta[2] = CLAMP_RANGE(ta[2] * (1.0f - optrans) + (ta[2] + tb[2]) * (ta[0]/lref * shadows_ccorrect + 
                           (1.0f - ta[0])/(1.0f - href) * (1.0f - shadows_ccorrect)) * optrans, min[2], max[2]);

    }

    _Lab_rescale(ta, &out[j]);
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  free(temp);
}



#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int bpp = 4*sizeof(float);

  // check if we need to reduce blocksize
  size_t maxsizes[3] = { 0 };        // the maximum dimensions for a work group
  size_t workgroupsize = 0;          // the maximum number of items in a work group
  unsigned long localmemsize = 0;    // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0;    // the maximum amount of items in work group for this kernel
   
  // make sure blocksize is not too large
  size_t blocksize = BLOCKSIZE;
  int blockwd;
  int blockht;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS &&
     dt_opencl_get_kernel_work_group_size(devid, gd->kernel_gaussian_transpose, &kernelworkgroupsize) == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] 
          || blocksize*blocksize > workgroupsize || blocksize*(blocksize+1)*bpp > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;    
    }

    blockwd = blockht = blocksize;

    if(blockwd * blockht > kernelworkgroupsize)
      blockht = kernelworkgroupsize / blockwd;
  }
  else
  {
    blockwd = blockht = 1;   // slow but safe
  }

  // width and height of intermediate buffers. Need to be multiples of BLOCKSIZE
  const size_t bwidth = width % blockwd == 0 ? width : (width / blockwd + 1)*blockwd;
  const size_t bheight = height % blockht == 0 ? height : (height / blockht + 1)*blockht;

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece ->iscale;
  const float shadows = 2.0*fmin(fmax(-1.0,(d->shadows/100.0f)), 1.0f);
  const float highlights = 2.0*fmin(fmax(-1.0,(d->highlights/100.0f)), 1.0f);
  const float compress = fmin(fmax(0,(d->compress/100.0)), 0.99f);  // upper limit 0.99f to avoid division by zero later
  const float shadows_ccorrect = (fmin(fmax(0,(d->shadows_ccorrect/100.0)), 1.0f) - 0.5f) * sign(shadows) + 0.5f;
  const float highlights_ccorrect = (fmin(fmax(0,(d->highlights_ccorrect/100.0)), 1.0f) - 0.5f) * sign(-highlights) + 0.5f;

  size_t origin[] = {0, 0, 0};
  size_t region[] = {width, height, 1};

  size_t local[] = {blockwd, blockht, 1};

  size_t sizes[3];

  cl_mem dev_temp1 = NULL;
  cl_mem dev_temp2 = NULL;

  // get intermediate vector buffers with read-write access
  dev_temp1 = dt_opencl_alloc_device_buffer(devid, bwidth*bheight*bpp);
  if(dev_temp1 == NULL) goto error;
  dev_temp2 = dt_opencl_alloc_device_buffer(devid, bwidth*bheight*bpp);
  if(dev_temp2 == NULL) goto error;


  // compute gaussian parameters
  float a0, a1, a2, a3, b1, b2, coefp, coefn;
  compute_gauss_params(sigma, d->order, &a0, &a1, &a2, &a3, &b1, &b2, &coefp, &coefn);

  // copy dev_in to intermediate buffer dev_temp1
  err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_temp1, origin, region, 0);
  if(err != CL_SUCCESS) goto error;

  // first blur step: column by column with dev_temp1 -> dev_temp2
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 0, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 1, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 4, sizeof(float), (void *)&a0);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 5, sizeof(float), (void *)&a1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 6, sizeof(float), (void *)&a2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 7, sizeof(float), (void *)&a3);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 8, sizeof(float), (void *)&b1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 9, sizeof(float), (void *)&b2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 10, sizeof(float), (void *)&coefp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 11, sizeof(float), (void *)&coefn);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) goto error;

  // intermediate step: transpose dev_temp2 -> dev_temp1
  sizes[0] = bwidth;
  sizes[1] = bheight;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 0, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 1, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 4, sizeof(int), (void *)&blocksize);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 5, bpp*blocksize*(blocksize+1), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) goto error;


  // second blur step: column by column of transposed image with dev_temp1 -> dev_temp2 (!! height <-> width !!)
  sizes[0] = ROUNDUPWD(height);
  sizes[1] = 1;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 0, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 1, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 4, sizeof(float), (void *)&a0);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 5, sizeof(float), (void *)&a1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 6, sizeof(float), (void *)&a2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 7, sizeof(float), (void *)&a3);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 8, sizeof(float), (void *)&b1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 9, sizeof(float), (void *)&b2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 10, sizeof(float), (void *)&coefp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_column, 11, sizeof(float), (void *)&coefn);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_gaussian_column, sizes);
  if(err != CL_SUCCESS) goto error;


  // transpose back dev_temp2 -> dev_temp1
  sizes[0] = bheight;
  sizes[1] = bwidth;
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 0, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 1, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 2, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 4, sizeof(int), (void *)&blocksize);
  dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_transpose, 5, bpp*blocksize*(blocksize+1), NULL);
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_gaussian_transpose, sizes, local);
  if(err != CL_SUCCESS) goto error;


  // once again produce copy of dev_in, as it is needed for final mixing step: dev_in -> dev_temp2
  err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_temp2, origin, region, 0);
  if(err != CL_SUCCESS) goto error;

  // final mixing step with dev_temp1 as mask: dev_temp2 -> dev_temp2
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 0, sizeof(cl_mem), (void *)&dev_temp2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 1, sizeof(cl_mem), (void *)&dev_temp1);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 4, sizeof(float), (void *)&shadows);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 5, sizeof(float), (void *)&highlights);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 6, sizeof(float), (void *)&compress);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 7, sizeof(float), (void *)&shadows_ccorrect);
  dt_opencl_set_kernel_arg(devid, gd->kernel_shadows_highlights_mix, 8, sizeof(float), (void *)&highlights_ccorrect);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_shadows_highlights_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  // copy final result from dev_temp2 -> dev_out
  err = dt_opencl_enqueue_copy_buffer_to_image(devid, dev_temp2, dev_out, 0, origin, region);
  if(err != CL_SUCCESS) goto error;

  if(piece->pipe->mask_display)
  {
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPWD(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_copy_alpha, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_copy_alpha, 1, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_copy_alpha, 2, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_copy_alpha, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_gaussian_copy_alpha, 4, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_gaussian_copy_alpha, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  if (dev_temp1 != NULL) dt_opencl_release_mem_object(dev_temp1);
  if (dev_temp2 != NULL) dt_opencl_release_mem_object(dev_temp2);
  return TRUE;

error:
  if (dev_temp1 != NULL) dt_opencl_release_mem_object(dev_temp1);
  if (dev_temp2 != NULL) dt_opencl_release_mem_object(dev_temp2);
  dt_print(DT_DEBUG_OPENCL, "[opencl_shadows&highlights] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece ->iscale;

  tiling->factor = 4.0f; // in + out + 2*temp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4*sigma;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}



static void
radius_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->radius= dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
shadows_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->shadows = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
highlights_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->highlights = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
compress_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->compress = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
shadows_ccorrect_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->shadows_ccorrect = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
highlights_ccorrect_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;
  p->highlights_ccorrect = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void 
commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[shadows&highlights] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_shadhi_data_t *d = (dt_iop_shadhi_data_t *)piece->data;
  d->order = p->order;
  d->radius = p->radius;
  d->shadows = p->shadows;
  d->highlights = p->highlights;
  d->compress = p->compress;
  d->shadows_ccorrect = p->shadows_ccorrect;
  d->highlights_ccorrect = p->highlights_ccorrect;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_shadhi_data_t));
  memset(piece->data,0,sizeof(dt_iop_shadhi_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t *)self->gui_data;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale1, p->shadows);
  dt_bauhaus_slider_set(g->scale2, p->highlights);
  dt_bauhaus_slider_set(g->scale3, p->radius);
  dt_bauhaus_slider_set(g->scale4, p->compress);
  dt_bauhaus_slider_set(g->scale5, p->shadows_ccorrect);
  dt_bauhaus_slider_set(g->scale6, p->highlights_ccorrect);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_shadhi_params_t));
  module->default_params = malloc(sizeof(dt_iop_shadhi_params_t));
  module->default_enabled = 0;
  module->priority = 490; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_shadhi_params_t);
  module->gui_data = NULL;
  dt_iop_shadhi_params_t tmp = (dt_iop_shadhi_params_t)
  {
    DT_IOP_GAUSSIAN_ZERO, 100.0f, 50.0f, 0.0f, -50.0f, 0.0f, 50.0f, 100.0f, 50.0f
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_shadhi_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_shadhi_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)malloc(sizeof(dt_iop_shadhi_global_data_t));
  module->data = gd;
  gd->kernel_gaussian_column = dt_opencl_create_kernel(program, "gaussian_column");
  gd->kernel_gaussian_transpose = dt_opencl_create_kernel(program, "gaussian_transpose");
  gd->kernel_shadows_highlights_mix = dt_opencl_create_kernel(program, "shadows_highlights_mix");
  gd->kernel_gaussian_copy_alpha = dt_opencl_create_kernel(program, "gaussian_copy_alpha");
}


void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_shadhi_global_data_t *gd = (dt_iop_shadhi_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_gaussian_column);
  dt_opencl_free_kernel(gd->kernel_gaussian_transpose);
  dt_opencl_free_kernel(gd->kernel_shadows_highlights_mix);
  dt_opencl_free_kernel(gd->kernel_gaussian_copy_alpha);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_shadhi_gui_data_t));
  dt_iop_shadhi_gui_data_t *g = (dt_iop_shadhi_gui_data_t *)self->gui_data;
  dt_iop_shadhi_params_t *p = (dt_iop_shadhi_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);

  g->scale1 = dt_bauhaus_slider_new_with_range(self,-100.0, 100.0, 2., p->shadows, 2);
  g->scale2 = dt_bauhaus_slider_new_with_range(self,-100.0, 100.0, 2., p->highlights, 2);
  g->scale3 = dt_bauhaus_slider_new_with_range(self,0.1, 200.0, 2., p->radius, 2);
  g->scale4 = dt_bauhaus_slider_new_with_range(self,0, 100.0, 2., p->compress, 2);
  g->scale5 = dt_bauhaus_slider_new_with_range(self,0, 100.0, 2., p->shadows_ccorrect, 2);
  g->scale6 = dt_bauhaus_slider_new_with_range(self,0, 100.0, 2., p->highlights_ccorrect, 2);
  dt_bauhaus_widget_set_label(g->scale1,_("shadows"));
  dt_bauhaus_widget_set_label(g->scale2,_("highlights"));
  dt_bauhaus_widget_set_label(g->scale3,_("radius"));
  dt_bauhaus_widget_set_label(g->scale4,_("compress"));
  dt_bauhaus_widget_set_label(g->scale5,_("shadows color adjustment"));
  dt_bauhaus_widget_set_label(g->scale6,_("highlights color adjustment"));
  dt_bauhaus_slider_set_format(g->scale1,"%.02f");
  dt_bauhaus_slider_set_format(g->scale2,"%.02f");
  dt_bauhaus_slider_set_format(g->scale3,"%.02f");
  dt_bauhaus_slider_set_format(g->scale4,"%.02f%%");
  dt_bauhaus_slider_set_format(g->scale5,"%.02f%%");
  dt_bauhaus_slider_set_format(g->scale6,"%.02f%%");

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale1, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale2, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale3, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale4, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale5, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale6, TRUE, TRUE, 0);

  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("correct shadows"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("correct highlights"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("spatial extent"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("compress the effect on shadows/highlights and\npreserve midtones"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale5), "tooltip-text", _("adjust saturation of shadows"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale6), "tooltip-text", _("adjust saturation of highlights"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (shadows_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (highlights_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (radius_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (compress_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (shadows_ccorrect_callback), self);
  g_signal_connect (G_OBJECT (g->scale6), "value-changed",
                    G_CALLBACK (highlights_ccorrect_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
