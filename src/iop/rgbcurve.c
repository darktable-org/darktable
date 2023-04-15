/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.

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
#include "common/iop_profile.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/rgb_norms.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/presets.h"
#include "gui/accelerators.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_IOP_RGBCURVE_RES 256
#define DT_IOP_RGBCURVE_MAXNODES 20
#define DT_IOP_RGBCURVE_MIN_X_DISTANCE 0.0025f
// max iccprofile file name length
// must be in synch with filename in dt_colorspaces_color_profile_t in colorspaces.h
#define DT_IOP_COLOR_ICC_LEN 512

DT_MODULE_INTROSPECTION(1, dt_iop_rgbcurve_params_t)

typedef enum rgbcurve_channel_t
{
  DT_IOP_RGBCURVE_R = 0,
  DT_IOP_RGBCURVE_G = 1,
  DT_IOP_RGBCURVE_B = 2,
  DT_IOP_RGBCURVE_MAX_CHANNELS = 3
} rgbcurve_channel_t;

typedef enum dt_iop_rgbcurve_autoscale_t
{
  DT_S_SCALE_AUTOMATIC_RGB = 0, // $DESCRIPTION: "RGB, linked channels"
  DT_S_SCALE_MANUAL_RGB = 1     // $DESCRIPTION: "RGB, independent channels"
} dt_iop_rgbcurve_autoscale_t;

typedef struct dt_iop_rgbcurve_node_t
{
  float x; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
  float y; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_rgbcurve_node_t;

typedef struct dt_iop_rgbcurve_params_t
{
  dt_iop_rgbcurve_node_t curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS]
                                    [DT_IOP_RGBCURVE_MAXNODES]; // actual nodes for each curve
  int curve_num_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS]; // $DEFAULT: 2 number of nodes per curve
  int curve_type[DT_IOP_RGBCURVE_MAX_CHANNELS]; // $DEFAULT: MONOTONE_HERMITE (CATMULL_ROM, MONOTONE_HERMITE, CUBIC_SPLINE)
  dt_iop_rgbcurve_autoscale_t curve_autoscale;  // $DEFAULT: DT_S_SCALE_AUTOMATIC_RGB $DESCRIPTION: "mode"
  gboolean compensate_middle_grey; // $DEFAULT: 0  $DESCRIPTION: "compensate middle gray" scale the curve and histogram so middle gray is at .5
  dt_iop_rgb_norms_t preserve_colors; // $DEFAULT: DT_RGB_NORM_LUMINANCE $DESCRIPTION: "preserve colors"
} dt_iop_rgbcurve_params_t;

typedef struct dt_iop_rgbcurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[DT_IOP_RGBCURVE_MAX_CHANNELS]; // curves for gui to draw
  int minmax_curve_nodes[DT_IOP_RGBCURVE_MAX_CHANNELS];
  int minmax_curve_type[DT_IOP_RGBCURVE_MAX_CHANNELS];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkWidget *autoscale; // (DT_S_SCALE_MANUAL_RGB, DT_S_SCALE_AUTOMATIC_RGB)
  GtkNotebook *channel_tabs;
  GtkWidget *colorpicker;
  GtkWidget *colorpicker_set_values;
  GtkWidget *interpolator;
  rgbcurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_ys[DT_IOP_RGBCURVE_RES];
  float draw_min_ys[DT_IOP_RGBCURVE_RES];
  float draw_max_ys[DT_IOP_RGBCURVE_RES];
  GtkWidget *chk_compensate_middle_grey;
  GtkWidget *cmb_preserve_colors;
  float zoom_factor;
  float offset_x, offset_y;
} dt_iop_rgbcurve_gui_data_t;

typedef struct dt_iop_rgbcurve_data_t
{
  dt_iop_rgbcurve_params_t params;
  dt_draw_curve_t *curve[DT_IOP_RGBCURVE_MAX_CHANNELS];    // curves for pipe piece and pixel processing
  float table[DT_IOP_RGBCURVE_MAX_CHANNELS][0x10000];      // precomputed look-up tables for tone curve
  float unbounded_coeffs[DT_IOP_RGBCURVE_MAX_CHANNELS][3]; // approximation for extrapolation
  int curve_changed[DT_IOP_RGBCURVE_MAX_CHANNELS];         // curve type or number of nodes changed?
  dt_colorspaces_color_profile_type_t type_work; // working color profile
  char filename_work[DT_IOP_COLOR_ICC_LEN];
} dt_iop_rgbcurve_data_t;

typedef float (*_curve_table_ptr)[0x10000];
typedef float (*_coeffs_table_ptr)[3];

typedef struct dt_iop_rgbcurve_global_data_t
{
  int kernel_rgbcurve;
} dt_iop_rgbcurve_global_data_t;

