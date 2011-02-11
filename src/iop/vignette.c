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
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE(1)

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)

typedef struct dt_iop_vector_2d_t
{
  double x;
  double y;
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params_t
{
  double scale;              // 0 - 1 Radie
  double falloff_scale;   // 0 - 1 Radie for falloff inner radie of falloff=scale and outer=scale+falloff_scale
  double strength;         // 0 - 1 strength of effect
  double uniformity;       // 0 - 1 uniformity of center
  double bsratio;            // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_vector_2d_t center;            // Center of vignette
}
dt_iop_vignette_params_t;

typedef struct dt_iop_vignette_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkToggleButton *togglebutton1,*togglebutton2;				// invert saturation, invert falloff vignette
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4,*scale5,*scale6,*scale7;	  	// scale, strength, uniformity, b/s ratio, falloff sclae, horiz. center, vert. center
}
dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  double scale;
  double falloff_scale;
  double strength;
  double uniformity;
  double bsratio;
  gboolean invert_falloff;
  gboolean  invert_saturation;
  dt_iop_vector_2d_t center;            // Center of vignette
}
dt_iop_vignette_data_t;

const char *name()
{
  return _("vignetting");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}




void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const int ch = piece->colors;

  const float iwscale=2.0/(piece->buf_in.width*roi_out->scale);
  const float ihscale=2.0/(piece->buf_in.height*roi_out->scale);
  const int ix= (roi_in->x);
  const int iy= (roi_in->y);
  const double dscale=data->scale/100.0;
  const double fscale=data->falloff_scale/100.0;
  const double strength=data->strength/100.0;
  const double bs=1.0-MAX(data->bsratio,0.0);
  const double ss=1.0+MIN(data->bsratio,0.0);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    const int k = ch*roi_out->width*j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    for(int i=0; i<roi_out->width; i++, in+=ch, out+=ch)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv = {
	(ix+i)*iwscale-data->center.x-1.0,
	(iy+j)*ihscale-data->center.y-1.0
      };

      // Calculate the pixel weight in vignette
      const double cplen=sqrt((pv.x*pv.x)+(pv.y*pv.y));  // Length from center to pv
      double weight=0.0;

      if( cplen>=dscale ) // pixel is outside the inner vingette circle, lets calculate weight of vignette
      {
        weight=((cplen-dscale)/fscale);
	if (weight >= 1.0)
	  weight = 1.0;
	else if (weight <= 0.0)
	  weight = 0.0;
	else
          weight=0.5 - cos( M_PI*weight )/2.0;
      }

      // Let's apply weighted effect on brightness and desaturation
      float col[3];
      for(int c=0; c<3; c++) col[c]=in[c];
      if( weight > 0 )
      {
        // Then apply falloff vignette
        double falloff=(data->invert_falloff==FALSE)?(1.0-(weight*bs*strength)):(weight*bs*strength);
        col[0]=CLIP( ((data->invert_falloff==FALSE)? in[0]*falloff: in[0]+falloff) );
        col[1]=CLIP( ((data->invert_falloff==FALSE)? in[1]*falloff: in[1]+falloff) );
        col[2]=CLIP( ((data->invert_falloff==FALSE)? in[2]*falloff: in[2]+falloff) );

        // apply saturation
        double mv=(col[0]+col[1]+col[2])/3.0;
        double wss=CLIP(weight*ss)*strength;
        if(data->invert_saturation==FALSE)
        {
          // Desaturate
          col[0]=CLIP( col[0]+((mv-col[0])* wss) );
          col[1]=CLIP( col[1]+((mv-col[1])* wss) );
          col[2]=CLIP( col[2]+((mv-col[2])* wss) );
        }
        else
        {
          wss*=2.0;	// Double effect if we gonna saturate
          col[0]=CLIP( col[0]-((mv-col[0])* wss) );
          col[1]=CLIP( col[1]-((mv-col[1])* wss) );
          col[2]=CLIP( col[2]-((mv-col[2])* wss) );
        }

      }
      for(int c=0; c<3; c++) out[c]=col[c];
    }
  }
}

