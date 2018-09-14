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
  float black;
  float grey_point;
} dt_iop_profilegamma_params_t;

typedef struct dt_iop_profilegamma_gui_data_t
{
  GtkWidget *camera_factor;
  GtkWidget *dynamic_range;
  GtkWidget *grey_point;
  GtkWidget *black;
} dt_iop_profilegamma_gui_data_t;

typedef struct dt_iop_profilegamma_data_t
{
  float camera_factor;
  float dynamic_range;
  float black;
  float grey_point;
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
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_accel_connect_slider_iop(self, "dynamic_range", GTK_WIDGET(g->dynamic_range));
  dt_accel_connect_slider_iop(self, "grey_point", GTK_WIDGET(g->grey_point));
}

static inline float Log2( float x)
{
  if (x > 0.f)
  {
    return logf(x) / logf(2.f);
  }
  else
  {
    return 0.f;
  }
}


void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *data = (dt_iop_profilegamma_data_t *)piece->data;

  const int ch = piece->colors;
  

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(data) schedule(static)
#endif
      for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
      {
        if (((float *)ivoid)[k] <= 0.f)
        {
          ((float *)ovoid)[k] = 0.;
        }
        else
        {
          float lg2 = data->camera_factor * Log2( ( ((float *)ivoid)[k] + powf(2, data->black)) / ( data->grey_point/100. + powf(2, data->black)));
          lg2 = (lg2 - data->black) / (data->dynamic_range);
          if (lg2 > 0)
          {
            ((float *)ovoid)[k] = lg2;
          }
          else
          {
            ((float *)ovoid)[k] = 0.;
          }
        }
      }
 
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  
}



static void autoblack_callback(GtkWidget *button, gpointer user_data)
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
  {
    dt_control_queue_redraw();
  }
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  
  const float min[3] = { self->picked_color_min[0], self->picked_color_min[1], self->picked_color_min[2] };
  const float max[3] = { self->picked_color_max[0], self->picked_color_max[1], self->picked_color_max[2] };
  float LABmin[3];
  float LABmax[3];
  
  dt_prophotorgb_to_Lab((const float *)min, (float *)LABmin);
  dt_prophotorgb_to_Lab((const float *)max, (float *)LABmax);
  
  // iterate until convergence
  for(int k = 0; k < 100 ; k++)
  {
    /*p->dynamic_range = fabsf( Log2( ( LABmax[0] + powf(2, p->black) ) / ( p->grey_point + powf(2, p->black) ) ) ) 
      + fabsf(Log2( ( LABmin[0]+powf(2, p->black) ) / ( p->grey_point + powf(2, p->black) ) ) );*/
    p->dynamic_range = fabsf( Log2( LABmax[0] / p->grey_point/100. ) ) 
      + fabsf(Log2( LABmin[0] / p->grey_point/100. ) );
    p->black = -p->dynamic_range / 2.;
  }
  
  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->dynamic_range, p->dynamic_range);
  dt_bauhaus_slider_set_soft(g->black, p->black);
  darktable.gui->reset = 0;
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void autogrey_point_callback(GtkWidget *button, gpointer user_data)
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
    dt_lib_colorpicker_set_area(darktable.lib, 0.85);
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    dt_control_queue_redraw();
  }
  
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  
  const float RGBavg[3] = { self->picked_color[0], self->picked_color[1],  self->picked_color [2] };
  float LABavg[3];
  
  dt_prophotorgb_to_Lab((const float *)RGBavg, (float *)LABavg);
  
  p->grey_point = LABavg[0];
  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->grey_point, p->grey_point);
  darktable.gui->reset = 0;
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/*
static void autocamera_factor_callback(GtkWidget *button, gpointer user_data)
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
    dt_lib_colorpicker_set_area(darktable.lib, 0.85);
    dt_dev_reprocess_all(self->dev);
  }
  else
  {
    dt_control_queue_redraw();
  }
  
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  
  const float RGBavg[3] = { self->picked_color[0], self->picked_color[1],  self->picked_color [2] };
  float LABavg[3];
  
  
  dt_prophotorgb_to_Lab((const float *)RGBavg, (float *)LABavg);
  
  p->camera_factor = (p->dynamic_range /2. + p->black) / Log2( (LABavg[0]/100. + powf(2, p->black)) / (p->grey_point/100. + powf(2, p->black)));

  
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->camera_factor, p->camera_factor);
  darktable.gui->reset = 0;
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
*/

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

static void black_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->black = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void dynamic_range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->dynamic_range = dt_bauhaus_slider_get(slider);
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
  d->black = p->black;
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
  
  dt_bauhaus_slider_set(g->dynamic_range, p->dynamic_range);
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set(g->camera_factor, p->camera_factor);
 dt_bauhaus_slider_set(g->black, p->black);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_enabled = 0;
  module->priority = 323; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_profilegamma_params_t);
  module->gui_data = NULL;
  dt_iop_profilegamma_params_t tmp = (dt_iop_profilegamma_params_t){ 1.5, 14., -7., 18.};
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

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  
  // grey_point slider
  g->grey_point = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.10, p->grey_point, 2);
  dt_bauhaus_widget_set_label(g->grey_point, NULL, _("middle grey value"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->grey_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point, "%.1f %%");
  gtk_widget_set_tooltip_text(g->grey_point, _("adjust to match a neutral tone"));
  g_signal_connect(G_OBJECT(g->grey_point), "value-changed", G_CALLBACK(grey_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->grey_point), "quad-pressed", G_CALLBACK(autogrey_point_callback), self);
  
  // Dynamic range slider
  g->dynamic_range = dt_bauhaus_slider_new_with_range(self, 4.0, 16.0, 0.05, p->dynamic_range, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->dynamic_range, 1., 32.0);
  dt_bauhaus_widget_set_label(g->dynamic_range, NULL, _("dynamic range"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->dynamic_range, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->dynamic_range, "%.2f EV");
  gtk_widget_set_tooltip_text(g->dynamic_range, _("stops range between blacks and whites"));
  g_signal_connect(G_OBJECT(g->dynamic_range), "value-changed", G_CALLBACK(dynamic_range_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->dynamic_range, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  g_signal_connect(G_OBJECT(g->dynamic_range), "quad-pressed", G_CALLBACK(autoblack_callback), self);
  
  // blac slider
  g->black = dt_bauhaus_slider_new_with_range(self, -8., 8., 0.05, p->black, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->black, -16., 16.0);
  dt_bauhaus_widget_set_label(g->black, NULL, _("black offset from the middle gray"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->black, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black, "%.2f EV");
  gtk_widget_set_tooltip_text(g->black, _("width of the range "));
  g_signal_connect(G_OBJECT(g->black), "value-changed", G_CALLBACK(black_callback), self);

  
  // camera factor slider
  g->camera_factor = dt_bauhaus_slider_new_with_range(self, 1., 4., 0.01, p->camera_factor, 2);
  dt_bauhaus_widget_set_label(g->camera_factor, NULL, _("linearity factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->camera_factor, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->camera_factor, _("increase to rocover contrast and saturation"));
  g_signal_connect(G_OBJECT(g->camera_factor), "value-changed", G_CALLBACK(camera_factor_callback), self);
  //dt_bauhaus_widget_set_quad_paint(g->camera_factor, dtgtk_cairo_paint_colorpicker, CPF_ACTIVE, NULL);
  //g_signal_connect(G_OBJECT(g->camera_factor), "quad-pressed", G_CALLBACK(autocamera_factor_callback), self);


}


void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
