/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <xmmintrin.h>

// NaN-safe clip: NaN compares false and will result in 0.0
#define CLIP(x) (((x)>=0.0)?((x)<=1.0?(x):1.0):0.0)
DT_MODULE(2)

typedef struct dt_iop_velvia_params_t
{
  float strength;
  float bias;
}
dt_iop_velvia_params_t;

/* legacy version 1 params */
typedef struct dt_iop_velvia_params1_t
{
  float saturation;
  float vibrance;
  float luminance;
  float clarity;
}
dt_iop_velvia_params1_t;

typedef struct dt_iop_velvia_gui_data_t
{
  GtkVBox   *vbox;
  GtkDarktableSlider *strength_scale;
  GtkDarktableSlider *bias_scale;
}
dt_iop_velvia_gui_data_t;

typedef struct dt_iop_velvia_data_t
{
  float strength;
  float bias;
}
dt_iop_velvia_data_t;

const char *name()
{
  return _("velvia");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "vibrance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mid-tones bias"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "vibrance",
                              GTK_WIDGET(g->strength_scale));
  dt_accel_connect_slider_iop(self, "mid-tones bias",
                              GTK_WIDGET(g->bias_scale));
}

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if (old_version == 1 && new_version == 2)
  {
    const dt_iop_velvia_params1_t *old = old_params;
    dt_iop_velvia_params_t *new = new_params;
    new->strength = old->saturation*old->vibrance/100.0f;
    new->bias = old->luminance;
    return 0;
  }
  return 1;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_velvia_data_t *data = (dt_iop_velvia_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const float strength = data->strength/100.0f;

  // Apply velvia saturation
  if(strength <= 0.0)
    memcpy(out, in, sizeof(float)*ch*roi_out->width*roi_out->height);
  else
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
    for(int k=0; k<roi_out->width*roi_out->height; k++)
    {
      float *inp = in + ch*k;
      float *outp = out + ch*k;
      // calculate vibrance, and apply boost velvia saturation at least saturated pixels
      float pmax=fmaxf(inp[0],fmaxf(inp[1],inp[2]));			// max value in RGB set
      float pmin=fminf(inp[0],fminf(inp[1],inp[2]));			// min value in RGB set
      float plum = (pmax+pmin)/2.0f;					        // pixel luminocity
      float psat =(plum<=0.5f) ? (pmax-pmin)/(1e-5f + pmax+pmin): (pmax-pmin)/(1e-5f + MAX(0.0f, 2.0f-pmax-pmin));

      float pweight=CLAMPS(((1.0f- (1.5f*psat)) + ((1.0f+(fabsf(plum-0.5f)*2.0f))*(1.0f-data->bias))) / (1.0f+(1.0f-data->bias)), 0.0f, 1.0f);		// The weight of pixel
      float saturation = strength*pweight;			// So lets calculate the final affection of filter on pixel

      // Apply velvia saturation values
      const __m128 inp_m  = _mm_load_ps(inp);
      const __m128 boost  = _mm_set1_ps(saturation);
      const __m128 min_m  = _mm_set1_ps(0.0f);
      const __m128 max_m  = _mm_set1_ps(1.0f);

      const __m128 inp_shuffled = _mm_mul_ps(_mm_add_ps(_mm_shuffle_ps(inp_m,inp_m,_MM_SHUFFLE(3,0,2,1)),_mm_shuffle_ps(inp_m,inp_m,_MM_SHUFFLE(3,1,0,2))),_mm_set1_ps(0.5f));

      _mm_stream_ps( outp, _mm_min_ps(max_m,_mm_max_ps(min_m, _mm_add_ps(inp_m, _mm_mul_ps(boost,_mm_sub_ps(inp_m,inp_shuffled))))));

      // equivalent to:
      /*
       outp[0]=CLAMPS(inp[0] + saturation*(inp[0]-0.5f*(inp[1]+inp[2])), 0.0f, 1.0f);
       outp[1]=CLAMPS(inp[1] + saturation*(inp[1]-0.5f*(inp[2]+inp[0])), 0.0f, 1.0f);
       outp[2]=CLAMPS(inp[2] + saturation*(inp[2]-0.5f*(inp[0]+inp[1])), 0.0f, 1.0f);
      */
    }
  }
  _mm_sfence();
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->strength = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
bias_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;
  p->bias= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_velvia_data_t *d = (dt_iop_velvia_data_t *)piece->data;
  d->strength = p->strength;
  d->bias = p->bias;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_velvia_data_t));
  memset(piece->data,0,sizeof(dt_iop_velvia_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)module->params;
  dtgtk_slider_set_value(g->strength_scale, p->strength);
  dtgtk_slider_set_value(g->bias_scale, p->bias);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_params = malloc(sizeof(dt_iop_velvia_params_t));
  module->default_enabled = 0;
  module->priority = 840; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_velvia_params_t);
  module->gui_data = NULL;
  dt_iop_velvia_params_t tmp = (dt_iop_velvia_params_t)
  {
    25,1.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_velvia_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_velvia_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_velvia_gui_data_t));
  dt_iop_velvia_gui_data_t *g = (dt_iop_velvia_gui_data_t *)self->gui_data;
  dt_iop_velvia_params_t *p = (dt_iop_velvia_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox), TRUE, TRUE, 5);

  g->strength_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1, p->strength, 0));
  dtgtk_slider_set_format_type(g->strength_scale,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_label(g->strength_scale,_("vibrance"));
  dtgtk_slider_set_unit(g->strength_scale,"%");
  g->bias_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0, 0.01, p->bias, 2));
  dtgtk_slider_set_format_type(g->bias_scale,DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_label(g->bias_scale,_("mid-tones bias"));

  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->strength_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox), GTK_WIDGET(g->bias_scale), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->strength_scale), "tooltip-text", _("the strength of saturation boost"), (char *)NULL);
  g_object_set(G_OBJECT(g->bias_scale), "tooltip-text", _("how much to spare highlights and shadows"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->strength_scale), "value-changed",
                    G_CALLBACK (strength_callback), self);
  g_signal_connect (G_OBJECT (g->bias_scale), "value-changed",
                    G_CALLBACK (bias_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