static void
scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
falloff_scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->falloff_scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->strength= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
uniformity_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->uniformity = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
bsratio_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->bsratio = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
centerx_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.x = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
centery_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->center.y = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
invert_saturation_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->invert_saturation = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
invert_falloff_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->invert_falloff = gtk_toggle_button_get_active(button);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[vignette] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_vignette_data_t *d = (dt_iop_vignette_data_t *)piece->data;
  d->scale = p->scale;
  d->falloff_scale = p->falloff_scale;
  d->strength= p->strength;
  d->uniformity= p->uniformity;
  d->bsratio= p->bsratio;
  d->invert_saturation= p->invert_saturation;
  d->center=p->center;
  d->invert_falloff = p->invert_falloff;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_vignette_data_t));
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
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->scale);
  dtgtk_slider_set_value(g->scale2, p->strength);
  dtgtk_slider_set_value(g->scale3, p->uniformity);
  dtgtk_slider_set_value(g->scale4, p->bsratio);
  dtgtk_slider_set_value(g->scale5, p->falloff_scale);
  dtgtk_slider_set_value(g->scale6, p->center.x);
  dtgtk_slider_set_value(g->scale7, p->center.y);
  gtk_toggle_button_set_active(g->togglebutton1, p->invert_saturation);
  gtk_toggle_button_set_active(g->togglebutton2, p->invert_falloff);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_params = malloc(sizeof(dt_iop_vignette_params_t));
  module->default_enabled = 0;
  module->priority = 997;
  module->params_size = sizeof(dt_iop_vignette_params_t);
  module->gui_data = NULL;
  dt_iop_vignette_params_t tmp = (dt_iop_vignette_params_t)
  {
    80,50,50,.0,0,FALSE,FALSE, {0,0}
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_vignette_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_vignette_params_t));
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
  GtkWidget *widget;

  self->gui_data = malloc(sizeof(dt_iop_vignette_gui_data_t));
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  widget = dtgtk_reset_label_new (_("scale"), self, &p->scale, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("fall-off strength"), self, &p->falloff_scale, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("strength"), self, &p->strength, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("uniformity"), self, &p->uniformity, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("b/s ratio"), self, &p->bsratio, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("saturation"), self, &p->invert_saturation, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("fall-off"), self, &p->invert_falloff, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("horizontal center"), self, &p->center.x, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("vertical center"), self, &p->center.y, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0000, 0.50, p->scale, 2));
  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.000, 1.0, p->falloff_scale, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0000, 0.50, p->strength, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 1.0000, 0.010, p->uniformity, 3));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0000, 1.0000, 0.010, p->bsratio, 1));
  g->scale6 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0000, 1.0000, 0.010, p->center.x, 3));
  g->scale7 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0000, 1.0000, 0.010, p->center.y, 3));
  g->togglebutton1 = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("invert")));
  g->togglebutton2 = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("invert")));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->togglebutton1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->togglebutton2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale6), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale7), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the radii scale of vignette for start of fall-off"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale5), "tooltip-text", _("the radii scale of vignette for end of fall-off"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("strength of effect"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale3), "tooltip-text", _("uniformity of vignette"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale4), "tooltip-text", _("brightness/saturation ratio\nof the result,\n-1 - only brightness\n 0 - 50/50 mix of brightness and saturation\n+1 - only saturation"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->togglebutton1), "tooltip-text", _("inverts effect of saturation..."), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->togglebutton2), "tooltip-text", _("inverts effect of fall-off, default is dark fall-off..."), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale6), "tooltip-text", _("horizontal offset of center of the effect"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->scale7), "tooltip-text", _("vertical offset of center of the effect"), (char *)NULL);

  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale1),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale2),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale5),DARKTABLE_SLIDER_FORMAT_PERCENT);

  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale4),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale6),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale7),DARKTABLE_SLIDER_FORMAT_RATIO);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (falloff_scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (strength_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (uniformity_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (bsratio_callback), self);
  g_signal_connect (G_OBJECT (g->togglebutton1), "toggled",
                    G_CALLBACK (invert_saturation_callback), self);
  g_signal_connect (G_OBJECT (g->togglebutton2), "toggled",
                    G_CALLBACK (invert_falloff_callback), self);
  g_signal_connect (G_OBJECT (g->scale6), "value-changed",
                    G_CALLBACK (centerx_callback), self);
  g_signal_connect (G_OBJECT (g->scale7), "value-changed",
                    G_CALLBACK (centery_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

