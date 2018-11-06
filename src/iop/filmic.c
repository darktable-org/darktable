/*
   This file is part of darktable,
   copyright (c) 2018 Aurélien Pierre, with guidance of Troy Sobotka.

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
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_filmic_params_t)

typedef enum dt_iop_filmic_mode_t
{
  filmic_LOG = 0,
  filmic_LINEAR = 1
} dt_iop_filmic_mode_t;

typedef enum dt_iop_filmic_pickcolor_type_t
{
  DT_PICKPROFLOG_NONE = 0,
  DT_PICKPROFLOG_GREY_POINT = 1,
  DT_PICKPROFLOG_BLACK_POINT = 2,
  DT_PICKPROFLOG_white_point = 3
} dt_iop_filmic_pickcolor_type_t;

typedef struct dt_iop_filmic_params_t
{
  dt_iop_filmic_mode_t mode;
  float white_point;
  float grey_point;
  float black_point;
  float security_factor;
} dt_iop_filmic_params_t;

typedef struct dt_iop_filmic_gui_data_t
{
  int apply_picked_color;
  int which_colorpicker;

  GtkWidget *mode;
  GtkWidget *mode_stack;
  GtkWidget *white_point;
  GtkWidget *grey_point;
  GtkWidget *black_point;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
} dt_iop_filmic_gui_data_t;

typedef struct dt_iop_filmic_data_t
{
  dt_iop_filmic_mode_t mode;
  float linear;
  float table[0x10000];      // precomputed look-up table
  float unbounded_coeffs[3]; // approximation for extrapolation of curve
  float white_point;
  float grey_point;
  float black_point;
  float security_factor;
} dt_iop_filmic_data_t;

typedef struct dt_iop_filmic_global_data_t
{
  int kernel_filmic;
  int kernel_filmic_log;
} dt_iop_filmic_global_data_t;


const char *name()
{
  return _("filmic");
}

int groups()
{
  return dt_iop_get_group("filmic", IOP_GROUP_COLOR);
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

static inline float Log2(float x)
{
  if(x > 0.0f)
  {
    return logf(x) / logf(2.0f);
  }
  else
  {
    return x;
  }
}

static inline float Log2Thres(float x, float Thres)
{
  if(x > Thres)
  {
    return logf(x) / logf(2.f);
  }
  else
  {
    return logf(Thres) / logf(2.f);
  }
}


// From data/kernels/extended.cl
static inline float fastlog2(float x)
{
  union { float f; unsigned int i; } vx = { x };
  union { unsigned int i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_filmic_data_t *data = (dt_iop_filmic_data_t *)piece->data;

  const int ch = piece->colors;

  /** The log2(x) -> -INF when x -> 0
  * thus very low values (noise) will get even lower, resulting in noise negative amplification,
  * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
  * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a threshold.
  * However, at this point of the pixelpipe, the RAW levels have already been corrected and everything can happen with black levels
  * in the exposure module. So we define the threshold as the first non-null 16 bit integer
  */

  const float noise = powf(2.0f, -16.0f);
  const float dynamic_range = data->white_point - data->black_point;
  const float grey = data->grey_point / 100.0f;
  const float black = data->black_point;
  fprintf(stderr, "%f", grey);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
  for(size_t j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(size_t i = 0; i < roi_out->width; i++)
    {
      // transform the pixel to sRGB:
      // Lab -> XYZ
      float XYZ[3] = { 0.0f };
      dt_Lab_to_XYZ(in, XYZ);

      // XYZ -> sRGB
      float rgb[3] = { 0.0f };
      dt_XYZ_to_prophotorgb(XYZ, rgb);

      // do the calculation in RGB space
      for(size_t c = 0; c < 3; c++)
      {
        float tmp = rgb[c] / grey;
        if (tmp < noise) tmp = noise;
        rgb[c] = (fastlog2(tmp) - black) / dynamic_range;
      }

      // transform the result back to Lab
      // sRGB -> XYZ
      dt_prophotorgb_to_XYZ(rgb, XYZ);

      // XYZ -> Lab
      dt_XYZ_to_Lab(XYZ, out);
      out[3] = in[3];

      in += ch;
      out += ch;
    }
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void apply_auto_grey(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float RGB[3] = { 0.0f };
  dt_Lab_to_prophotorgb(self->picked_color, RGB);

  float grey = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  p->grey_point = 100.f * grey;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  float RGB[3] = { 0.0f };
  dt_Lab_to_prophotorgb(self->picked_color_min, RGB);

  // Black
  float black = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  float EVmin = Log2Thres(black / (p->grey_point / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->black_point, p->black_point);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_white_point(dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);

  float RGB[3] = { 0.0f };
  dt_Lab_to_prophotorgb(self->picked_color_max, RGB);

  // White
  float white = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  float EVmax = Log2Thres(white / (p->grey_point / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->white_point, p->white_point);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void security_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float previous = p->security_factor;
  p->security_factor = dt_bauhaus_slider_get(slider);
  float ratio = (p->security_factor - previous) / (previous + 100.0f);

  float EVmin = p->black_point;
  EVmin = EVmin + ratio * EVmin;

  float EVmax = p->white_point;
  EVmax = EVmax + ratio * EVmax;

  p->white_point = EVmax;
  p->black_point = EVmin;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->white_point, p->white_point);
  dt_bauhaus_slider_set_soft(g->black_point, p->black_point);
  darktable.gui->reset = 0;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void optimize_button_pressed_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  dt_iop_request_focus(self);
  dt_lib_colorpicker_set_area(darktable.lib, 0.99);
  dt_control_queue_redraw();
  self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;
  dt_dev_reprocess_all(self->dev);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
  {
    dt_control_log(_("wait for the preview to be updated."));
    return;
  }

  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  float noise = powf(2.0f, -16.0f);
  float RGB[3] = { 0.0f };

  // Grey
  dt_Lab_to_prophotorgb(self->picked_color, RGB);
  float grey = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  p->grey_point = 100.f * grey;

  // Black
  dt_Lab_to_prophotorgb(self->picked_color_min, RGB);
  float black = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  float EVmin = Log2Thres(black / (p->grey_point / 100.0f), noise);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  // White
  dt_Lab_to_prophotorgb(self->picked_color_max, RGB);
  float white = fmax(fmax(RGB[0], RGB[1]), RGB[2]);
  float EVmax = Log2Thres(white / (p->grey_point / 100.0f), noise);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->black_point = EVmin;
  p->white_point = EVmax;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set(g->black_point, p->black_point);
  dt_bauhaus_slider_set(g->white_point, p->white_point);
  darktable.gui->reset = 0;

  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void grey_point_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->grey_point = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void white_point_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->white_point = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void black_point_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->black_point = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;
  p->mode = dt_bauhaus_combobox_get(combo);

  switch(p->mode)
  {
    case filmic_LOG:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
    case filmic_LINEAR:
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "linear");
      break;
    default:
      p->mode = filmic_LOG;
      gtk_stack_set_visible_child_name(GTK_STACK(g->mode_stack), "log");
      break;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void disable_colorpick(struct dt_iop_module_t *self)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  g->which_colorpicker = DT_PICKPROFLOG_NONE;
}

static int call_apply_picked_color(struct dt_iop_module_t *self, dt_iop_filmic_gui_data_t *g)
{
  int handled = 1;
  switch(g->which_colorpicker)
  {
     case DT_PICKPROFLOG_GREY_POINT:
       apply_auto_grey(self);
       break;
     case DT_PICKPROFLOG_BLACK_POINT:
       apply_auto_black(self);
       break;
     case DT_PICKPROFLOG_white_point:
       apply_auto_white_point(self);
       break;
     default:
       handled = 0;
       break;
  }
  return handled;
}

static int get_colorpick_from_button(GtkWidget *button, dt_iop_filmic_gui_data_t *g)
{
  int which_colorpicker = DT_PICKPROFLOG_NONE;

  if(button == g->grey_point)
    which_colorpicker = DT_PICKPROFLOG_GREY_POINT;
  else if(button == g->black_point)
    which_colorpicker = DT_PICKPROFLOG_BLACK_POINT;
  else if(button == g->white_point)
    which_colorpicker = DT_PICKPROFLOG_white_point;

  return which_colorpicker;
}

static void set_colorpick_state(dt_iop_filmic_gui_data_t *g, const int which_colorpicker)
{
  dt_bauhaus_widget_set_quad_active(g->grey_point, which_colorpicker == DT_PICKPROFLOG_GREY_POINT);
  dt_bauhaus_widget_set_quad_active(g->black_point, which_colorpicker == DT_PICKPROFLOG_BLACK_POINT);
  dt_bauhaus_widget_set_quad_active(g->white_point, which_colorpicker == DT_PICKPROFLOG_white_point);
}

static void color_picker_callback(GtkWidget *button, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
  {
    g->which_colorpicker = get_colorpick_from_button(button, g);

    if(g->which_colorpicker != DT_PICKPROFLOG_NONE)
    {
      dt_iop_request_focus(self);
      self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

      if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF) dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      g->apply_picked_color = 1;

      dt_dev_reprocess_all(self->dev);
    }
  }
  else
  {
    if(self->request_color_pick != DT_REQUEST_COLORPICK_MODULE || self->picked_color_max[0] < 0.0f)
    {
      dt_control_log(_("wait for the preview to be updated."));
      return;
    }
    self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

    if(g->apply_picked_color)
    {
      call_apply_picked_color(self, g);
      g->apply_picked_color = 0;
    }

    const int which_colorpicker = get_colorpick_from_button(button, g);
    if(which_colorpicker != g->which_colorpicker && which_colorpicker != DT_PICKPROFLOG_NONE)
    {
      g->which_colorpicker = which_colorpicker;

      self->request_color_pick = DT_REQUEST_COLORPICK_MODULE;

      if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF) dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      g->apply_picked_color = 1;

      dt_dev_reprocess_all(self->dev);
    }
    else
    {
      g->which_colorpicker = DT_PICKPROFLOG_NONE;
    }
  }

  set_colorpick_state(g, g->which_colorpicker);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(!in)
  {
    disable_colorpick(self);
    g->apply_picked_color = 0;
    set_colorpick_state(g, g->which_colorpicker);
  }
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  int handled = 0;
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF && which == 1)
  {
    handled = call_apply_picked_color(self, g);
    g->apply_picked_color = 0;
  }

  return handled;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)p1;
  dt_iop_filmic_data_t *d = (dt_iop_filmic_data_t *)piece->data;

  d->white_point = p->white_point;
  d->grey_point = p->grey_point;
  d->black_point = p->black_point;
  d->security_factor = p->security_factor;
  d->mode = p->mode;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmic_data_t));
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
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)module->params;

  disable_colorpick(self);

  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_slider_set_soft(g->white_point, p->white_point);
  dt_bauhaus_slider_set_soft(g->grey_point, p->grey_point);
  dt_bauhaus_slider_set_soft(g->black_point, p->black_point);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);

  set_colorpick_state(g, g->which_colorpicker);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_filmic_params_t));
  module->default_enabled = 0;
  module->priority = 642; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_filmic_params_t);
  module->gui_data = NULL;
  dt_iop_filmic_params_t tmp = (dt_iop_filmic_params_t){ 0.18, -5., 10. };
  memcpy(module->params, &tmp, sizeof(dt_iop_filmic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_filmic_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  //const int program = 2; // basic.cl, from programs.conf
  dt_iop_filmic_global_data_t *gd
      = (dt_iop_filmic_global_data_t *)malloc(sizeof(dt_iop_filmic_global_data_t));

  module->data = gd;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  //dt_iop_filmic_global_data_t *gd = (dt_iop_filmic_global_data_t *)module->data;
  free(module->data);
  module->data = NULL;
}


void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_filmic_gui_data_t));
  dt_iop_filmic_gui_data_t *g = (dt_iop_filmic_gui_data_t *)self->gui_data;
  dt_iop_filmic_params_t *p = (dt_iop_filmic_params_t *)self->params;

  disable_colorpick(self);
  g->apply_picked_color = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  // mode choice
  g->mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  dt_bauhaus_combobox_add(g->mode, _("logarithmic"));
  dt_bauhaus_combobox_add(g->mode, _("linear"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->mode), TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->mode, _("tone mapping method"));
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);

  // prepare the modes widgets stack
  g->mode_stack = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(g->mode_stack), FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_stack, TRUE, TRUE, 0);

  /**** LOG MODE ****/

  GtkWidget *vbox_log = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));

  // grey_point slider
  g->grey_point = dt_bauhaus_slider_new_with_range(self, 0.1, 100., 0.5, p->grey_point, 2);
  dt_bauhaus_widget_set_label(g->grey_point, NULL, _("middle grey luminance"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->grey_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->grey_point, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point, _("adjust to match the average luma of the subject"));
  g_signal_connect(G_OBJECT(g->grey_point), "value-changed", G_CALLBACK(grey_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->grey_point, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->grey_point, TRUE);
  g_signal_connect(G_OBJECT(g->grey_point), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Shadows range slider
  g->black_point = dt_bauhaus_slider_new_with_range(self, -16.0, -0.0, 0.1, p->black_point, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->black_point, -16., 16.0);
  dt_bauhaus_widget_set_label(g->black_point, NULL, _("black relative exposure"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->black_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->black_point, "%.2f EV");
  gtk_widget_set_tooltip_text(g->black_point, _("number of stops between middle grey and pure black\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->black_point), "value-changed", G_CALLBACK(black_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->black_point, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->black_point, TRUE);
  g_signal_connect(G_OBJECT(g->black_point), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Dynamic range slider
  g->white_point = dt_bauhaus_slider_new_with_range(self, 0.5, 16.0, 0.1, p->white_point, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->white_point, 0.01, 32.0);
  dt_bauhaus_widget_set_label(g->white_point, NULL, _("white relative exposure"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->white_point, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->white_point, "%.2f EV");
  gtk_widget_set_tooltip_text(g->white_point, _("number of stops between pure black and pure white\nthis is a reading a posemeter would give you on the scene"));
  g_signal_connect(G_OBJECT(g->white_point), "value-changed", G_CALLBACK(white_point_callback), self);
  dt_bauhaus_widget_set_quad_paint(g->white_point, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->white_point, TRUE);
  g_signal_connect(G_OBJECT(g->white_point), "quad-pressed", G_CALLBACK(color_picker_callback), self);

  // Auto tune slider
  gtk_box_pack_start(GTK_BOX(vbox_log), dt_ui_section_label_new(_("optimize automatically")), FALSE, FALSE, 5);
  g->security_factor = dt_bauhaus_slider_new_with_range(self, -100., 100., 0.1, p->security_factor, 2);
  dt_bauhaus_widget_set_label(g->security_factor, NULL, _("security factor"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->security_factor, TRUE, TRUE, 0);
  dt_bauhaus_slider_set_format(g->security_factor, "%.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("enlarge or shrink the computed dynamic range\nthis is usefull when noise perturbates the measurements"));
  g_signal_connect(G_OBJECT(g->security_factor), "value-changed", G_CALLBACK(security_threshold_callback), self);

  g->auto_button = gtk_button_new_with_label(_("auto tune"));
  gtk_widget_set_tooltip_text(g->auto_button, _("make an optimization with some guessing"));
  gtk_box_pack_start(GTK_BOX(vbox_log), g->auto_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->auto_button), "clicked", G_CALLBACK(optimize_button_pressed_callback), self);

  gtk_widget_show_all(vbox_log);
  gtk_stack_add_named(GTK_STACK(g->mode_stack), vbox_log, "log");
}


void gui_cleanup(dt_iop_module_t *self)
{
  disable_colorpick(self);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
