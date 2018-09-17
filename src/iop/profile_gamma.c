 /*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2014 LebedevRI.
    copyright (c) 2018 Aurélien Pierre.

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_profilegamma_params_t)

typedef struct dt_iop_profilegamma_params_t
{
  float camera_factor;
  float dynamic_range;
  float grey_point;
  float black_level;
  float black_EV;
  float black_target;
} dt_iop_profilegamma_params_t;

typedef struct dt_iop_profilegamma_gui_data_t
{
  GtkWidget *camera_factor;
  GtkWidget *dynamic_range;
  GtkWidget *grey_point;
  GtkWidget *black_level;
  GtkWidget *black_EV;
  GtkWidget *black_target;
} dt_iop_profilegamma_gui_data_t;

typedef struct dt_iop_profilegamma_data_t
{
  float camera_factor;
  float dynamic_range;
  float grey_point;
  float black_level;
  float black_EV;
  float black_target;
} dt_iop_profilegamma_data_t;

typedef struct dt_iop_profilegamma_global_data_t
{
  int kernel_profilegamma;
} dt_iop_profilegamma_global_data_t;

const char *name()
{
  return _("unbreak input profile");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "dynamic_range"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "grey_point"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black_EV"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "black_level"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "camera_factor"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_accel_connect_slider_iop(self, "dynamic_range", GTK_WIDGET(g->dynamic_range));
  dt_accel_connect_slider_iop(self, "grey_point", GTK_WIDGET(g->grey_point));
  dt_accel_connect_slider_iop(self, "black_level", GTK_WIDGET(g->black_level));
  dt_accel_connect_slider_iop(self, "black_EV", GTK_WIDGET(g->black_EV));
  dt_accel_connect_slider_iop(self, "camera_factor", GTK_WIDGET(g->camera_factor));
}

static inline float Log2( float x)
{
  if (x > 0.)
  {
    return logf(x) / logf(2.f);
  }
  else
  { 
    return x;
  }
}

static inline float ThresLog2( float x, float thres)
{
  // Translates the log2 function to the north-west to threshold the noisier values
  // This is to avoid noise amplification in values -> 0. 

  if ( x <= thres )
  {
    return logf(thres) / logf(2.f);
  }
  else
  {
    return logf(x + thres) / logf(2.f) ;
  }
  

}

static inline float Sign( float x)
{
  if (x >= 0.f) {return 1. ;} else {return -1. ;} 
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *data = (dt_iop_profilegamma_data_t *)piece->data;

  const int ch = piece->colors;
  const float Thres = powf(2, data->black_level);
  const float grey = data->grey_point/100.;
  
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
  {
    const float pixel = ((float *)ivoid)[k];
    float lg2 = ThresLog2((data->camera_factor * ( (pixel + powf(2, data->black_level)) / ( grey + powf(2, data->black_level)))) , Thres);
    lg2 = ( (lg2 - data->black_EV ) / (data->dynamic_range) ) ;
    //lg2 = (lg2 - powf(2, data->black_level)) / (1. - powf(2, data->black_level));
    ((float *)ovoid)[k] = lg2;
  }
 
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  
}


static void black_target_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->black_target = dt_bauhaus_slider_get(slider);
  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  
  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
  
    /*******************
    
    This section optimizes the camera parameters (black level correction and camera linearity factor) such that :
      - the target dynamic range (of the color chart used to produce the ICC input profile) is matched,
      - the input grey level is in the center of the histogram,
      - the black point of the ICC profile / color chart is matched,
      - the dynamic range is centered on 0 (black EV = - white EV).
      
    It assumes that :
      - the data will be color-corrected afterwards with an input ICC profile
      - it is better to remap the data dynamic range to the color chart's before the ICC color correction
      - a contrast + gamma curve will be applied at the end of the pixel pipe to recover a proper contrast and saturation
      
    IT 8 and data-color charts have L values between 17 % and 96 %, hence 2.5 EV of dynamic range.
    ***********************/
    dt_iop_request_focus(self);
    dt_lib_colorpicker_set_area(darktable.lib, 1.);
    dt_dev_reprocess_all(self->dev);
  
    const float min[3] = { self->picked_color_min[0], self->picked_color_min[1], self->picked_color_min[2] };
    const float max[3] = { self->picked_color_max[0], self->picked_color_max[1], self->picked_color_max[2] };
    
    float LABmin[3];
    float LABmax[3];
    
    const float RGBmin = fminf(fminf(self->picked_color_min[0], self->picked_color_min[1]), self->picked_color_min[2]);
    const float RGBmax = fmaxf(fmaxf(self->picked_color_max[0], self->picked_color_max[1]), self->picked_color_max[2]);

    dt_prophotorgb_to_Lab((const float *)min, (float *)LABmin);
    dt_prophotorgb_to_Lab((const float *)max, (float *)LABmax);
    
    p->dynamic_range = 32.;
    //p->camera_factor = 2.5;

    float EVmax = Log2( p->camera_factor * (LABmax[0]) / (p->grey_point) );
    float EVmin = Log2( p->camera_factor * (LABmin[0]) / (p->grey_point) );
    
    p->dynamic_range = fabsf(EVmax - EVmin);
    
    // Convert the black level from EV to luminance % to scale for the LAB readings
    float black_level_L = powf(2., -p->dynamic_range) * 100.;

    //int stops =0;
    
    //while (fabsf(EVmin - (fabsf(EVmax - EVmin) / 2.)) > 1e-6 && stops < 1000)
    //{
      black_level_L = (RGBmax - RGBmin * powf(2., p->dynamic_range)) / (powf(2, p->dynamic_range) - 1.) * 100. ;
      
      if (black_level_L > p->grey_point ) { black_level_L = p->grey_point;}
      //if (black_level_L < powf(2., -p->dynamic_range) * 100. ) { black_level_L = powf(2., -p->dynamic_range) * 100.;}
      
      //p->camera_factor = (p->grey_point + black_level_L) / powf((((RGBmax * 100.) + black_level_L) *( (RGBmin * 100.) + black_level_L)), 0.5);

      EVmin = Log2( p->camera_factor * (LABmin[0] + black_level_L) / (p->grey_point + black_level_L) );
      EVmax = Log2( p->camera_factor * (LABmax[0] + black_level_L) / (p->grey_point + black_level_L) );
      
      if (p->camera_factor > 3.) p->camera_factor = 3.;
      if (p->camera_factor < 1.) p->camera_factor = 1.;
        
      //  ++stops;

    //}

    p->black_EV = EVmin - (p->camera_factor * (p->black_target/100.) * fabsf(EVmax - EVmin) );
    p->dynamic_range = fabsf(EVmax - fminf(p->black_EV, EVmin));

    if (p->black_EV > -p->dynamic_range / 2.) p->black_EV = -p->dynamic_range / 2.;
    if (p->black_EV < -16.) p->black_EV = -16.;
    if (p->dynamic_range < 2.) p->dynamic_range = 2.;
    if (p->dynamic_range > 16.) p->dynamic_range = 16.;
    
    p->black_level = Log2(black_level_L/100.) + (p->camera_factor * (p->black_target/100.) * fabsf(EVmax - EVmin)  );
    if (p->black_level > 0.) p->black_level = 0.;
    if (p->black_level < -16. ) p->black_level = -16.;

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
    dt_bauhaus_slider_set_soft(g->black_EV, p->black_EV);
    dt_bauhaus_slider_set_soft(g->black_level, p->black_level);
    dt_bauhaus_slider_set_soft(g->camera_factor, p->camera_factor);
    dt_bauhaus_slider_set_soft(g->black_target, p->black_target);
    darktable.gui->reset = 0;
    
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  }
  else
  {
    dt_control_queue_redraw();
  }
  
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
    
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  
}


