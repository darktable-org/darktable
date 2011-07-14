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
#include "dtgtk/slider.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <stdlib.h>

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
  GtkDarktableSlider *luma;
  GtkDarktableSlider *chroma;
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
  return _("denoising (extra slow)");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/nlmeans/luma");
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/nlmeans/chroma");
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
  cl_int err = -999;
  const int P = ceilf(3 * roi_in->scale / piece->iscale); // pixel filter size
  const int K = ceilf(7 * roi_in->scale / piece->iscale); // nbhood

  if(P <= 1)
  {
    size_t origin[] = {0, 0, 0};
    size_t region[] = {roi_in->width, roi_in->height, 1};
    err = dt_opencl_enqueue_copy_image(darktable.opencl->dev[devid].cmd_queue, dev_in, dev_out, origin, origin, region, 0, NULL, NULL);
    if (err != CL_SUCCESS) goto error;
    return TRUE;
  }
  float max_L = 100.0f, max_C = 256.0f;
  float nL = 1.0f/(d->luma*max_L), nC = 1.0f/(d->chroma*max_C);
  nL *= nL; nC *= nC;
  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 2, sizeof(int32_t), (void *)&P);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 3, sizeof(int32_t), (void *)&K);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 4, sizeof(float), (void *)&nL);
  dt_opencl_set_kernel_arg(darktable.opencl, devid, gd->kernel_nlmeans, 5, sizeof(float), (void *)&nC);
  err = dt_opencl_enqueue_kernel_2d(darktable.opencl, devid, gd->kernel_nlmeans, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_nlmeans] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/** process, all real work is done here. */
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
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
    memcpy (o, i, sizeof(float)*4*roi_out->width*roi_out->height);
    return;
  }

  // adjust to Lab, make L more important
  float max_L = 100.0f, max_C = 256.0f;
  float nL = 1.0f/(d->luma*max_L), nC = 1.0f/(d->chroma*max_C);
  const float norm2[4] = { nL*nL, nC*nC, nC*nC, 1.0f };

#define SLIDING_WINDOW // brings down time from 15 secs to 3 secs on a core2 duo
#ifdef SLIDING_WINDOW
  float *Sa = dt_alloc_align(64, sizeof(float)*roi_out->width*dt_get_num_threads());
#else
  float *S = dt_alloc_align(64, sizeof(float)*roi_out->width*roi_out->height);
#endif
  // we want to sum up weights in col[3], so need to init to 0:
  memset(o, 0x0, sizeof(float)*roi_out->width*roi_out->height*4);

  // for each shift vector
  for(int kj=-K;kj<=K;kj++)
  {
    for(int ki=-K;ki<=K;ki++)
    {
#ifdef SLIDING_WINDOW
      int inited_slide = 0;
      // don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else we will add up errors)
      // do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more memory
#ifdef _OPENMP
#  pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, roi_out, roi_in, i, o, Sa)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        if(j+kj < 0 || j+kj >= roi_out->height) continue;
        float *S = Sa + dt_get_thread_num() * roi_out->width;
        const float *ins = ((float *)i) + 4*(roi_in->width *(j+kj) + ki);
        float *out = ((float *)o) + 4*roi_out->width*j;

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
            float *s = S;
            const float *inp  = ((float *)i) + 4* roi_in->width *(j+jj);
            const float *inps = ((float *)i) + 4*(roi_in->width *(j+jj+kj) + ki);
            for(int i=0; i<roi_out->width; i++)
            {
              if(i+ki >= 0 && i+ki < roi_out->width)
              {
                for(int k=0;k<3;k++)
                  s[0] += (inp[4*i + k] - inps[4*i + k])*(inp[4*i + k] - inps[4*i + k]) * norm2[k];
              }
              s++;
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
            const float w = gh(slide);
            for(int k=0;k<3;k++) out[k] += ins[k] * w;
            out[3] += w;
          }
          s   ++;
          ins += 4;
          out += 4;
        }
        if(inited_slide && j+P+1+MAX(0,kj) < roi_out->height)
        {
          // sliding window in j direction:
          float *s = S;
          const float *inp  = ((float *)i) + 4* roi_in->width *(j+P+1);
          const float *inps = ((float *)i) + 4*(roi_in->width *(j+P+1+kj) + ki);
          const float *inm  = ((float *)i) + 4* roi_in->width *(j-P);
          const float *inms = ((float *)i) + 4*(roi_in->width *(j-P+kj) + ki);
          for(int i=0; i<roi_out->width; i++)
          {
            if(i+ki >= 0 && i+ki < roi_out->width) for(int k=0;k<3;k++)
              s[0] += ((inp[4*i + k] - inps[4*i + k])*(inp[4*i + k] - inps[4*i + k])
                    -  (inm[4*i + k] - inms[4*i + k])*(inm[4*i + k] - inms[4*i + k])) * norm2[k];
            s++;
          }
        }
        else inited_slide = 0;
      }