const char *name()
{
  return _("rgb curve");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_GRADING;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("alter an image’s tones using curves in RGB color space"),
                                      _("corrective and creative"),
                                      _("linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("linear, RGB, display-referred"));
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_rgbcurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 6;
  p.curve_num_nodes[DT_IOP_RGBCURVE_G] = 7;
  p.curve_num_nodes[DT_IOP_RGBCURVE_B] = 7;
  p.curve_type[DT_IOP_RGBCURVE_R] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_G] = CUBIC_SPLINE;
  p.curve_type[DT_IOP_RGBCURVE_B] = CUBIC_SPLINE;
  p.curve_autoscale = DT_S_SCALE_AUTOMATIC_RGB;
  p.compensate_middle_grey = TRUE;
  p.preserve_colors = 1;

  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_G][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_B][k].y = linear_ab[k];

  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].x = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].x = 0.003862;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].x = 0.076613;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].x = 0.169355;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].x = 0.774194;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].x = 1.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][0].y = 0.000000;
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y = 0.007782;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y = 0.156182;
  p.curve_nodes[DT_IOP_RGBCURVE_R][3].y = 0.290352;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y = 0.773852;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y = 1.000000;
  dt_gui_presets_add_generic(_("contrast compression"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  p.curve_num_nodes[DT_IOP_RGBCURVE_R] = 7;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  // Linear - no contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("gamma 1.0 (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Linear contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  dt_gui_presets_add_generic(_("contrast - med (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  dt_gui_presets_add_generic(_("contrast - high (linear)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma contrast
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.020;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.030;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.020;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - med (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];
  p.curve_nodes[DT_IOP_RGBCURVE_R][1].y -= 0.040;
  p.curve_nodes[DT_IOP_RGBCURVE_R][2].y -= 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][4].y += 0.060;
  p.curve_nodes[DT_IOP_RGBCURVE_R][5].y += 0.040;
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].x, 2.2f);
  for(int k = 1; k < 6; k++)
    p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(p.curve_nodes[DT_IOP_RGBCURVE_R][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - high (gamma 2.2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  /** for pure power-like functions, we need more nodes close to the bounds**/

  p.curve_type[DT_IOP_RGBCURVE_R] = MONOTONE_HERMITE;

  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = linear_L[k];

  // Gamma 2.0 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(linear_L[k], 2.0f);
  dt_gui_presets_add_generic(_("gamma 2.0"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Gamma 0.5 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(linear_L[k], 0.5f);
  dt_gui_presets_add_generic(_("gamma 0.5"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Log2 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = logf(linear_L[k] + 1.0f) / logf(2.0f);
  dt_gui_presets_add_generic(_("logarithm (base 2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  // Exp2 - no contrast
  for(int k = 1; k < 6; k++) p.curve_nodes[DT_IOP_RGBCURVE_R][k].y = powf(2.0f, linear_L[k]) - 1.0f;
  dt_gui_presets_add_generic(_("exponential (base 2)"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
}

static float _curve_to_mouse(const float x, const float zoom_factor, const float offset)
{
  return (x - offset) * zoom_factor;
}

static float _mouse_to_curve(const float x, const float zoom_factor, const float offset)
{
  return (x / zoom_factor) + offset;
}

static void picker_scale(const float *const in, float *out, dt_iop_rgbcurve_params_t *p,
                         const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(p->curve_autoscale)
  {
    case DT_S_SCALE_MANUAL_RGB:
      if(p->compensate_middle_grey && work_profile)
      {
        for(int c = 0; c < 3; c++) out[c] = dt_ioppr_compensate_middle_grey(in[c], work_profile);
      }
      else
      {
        for(int c = 0; c < 3; c++) out[c] = in[c];
      }
      break;
    case DT_S_SCALE_AUTOMATIC_RGB:
    {
      const float val
          = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(in,
                                                               work_profile->matrix_in,
                                                               work_profile->lut_in,
                                                               work_profile->unbounded_coeffs_in,
                                                               work_profile->lutsize,
                                                               work_profile->nonlinearlut)
                           : dt_camera_rgb_luminance(in);
      if(p->compensate_middle_grey && work_profile)
      {
        out[0] = dt_ioppr_compensate_middle_grey(val, work_profile);
      }
      else
      {
        out[0] = val;
      }
      out[1] = out[2] = 0.f;
    }
    break;
  }

  for(int c = 0; c < 3; c++) out[c] = CLAMP(out[c], 0.0f, 1.0f);
}

static void _rgbcurve_show_hide_controls(dt_iop_rgbcurve_params_t *p, dt_iop_rgbcurve_gui_data_t *g)
{
  gtk_notebook_set_show_tabs(g->channel_tabs, p->curve_autoscale == DT_S_SCALE_MANUAL_RGB);

  gtk_widget_set_visible(g->cmb_preserve_colors, p->curve_autoscale == DT_S_SCALE_AUTOMATIC_RGB);
}

static gboolean _is_identity(dt_iop_rgbcurve_params_t *p, rgbcurve_channel_t channel)
{
  for(int k=0; k<p->curve_num_nodes[channel]; k++)
    if(p->curve_nodes[channel][k].x != p->curve_nodes[channel][k].y) return FALSE;

  return TRUE;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  if(w == g->autoscale)
  {
    g->channel = DT_IOP_RGBCURVE_R;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), DT_IOP_RGBCURVE_R);

    _rgbcurve_show_hide_controls(p, g);

    // switching to manual scale, if G and B not touched yet, just make them identical to global setting (R)
    if(p->curve_autoscale == DT_S_SCALE_MANUAL_RGB
      && _is_identity(p, DT_IOP_RGBCURVE_G)
      && _is_identity(p, DT_IOP_RGBCURVE_B))
    {
      for(int k=0; k<DT_IOP_RGBCURVE_MAXNODES; k++)
        p->curve_nodes[DT_IOP_RGBCURVE_G][k]
          = p->curve_nodes[DT_IOP_RGBCURVE_B][k] = p->curve_nodes[DT_IOP_RGBCURVE_R][k];

      p->curve_num_nodes[DT_IOP_RGBCURVE_G] = p->curve_num_nodes[DT_IOP_RGBCURVE_B]
        = p->curve_num_nodes[DT_IOP_RGBCURVE_R];
      p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = p->curve_type[DT_IOP_RGBCURVE_R];
    }
  }
  else if(w == g->chk_compensate_middle_grey)
  {
    const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
    if(work_profile == NULL) return;

    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      {
        if(p->compensate_middle_grey)
        {
          // we transform the curve nodes from the image colorspace to lab
          p->curve_nodes[ch][k].x = dt_ioppr_compensate_middle_grey(p->curve_nodes[ch][k].x, work_profile);
          p->curve_nodes[ch][k].y = dt_ioppr_compensate_middle_grey(p->curve_nodes[ch][k].y, work_profile);
        }
        else
        {
          // we transform the curve nodes from lab to the image colorspace
          p->curve_nodes[ch][k].x = dt_ioppr_uncompensate_middle_grey(p->curve_nodes[ch][k].x, work_profile);
          p->curve_nodes[ch][k].y = dt_ioppr_uncompensate_middle_grey(p->curve_nodes[ch][k].y, work_profile);
        }
      }
    }

    self->histogram_middle_grey = p->compensate_middle_grey;
  }
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  const int combo = dt_bauhaus_combobox_get(widget);

  if(combo == 0)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = CUBIC_SPLINE;
  else if(combo == 1)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = CATMULL_ROM;
  else if(combo == 2)
    p->curve_type[DT_IOP_RGBCURVE_R] = p->curve_type[DT_IOP_RGBCURVE_G] = p->curve_type[DT_IOP_RGBCURVE_B]
        = MONOTONE_HERMITE;

  dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void tab_switch_callback(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  g->channel = (rgbcurve_channel_t)page_num;

  gtk_widget_queue_draw(self->widget);
}

static inline int _add_node(dt_iop_rgbcurve_node_t *curve_nodes, int *nodes, float x, float y)
{
  int selected = -1;
  if(curve_nodes[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(curve_nodes[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    curve_nodes[i].x = curve_nodes[i - 1].x;
    curve_nodes[i].y = curve_nodes[i - 1].y;
  }
  // found a new point
  curve_nodes[selected].x = x;
  curve_nodes[selected].y = y;
  (*nodes)++;
  return selected;
}

static inline int _add_node_from_picker(dt_iop_rgbcurve_params_t *p, const float *const in, const float increment,
                                        const int ch, const dt_iop_order_iccprofile_info_t *const work_profile)
{
  float x = 0.f;
  float y = 0.f;
  float val = 0.f;

  if(p->curve_autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    val = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(in,
                                                             work_profile->matrix_in,
                                                             work_profile->lut_in,
                                                             work_profile->unbounded_coeffs_in,
                                                             work_profile->lutsize,
                                                             work_profile->nonlinearlut)
                         : dt_camera_rgb_luminance(in);
  else
    val = in[ch];

  if(p->compensate_middle_grey && work_profile)
    y = x = dt_ioppr_compensate_middle_grey(val, work_profile);
  else
    y = x = val;

  x -= increment;
  y += increment;

  CLAMP(x, 0.f, 1.f);
  CLAMP(y, 0.f, 1.f);

  return _add_node(p->curve_nodes[ch], &p->curve_num_nodes[ch], x, y);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  if(picker == g->colorpicker_set_values)
  {
    dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
    dt_iop_rgbcurve_params_t *d = (dt_iop_rgbcurve_params_t *)self->default_params;

    const int ch = g->channel;
    const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

    // reset current curve
    p->curve_num_nodes[ch] = d->curve_num_nodes[ch];
    p->curve_type[ch] = d->curve_type[ch];
    for(int k = 0; k < DT_IOP_RGBCURVE_MAXNODES; k++)
    {
      p->curve_nodes[ch][k].x = d->curve_nodes[ch][k].x;
      p->curve_nodes[ch][k].y = d->curve_nodes[ch][k].y;
    }

    const GdkModifierType state = dt_key_modifier_state();
    int picker_set_upper_lower; // flat=0, lower=-1, upper=1
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
      picker_set_upper_lower = 1;
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
      picker_set_upper_lower = -1;
    else
      picker_set_upper_lower = 0;

    // now add 4 nodes: min, avg, center, max
    const float increment = 0.05f * picker_set_upper_lower;

    _add_node_from_picker(p, self->picked_color_min, 0.f, ch, work_profile);
    _add_node_from_picker(p, self->picked_color, increment, ch, work_profile);
    _add_node_from_picker(p, self->picked_color_max, 0.f, ch, work_profile);

    if(p->curve_num_nodes[ch] == 5)
      _add_node(p->curve_nodes[ch], &p->curve_num_nodes[ch],
                p->curve_nodes[ch][1].x - increment + (p->curve_nodes[ch][3].x - p->curve_nodes[ch][1].x) / 2.f,
                p->curve_nodes[ch][1].y + increment + (p->curve_nodes[ch][3].y - p->curve_nodes[ch][1].y) / 2.f);

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  dt_control_queue_redraw_widget(self->widget);
}

static gboolean _sanity_check(const float x, const int selected, const int nodes,
                              const dt_iop_rgbcurve_node_t *curve)
{
  gboolean point_valid = TRUE;

  // check if it is not too close to other node
  const float min_dist = DT_IOP_RGBCURVE_MIN_X_DISTANCE; // in curve coordinates
  if((selected > 0 && x - curve[selected - 1].x <= min_dist)
     || (selected < nodes - 1 && curve[selected + 1].x - x <= min_dist))
    point_valid = FALSE;

  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((selected > 0 && (curve[selected - 1].x >= x)) || (selected < nodes - 1 && (curve[selected + 1].x <= x)))
  {
    point_valid = FALSE;
  }

  return point_valid;
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  const int ch = g->channel;
  dt_iop_rgbcurve_node_t *curve = p->curve_nodes[ch];

  float multiplier = dt_accel_get_speed_multiplier(widget, state);
  dx *= multiplier;
  dy *= multiplier;

  const float new_x = CLAMP(curve[g->selected].x + dx, 0.0f, 1.0f);
  const float new_y = CLAMP(curve[g->selected].y + dy, 0.0f, 1.0f);

  gtk_widget_queue_draw(widget);

  if(_sanity_check(new_x, g->selected, p->curve_num_nodes[ch], p->curve_nodes[ch]))
  {
    curve[g->selected].x = new_x;
    curve[g->selected].y = new_y;

    dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
  }

  return TRUE;
}

#define RGBCURVE_DEFAULT_STEP (0.001f)

static gboolean _area_scrolled_callback(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  gdouble delta_y;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  if(darktable.develop->darkroom_skip_mouse_events)
  {
    if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
    {
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);

      const float mx = g->mouse_x;
      const float my = g->mouse_y;
      const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x),
                  liny = _mouse_to_curve(my, g->zoom_factor, g->offset_y);

      g->zoom_factor *= 1.0 - 0.1 * delta_y;
      if(g->zoom_factor < 1.f) g->zoom_factor = 1.f;

      g->offset_x = linx - (mx / g->zoom_factor);
      g->offset_y = liny - (my / g->zoom_factor);

      g->offset_x = CLAMP(g->offset_x, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);
      g->offset_y = CLAMP(g->offset_y, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);

      gtk_widget_queue_draw(self->widget);
    }

    return TRUE;
  }

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) return TRUE;

  if(g->selected < 0) return TRUE;

  dt_iop_color_picker_reset(self, TRUE);

  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -RGBCURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean _area_key_press_callback(GtkWidget *widget, GdkEventKey *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  if(darktable.develop->darkroom_skip_mouse_events) return FALSE;

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) return TRUE;

  if(g->selected < 0) return FALSE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = RGBCURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -RGBCURVE_DEFAULT_STEP;
  }

  if(!handled) return FALSE;

  dt_iop_color_picker_reset(self, TRUE);
  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef RGBCURVE_DEFAULT_STEP

static gboolean _area_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  if(!(event->state & GDK_BUTTON1_MASK))
    g->selected = -1;

  gtk_widget_queue_draw(widget);
  return FALSE;
}

static gboolean _area_draw_callback(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_develop_t *dev = darktable.develop;

  const int ch = g->channel;
  const int nodes = p->curve_num_nodes[ch];
  const int autoscale = p->curve_autoscale;
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  if(g->minmax_curve_type[ch] != p->curve_type[ch] || g->minmax_curve_nodes[ch] != p->curve_num_nodes[ch])
  {
    dt_draw_curve_destroy(g->minmax_curve[ch]);
    g->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->curve_type[ch]);
    g->minmax_curve_nodes[ch] = p->curve_num_nodes[ch];
    g->minmax_curve_type[ch] = p->curve_type[ch];
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(g->minmax_curve[ch], p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }
  else
  {
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      dt_draw_curve_set_point(g->minmax_curve[ch], k, p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }
  dt_draw_curve_t *minmax_curve = g->minmax_curve[ch];
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_RGBCURVE_RES, NULL, g->draw_ys);

  float unbounded_coeffs[3];
  const float xm = curve_nodes[nodes - 1].x;
  {
    const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
    const float y[4] = { g->draw_ys[CLAMP((int)(x[0] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[1] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[2] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)],
                         g->draw_ys[CLAMP((int)(x[3] * DT_IOP_RGBCURVE_RES), 0, DT_IOP_RGBCURVE_RES - 1)] };
    dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);
  }

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;
  char text[256];

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);

  cairo_translate(cr, 0, height);

  dt_draw_grid_zoomed(cr, 4, 0.f, 0.f, 1.f, 1.f, width, height, g->zoom_factor, g->offset_x, g->offset_y);

  const double dashed[] = { 4.0, 4.0 };
  const int len = sizeof(dashed) / sizeof(dashed[0]);
  cairo_set_dash(cr, dashed, len, 0);
  dt_draw_grid_zoomed(cr, 8, 0.f, 0.f, 1.f, 1.f, width, height, g->zoom_factor, g->offset_x, g->offset_y);
  cairo_set_dash(cr, dashed, 0, 0);

  // draw identity line
  cairo_move_to(cr, _curve_to_mouse(0.f, g->zoom_factor, g->offset_x) * width,
                _curve_to_mouse(0.f, g->zoom_factor, g->offset_y) * -height);
  cairo_line_to(cr, _curve_to_mouse(1.f, g->zoom_factor, g->offset_x) * width,
                _curve_to_mouse(1.f, g->zoom_factor, g->offset_y) * -height);
  cairo_stroke(cr);

  // if autoscale is on: do not display g and b curves
  if((autoscale != DT_S_SCALE_MANUAL_RGB) && ch != DT_IOP_RGBCURVE_R) goto finally;

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);

  for(int k = 0; k < nodes; k++)
  {
    const float x = _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(g->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw histogram in background
  // only if module is enabled
  if(self->enabled)
  {
    const uint32_t *hist = self->histogram;
    const gboolean is_linear = darktable.lib->proxy.histogram.is_linear;
    float hist_max;

    if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
      hist_max = fmaxf(self->histogram_max[DT_IOP_RGBCURVE_R], fmaxf(self->histogram_max[DT_IOP_RGBCURVE_G],self->histogram_max[DT_IOP_RGBCURVE_B]));
    else
      hist_max = self->histogram_max[ch];

    if(!is_linear)
      hist_max = logf(1.0 + hist_max);

    if(hist && hist_max > 0.0f)
    {
      cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
      cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);

      if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
      {
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
        for(int k=DT_IOP_RGBCURVE_R; k<DT_IOP_RGBCURVE_MAX_CHANNELS; k++)
        {
          set_color(cr, darktable.bauhaus->graph_colors[k]);
          dt_draw_histogram_8_zoomed(cr, hist, 4, k, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                     is_linear);
        }
      }
      else if(autoscale == DT_S_SCALE_MANUAL_RGB)
      {
        set_color(cr, darktable.bauhaus->graph_colors[ch]);
        dt_draw_histogram_8_zoomed(cr, hist, 4, ch, g->zoom_factor, g->offset_x * 255.0, g->offset_y * hist_max,
                                   is_linear);
      }

      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.2);
    }

    if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
    {
      const dt_iop_order_iccprofile_info_t *const work_profile
          = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

      dt_aligned_pixel_t picker_mean, picker_min, picker_max;

      // the global live samples ...
      GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      if(samples)
      {
        const dt_iop_order_iccprofile_info_t *const histogram_profile = dt_ioppr_get_histogram_profile_info(dev);
        if(work_profile && histogram_profile)
        {
          for(; samples; samples = g_slist_next(samples))
          {
            dt_colorpicker_sample_t *sample = samples->data;

            // this functions need a 4c image
            for(int k = 0; k < 3; k++)
            {
              picker_mean[k] = sample->scope[DT_PICK_MEAN][k];
              picker_min[k] = sample->scope[DT_PICK_MIN][k];
              picker_max[k] = sample->scope[DT_PICK_MAX][k];
            }
            picker_mean[3] = picker_min[3] = picker_max[3] = 1.f;

            dt_ioppr_transform_image_colorspace_rgb(picker_mean, picker_mean, 1, 1, histogram_profile,
                                                    work_profile, "rgb curve");
            dt_ioppr_transform_image_colorspace_rgb(picker_min, picker_min, 1, 1, histogram_profile, work_profile,
                                                    "rgb curve");
            dt_ioppr_transform_image_colorspace_rgb(picker_max, picker_max, 1, 1, histogram_profile, work_profile,
                                                    "rgb curve");

            picker_scale(picker_mean, picker_mean, p, work_profile);
            picker_scale(picker_min, picker_min, p, work_profile);
            picker_scale(picker_max, picker_max, p, work_profile);

            // Convert abcissa to log coordinates if needed
            picker_min[ch] = _curve_to_mouse(picker_min[ch], g->zoom_factor, g->offset_x);
            picker_max[ch] = _curve_to_mouse(picker_max[ch], g->zoom_factor, g->offset_x);
            picker_mean[ch] = _curve_to_mouse(picker_mean[ch], g->zoom_factor, g->offset_x);

            cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.15);
            cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                            -height);
            cairo_fill(cr);
            cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.5);
            cairo_move_to(cr, width * picker_mean[ch], 0);
            cairo_line_to(cr, width * picker_mean[ch], -height);
            cairo_stroke(cr);
          }
      }
      }

      // ... and the local sample
      if(self->picked_color_max[ch] >= 0.0f)
      {
        PangoLayout *layout;
        PangoRectangle ink;
        PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        pango_font_description_set_absolute_size(desc, PANGO_SCALE);
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);

        picker_scale(self->picked_color, picker_mean, p, work_profile);
        picker_scale(self->picked_color_min, picker_min, p, work_profile);
        picker_scale(self->picked_color_max, picker_max, p, work_profile);

        // scale conservatively to 100% of width:
        snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);

        picker_min[ch] = _curve_to_mouse(picker_min[ch], g->zoom_factor, g->offset_x);
        picker_max[ch] = _curve_to_mouse(picker_max[ch], g->zoom_factor, g->offset_x);
        picker_mean[ch] = _curve_to_mouse(picker_mean[ch], g->zoom_factor, g->offset_x);

        cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.33);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);

        picker_scale(self->picked_color, picker_mean, p, work_profile);
        picker_scale(self->picked_output_color, picker_min, p, work_profile);
        snprintf(text, sizeof(text), "%.1f → %.1f", picker_mean[ch] * 255.f, picker_min[ch] * 255.f);

        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.04) * height);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, 0.02f * width, -0.94 * height - ink.height - ink.y);
        pango_cairo_show_layout(cr, layout);
        cairo_stroke(cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
      }
    }
  }

  // draw zoom info
  if(darktable.develop->darkroom_skip_mouse_events)
  {
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "zoom: 100 x: 100 y: 100");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    snprintf(text, sizeof(text), "zoom: %i x: %i y: %i", (int)((g->zoom_factor - 1.f) * 100.f),
             (int)(g->offset_x * 100.f), (int)(g->offset_y * 100.f));

    cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, 0.5);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  else if(g->selected >= 0)
  {
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width * 1.0 / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    const float min_scale_value = 0.0f;
    const float max_scale_value = 255.0f;

    const float x_node_value = curve_nodes[g->selected].x * (max_scale_value - min_scale_value) + min_scale_value;
    const float y_node_value = curve_nodes[g->selected].y * (max_scale_value - min_scale_value) + min_scale_value;
    const float d_node_value = y_node_value - x_node_value;
    snprintf(text, sizeof(text), "%.1f / %.1f ( %+.1f)", x_node_value, y_node_value, d_node_value);

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

    // enlarge selected node
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);

  const float y_offset = _curve_to_mouse(g->draw_ys[0], g->zoom_factor, g->offset_y);
  cairo_move_to(cr, 0, -height * y_offset);

  for(int k = 1; k < DT_IOP_RGBCURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_RGBCURVE_RES - 1.0f);
    float yy;

    if(xx > xm)
    {
      yy = dt_iop_eval_exp(unbounded_coeffs, xx);
    }
    else
    {
      yy = g->draw_ys[k];
    }

    const float x = _curve_to_mouse(xx, g->zoom_factor, g->offset_x),
                y = _curve_to_mouse(yy, g->zoom_factor, g->offset_y);

    cairo_line_to(cr, x * width, -height * y);
  }
  cairo_stroke(cr);

