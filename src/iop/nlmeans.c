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
#include "bauhaus/bauhaus.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

#define ROUNDUP(a, n)		((a) % (n) == 0 ? (a) : ((a) / (n) + 1) * (n))

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE(1)

typedef struct dt_iop_nlmeans_params_t
{
  // these are stored in db.
  float luma;
  float chroma;
}
dt_iop_nlmeans_params_t;

typedef struct dt_iop_nlmeans_gui_data_t
{
  GtkWidget *luma;
  GtkWidget *chroma;
}
dt_iop_nlmeans_gui_data_t;

typedef dt_iop_nlmeans_params_t dt_iop_nlmeans_data_t;

typedef struct dt_iop_nlmeans_global_data_t
{
  int kernel_nlmeans;
}
dt_iop_nlmeans_global_data_t;

const char *name()
{
  return _("denoise (non-local means)");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
flags ()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "luma"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "chroma"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "luma", GTK_WIDGET(g->luma));
  dt_accel_connect_slider_iop(self, "chroma", GTK_WIDGET(g->chroma));
}

/** modify regions of interest (optional, per pixel ops don't need this) */
// void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);
// void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);

static float gh(const float f)
{
  // return 0.0001f + dt_fast_expf(-fabsf(f)*800.0f);
  // return 1.0f/(1.0f + f*f);
  // make spread bigger: less smoothing
  const float spread = 100.f;
  return 1.0f/(1.0f + fabsf(f)*spread);
}

// temporarily disabled, because it is really quite unbearably slow the way it is implemented now..
#if 0//def HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;
  dt_iop_nlmeans_global_data_t *gd = (dt_iop_nlmeans_global_data_t *)self->data;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_int err = -999;
  const int P = ceilf(3 * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood

  if(P <= 1)
  {
    size_t origin[] = { 0, 0, 0};
    size_t region[] = { width, height, 1};
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if (err != CL_SUCCESS) goto error;
    return TRUE;
  }
  float max_L = 100.0f, max_C = 256.0f;
  float nL = 1.0f/(d->luma*max_L), nC = 1.0f/(d->chroma*max_C);
  nL *= nL; nC *= nC;
  size_t sizes[] = { ROUNDUP(width, 4), ROUNDUP(height, 4), 1};
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 4, sizeof(int32_t), (void *)&P);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 5, sizeof(int32_t), (void *)&K);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 6, sizeof(float), (void *)&nL);
  dt_opencl_set_kernel_arg(devid, gd->kernel_nlmeans, 7, sizeof(float), (void *)&nC);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_nlmeans, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_nlmeans] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_nlmeans_params_t *d = (dt_iop_nlmeans_params_t *)piece->data;

  // adjust to zoom size:
  const int P = ceilf(3 * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood
  if(P <= 1)
  {
    // nothing to do from this distance:
    memcpy (ovoid, ivoid, sizeof(float)*4*roi_out->width*roi_out->height);
    return;
  }

  // adjust to Lab, make L more important
  // float max_L = 100.0f, max_C = 256.0f;
  // float nL = 1.0f/(d->luma*max_L), nC = 1.0f/(d->chroma*max_C);
  float max_L = 120.0f, max_C = 512.0f;
  float nL = 1.0f/max_L, nC = 1.0f/max_C;
  const float norm2[4] = { nL*nL, nC*nC, nC*nC, 1.0f };

  float *Sa = dt_alloc_align(64, sizeof(float)*roi_out->width*dt_get_num_threads());
  // we want to sum up weights in col[3], so need to init to 0:
  memset(ovoid, 0x0, sizeof(float)*roi_out->width*roi_out->height*4);

  // for each shift vector
  for(int kj=-K;kj<=K;kj++)
  {
    for(int ki=-K;ki<=K;ki++)
    {
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
          for(int jj=-Pm;jj<=PM;jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp  = ((float *)ivoid) + 4*i + 4* roi_in->width *(j+jj);
            const float *inps = ((float *)ivoid) + 4*i + 4*(roi_in->width *(j+jj+kj) + ki);
            const int last = roi_out->width + MIN(0, -ki);
            for(; i<last; i++, inp+=4, inps+=4, s++)
            {
              for(int k=0;k<3;k++)
                s[0] += (inp[k] - inps[k])*(inp[k] - inps[k]) * norm2[k];
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i=0;i<2*P+1;i++) slide += s[i];
        for(int i=0; i<roi_out->width; i++)
        {
          if(i-P > 0 && i+P<roi_out->width)
            slide += s[P] - s[-P-1];
          if(i+ki >= 0 && i+ki < roi_out->width)
          {
            const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
            _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(gh(slide)));
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
            for(int k=0;k<3;k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                    -  (inm[k] - inms[k])*(inm[k] - inms[k])) * norm2[k];
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
            sv += inpv0*inpv0 * _mm_set1_ps(norm2[0]);

            const __m128 inpv1 = _mm_movehl_ps(inp34lo,inp12lo);
            sv += inpv1*inpv1 * _mm_set1_ps(norm2[1]);

            const __m128 inpv2 = _mm_movelh_ps(inp12hi,inp34hi);
            sv += inpv2*inpv2 * _mm_set1_ps(norm2[2]);

            const __m128 inm1 = _mm_load_ps(inm)    - _mm_load_ps(inms);
            const __m128 inm2 = _mm_load_ps(inm+4)  - _mm_load_ps(inms+4);
            const __m128 inm3 = _mm_load_ps(inm+8)  - _mm_load_ps(inms+8);
            const __m128 inm4 = _mm_load_ps(inm+12) - _mm_load_ps(inms+12);

            const __m128 inm12lo = _mm_unpacklo_ps(inm1,inm2);
            const __m128 inm34lo = _mm_unpacklo_ps(inm3,inm4);
            const __m128 inm12hi = _mm_unpackhi_ps(inm1,inm2);
            const __m128 inm34hi = _mm_unpackhi_ps(inm3,inm4);

            const __m128 inmv0 = _mm_movelh_ps(inm12lo,inm34lo);
            sv -= inmv0*inmv0 * _mm_set1_ps(norm2[0]);

            const __m128 inmv1 = _mm_movehl_ps(inm34lo,inm12lo);
            sv -= inmv1*inmv1 * _mm_set1_ps(norm2[1]);

            const __m128 inmv2 = _mm_movelh_ps(inm12hi,inm34hi);
            sv -= inmv2*inmv2 * _mm_set1_ps(norm2[2]);

            _mm_store_ps(s, sv);
          }
          for(; i<last; i++, inp+=4, inps+=4, inm+=4, inms+=4, s++)
          {
            float stmp = s[0];
            for(int k=0;k<3;k++)
              stmp += ((inp[k] - inps[k])*(inp[k] - inps[k])
                    -  (inm[k] - inms[k])*(inm[k] - inms[k])) * norm2[k];
            s[0] = stmp;
          }
        }
        else inited_slide = 0;
      }
    }
  }
  // normalize and apply chroma/luma blending
  // bias a bit towards higher values for low input values:
  const __m128 weight = _mm_set_ps(1.0f, powf(d->chroma, 0.6), powf(d->chroma, 0.6), powf(d->luma, 0.6));
  const __m128 invert = _mm_sub_ps(_mm_set1_ps(1.0f), weight);
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(ovoid,ivoid,roi_out,d)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + 4*roi_out->width*j;
    float *in  = ((float *)ivoid) + 4*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      _mm_store_ps(out, _mm_add_ps(
          _mm_mul_ps(_mm_load_ps(in),  invert),
          _mm_mul_ps(_mm_load_ps(out), _mm_div_ps(weight, _mm_set1_ps(out[3])))));
      out += 4;
      in  += 4;
    }
  }
  // free shared tmp memory:
  free(Sa);
}

