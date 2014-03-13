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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/opencl.h"
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
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE_INTROSPECTION(1, dt_iop_splittoning_params_t)

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
  GtkWidget *scale1,*scale2;       							//  balance, compress
  GtkDarktableButton *colorpick1,*colorpick2;	   					// shadow, highlight
  GtkWidget *gslider1,*gslider2,*gslider3,*gslider4;		//highlight hue, highlight saturation, shadow hue, shadow saturation
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

typedef struct dt_iop_splittoning_global_data_t
{
  int kernel_splittoning;
}
dt_iop_splittoning_global_data_t;


const char *name()
{
  return _("split toning");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
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

void init_presets (dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  // shadows: #ED7212
  // highlights: #ECA413
  // balance : 63
  // compress : 0
  dt_gui_presets_add_generic(_("authentic sepia"), self->op, self->version(), &(dt_iop_splittoning_params_t)
  {
    26.0/360.0, 92.0/100.0, 40.0/360.0, 92.0/100.0, 0.63, 0.0
  } , sizeof(dt_iop_splittoning_params_t), 1);

  //shadows: #446CBB
  //highlights: #446CBB
  //balance : 0
  //compress : 5.22
  dt_gui_presets_add_generic(_("authentic cyanotype"), self->op, self->version(), &(dt_iop_splittoning_params_t)
  {
    220.0/360.0, 64.0/100.0, 220.0/360.0, 64.0/100.0, 0.0, 5.22
  } , sizeof(dt_iop_splittoning_params_t), 1);

  // shadows : #A16C5E
  // highlights : #A16C5E
  // balance : 100
  // compress : 0
  dt_gui_presets_add_generic(_("authentic platinotype"), self->op, self->version(), &(dt_iop_splittoning_params_t)
  {
    13.0/360.0, 42.0/100.0, 13.0/360.0, 42.0/100.0, 100.0, 0.0
  } , sizeof(dt_iop_splittoning_params_t), 1);

  // shadows: #211A14
  // highlights: #D9D0C7
  // balance : 60
  // compress : 0
  dt_gui_presets_add_generic(_("chocolate brown"), self->op, self->version(), &(dt_iop_splittoning_params_t)
  {
    28.0/360.0, 39.0/100.0, 28.0/360.0, 8.0/100.0, 0.60, 0.0
  } , sizeof(dt_iop_splittoning_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_splittoning_data_t *data = (dt_iop_splittoning_data_t *)piece->data;
  float *in;
  float *out;
  const int ch = piece->colors;

  const float compress=(data->compress/110.0)/2.0;  // Dont allow 100% compression..
#ifdef _OPENMP
  #pragma omp parallel for default(none) shared(ivoid,ovoid,roi_out,data) private(in,out) schedule(static)
#endif
  for(int k=0; k<roi_out->height; k++)
  {
    in = ((float *)ivoid) + (size_t)ch*k*roi_out->width;
    out = ((float *)ovoid) + (size_t)ch*k*roi_out->width;
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

      out[3]=in[3];
    }
  }
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_splittoning_data_t *d = (dt_iop_splittoning_data_t *)piece->data;
  dt_iop_splittoning_global_data_t *gd = (dt_iop_splittoning_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  const float compress = (d->compress/110.0)/2.0;  // Dont allow 100% compression..
  const float balance = d->balance;
  const float shadow_hue = d->shadow_hue;
  const float shadow_saturation = d->shadow_saturation;
  const float highlight_hue = d->highlight_hue;
  const float highlight_saturation = d->highlight_saturation;

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 3, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 4, sizeof(float), &compress);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 5, sizeof(float), &balance);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 6, sizeof(float), &shadow_hue);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 7, sizeof(float), &shadow_saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 8, sizeof(float), &highlight_hue);
  dt_opencl_set_kernel_arg(devid, gd->kernel_splittoning, 9, sizeof(float), &highlight_saturation);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_splittoning, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_splittoning] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl from programs.conf
  dt_iop_splittoning_global_data_t *gd = (dt_iop_splittoning_global_data_t *)malloc(sizeof(dt_iop_splittoning_global_data_t));
  module->data = gd;
  gd->kernel_splittoning = dt_opencl_create_kernel(program, "splittoning");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_splittoning_global_data_t *gd = (dt_iop_splittoning_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_splittoning);
  free(module->data);
  module->data = NULL;
}