finally:
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _area_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;

  // drag the draw area
  if(darktable.develop->darkroom_skip_mouse_events)
  {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    const int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;

    const float mx = g->mouse_x;
    const float my = g->mouse_y;

    g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
    g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

    if(event->state & GDK_BUTTON1_MASK)
    {
      g->offset_x += (mx - g->mouse_x) / g->zoom_factor;
      g->offset_y += (my - g->mouse_y) / g->zoom_factor;

      g->offset_x = CLAMP(g->offset_x, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);
      g->offset_y = CLAMP(g->offset_y, 0.f, (g->zoom_factor - 1.f) / g->zoom_factor);

      gtk_widget_queue_draw(self->widget);
    }
    return TRUE;
  }

  const int ch = g->channel;
  const int nodes = p->curve_num_nodes[ch];
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  // if autoscale is on: do not modify g and b curves
  if((p->curve_autoscale != DT_S_SCALE_MANUAL_RGB) && g->channel != DT_IOP_RGBCURVE_R) goto finally;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;

  const double old_m_x = g->mouse_x;
  const double old_m_y = g->mouse_y;

  g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

  const float mx = g->mouse_x;
  const float my = g->mouse_y;
  const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x),
              liny = _mouse_to_curve(my, g->zoom_factor, g->offset_y);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(g->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x
          = old_m_x - _curve_to_mouse(curve_nodes[g->selected].x, g->zoom_factor, g->offset_x);
      const float translate_mouse_y
          = old_m_y - _curve_to_mouse(curve_nodes[g->selected].y, g->zoom_factor, g->offset_y);
      // dx & dy are in linear coordinates
      const float dx = _mouse_to_curve(g->mouse_x - translate_mouse_x, g->zoom_factor, g->offset_x)
                       - _mouse_to_curve(old_m_x - translate_mouse_x, g->zoom_factor, g->offset_x);
      const float dy = _mouse_to_curve(g->mouse_y - translate_mouse_y, g->zoom_factor, g->offset_y)
                       - _mouse_to_curve(old_m_y - translate_mouse_y, g->zoom_factor, g->offset_y);

      dt_iop_color_picker_reset(self, TRUE);
      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < DT_IOP_RGBCURVE_MAXNODES && g->selected >= -1)
    {
      dt_iop_color_picker_reset(self, TRUE);
      // no vertex was close, create a new one!
      g->selected = _add_node(curve_nodes, &p->curve_num_nodes[ch], linx, liny);
      dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f * .04f; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      const float dist = (my - _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y))
                             * (my - _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y))
                         + (mx - _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x))
                               * (mx - _curve_to_mouse(curve_nodes[k].x, g->zoom_factor, g->offset_x));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    g->selected = nearest;
  }