/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  // init defaults:
  dt_iop_nlmeans_params_t tmp = (dt_iop_nlmeans_params_t)
  {
    0.1f, 0.3f
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_nlmeans_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_nlmeans_params_t));
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_nlmeans_params_t));
  module->default_params = malloc(sizeof(dt_iop_nlmeans_params_t));
  // about the first thing to do in Lab space:
  module->priority = 470; // module order created by iop_dependencies.py, do not edit!
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

void init_global(dt_iop_module_so_t *module)
{
  const int program = 5; // nlmeans.cl, from programs.conf
  dt_iop_nlmeans_global_data_t *gd = (dt_iop_nlmeans_global_data_t *)malloc(sizeof(dt_iop_nlmeans_global_data_t));
  module->data = gd;
  gd->kernel_nlmeans = dt_opencl_create_kernel(program, "nlmeans");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_nlmeans_global_data_t *gd = (dt_iop_nlmeans_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_nlmeans);
  free(module->data);
  module->data = NULL;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)params;
  dt_iop_nlmeans_data_t *d = (dt_iop_nlmeans_data_t *)piece->data;
  d->luma   = MAX(0.0001f, p->luma);
  d->chroma = MAX(0.0001f, p->chroma);
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
luma_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->luma = dt_bauhaus_slider_get(g->luma)*(1.0f/100.0f);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
chroma_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->chroma = dt_bauhaus_slider_get(g->chroma)*(1.0f/100.0f);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  dt_bauhaus_slider_set(g->luma,   p->luma   * 100.f);
  dt_bauhaus_slider_set(g->chroma, p->chroma * 100.f);
}

void gui_init     (dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_nlmeans_gui_data_t));
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g->luma   = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 1., 10.f, 0);
  g->chroma = dt_bauhaus_slider_new_with_range(self, 0.0f, 100.0f, 1., 30.f, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->luma, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->chroma, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->luma, _("luma"));
  dt_bauhaus_slider_set_format(g->luma, "%.0f%%");
  dt_bauhaus_widget_set_label(g->chroma, _("chroma"));
  dt_bauhaus_slider_set_format(g->chroma, "%.0f%%");
  g_object_set (GTK_OBJECT(g->luma),   "tooltip-text", _("how much to smooth brightness"), (char *)NULL);
  g_object_set (GTK_OBJECT(g->chroma), "tooltip-text", _("how much to smooth colors"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->luma),   "value-changed", G_CALLBACK (luma_callback),   self);
  g_signal_connect (G_OBJECT (g->chroma), "value-changed", G_CALLBACK (chroma_callback), self);
}

void gui_cleanup  (dt_iop_module_t *self)
{
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