#else
      // construct summed area table of weights:
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(i,S,roi_in,roi_out,kj,ki)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        const float *in  = ((float *)i) + 4* roi_in->width * j;
        const float *ins = ((float *)i) + 4*(roi_in->width *(j+kj) + ki);
        float *out = ((float *)S) + roi_out->width*j;
        if(j+kj < 0 || j+kj >= roi_out->height) memset(out, 0x0, sizeof(float)*roi_out->width);
        else for(int i=0; i<roi_out->width; i++)
        {
          if(i+ki < 0 || i+ki >= roi_out->width) out[0] = 0.0f;
          else
          {
            out[0]  = (in[0] - ins[0])*(in[0] - ins[0]) * norm2[0];
            out[0] += (in[1] - ins[1])*(in[1] - ins[1]) * norm2[1];
            out[0] += (in[2] - ins[2])*(in[2] - ins[2]) * norm2[2];
          }
          in  += 4;
          ins += 4;
          out ++;
        }
      }
      // now sum up:
      // horizontal phase:
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(S,roi_out)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        int stride = 1;
        while(stride < roi_out->width)
        {
          float *out = ((float *)S) + roi_out->width*j;
          for(int i=0;i<roi_out->width-stride;i++)
          {
            out[0] += out[stride];
            out ++;
          }
          stride <<= 1;
        }
      }
      // vertical phase:
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(S,roi_out)
#endif
      for(int i=0; i<roi_out->width; i++)
      {
        int stride = 1;
        while(stride < roi_out->height)
        {
          float *out = S + i;
          for(int j=0;j<roi_out->height-stride;j++)
          {
            out[0] += out[roi_out->width*stride];
            out += roi_out->width;
          }
          stride <<= 1;
        }
      }
      // now the denoising loop:
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(i,o,S,kj,ki,roi_in,roi_out)
#endif
      for(int j=0; j<roi_out->height; j++)
      {
        if(j+kj < 0 || j+kj >= roi_out->height) continue;
        const float *in  = ((float *)i) + 4*(roi_in->width *(j+kj) + ki);
        float *out = ((float *)o) + 4*roi_out->width*j;
        const float *s = S + roi_out->width*j;
        const int offy = MIN(j, MIN(roi_out->height - j - 1, P)) * roi_out->width;
        for(int i=0; i<roi_out->width; i++)
        {
          if(i+ki >= 0 && i+ki < roi_out->width)
          {
            const int offx = MIN(i, MIN(roi_out->width - i - 1, P));
            const float m1 = s[offx - offy], m2 = s[- offx + offy], p1 = s[offx + offy], p2 = s[- offx - offy];
            const float w = gh(p1 + p2 - m1 - m2);
            for(int k=0;k<3;k++) out[k] += in[k] * w;
            out[3] += w;
          }
          s   ++;
          in  += 4;
          out += 4;
        }
      }
#endif
    }
  }
  // normalize:
#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(o,roi_out)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    float *out = ((float *)o) + 4*roi_out->width*j;
    for(int i=0; i<roi_out->width; i++)
    {
      for(int k=0;k<3;k++) out[k] *= 1.0f/out[3];
      out += 4;
    }
  }
#ifdef SLIDING_WINDOW
  // free shared tmp memory:
  free(Sa);
#else
  // free the summed area table:
  free(S);
#endif
}

/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  // init defaults:
  dt_iop_nlmeans_params_t tmp = (dt_iop_nlmeans_params_t)
  {
    0.5f, 0.5f
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
  module->priority = 444; // module order created by iop_dependencies.py, do not edit!
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
  gd->kernel_nlmeans = dt_opencl_create_kernel(darktable.opencl, program, "nlmeans");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_nlmeans_global_data_t *gd = (dt_iop_nlmeans_global_data_t *)module->data;
  dt_opencl_free_kernel(darktable.opencl, gd->kernel_nlmeans);
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
  p->luma = dtgtk_slider_get_value(g->luma)*(1.0f/100.0f);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
chroma_callback(GtkRange *range, dt_iop_module_t *self)
{
  // this is important to avoid cycles!
  if(darktable.gui->reset) return;
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  p->chroma = dtgtk_slider_get_value(g->chroma)*(1.0f/100.0f);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/** gui callbacks, these are needed. */
void gui_update    (dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  dt_iop_nlmeans_params_t *p = (dt_iop_nlmeans_params_t *)self->params;
  dtgtk_slider_set_value(g->luma,   p->luma   * 100.f);
  dtgtk_slider_set_value(g->chroma, p->chroma * 100.f);
}

void gui_init     (dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_nlmeans_gui_data_t));
  dt_iop_nlmeans_gui_data_t *g = (dt_iop_nlmeans_gui_data_t *)self->gui_data;
  self->widget = gtk_vbox_new(TRUE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  // TODO: adjust defaults:
  g->luma   = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0f, 100.0f, 1., 50.f, 0));
  g->chroma = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 0.0f, 100.0f, 1., 50.f, 0));
  dtgtk_slider_set_default_value(g->luma,   50.f);
  dtgtk_slider_set_default_value(g->chroma, 50.f);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->luma), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->chroma), TRUE, TRUE, 0);
  dtgtk_slider_set_label(g->luma, _("luma"));
  dtgtk_slider_set_unit (g->luma, "%");
  dtgtk_slider_set_label(g->chroma, _("chroma"));
  dtgtk_slider_set_unit (g->chroma, "%");
  dtgtk_slider_set_accel(g->luma,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/nlmeans/luma");
  dtgtk_slider_set_accel(g->chroma,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/nlmeans/chroma");
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