finally:
  if(g->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean _area_button_press_callback(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;
  dt_iop_rgbcurve_params_t *d = (dt_iop_rgbcurve_params_t *)self->default_params;
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  if(darktable.develop->darkroom_skip_mouse_events) return TRUE;

  const int ch = g->channel;
  const int autoscale = p->curve_autoscale;
  const int nodes = p->curve_num_nodes[ch];
  dt_iop_rgbcurve_node_t *curve_nodes = p->curve_nodes[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && dt_modifier_is(event->state, GDK_CONTROL_MASK)
       && nodes < DT_IOP_RGBCURVE_MAXNODES && g->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      const int width = allocation.width - 2 * inset;
      const int height = allocation.height - 2 * inset;

      g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
      g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;

      const float mx = g->mouse_x;
      const float linx = _mouse_to_curve(mx, g->zoom_factor, g->offset_x);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(curve_nodes[0].x > mx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(curve_nodes[k].x > mx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;

        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(g->minmax_curve[ch], linx);

        if(y >= 0.0f && y <= 1.0f) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(curve_nodes, &p->curve_num_nodes[ch], linx, y);

          // maybe set the new one as being selected
          const float min = .04f * .04f; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            const float other_y = _curve_to_mouse(curve_nodes[k].y, g->zoom_factor, g->offset_y);
            const float dist = (y - other_y) * (y - other_y);
            if(dist < min) g->selected = selected;
          }

          dt_iop_color_picker_reset(self, TRUE);
          dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
          gtk_widget_queue_draw(self->widget);
        }

      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
    {
      // reset current curve
      // if autoscale is on: allow only reset of L curve
      if(!((autoscale != DT_S_SCALE_MANUAL_RGB) && ch != DT_IOP_RGBCURVE_R))
      {
        p->curve_num_nodes[ch] = d->curve_num_nodes[ch];
        p->curve_type[ch] = d->curve_type[ch];
        for(int k = 0; k < d->curve_num_nodes[ch]; k++)
        {
          p->curve_nodes[ch][k].x = d->curve_nodes[ch][k].x;
          p->curve_nodes[ch][k].y = d->curve_nodes[ch][k].y;
        }
        g->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);
        dt_iop_color_picker_reset(self, TRUE);
        dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
        gtk_widget_queue_draw(self->widget);
      }
      else
      {
        if(ch != DT_IOP_RGBCURVE_R)
        {
          p->curve_autoscale = DT_S_SCALE_MANUAL_RGB;
          g->selected = -2; // avoid motion notify re-inserting immediately.
          dt_bauhaus_combobox_set(g->autoscale, 1);
          dt_iop_color_picker_reset(self, TRUE);
          dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
  }
  else if(event->button == 3 && g->selected >= 0)
  {
    if(g->selected == 0 || g->selected == nodes - 1)
    {
      const float reset_value = g->selected == 0 ? 0.f : 1.f;
      curve_nodes[g->selected].y = curve_nodes[g->selected].x = reset_value;
      dt_iop_color_picker_reset(self, TRUE);
      dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
      gtk_widget_queue_draw(self->widget);
      return TRUE;
    }

    for(int k = g->selected; k < nodes - 1; k++)
    {
      curve_nodes[k].x = curve_nodes[k + 1].x;
      curve_nodes[k].y = curve_nodes[k + 1].y;
    }
    curve_nodes[nodes - 1].x = curve_nodes[nodes - 1].y = 0;
    g->selected = -2; // avoid re-insertion of that point immediately after this
    p->curve_num_nodes[ch]--;
    dt_iop_color_picker_reset(self, TRUE);
    dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget + ch);
    gtk_widget_queue_draw(self->widget);
    return TRUE;
  }
  return FALSE;
}

void gui_reset(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  g->channel = DT_IOP_RGBCURVE_R;
  g->selected = -1;
  g->offset_x = g->offset_y = 0.f;
  g->zoom_factor = 1.f;

  dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);

  gtk_widget_queue_draw(self->widget);
}

void change_image(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  if(g)
  {
    if(!g->channel)
      g->channel = DT_IOP_RGBCURVE_R;
    g->mouse_x = g->mouse_y = -1.0;
    g->selected = -1;
    g->offset_x = g->offset_y = 0.f;
    g->zoom_factor = 1.f;
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = IOP_GUI_ALLOC(rgbcurve);
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->default_params;

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    g->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->curve_type[ch]);
    g->minmax_curve_nodes[ch] = p->curve_num_nodes[ch];
    g->minmax_curve_type[ch] = p->curve_type[ch];
    for(int k = 0; k < p->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(g->minmax_curve[ch], p->curve_nodes[ch][k].x, p->curve_nodes[ch][k].y);
  }

  g->channel = DT_IOP_RGBCURVE_R;
  change_image(self);

  g->autoscale = dt_bauhaus_combobox_from_params(self, "curve_autoscale");
  gtk_widget_set_tooltip_text(g->autoscale, _("choose between linked and independent channels."));

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  g->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());
  dt_action_define_iop(self, NULL, N_("channel"), GTK_WIDGET(g->channel_tabs), &dt_action_def_tabs_rgb);
  dt_ui_notebook_page(g->channel_tabs, N_("R"), _("curve nodes for r channel"));
  dt_ui_notebook_page(g->channel_tabs, N_("G"), _("curve nodes for g channel"));
  dt_ui_notebook_page(g->channel_tabs, N_("B"), _("curve nodes for b channel"));
  g_signal_connect(G_OBJECT(g->channel_tabs), "switch_page", G_CALLBACK(tab_switch_callback), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->channel_tabs), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_grid_new(), TRUE, TRUE, 0);

  // color pickers
  g->colorpicker = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA | DT_COLOR_PICKER_IO, hbox);
  gtk_widget_set_tooltip_text(g->colorpicker, _("pick GUI color from image\nctrl+click or right-click to select an area"));
  gtk_widget_set_name(g->colorpicker, "keep-active");
  g->colorpicker_set_values = dt_color_picker_new(self, DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_IO, hbox);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->colorpicker_set_values),
                               dtgtk_cairo_paint_colorpicker_set_values, 0, NULL);
  dt_gui_add_class(g->colorpicker_set_values, "dt_transparent_background");
  gtk_widget_set_size_request(g->colorpicker_set_values, DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
  gtk_widget_set_tooltip_text(g->colorpicker_set_values, _("create a curve based on an area from the image\n"
                                                           "drag to create a flat curve\n"
                                                           "ctrl+drag to create a positive curve\n"
                                                           "shift+drag to create a negative curve"));

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), vbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("curve"), GTK_WIDGET(g->area), NULL);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->area), TRUE, TRUE, 0);

  // FIXME: that tooltip goes in the way of the numbers when you hover a node to get a reading
  // gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double click to reset curve"));

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_area_draw_callback), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(_area_button_press_callback), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(_area_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(_area_leave_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(_area_scrolled_callback), self);
  g_signal_connect(G_OBJECT(g->area), "key-press-event", G_CALLBACK(_area_key_press_callback), self);

  /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  g->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->interpolator, NULL, N_("interpolation method"));
  dt_bauhaus_combobox_add(g->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(g->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->interpolator, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->interpolator,
      _("change this method if you see oscillations or cusps in the curve\n"
        "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
        "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
        "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)"));
  g_signal_connect(G_OBJECT(g->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  g->chk_compensate_middle_grey = dt_bauhaus_toggle_from_params(self, "compensate_middle_grey");
  gtk_widget_set_tooltip_text(g->chk_compensate_middle_grey, _("compensate middle gray"));

  g->cmb_preserve_colors = dt_bauhaus_combobox_from_params(self, "preserve_colors");
  gtk_widget_set_tooltip_text(g->cmb_preserve_colors, _("method to preserve colors when applying contrast"));
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)self->params;

  dt_bauhaus_combobox_set(g->autoscale, p->curve_autoscale);
  dt_bauhaus_combobox_set(g->interpolator, p->curve_type[DT_IOP_RGBCURVE_R]);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->chk_compensate_middle_grey), p->compensate_middle_grey);
  dt_bauhaus_combobox_set(g->cmb_preserve_colors, p->preserve_colors);

  _rgbcurve_show_hide_controls(p, g);

  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_rgbcurve_gui_data_t *g = (dt_iop_rgbcurve_gui_data_t *)self->gui_data;

  for(int k = 0; k < DT_IOP_RGBCURVE_MAX_CHANNELS; k++) dt_draw_curve_destroy(g->minmax_curve[k]);

  IOP_GUI_FREE;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)malloc(sizeof(dt_iop_rgbcurve_data_t));
  dt_iop_rgbcurve_params_t *default_params = (dt_iop_rgbcurve_params_t *)self->default_params;
  piece->data = (void *)d;
  memcpy(&d->params, default_params, sizeof(dt_iop_rgbcurve_params_t));

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->curve_type[ch]);
    d->params.curve_num_nodes[ch] = default_params->curve_num_nodes[ch];
    d->params.curve_type[ch] = default_params->curve_type[ch];
    for(int k = 0; k < default_params->curve_num_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->curve_nodes[ch][k].x,
                                    default_params->curve_nodes[ch][k].y);
  }

  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_R][k] = k / 0x10000; // identity for r
  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_G][k] = k / 0x10000; // identity for g
  for(int k = 0; k < 0x10000; k++) d->table[DT_IOP_RGBCURVE_B][k] = k / 0x10000; // identity for b
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)(piece->data);
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  module->request_histogram |= (DT_REQUEST_ON | DT_REQUEST_EXPANDED);

  dt_iop_rgbcurve_params_t *d = module->default_params;

  d->curve_nodes[0][1].x = d->curve_nodes[0][1].y =
  d->curve_nodes[1][1].x = d->curve_nodes[1][1].y =
  d->curve_nodes[2][1].x = d->curve_nodes[2][1].y = 1.0;

  module->histogram_middle_grey = d->compensate_middle_grey;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 25; // rgbcurve.cl, from programs.conf
  dt_iop_rgbcurve_global_data_t *gd
      = (dt_iop_rgbcurve_global_data_t *)malloc(sizeof(dt_iop_rgbcurve_global_data_t));
  module->data = gd;

  gd->kernel_rgbcurve = dt_opencl_create_kernel(program, "rgbcurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_rgbcurve_global_data_t *gd = (dt_iop_rgbcurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_rgbcurve);
  free(module->data);
  module->data = NULL;
}