static void autogrey_point_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  
  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    dt_iop_request_focus(self);
    dt_lib_colorpicker_set_area(darktable.lib, 1.);
    dt_dev_reprocess_all(self->dev);
    dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  
    const float RGBavg[3] = { self->picked_color[0], self->picked_color[1],  self->picked_color [2] };
    float LABavg[3];

    dt_prophotorgb_to_Lab((const float *)RGBavg, (float *)LABavg);

    p->grey_point = LABavg[0];

    dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

    darktable.gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->grey_point, p->grey_point);
    darktable.gui->reset = 0;
  }
  else
  {
    dt_control_queue_redraw();
  }
  
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_level_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->black_level = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void camera_factor_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->camera_factor = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void grey_point_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->grey_point = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dynamic_range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  
  float previous = p->dynamic_range;
  p->dynamic_range = dt_bauhaus_slider_get(slider);
  float ratio = (p->dynamic_range - previous) / previous;
  p->black_EV = p->black_EV + p->black_EV * ratio;
  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->black_EV, p->black_EV);
  darktable.gui->reset = 0; 
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_ev_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->black_EV = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void autofix_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  else
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_request_focus(self);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)p1;
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  
  d->dynamic_range = p->dynamic_range;
  d->grey_point = p->grey_point;
  d->camera_factor = p->camera_factor;
  d->black_level = p->black_level;
  d->black_EV = p->black_EV;
  d->black_target = p->black_target;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_profilegamma_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)module->params;
  
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  
  dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
  dt_bauhaus_slider_set_soft(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set_soft(g->camera_factor, p->camera_factor);
  dt_bauhaus_slider_set_soft(g->black_level, p->black_level);
  dt_bauhaus_slider_set_soft(g->black_EV, p->black_EV);
  dt_bauhaus_slider_set_soft(g->black_target, p->black_target);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_enabled = 0;
  module->priority = 323; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_profilegamma_params_t);
  module->gui_data = NULL;
  dt_iop_profilegamma_params_t tmp = (dt_iop_profilegamma_params_t){ 2.5, 5., 50., -5., -2.5, 0.};
  memcpy(module->params, &tmp, sizeof(dt_iop_profilegamma_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_profilegamma_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_profilegamma_global_data_t *gd
      = (dt_iop_profilegamma_global_data_t *)malloc(sizeof(dt_iop_profilegamma_global_data_t));
  module->data = gd;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_profilegamma);
  free(module->data);
  module->data = NULL;
}


