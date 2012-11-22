/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

typedef struct dt_iop_nlmeans_params_t
{
  float radius;      // search radius
  float strength;    // noise level after equilization
  float a[3], b[3];  // fit for poissonian-gaussian noise per color channel.
}
dt_iop_nlmeans_params_t;

typedef struct dt_iop_nlmeans_gui_data_t
{
  GtkWidget *radius;
  GtkWidget *strength;
}
dt_iop_nlmeans_gui_data_t;

typedef dt_iop_nlmeans_params_t dt_iop_nlmeans_data_t;

const char *name()
{
  return _("denoise (profiled)");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

typedef union floatint_t
{
  float f;
  uint32_t i;
}
floatint_t;

// very fast approximation for 2^-x
static inline float
fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;
  const int P = ceilf(d->radius * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood

  tiling->factor = 2.5f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = P+K;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;

  // TODO: fixed K to use adaptive size trading variance and bias!
  // adjust to zoom size:
  const int P = ceilf(d->radius * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood XXX see above comment
  // TODO: use d->strength to precodition data

  // P == 0 : this will degenerate to a (fast) bilateral filter.

  float *Sa = dt_alloc_align(64, sizeof(float)*roi_out->width*dt_get_num_threads());
  // we want to sum up weights in col[3], so need to init to 0:
  memset(ovoid, 0x0, sizeof(float)*roi_out->width*roi_out->height*4);

  // for each shift vector
  for(int kj=-K; kj<=K; kj++)
  {
    for(int ki=-K; ki<=K; ki++)
    {
      // TODO: adaptive K tests here!
      // TODO: expf eval for real bilateral experience :)

      int inited_slide = 0;
      // don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else we will add up errors)
      // do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more memory
#ifdef _OPENMP
      #  pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, roi_out, roi_in, ivoid, ovoid, Sa)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        if(j+kj < 0 || j+kj >= roi_out->height) continue;
        float *S = Sa + dt_get_thread_num() * roi_out->width;
        const float *ins = ((float *)ivoid) + 4*(roi_in->width *(j+kj) + ki);
        float *out = ((float *)ovoid) + 4*roi_out->width*j;

        const int Pm = MIN(MIN(P, j+kj), j);
        const int PM = MIN(MIN(P, roi_out->height-1-j-kj), roi_out->height-1-j);
        // first line of every thread
        // TODO: also every once in a while to assert numerical precision!
        if(!inited_slide)
        {
          // sum up a line
          memset(S, 0x0, sizeof(float)*roi_out->width);
          for(int jj=-Pm; jj<=PM; jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp  = ((float *)ivoid) + 4*i + 4* roi_in->width *(j+jj);
            const float *inps = ((float *)ivoid) + 4*i + 4*(roi_in->width *(j+jj+kj) + ki);
            const int last = roi_out->width + MIN(0, -ki);
            for(; i<last; i++, inp+=4, inps+=4, s++)
            {
              for(int k=0; k<3; k++)
                s[0] += (inp[k] - inps[k])*(inp[k] - inps[k]);
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i=0; i<2*P+1; i++) slide += s[i];
        for(int i=0; i<roi_out->width; i++)
        {
          // FIXME: the comment above is actually relevant even for 1000 px width already.
          // XXX    numerical precision will not forgive us:
          if(i-P > 0 && i+P<roi_out->width)
            slide += s[P] - s[-P-1];
          if(i+ki >= 0 && i+ki < roi_out->width)
          {
            const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
            _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(fast_mexp2f(slide)));
          }
          s   ++;
          ins += 4;
          out += 4;
        }
        if(inited_slide && j+P+1+MAX(0,kj) < roi_out->height)
        {
          // sliding window in j direction:
          int i = MAX(0, -ki);
          float *s = S + i;
          const float *inp  = ((float *)ivoid) + 4*i + 4* roi_in->width *(j+P+1);
          const float *inps = ((float *)ivoid) + 4*i + 4*(roi_in->width *(j+P+1+kj) + ki);
          const float *inm  = ((float *)ivoid) + 4*i + 4* roi_in->width *(j-P);
          const float *inms = ((float *)ivoid) + 4*i + 4*(roi_in->width *(j-P+kj) + ki);
          const int last = roi_out->width + MIN(0, -ki);
          for(; ((unsigned long)s & 0xf) != 0 && i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0; k<3; k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                       -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
          /* Process most of the line 4 pixels at a time */
          for(; i<last-4; i+=4, inp+=16, inps+=16, inm+=16, inms+=16, s+=4)
          {
            __m128 sv = _mm_load_ps(s);
            const __m128 inp1 = _mm_load_ps(inp)    - _mm_load_ps(inps);
            const __m128 inp2 = _mm_load_ps(inp+4)  - _mm_load_ps(inps+4);
            const __m128 inp3 = _mm_load_ps(inp+8)  - _mm_load_ps(inps+8);
            const __m128 inp4 = _mm_load_ps(inp+12) - _mm_load_ps(inps+12);

            const __m128 inp12lo = _mm_unpacklo_ps(inp1,inp2);
            const __m128 inp34lo = _mm_unpacklo_ps(inp3,inp4);
            const __m128 inp12hi = _mm_unpackhi_ps(inp1,inp2);
            const __m128 inp34hi = _mm_unpackhi_ps(inp3,inp4);

            const __m128 inpv0 = _mm_movelh_ps(inp12lo,inp34lo);
            sv += inpv0*inpv0;

            const __m128 inpv1 = _mm_movehl_ps(inp34lo,inp12lo);
            sv += inpv1*inpv1;

            const __m128 inpv2 = _mm_movelh_ps(inp12hi,inp34hi);
            sv += inpv2*inpv2;

            const __m128 inm1 = _mm_load_ps(inm)    - _mm_load_ps(inms);
            const __m128 inm2 = _mm_load_ps(inm+4)  - _mm_load_ps(inms+4);
            const __m128 inm3 = _mm_load_ps(inm+8)  - _mm_load_ps(inms+8);
            const __m128 inm4 = _mm_load_ps(inm+12) - _mm_load_ps(inms+12);

            const __m128 inm12lo = _mm_unpacklo_ps(inm1,inm2);
            const __m128 inm34lo = _mm_unpacklo_ps(inm3,inm4);
            const __m128 inm12hi = _mm_unpackhi_ps(inm1,inm2);
            const __m128 inm34hi = _mm_unpackhi_ps(inm3,inm4);

            const __m128 inmv0 = _mm_movelh_ps(inm12lo,inm34lo);
            sv -= inmv0*inmv0;

            const __m128 inmv1 = _mm_movehl_ps(inm34lo,inm12lo);
            sv -= inmv1*inmv1;

            const __m128 inmv2 = _mm_movelh_ps(inm12hi,inm34hi);
            sv -= inmv2*inmv2;

            _mm_store_ps(s, sv);
          }
          for(; i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0; k<3; k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                       -  (inm[k] - inms[k])*(inm[k] - inms[k]));
            s[0] = stmp;
          }
        }
        else inited_slide = 0;
      }
    }
  }
  // normalize
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(ovoid,ivoid,roi_out,d)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + 4*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      _mm_store_ps(out, _mm_mul_ps(_mm_load_ps(out), _mm_set1_ps(1.0f/out[3])));
      out += 4;
    }
  }
  // free shared tmp memory:
  free(Sa);

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  // init defaults:
  dt_iop_nlmeans_params_t tmp = (dt_iop_nlmeans_params_t)
  {
    2.0f, 50.0f, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_nlmeans_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_nlmeans_params_t));
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_nlmeans_params_t));
  module->default_params = malloc(sizeof(dt_iop_nlmeans_params_t));
  module->priority = 169; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_nlmeans_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL; // just to be sure
  free(module->params);
  module->params = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)params;
  dt_iop_nlmeans_data_t *d = (dt_iop_nlmeans_data_t *)piece->data;
  memcpy(d, p, sizeof(*d));
}

void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_nlmeans_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
radius_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->radius = (int)dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
static void
strength_callback(GtkWidget *w, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  dt_bauhaus_slider_set(g->radius,   p->radius);
  dt_bauhaus_slider_set(g->strength, p->strength);
}

void gui_init     (dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_nlmeans_gui_data_t));
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g->radius   = dt_bauhaus_slider_new_with_range(self, 1.0f, 4.0f, 1., 2.f, 0);
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 1., 50.f, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->radius, _("patch size"));
  dt_bauhaus_slider_set_format(g->radius, "%.0f");
  dt_bauhaus_widget_set_label(g->strength, _("strength"));
  dt_bauhaus_slider_set_format(g->strength, "%.0f%%");
  g_object_set (GTK_OBJECT(g->radius),   "tooltip-text", _("radius of the patches to match"), (char *)NULL);
  g_object_set (GTK_OBJECT(g->strength), "tooltip-text", _("strength of the effect"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->radius),   "value-changed", G_CALLBACK (radius_callback),   self);
  g_signal_connect (G_OBJECT (g->strength), "value-changed", G_CALLBACK (strength_callback), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
