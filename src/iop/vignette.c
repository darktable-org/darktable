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

DT_MODULE(2)

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)

typedef struct dt_iop_dvector_2d_t
{
  double x;
  double y;
} dt_iop_dvector_2d_t;

typedef struct dt_iop_fvector_2d_t
{
  float x;
  float y;
} dt_iop_vector_2d_t;

typedef struct dt_iop_vignette_params1_t
{
  double scale;              // 0 - 100 Radie
  double falloff_scale;   // 0 - 100 Radie for falloff inner radie of falloff=scale and outer=scale+falloff_scale
  double strength;         // 0 - 1 strength of effect
  double uniformity;       // 0 - 1 uniformity of center
  double bsratio;            // -1 - +1 ratio of brightness/saturation effect
  gboolean invert_falloff;
  gboolean invert_saturation;
  dt_iop_dvector_2d_t center;            // Center of vignette
}
dt_iop_vignette_params1_t;

typedef struct dt_iop_vignette_params_t
{
  float scale;			// 0 - 100 Inner radius, percent of largest image dimension
  float falloff_scale;		// 0 - 100 Radius for falloff -- outer radius = inner radius + falloff_scale
  float brightness;		// -1 - 1 Strength of brightness reduction
  float saturation;		// -1 - 1 Strength of saturation reduction
  dt_iop_vector_2d_t center;	// Center of vignette
  gboolean autoratio;		//
  float whratio;		// 0-1 = width/height ratio, 1-2 = height/width ratio + 1
  float shape;
}
dt_iop_vignette_params_t;

typedef struct dt_iop_vignette_gui_data_t
{
  GtkDarktableSlider *scale;
  GtkDarktableSlider *falloff_scale;
  GtkDarktableSlider *brightness;
  GtkDarktableSlider *saturation;
  GtkDarktableSlider *center_x;
  GtkDarktableSlider *center_y;
  GtkToggleButton *autoratio;
  GtkDarktableSlider *whratio;
  GtkDarktableSlider *shape;
}
dt_iop_vignette_gui_data_t;

typedef struct dt_iop_vignette_data_t
{
  float scale;
  float falloff_scale;
  float brightness;
  float saturation;
  dt_iop_vector_2d_t center;	// Center of vignette
  gboolean autoratio;
  float whratio;
  float shape;
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

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if (old_version == 1 && new_version == 2)
  {
    const dt_iop_vignette_params1_t *old = old_params;
    dt_iop_vignette_params_t *new = new_params;
    new->scale = old->scale;
    new->falloff_scale = old->falloff_scale;
    new->brightness= -(1.0-MAX(old->bsratio,0.0))*old->strength/100.0;
    new->saturation= -(1.0+MIN(old->bsratio,0.0))*old->strength/100.0;
    if (old->invert_saturation)
      new->saturation *= -2.0;	// Double effect for increasing saturation
    if (old->invert_falloff)
      new->brightness = -new->brightness;
    new->center.x= old->center.x;
    new->center.y= old->center.y;
    new->autoratio= TRUE;
    new->whratio= 1.0;
    new->shape= 1.0;
    return 0;
  }
  return 1;
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_vignette_data_t *data = (dt_iop_vignette_data_t *)piece->data;
  const dt_iop_roi_t *buf_in = &piece->buf_in;
  const int ch = piece->colors;

  /* Center coordinates of buf_in */
  const dt_iop_vector_2d_t buf_center =
  {
    (buf_in->width - buf_in->x) / 2.0 + buf_in->x,
    (buf_in->height - buf_in->y) / 2.0 + buf_in->y
  };
  /* Coordinates of buf_center in terms of roi_in */
  const dt_iop_vector_2d_t roi_center =
  {
    buf_center.x * roi_in->scale - roi_in->x,
    buf_center.y * roi_in->scale - roi_in->y
  };
  float xscale;
  float yscale;

  /* w/h ratio follows piece dimensions */
  if (data->autoratio)
  {
    xscale=2.0/(buf_in->width*roi_out->scale);
    yscale=2.0/(buf_in->height*roi_out->scale);
  }
  else				/* specified w/h ratio, scale proportional to longest side */
  {
    const float basis = 2.0 / (MAX(buf_in->height, buf_in->width) * roi_out->scale);
    // w/h ratio from 0-1 use as-is
    if (data->whratio <= 1.0)
    {
      yscale=basis;
      xscale=yscale/data->whratio;
    }
    // w/h ratio from 1-2 interpret as 1-inf
    // that is, the h/w ratio + 1
    else
    {
      xscale=basis;
      yscale=xscale/(2.0-data->whratio);
    }
  }
  const float dscale=data->scale/100.0;
  // A minimum falloff is used, based on the image size, to smooth out aliasing artifacts
  const float min_falloff=100.0/MIN(buf_in->width, buf_in->height);
  const float fscale=MAX(data->falloff_scale,min_falloff)/100.0;
  const float shape=MAX(data->shape,0.001);
  const float exp1=2.0/shape;
  const float exp2=shape/2.0;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data, yscale, xscale) schedule(static)