void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_profilegamma_gui_data_t));
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("camera & signal properties")), FALSE, FALSE, 5);
  
  // camera factor slider
  g->camera_factor = dt_bauhaus_slider_new_with_range(self, 1., 3., 0.01, p->camera_factor, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->camera_factor, 0.01, 32.0);
  dt_bauhaus_widget_set_label(g->camera_factor, NULL, _("exposure linear factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->camera_factor, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->camera_factor, _("increase to keep more contrast and saturation"));
  g_signal_connect(G_OBJECT(g->camera_factor), "value-changed", G_CALLBACK(camera_factor_callback), self);
  //dt_bauhaus_widget_set_quad_paint(g->camera_factor, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  //g_signal_connect(G_OBJECT(g->camera_factor), "quad-pressed", G_CALLBACK(autocamera_factor_callback), self);
  
  // black level input slider
  g->black_level = dt_bauhaus_slider_new_with_range(self, -16., 0., 0.1, p->black_level, 1);
  dt_bauhaus_slider_enable_soft_boundaries(g->black_level, -32., 32.0);
  dt_bauhaus_slider_set_format(g->black_level, "%.1f EV");
  dt_bauhaus_widget_set_label(g->black_level, NULL, _("black level correction"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_level, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->black_level, _("increase if you see strong noise in blacks"));
  g_signal_connect(G_OBJECT(g->black_level), "value-changed", G_CALLBACK(black_level_callback), self);

  
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("scene & lighting properties")), FALSE, FALSE, 5);
  
  // grey_point slider
  g->grey_point = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->grey_point, 2);
  dt_bauhaus_widget_set_label(g->grey_point, NULL, _("middle grey target value"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point, "%.1f %%");
  gtk_widget_set_tooltip_text(g->grey_point, _("adjust to match a neutral tone"));
  g_signal_connect(G_OBJECT(g->grey_point), "value-changed", G_CALLBACK(grey_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->grey_point), "quad-pressed", G_CALLBACK(autogrey_point_callback), self);
  
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("target color profile properties")), FALSE, FALSE, 5);
  
  // Dynamic range slider
  g->dynamic_range = dt_bauhaus_slider_new_with_range(self, 4.0, 16.0, 0.5, p->dynamic_range, 1);
  dt_bauhaus_slider_enable_soft_boundaries(g->dynamic_range, 0.01, 32.0);
  dt_bauhaus_widget_set_label(g->dynamic_range, NULL, _("dynamic range"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->dynamic_range, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->dynamic_range, "%.1f EV");
  gtk_widget_set_tooltip_text(g->dynamic_range, _("stops range between blacks and whites"));
  g_signal_connect(G_OBJECT(g->dynamic_range), "value-changed", G_CALLBACK(dynamic_range_callback), self);

  
  // Dynamic range slider
  g->black_EV = dt_bauhaus_slider_new_with_range(self, -8.0, -0., 0.5, p->black_EV, 1);
  dt_bauhaus_slider_enable_soft_boundaries(g->black_EV, -16., 16.0);
  dt_bauhaus_widget_set_label(g->black_EV, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_EV, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_EV, "%.1f EV");
  gtk_widget_set_tooltip_text(g->black_EV, _("stops range between blacks and grey"));
  g_signal_connect(G_OBJECT(g->black_EV), "value-changed", G_CALLBACK(black_ev_callback), self);
  
  
  
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(_("optimize automatically")), FALSE, FALSE, 5);
  
  
  g->black_target = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->black_target, 1);
  dt_bauhaus_widget_set_label(g->black_target, NULL, _("black point offset"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black_target, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_target, "%.1f %%");
  gtk_widget_set_tooltip_text(g->black_target, _("adjust to match a deep black"));
  g_signal_connect(G_OBJECT(g->black_target), "value-changed", G_CALLBACK(black_target_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_target, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->black_target), "quad-pressed", G_CALLBACK(autofix_callback), self);
}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
