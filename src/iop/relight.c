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
#include "common/debug.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/slider.h"
#include "dtgtk/gradientslider.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_relight_params_t
{
  float ev;
  float center;
  float width;
}
dt_iop_relight_params_t;

void init_presets (dt_iop_module_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("fill-light 0.25EV with 4 zones"), self->op, &(dt_iop_relight_params_t)
  {
    0.25,0.25,4.0
  } , sizeof(dt_iop_relight_params_t), 1);
  dt_gui_presets_add_generic(_("fill-shadow -0.25EV with 4 zones"), self->op, &(dt_iop_relight_params_t)
  {
    -0.25,0.25,4.0
  } , sizeof(dt_iop_relight_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}

typedef struct dt_iop_relight_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;                                            // left and right controlboxes
  GtkLabel  *label1,*label2,*label3;            		       	// ev, center, width
  GtkDarktableSlider *scale1,*scale2;        			// ev,width
  GtkDarktableGradientSlider *gslider1;				// center
  GtkDarktableToggleButton *tbutton1;                     // Pick median lightess
}
dt_iop_relight_gui_data_t;

typedef struct dt_iop_relight_data_t
{
  float ev;			          	// The ev of relight -4 - +4 EV
  float center;		          		// the center light value for relight
  float width;			        	// the width expressed in zones
}
dt_iop_relight_data_t;

const char *name()
{
  return _("fill light");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES|IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/relight/exposure");
//  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/relight/width");
}

#define GAUSS(a,b,c,x) (a*pow(2.718281828,(-pow((x-b),2)/(pow(c,2)))))

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_relight_data_t *data = (dt_iop_relight_data_t *)piece->data;
  const int ch = piece->colors;

  // Precalculate parameters for gauss function
  const float a = 1.0;                                                                // Height of top
  const float b = -1.0+(data->center*2);                                 // Center of top
  const float c = (data->width/10.0)/2.0;      				                    // Width

#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    float *in = ((float *)ivoid) + ch*k*roi_out->width;
    float *out = ((float *)ovoid) + ch*k*roi_out->width;
    for(int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      const float lightness = in[0]/100.0;
      const float x = -1.0+(lightness*2.0);
      float gauss = GAUSS(a,b,c,x);

      if(isnan(gauss) || isinf(gauss))
        gauss = 0.0;

      float relight = 1.0 / exp2f ( -data->ev * CLIP(gauss));

      if(isnan(relight) || isinf(relight))
        relight = 1.0;

      out[0] = 100.0*CLIP (lightness*relight);
      out[1] = in[1];
      out[2] = in[2];
    }
  }
}

static void
picker_callback (GtkDarktableToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (button)))
  {
    dt_iop_request_focus (self);
    self->request_color_pick = 1;
  }
  else
    self->request_color_pick = 0;
}

static void
ev_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->ev = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
width_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->width = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
center_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;

  {
    p->center = dtgtk_gradient_slider_get_value(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[relight] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_relight_data_t *d = (dt_iop_relight_data_t *)piece->data;
  d->ev = p->ev;
  d->width = p->width;
  d->center = p->center;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_relight_data_t));
  memset(piece->data,0,sizeof(dt_iop_relight_data_t));
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

  self->request_color_pick = 0;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;

  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)module->params;
  dtgtk_slider_set_value (g->scale1, p->ev);
  dtgtk_slider_set_value (g->scale2, p->width);
  dtgtk_gradient_slider_set_value(g->gslider1,p->center);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_relight_params_t));
  module->default_params = malloc(sizeof(dt_iop_relight_params_t));
  module->default_enabled = 0;
  module->priority = 608; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_relight_params_t);
  module->gui_data = NULL;
  dt_iop_relight_params_t tmp = (dt_iop_relight_params_t)
  {
    0.33,0,4
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_relight_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_relight_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  self->request_color_pick=0;
  return 1;
}

static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < self->picked_color_min[0]) return FALSE;
  if(!self->request_color_pick) return FALSE;
  const float *Lab = self->picked_color;

  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;
  p->center = Lab[0];
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  darktable.gui->reset = 1;
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dtgtk_gradient_slider_set_value(DTGTK_GRADIENT_SLIDER(g->gslider1),p->center);
  darktable.gui->reset = 0;

  return FALSE;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_relight_gui_data_t));
  dt_iop_relight_gui_data_t *g = (dt_iop_relight_gui_data_t *)self->gui_data;
  dt_iop_relight_params_t *p = (dt_iop_relight_params_t *)self->params;

  self->widget = gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g_signal_connect (G_OBJECT (self->widget), "expose-event", G_CALLBACK (expose), self);

  /* adding the labels */
  g->scale1 = DTGTK_SLIDER (dtgtk_slider_new_with_range (DARKTABLE_SLIDER_BAR,-2.0, 2.0,0.05, p->ev, 2));
  g->scale2 = DTGTK_SLIDER (dtgtk_slider_new_with_range (DARKTABLE_SLIDER_BAR,2, 10, 0.5, p->width, 1));
  dtgtk_slider_set_label(g->scale1, _("exposure"));
  dtgtk_slider_set_unit(g->scale1, "EV");
  dtgtk_slider_set_force_sign(g->scale1, TRUE);
  dtgtk_slider_set_label(g->scale2, _("width"));
//  dtgtk_slider_set_accel(g->scale1,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/relight/exposure");
//  dtgtk_slider_set_accel(g->scale2,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/relight/width");

  /* lightnessslider */
  GtkBox *hbox=GTK_BOX (gtk_hbox_new (FALSE,2));
  int lightness=32768;
  g->gslider1=DTGTK_GRADIENT_SLIDER (dtgtk_gradient_slider_new_with_color ((GdkColor)
  {
    0,0,0,0
  },(GdkColor)
  {
    0,lightness,lightness,lightness
  }));
  g_object_set(G_OBJECT (g->gslider1), "tooltip-text", _("select the center of fill-light"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->gslider1), "value-changed",
                    G_CALLBACK (center_callback), self);
  g->tbutton1 = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT));
  gtk_widget_set_size_request(GTK_WIDGET(g->tbutton1),22,22);

  g_signal_connect (G_OBJECT (g->tbutton1), "toggled",
                    G_CALLBACK (picker_callback), self);

  gtk_box_pack_start (hbox,GTK_WIDGET (g->gslider1),TRUE,TRUE,0);
  gtk_box_pack_start (hbox,GTK_WIDGET (g->tbutton1),FALSE,FALSE,0);

  /* add controls to widget ui */
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET (g->scale1),TRUE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET (hbox), TRUE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET (g->scale2),TRUE,FALSE,0);


  g_object_set(G_OBJECT(g->tbutton1), "tooltip-text", _("toggle tool for picking median lightness in image"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the fill-light in EV"), (char *)NULL);
  /* xgettext:no-c-format */
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("width of fill-light area defined in zones"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (ev_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (width_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
