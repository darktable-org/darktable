/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/colorspaces.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define MAX_RADIUS  32

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
#define LCLIP(x) ((x<0)?0.0:(x>100.0)?100.0:x)
DT_MODULE(1)

typedef struct dt_iop_soften_params_t
{
  float size;
  float saturation;
  float brightness;
  float amount;
}
dt_iop_soften_params_t;

typedef struct dt_iop_soften_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;       // size,saturation,brightness,amount
}
dt_iop_soften_gui_data_t;

typedef struct dt_iop_soften_data_t
{
  float size;
  float saturation;
  float brightness;
  float amount;
}
dt_iop_soften_data_t;

const char *name()
{
  return _("soften");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "size"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "brightness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "mix"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_soften_gui_data_t *g = (dt_iop_soften_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "size", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "brightness", GTK_WIDGET(g->scale3));
  dt_accel_connect_slider_iop(self, "mix", GTK_WIDGET(g->scale4));
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_soften_data_t *data = (dt_iop_soften_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  const float brightness = 1.0 / exp2f ( -data->brightness );
  const float saturation = data->saturation/100.0;
  /* create overexpose image and then blur */
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,out,roi_out) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    int index = ch*k;
    float h,s,l;
    rgb2hsl(&in[index],&h,&s,&l);
    s*=saturation;
    l*=brightness;
    hsl2rgb(&out[index],h,CLIP(s),CLIP(l));
  }

  const float w = piece->iwidth*piece->iscale;
  const float h = piece->iheight*piece->iscale;
  int mrad = sqrt( w*w + h*h) * 0.01;
  int rad = mrad*(fmin(100.0,data->size+1)/100.0);
  const int radius = MIN(mrad, ceilf(rad * roi_in->scale / piece->iscale));

  /* horizontal blur out into out */
  const int range = 2*radius+1;
  const int hr = range/2;

  const int size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;
  float *scanline[3]= {0};
  scanline[0]  = malloc((size*sizeof(float))*ch);
  scanline[1]  = malloc((size*sizeof(float))*ch);
  scanline[2]  = malloc((size*sizeof(float))*ch);

  for(int iteration=0; iteration<8; iteration++)
  {
    int index=0;
    for(int y=0; y<roi_out->height; y++)
    {
      for(int k=0; k<3; k++)
      {
        float L=0;
        int hits = 0;
        for(int x=-hr; x<roi_out->width; x++)
        {
          int op = x - hr-1;
          int np = x+hr;
          if(op>=0)
          {
            L-=out[(index+op)*ch+k];
            hits--;
          }
          if(np < roi_out->width)
          {
            L+=out[(index+np)*ch+k];
            hits++;
          }
          if(x>=0)
            scanline[k][x] = L/hits;
        }
      }

      for (int k=0; k<3; k++)
        for (int x=0; x<roi_out->width; x++)
          out[(index+x)*ch+k] = scanline[k][x];

      index+=roi_out->width;
    }

    /* vertical pass on blurlightness */
    const int opoffs = -(hr+1)*roi_out->width;
    const int npoffs = (hr)*roi_out->width;
    for(int x=0; x < roi_out->width; x++)
    {
      for(int k=0; k<3; k++)
      {
        float L=0;
        int hits=0;
        int index = -hr*roi_out->width+x;
        for(int y=-hr; y<roi_out->height; y++)
        {
          int op=y-hr-1;
          int np= y + hr;

          if(op>=0)
          {
            L-=out[(index+opoffs)*ch+k];
            hits--;
          }
          if(np < roi_out->height)
          {
            L+=out[(index+npoffs)*ch+k];
            hits++;
          }
          if(y>=0)
            scanline[k][y] = L/hits;
          index += roi_out->width;
        }
      }

      for(int k=0; k<3; k++)
        for (int y=0; y<roi_out->height; y++)
          out[(y*roi_out->width+x)*ch+k] = scanline[k][y];

    }
  }


  const float amount = data->amount/100.0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    int index = ch*k;
    out[index+0] = in[index+0]*(1-amount) + CLIP(out[index+0])*amount;
    out[index+1] = in[index+1]*(1-amount) + CLIP(out[index+1])*amount;
    out[index+2] = in[index+2]*(1-amount) + CLIP(out[index+2])*amount;
  }

  for(int i=0; i<3; ++i)
    if(scanline[i])
      free(scanline[i]);
}

static void
size_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;
  p->size= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;
  p->saturation = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
brightness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;
  p->brightness = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
amount_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;
  p->amount = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[bloom] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_soften_data_t *d = (dt_iop_soften_data_t *)piece->data;
  d->size= p->size;
  d->saturation = p->saturation;
  d->brightness = p->brightness;
  d->amount = p->amount;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_soften_data_t));
  memset(piece->data,0,sizeof(dt_iop_soften_data_t));
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
  dt_iop_soften_gui_data_t *g = (dt_iop_soften_gui_data_t *)self->gui_data;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->size);
  dtgtk_slider_set_value(g->scale2, p->saturation);
  dtgtk_slider_set_value(g->scale3, p->brightness);
  dtgtk_slider_set_value(g->scale4, p->amount);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_soften_params_t));
  module->default_params = malloc(sizeof(dt_iop_soften_params_t));
  module->default_enabled = 0;
  module->priority = 840; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_soften_params_t);
  module->gui_data = NULL;
  dt_iop_soften_params_t tmp = (dt_iop_soften_params_t)
  {
    50,100.0,0.33,50
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_soften_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_soften_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_soften_gui_data_t));
  dt_iop_soften_gui_data_t *g = (dt_iop_soften_gui_data_t *)self->gui_data;
  dt_iop_soften_params_t *p = (dt_iop_soften_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, p->size, 2));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, p->saturation, 2));
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,-2.0, 2.0, 0.01, p->brightness, 2));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 2, p->amount, 2));
  dtgtk_slider_set_label(g->scale1,_("size"));
  dtgtk_slider_set_unit(g->scale1,"%");
  dtgtk_slider_set_label(g->scale2,_("saturation"));
  dtgtk_slider_set_unit(g->scale2,"%");
  dtgtk_slider_set_label(g->scale3,_("brightness"));
  dtgtk_slider_set_unit(g->scale3,"EV");
  dtgtk_slider_set_force_sign(g->scale3, TRUE);
  dtgtk_slider_set_label(g->scale4,_("mix"));
  dtgtk_slider_set_unit(g->scale4,"%");
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(g->scale4,DARKTABLE_SLIDER_FORMAT_PERCENT);

  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the size of blur"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("the saturation of blur"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale3), "tooltip-text", _("the brightness of blur"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale4), "tooltip-text", _("the mix of effect"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (size_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (brightness_callback), self);
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (amount_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