// this will be called from process*()
// it must be executed only if profile info has changed
static void _generate_curve_lut(dt_dev_pixelpipe_t *pipe, dt_iop_rgbcurve_data_t *d)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);

  dt_iop_rgbcurve_node_t curve_nodes[3][DT_IOP_RGBCURVE_MAXNODES];

  if(work_profile)
  {
    if(d->type_work == work_profile->type && strcmp(d->filename_work, work_profile->filename) == 0) return;
  }

  if(work_profile && d->params.compensate_middle_grey)
  {
    d->type_work = work_profile->type;
    g_strlcpy(d->filename_work, work_profile->filename, sizeof(d->filename_work));

    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
      {
        curve_nodes[ch][k].x = dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].x, work_profile);
        curve_nodes[ch][k].y = dt_ioppr_uncompensate_middle_grey(d->params.curve_nodes[ch][k].y, work_profile);
      }
    }
  }
  else
  {
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      memcpy(curve_nodes[ch], d->params.curve_nodes[ch], sizeof(dt_iop_rgbcurve_node_t) * DT_IOP_RGBCURVE_MAXNODES);
    }
  }

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_changed[ch])
    {
      dt_draw_curve_destroy(d->curve[ch]);
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, d->params.curve_type[ch]);
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
        (void)dt_draw_curve_add_point(d->curve[ch], curve_nodes[ch][k].x, curve_nodes[ch][k].y);
    }
    else
    {
      for(int k = 0; k < d->params.curve_num_nodes[ch]; k++)
        dt_draw_curve_set_point(d->curve[ch], k, curve_nodes[ch][k].x, curve_nodes[ch][k].y);
    }

    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }

  // extrapolation for each curve (right hand side only):
  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
  {
    const float xm_L = curve_nodes[ch][d->params.curve_num_nodes[ch] - 1].x;
    const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
    const float y_L[4] = { d->table[ch][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                           d->table[ch][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
    dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs[ch]);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)(piece->data);
  dt_iop_rgbcurve_params_t *p = (dt_iop_rgbcurve_params_t *)p1;

  if(pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
  {
    piece->request_histogram |= DT_REQUEST_ON;
    self->histogram_middle_grey = p->compensate_middle_grey;
  }
  else
  {
    piece->request_histogram &= ~DT_REQUEST_ON;
  }

  for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    d->curve_changed[ch]
        = (d->params.curve_type[ch] != p->curve_type[ch] || d->params.curve_nodes[ch] != p->curve_nodes[ch]);

  memcpy(&d->params, p, sizeof(dt_iop_rgbcurve_params_t));

  // working color profile
  d->type_work = DT_COLORSPACE_NONE;
  d->filename_work[0] = '\0';
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  dt_iop_rgbcurve_data_t *d = (dt_iop_rgbcurve_data_t *)piece->data;
  dt_iop_rgbcurve_global_data_t *gd = (dt_iop_rgbcurve_global_data_t *)self->global_data;

  _generate_curve_lut(piece->pipe, d);

  cl_int err = CL_SUCCESS;

  cl_mem dev_r = NULL;
  cl_mem dev_g = NULL;
  cl_mem dev_b = NULL;
  cl_mem dev_coeffs_r = NULL;
  cl_mem dev_coeffs_g = NULL;
  cl_mem dev_coeffs_b = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale = d->params.curve_autoscale;
  const int preserve_colors = d->params.preserve_colors;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto cleanup;

  dev_r = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_R], 256, 256, sizeof(float));
  if(dev_r == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 1\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_g = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_G], 256, 256, sizeof(float));
  if(dev_g == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 2\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_b = dt_opencl_copy_host_to_device(devid, d->table[DT_IOP_RGBCURVE_B], 256, 256, sizeof(float));
  if(dev_b == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 3\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_r = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs[0]);
  if(dev_coeffs_r == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 4\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_g = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs[1]);
  if(dev_coeffs_g == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 5\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  dev_coeffs_b = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, d->unbounded_coeffs[2]);
  if(dev_coeffs_b == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error allocating memory 6\n");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rgbcurve, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(dev_r), CLARG(dev_g), CLARG(dev_b),
    CLARG(dev_coeffs_r), CLARG(dev_coeffs_g), CLARG(dev_coeffs_b), CLARG(autoscale), CLARG(preserve_colors),
    CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(use_work_profile));
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rgbcurve process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  if(dev_r) dt_opencl_release_mem_object(dev_r);
  if(dev_g) dt_opencl_release_mem_object(dev_g);
  if(dev_b) dt_opencl_release_mem_object(dev_b);
  if(dev_coeffs_r) dt_opencl_release_mem_object(dev_coeffs_r);
  if(dev_coeffs_g) dt_opencl_release_mem_object(dev_coeffs_g);
  if(dev_coeffs_b) dt_opencl_release_mem_object(dev_coeffs_b);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_rgbcurve] couldn't enqueue kernel! %s\n", cl_errstr(err));

  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  const float *const restrict in = (float*)ivoid;
  float *const restrict out = (float*)ovoid;
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         in, out, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  dt_iop_rgbcurve_data_t *const restrict d = (dt_iop_rgbcurve_data_t *)(piece->data);

  _generate_curve_lut(piece->pipe, d);

  const float xm_L = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_R][0];
  const float xm_g = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_G][0];
  const float xm_b = 1.0f / d->unbounded_coeffs[DT_IOP_RGBCURVE_B][0];

  const int width = roi_out->width;
  const int height = roi_out->height;
  const size_t npixels = (size_t)width * height;
  const int autoscale = d->params.curve_autoscale;
  const _curve_table_ptr restrict table = d->table;
  const _coeffs_table_ptr restrict unbounded_coeffs = d->unbounded_coeffs;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(autoscale, npixels, work_profile, xm_b, xm_g, xm_L) \
  dt_omp_sharedconst(in, out, table, unbounded_coeffs, d) \
  schedule(static)