#endif
  for(int j=0; j<roi_out->height; j++)
  {
    const int k = ch*roi_out->width*j;
    const float *in = (const float *)ivoid + k;
    float *out = (float *)ovoid + k;
    for(int i=0; i<roi_out->width; i++, in+=ch, out+=ch)
    {
      // current pixel coord translated to local coord
      const dt_iop_vector_2d_t pv =
      {
        fabsf((i-roi_center.x)*xscale-data->center.x),
        fabsf((j-roi_center.y)*yscale-data->center.y)
      };

      // Calculate the pixel weight in vignette
      const float cplen=powf(powf(pv.x,exp1)+powf(pv.y,exp1),exp2);  // Length from center to pv
      float weight=0.0;

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
      float col0=in[0], col1=in[1], col2=in[2];
      if( weight > 0 )
      {
        // Then apply falloff vignette
        float falloff=(data->brightness<=0)?(1.0+(weight*data->brightness)):(weight*data->brightness);
        col0=CLIP( ((data->brightness<0)? col0*falloff: col0+falloff) );
        col1=CLIP( ((data->brightness<0)? col1*falloff: col1+falloff) );
        col2=CLIP( ((data->brightness<0)? col2*falloff: col2+falloff) );

        // apply saturation
        float mv=(col0+col1+col2)/3.0;
        float wss=weight*data->saturation;
        col0=CLIP( col0-((mv-col0)* wss) );
        col1=CLIP( col1-((mv-col1)* wss) );
        col2=CLIP( col2-((mv-col2)* wss) );
      }
      out[0]=col0;
      out[1]=col1;
      out[2]=col2;
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
brightness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->brightness= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
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
shape_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->shape = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
autoratio_callback (GtkToggleButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->autoratio = gtk_toggle_button_get_active(button);
  dt_iop_vignette_gui_data_t *g = (dt_iop_vignette_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
whratio_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_vignette_params_t *p = (dt_iop_vignette_params_t *)self->params;
  p->whratio = dtgtk_slider_get_value(slider);
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
  d->brightness= p->brightness;
  d->saturation= p->saturation;
  d->center=p->center;
  d->autoratio=p->autoratio;
  d->whratio=p->whratio;
  d->shape=p->shape;
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
  dtgtk_slider_set_value(g->scale, p->scale);
  dtgtk_slider_set_value(g->falloff_scale, p->falloff_scale);
  dtgtk_slider_set_value(g->brightness, p->brightness);
  dtgtk_slider_set_value(g->saturation, p->saturation);
  dtgtk_slider_set_value(g->center_x, p->center.x);
  dtgtk_slider_set_value(g->center_y, p->center.y);
  gtk_toggle_button_set_active(g->autoratio, p->autoratio);
  dtgtk_slider_set_value(g->whratio, p->whratio);
  dtgtk_slider_set_value(g->shape, p->shape);
  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);
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
    80,50,-0.5,-0.5, {0,0}, FALSE, 1.0, 1.0
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
  GtkVBox   *vbox1,  *vbox2;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox2), TRUE, TRUE, 5);

  widget = dtgtk_reset_label_new (_("scale"), self, &p->scale, sizeof p->scale);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("fall-off strength"), self, &p->falloff_scale, sizeof p->falloff_scale);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("brightness"), self, &p->brightness, sizeof p->brightness);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("saturation"), self, &p->saturation, sizeof p->saturation);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("horizontal center"), self, &p->center.x, sizeof p->center.x);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("vertical center"), self, &p->center.y, sizeof p->center.y);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("shape"), self, &p->shape, sizeof p->shape);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("automatic ratio"), self, &p->autoratio, sizeof p->autoratio);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);
  widget = dtgtk_reset_label_new (_("width/height ratio"), self, &p->whratio, sizeof p->whratio);
  gtk_box_pack_start(GTK_BOX(vbox1), GTK_WIDGET(widget), TRUE, TRUE, 0);

  g->scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.5, p->scale, 2));
  g->falloff_scale = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1.0, p->falloff_scale, 2));
  g->brightness = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->brightness, 3));
  g->saturation = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->saturation, 3));
  g->center_x = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->center.x, 3));
  g->center_y = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-1.0, 1.0, 0.01, p->center.y, 3));
  g->shape = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 5.0, 0.1, p->shape, 2));
  g->autoratio = GTK_TOGGLE_BUTTON(gtk_toggle_button_new_with_label(_("automatic")));
  g->whratio = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 2.0, 0.01, p->shape, 3));

  gtk_widget_set_sensitive(GTK_WIDGET(g->whratio), !p->autoratio);

  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->falloff_scale), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->brightness), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->saturation), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->center_x), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->center_y), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->shape), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->autoratio), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox2), GTK_WIDGET(g->whratio), TRUE, TRUE, 0);

  gtk_object_set(GTK_OBJECT(g->scale), "tooltip-text", _("the radii scale of vignette for start of fall-off"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->falloff_scale), "tooltip-text", _("the radii scale of vignette for end of fall-off"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->brightness), "tooltip-text", _("strength of effect on brightness"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->saturation), "tooltip-text", _("strength of effect on saturation"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->center_x), "tooltip-text", _("horizontal offset of center of the effect"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->center_y), "tooltip-text", _("vertical offset of center of the effect"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->center_y), "tooltip-text", _("shape factor\n0 produces a rectangle\n1 produces a circle or elipse\n2 produces a diamond"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->autoratio), "tooltip-text", _("enable to have the ratio automatically follow the image size"), (char *)NULL);
  gtk_object_set(GTK_OBJECT(g->whratio), "tooltip-text", _("width-to-height ratio"), (char *)NULL);

  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->scale),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->falloff_scale),DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->center_x),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->center_y),DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_format_type(DTGTK_SLIDER(g->whratio),DARKTABLE_SLIDER_FORMAT_RATIO);

  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->falloff_scale), "value-changed",
                    G_CALLBACK (falloff_scale_callback), self);
  g_signal_connect (G_OBJECT (g->brightness), "value-changed",
                    G_CALLBACK (brightness_callback), self);
  g_signal_connect (G_OBJECT (g->saturation), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->center_x), "value-changed",
                    G_CALLBACK (centerx_callback), self);
  g_signal_connect (G_OBJECT (g->center_y), "value-changed",
                    G_CALLBACK (centery_callback), self);
  g_signal_connect (G_OBJECT (g->shape), "value-changed",
                    G_CALLBACK (shape_callback), self);
  g_signal_connect (G_OBJECT (g->autoratio), "toggled",
                    G_CALLBACK (autoratio_callback), self);
  g_signal_connect (G_OBJECT (g->whratio), "value-changed",
                    G_CALLBACK (whratio_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

