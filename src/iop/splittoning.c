/*
    This file is part of darktable,
    copyright (c) 2010-2011 Henrik Andersson.

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
#include "dtgtk/label.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/slider.h"
#include "dtgtk/gradientslider.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef struct dt_iop_splittoning_params_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;						// center luminance of gradient
  float compress;						// Compress range
}
dt_iop_splittoning_params_t;

typedef struct dt_iop_splittoning_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkWidget  *label1,*label2,*label3,*label4,*label5,*label6;	 			 // highlight hue, h sat, shadow hue, s sat, balance, compress
  GtkDarktableSlider *scale1,*scale2;       							//  balance, compress
  GtkDarktableButton *colorpick1,*colorpick2;	   					// shadow, highlight
  GtkDarktableGradientSlider *gslider1,*gslider2,*gslider3,*gslider4;		//highligh hue, highlight saturation, shadow hue, shadow saturation
}
dt_iop_splittoning_gui_data_t;

typedef struct dt_iop_splittoning_data_t
{
  float shadow_hue;
  float shadow_saturation;
  float highlight_hue;
  float highlight_saturation;
  float balance;						// center luminance of gradient}
  float compress;						// Compress range
}
dt_iop_splittoning_data_t;

const char *name()
{
  return _("split toning");
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
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick primary color"),
                        0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick secondary color"),
                        0, 0);

  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "balance"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "compress"));

}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_splittoning_gui_data_t *g =
      (dt_iop_splittoning_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "pick primary color",
                              GTK_WIDGET(g->colorpick1));
  dt_accel_connect_button_iop(self, "pick secondary color",
                              GTK_WIDGET(g->colorpick2));

  dt_accel_connect_slider_iop(self, "balance", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "compress", GTK_WIDGET(g->scale2));
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_splittoning_data_t *data = (dt_iop_splittoning_data_t *)piece->data;
  float *in;
  float *out;
  const int ch = piece->colors;

  // Get lowest/highest l in image
  float lhigh=0.0;
  float llow=1.0;

  in  = (float *)ivoid;
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(in,roi_out,lhigh,llow) schedule(static)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    int index = k*ch;
    float h,s,l;
    rgb2hsl(&in[index],&h,&s,&l);
    lhigh=fmax(lhigh,l);
    llow=fmin(llow,l);
  }

  const float compress=(data->compress/110.0)/2.0;  // Dont allow 100% compression..
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(ivoid,ovoid,roi_out,data) private(in,out) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    in = ((float *)ivoid) + ch*k*roi_out->width;
    out = ((float *)ovoid) + ch*k*roi_out->width;
    for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
    {
      double ra,la;
      float mixrgb[3];
      float h,s,l;
      rgb2hsl(in,&h,&s,&l);
      if(l < data->balance-compress || l > data->balance+compress)
      {
        h=l<data->balance?data->shadow_hue:data->highlight_hue;
        s=l<data->balance?data->shadow_saturation:data->highlight_saturation;
        ra=l<data->balance?CLIP((fabs(-data->balance+compress+l)*2.0)):CLIP((fabs(-data->balance-compress+l)*2.0));
        la=(1.0-ra);

        hsl2rgb(mixrgb,h,s,l);

        out[0]=CLIP(in[0]*la + mixrgb[0]*ra);
        out[1]=CLIP(in[1]*la + mixrgb[1]*ra);
        out[2]=CLIP(in[2]*la + mixrgb[2]*ra);
      }
      else
      {
        out[0]=in[0];
        out[1]=in[1];
        out[2]=in[2];
      }
    }
  }
}

static void
balance_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->balance= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
compress_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->compress= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
hue_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  float color[3];
  GtkWidget *preview;
  GtkDarktableGradientSlider *sslider=NULL;
  if( slider == g->gslider1 )
  {
    // Shadows
    p->shadow_hue=hue=dtgtk_gradient_slider_get_value(slider);
    saturation=p->shadow_saturation;
    preview=GTK_WIDGET(g->colorpick1);
    sslider=g->gslider2;
  }
  else
  {
    p->highlight_hue=hue=dtgtk_gradient_slider_get_value(slider);
    saturation=p->highlight_saturation;
    preview=GTK_WIDGET(g->colorpick2);
    sslider=g->gslider4;
  }

  hsl2rgb(color,hue,saturation,0.5);

  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;

  dtgtk_gradient_slider_set_stop(sslider,1.0,c);  // Update saturation end color
  gtk_widget_modify_fg(preview,GTK_STATE_NORMAL,&c); // update color preview

  if(self->dt->gui->reset) return;
  gtk_widget_draw(GTK_WIDGET(sslider),NULL);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback(GtkDarktableGradientSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  float color[3];
  GtkWidget *preview;
  if( slider == g->gslider2 )
  {
    // Shadows
    hue=dtgtk_gradient_slider_get_value(g->gslider1);
    p->shadow_saturation=saturation=dtgtk_gradient_slider_get_value(slider);
    preview=GTK_WIDGET(g->colorpick1);
  }
  else
  {
    hue=dtgtk_gradient_slider_get_value(g->gslider3);
    p->highlight_saturation=saturation=dtgtk_gradient_slider_get_value(slider);
    preview=GTK_WIDGET(g->colorpick2);
  }

  hsl2rgb(color,hue,saturation,0.5);

  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;

  gtk_widget_modify_fg(preview,GTK_STATE_NORMAL,&c); // Update color preview

  if(self->dt->gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void
colorpick_button_callback(GtkButton *button,gpointer user_data)
{
  GtkColorSelectionDialog  *csd=(GtkColorSelectionDialog  *)user_data;
  gtk_dialog_response(GTK_DIALOG(csd),(GTK_WIDGET(button)==csd->ok_button)?GTK_RESPONSE_ACCEPT:0);
}

static void
colorpick_callback (GtkDarktableButton *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  GtkColorSelectionDialog  *csd = GTK_COLOR_SELECTION_DIALOG(gtk_color_selection_dialog_new(_("select tone color")));
  gtk_window_set_transient_for(GTK_WINDOW(csd), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  g_signal_connect (G_OBJECT (csd->ok_button), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);
  g_signal_connect (G_OBJECT (csd->cancel_button), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);

  GtkColorSelection *cs = GTK_COLOR_SELECTION(gtk_color_selection_dialog_get_color_selection(csd));
  GdkColor c;
  float color[3],h,s,l;
  h=(button==g->colorpick1)?p->shadow_hue:p->highlight_hue;
  s=(button==g->colorpick1)?p->shadow_saturation:p->highlight_saturation;
  l=0.5;
  hsl2rgb(color,h,s,l);

  c.red= 65535 * color[0];
  c.green= 65535 * color[1];
  c.blue= 65535 * color[2];
  gtk_color_selection_set_current_color(cs,&c);
  if(gtk_dialog_run(GTK_DIALOG(csd))==GTK_RESPONSE_ACCEPT)
  {
    gtk_color_selection_get_current_color(cs,&c);
    color[0]=c.red/65535.0;
    color[1]=c.green/65535.0;
    color[2]=c.blue/65535.0;
    rgb2hsl(color,&h,&s,&l);
    l=0.5;
    hsl2rgb(color,h,s,l);

    dtgtk_gradient_slider_set_value( (button==g->colorpick1)? g->gslider1: g->gslider3 ,h );
    dtgtk_gradient_slider_set_value( (button==g->colorpick1)? g->gslider2: g->gslider4 ,s );
  }
  gtk_widget_destroy(GTK_WIDGET(csd));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}



void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[splittoning] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;
  d->shadow_hue= p->shadow_hue;
  d->highlight_hue= p->highlight_hue;
  d->shadow_saturation = p->shadow_saturation;
  d->highlight_saturation = p->highlight_saturation;
  d->balance = p->balance/100.0;
  d->compress=p->compress;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_splittoning_data_t));
  memset(piece->data,0,sizeof(dt_iop_splittoning_data_t));
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
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)module->params;

  dtgtk_gradient_slider_set_value(g->gslider1,p->shadow_hue);
  dtgtk_gradient_slider_set_value(g->gslider3,p->highlight_hue);
  dtgtk_gradient_slider_set_value(g->gslider4,p->highlight_saturation);
  dtgtk_gradient_slider_set_value(g->gslider2,p->shadow_saturation);
  dtgtk_slider_set_value(g->scale1, p->balance*100.0);
  dtgtk_slider_set_value(g->scale2, p->compress);

  float color[3];
  hsl2rgb(color,p->shadow_hue,p->shadow_saturation,0.5);

  GdkColor c;
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;

  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick1),GTK_STATE_NORMAL,&c);

  hsl2rgb(color,p->highlight_hue,p->highlight_saturation,0.5);
  c.red=color[0]*65535.0;
  c.green=color[1]*65535.0;
  c.blue=color[2]*65535.0;

  gtk_widget_modify_fg(GTK_WIDGET(g->colorpick2),GTK_STATE_NORMAL,&c);

}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_enabled = 0;
  module->priority = 895; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_splittoning_params_t);
  module->gui_data = NULL;
  dt_iop_splittoning_params_t tmp = (dt_iop_splittoning_params_t)
  {
    0,0.5,0.2,0.5, 0.5,33.0
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_splittoning_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_splittoning_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_splittoning_gui_data_t));
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  // Shadows
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(dtgtk_label_new(_("shadows"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT)), FALSE, FALSE, 5);

  g->colorpick1 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick1),32,32);

  GtkWidget *hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_end(GTK_BOX(hbox),GTK_WIDGET(g->colorpick1),FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g->label1 = dtgtk_reset_label_new (_("hue"), self, &p->shadow_hue, sizeof(float));
  g->label2 = dtgtk_reset_label_new (_("saturation"), self, &p->shadow_saturation, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  int lightness=32768;
  g->gslider1=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor)
  {
    0,lightness,0,0
  },(GdkColor)
  {
    0,lightness,0,0
  }));
  dtgtk_gradient_slider_set_stop(g->gslider1,0.166,(GdkColor)
  {
    0,lightness,lightness,0
  });
  dtgtk_gradient_slider_set_stop(g->gslider1,0.332,(GdkColor)
  {
    0,0,lightness,0
  });
  dtgtk_gradient_slider_set_stop(g->gslider1,0.498,(GdkColor)
  {
    0,0,lightness,lightness
  });
  dtgtk_gradient_slider_set_stop(g->gslider1,0.664,(GdkColor)
  {
    0,0,0,lightness
  });
  dtgtk_gradient_slider_set_stop(g->gslider1,0.83,(GdkColor)
  {
    0,lightness,0,lightness
  });
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider1), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->gslider1), "tooltip-text", _("select the hue tone for shadows"), (char *)NULL);

  g->gslider2=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor)
  {
    0,lightness,lightness,lightness
  },(GdkColor)
  {
    0,lightness,lightness,lightness
  }));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider2), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->gslider2), "tooltip-text", _("select the saturation shadow tone"), (char *)NULL);

  // Highlights
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(dtgtk_label_new(_("highlights"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT)), FALSE, FALSE, 5);

  g->colorpick2 = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE));
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick2),32,32);

  hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_end(GTK_BOX(hbox),GTK_WIDGET(g->colorpick2),FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g->label3 = dtgtk_reset_label_new (_("hue"), self, &p->highlight_hue, sizeof(float));
  g->label4 = dtgtk_reset_label_new (_("saturation"), self, &p->highlight_saturation, sizeof(float));
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);


  g->gslider3=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor)
  {
    0,lightness,0,0
  },(GdkColor)
  {
    0,lightness,0,0
  }));
  dtgtk_gradient_slider_set_stop(g->gslider3,0.166,(GdkColor)
  {
    0,lightness,lightness,0
  });
  dtgtk_gradient_slider_set_stop(g->gslider3,0.332,(GdkColor)
  {
    0,0,lightness,0
  });
  dtgtk_gradient_slider_set_stop(g->gslider3,0.498,(GdkColor)
  {
    0,0,lightness,lightness
  });
  dtgtk_gradient_slider_set_stop(g->gslider3,0.664,(GdkColor)
  {
    0,0,0,lightness
  });
  dtgtk_gradient_slider_set_stop(g->gslider3,0.83,(GdkColor)
  {
    0,lightness,0,lightness
  });
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider3), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->gslider3), "tooltip-text", _("select the hue tone for highlights"), (char *)NULL);

  g->gslider4=DTGTK_GRADIENT_SLIDER(dtgtk_gradient_slider_new_with_color((GdkColor)
  {
    0,lightness,lightness,lightness
  },(GdkColor)
  {
    0,lightness,lightness,lightness
  }));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->gslider4), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->gslider4), "tooltip-text", _("select the saturation highlights tone"), (char *)NULL);

  // Additional paramters
  hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 5);

  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->balance*100.0, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_RATIO);
  dtgtk_slider_set_label(g->scale1,_("balance"));
  dtgtk_slider_set_unit(g->scale1,"%");
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);

  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->compress, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_label(g->scale2,_("compress"));
  dtgtk_slider_set_unit(g->scale2,"%");
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);


  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the balance of center of splittoning"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("compress the effect on highlighs/shadows and\npreserve midtones"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->gslider1), "value-changed",
                    G_CALLBACK (hue_callback), self);
  g_signal_connect (G_OBJECT (g->gslider3), "value-changed",
                    G_CALLBACK (hue_callback), self);

  g_signal_connect (G_OBJECT (g->gslider2), "value-changed",
                    G_CALLBACK (saturation_callback), self);
  g_signal_connect (G_OBJECT (g->gslider4), "value-changed",
                    G_CALLBACK (saturation_callback), self);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (balance_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (compress_callback), self);


  g_signal_connect (G_OBJECT (g->colorpick1), "clicked",
                    G_CALLBACK (colorpick_callback), self);
  g_signal_connect (G_OBJECT (g->colorpick2), "clicked",
                    G_CALLBACK (colorpick_callback), self);

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