static void
balance_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->balance = dt_bauhaus_slider_get(slider)/100.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
compress_callback (GtkWidget* slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  p->compress= dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static inline void
update_colorpicker_fg(
  GtkWidget* colorpicker,
  float hue,
  float sat)
{
  float rgb[3];
  GdkColor c;
  hsl2rgb(rgb, hue, sat, 0.5);
  c.red = rgb[0]*65535.0;
  c.green = rgb[1]*65535.0;
  c.blue = rgb[2]*65535.0;
  gtk_widget_modify_fg(colorpicker,GTK_STATE_NORMAL,&c);
}

static inline void
update_saturation_slider_end_color(
  GtkWidget* slider,
  float hue)
{
  float rgb[3];
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

static inline void
update_balance_slider_colors(
  GtkWidget* slider,
  float hue1,
  float hue2)
{
  float rgb[3];
  if (hue1 != -1)
  {
    hsl2rgb(rgb, hue1, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 0.0, rgb[0], rgb[1], rgb[2]);
  }
  if (hue2 != -1)
  {
    hsl2rgb(rgb, hue2, 1.0, 0.5);
    dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
  }
}

static void
hue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  GtkWidget* colorpicker;
  GtkWidget* sat_slider=NULL;
  if( slider == g->gslider1 )
  {
    // Shadows
    hue = p->shadow_hue = dt_bauhaus_slider_get(slider);
    saturation = p->shadow_saturation;
    colorpicker = GTK_WIDGET(g->colorpick1);
    sat_slider = g->gslider2;
    update_balance_slider_colors(g->scale1, -1, hue);
  }
  else
  {
    hue = p->highlight_hue = dt_bauhaus_slider_get(slider);
    saturation=p->highlight_saturation;
    colorpicker=GTK_WIDGET(g->colorpick2);
    sat_slider=g->gslider4;
    update_balance_slider_colors(g->scale1, hue, -1);
  }

  update_colorpicker_fg(colorpicker, hue, saturation);
  update_saturation_slider_end_color(sat_slider, hue);

  if(self->dt->gui->reset) return;

  gtk_widget_queue_draw(GTK_WIDGET(sat_slider));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;

  double hue=0;
  double saturation=0;
  GtkWidget* colorpicker;
  if( slider == g->gslider2 )
  {
    // Shadows
    hue=dt_bauhaus_slider_get(g->gslider1);
    p->shadow_saturation=saturation=dt_bauhaus_slider_get(slider);
    colorpicker=GTK_WIDGET(g->colorpick1);
  }
  else
  {
    hue=dt_bauhaus_slider_get(g->gslider3);
    p->highlight_saturation=saturation=dt_bauhaus_slider_get(slider);
    colorpicker=GTK_WIDGET(g->colorpick2);
  }

  update_colorpicker_fg(colorpicker, hue, saturation);

  if(self->dt->gui->reset) return;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void
colorpick_button_callback(GtkButton *button,gpointer user_data)
{
  GtkColorSelectionDialog  *csd=(GtkColorSelectionDialog  *)user_data;
  GtkWidget *okButton = 0;
  g_object_get(G_OBJECT(csd), "ok-button", &okButton, NULL);

  gtk_dialog_response(GTK_DIALOG(csd), (GTK_WIDGET(button)==okButton)?GTK_RESPONSE_ACCEPT:0);
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

  GtkWidget *okButton, *cancelButton = 0;
  g_object_get(G_OBJECT(csd), "ok-button", &okButton, NULL);
  g_object_get(G_OBJECT(csd), "cancel-button", &cancelButton, NULL);

  g_signal_connect (G_OBJECT (okButton), "clicked",
                    G_CALLBACK (colorpick_button_callback), csd);
  g_signal_connect (G_OBJECT (cancelButton), "clicked",
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

    dt_bauhaus_slider_set( (button==g->colorpick1)? g->gslider1: g->gslider3 ,h );
    dt_bauhaus_slider_set( (button==g->colorpick1)? g->gslider2: g->gslider4 ,s );
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
  d->balance = p->balance;
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

  dt_bauhaus_slider_set(g->gslider1,p->shadow_hue);
  dt_bauhaus_slider_set(g->gslider3,p->highlight_hue);
  dt_bauhaus_slider_set(g->gslider4,p->highlight_saturation);
  dt_bauhaus_slider_set(g->gslider2,p->shadow_saturation);
  dt_bauhaus_slider_set(g->scale1, p->balance*100.0);
  dt_bauhaus_slider_set(g->scale2, p->compress);

  update_colorpicker_fg(GTK_WIDGET(g->colorpick1), p->shadow_hue, p->shadow_saturation);
  update_colorpicker_fg(GTK_WIDGET(g->colorpick2), p->highlight_hue, p->highlight_saturation);
  update_saturation_slider_end_color(g->gslider2, p->shadow_hue);
  update_saturation_slider_end_color(g->gslider4, p->highlight_hue);

  update_balance_slider_colors(g->scale1, p->highlight_hue, p->shadow_hue);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_params = malloc(sizeof(dt_iop_splittoning_params_t));
  module->default_enabled = 0;
  module->priority = 894; // module order created by iop_dependencies.py, do not edit!
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

static inline void
gui_init_tab(
  struct dt_iop_module_t *self,
  const char* name,
  GtkDarktableButton** ppcolor,
  GtkWidget** pphue,
  GtkWidget** ppsaturation)
{
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(dtgtk_label_new(name,DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT)), FALSE, FALSE, 5);

  // color button
  GtkDarktableButton* color;
  *ppcolor = color = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE|CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  gtk_widget_set_size_request(GTK_WIDGET(color),32,32);

  // hue slider
  GtkWidget* hue;
  *pphue = hue =( dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.01f, 0.0f, 2, 0));
  dt_bauhaus_slider_set_stop(hue, 0.0f, 1.0f, 0.0f, 0.0f);
  dt_bauhaus_widget_set_label(hue, NULL, _("hue"));
  dt_bauhaus_slider_set_stop(hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(hue, 1.0f, 1.0f, 0.0f, 0.0f);
  g_object_set(G_OBJECT(hue), "tooltip-text", _("select the hue tone"), (char *)NULL);

  // saturation slider
  GtkWidget* saturation;
  *ppsaturation = saturation = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.0f, 0.01f, 0.0f, 2);
  dt_bauhaus_widget_set_label(saturation, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  g_object_set(G_OBJECT(saturation), "tooltip-text", _("select the saturation tone"), (char *)NULL);

  // pack the widgets
  GtkWidget* vbox = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hue), FALSE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(vbox), GTK_WIDGET(saturation), FALSE, TRUE, 0);

  GtkWidget* hbox = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox), TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(hbox), GTK_WIDGET(color),FALSE,FALSE,0);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_splittoning_gui_data_t));
  dt_iop_splittoning_gui_data_t *g = (dt_iop_splittoning_gui_data_t *)self->gui_data;
  dt_iop_splittoning_params_t *p = (dt_iop_splittoning_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE));

  // Shadows
  gui_init_tab(self, _("shadows"), &g->colorpick1, &g->gslider1, &g->gslider2);

  // Highlights
  gui_init_tab(self, _("highlights"), &g->colorpick2, &g->gslider3, &g->gslider4);

  // Additional parameters
  GtkWidget* hbox=GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  GtkWidget* vbox = GTK_WIDGET(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(vbox), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g->scale1 = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0, 100.0, 0.1, p->balance*100.0, 2, 0);
  dt_bauhaus_slider_set_format(g->scale1, "%.2f");
  dt_bauhaus_slider_set_stop(g->scale1, 0.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_slider_set_stop(g->scale1, 1.0f, 0.5f, 0.5f, 0.5f);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("balance"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);

  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->compress, 2);
  dt_bauhaus_slider_set_format(g->scale2, "%.2f%%");
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("compress"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);


  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the balance of center of splittoning"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("compress the effect on highlights/shadows and\npreserve midtones"), (char *)NULL);

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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