#endif
  for(int y = 0; y < 4*npixels; y += 4)
  {
    if(autoscale == DT_S_SCALE_MANUAL_RGB)
    {
      out[y+0] = (in[y+0] < xm_L) ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[y+0] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], in[y+0]);
      out[y+1] = (in[y+1] < xm_g) ? table[DT_IOP_RGBCURVE_G][CLAMP((int)(in[y+1] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_G], in[y+1]);
      out[y+2] = (in[y+2] < xm_b) ? table[DT_IOP_RGBCURVE_B][CLAMP((int)(in[y+2] * 0x10000ul), 0, 0xffff)]
                                  : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_B], in[y+2]);
    }
    else if(autoscale == DT_S_SCALE_AUTOMATIC_RGB)
    {
      if(d->params.preserve_colors == DT_RGB_NORM_NONE)
      {
        for(int c = 0; c < 3; c++)
        {
          out[y+c] = (in[y+c] < xm_L) ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(in[y+c] * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], in[y+c]);
        }
      }
      else
      {
        float ratio = 1.f;
        const float lum = dt_rgb_norm(in + y, d->params.preserve_colors, work_profile);
        if(lum > 0.f)
        {
          const float curve_lum = (lum < xm_L)
            ? table[DT_IOP_RGBCURVE_R][CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
            : dt_iop_eval_exp(unbounded_coeffs[DT_IOP_RGBCURVE_R], lum);
          ratio = curve_lum / lum;
        }
        for(size_t c = 0; c < 3; c++)
        {
          out[y+c] = (ratio * in[y+c]);
        }
      }
    }
    out[y+3] = in[y+3];
  }
}

#undef DT_GUI_CURVE_EDITOR_INSET
#undef DT_IOP_RGBCURVE_RES
#undef DT_IOP_RGBCURVE_MAXNODES
#undef DT_IOP_RGBCURVE_MIN_X_DISTANCE
#undef DT_IOP_COLOR_ICC_LEN

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

