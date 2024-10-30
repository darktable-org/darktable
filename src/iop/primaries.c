/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

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
#include "common/custom_primaries.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>

// Linear (matrix) transformation of the RGB data based on user-defined
// rotations and scalings of the working space primaries.
// The process is linear (basic channel mixing) but the parametrization
// used here is potentially useful.
//
// Allows tinting of the achromatic axis as well,
// thanks to idea from Troy Sobotka at:
// https://github.com/sobotka/SB2383-Configuration-Generation

DT_MODULE_INTROSPECTION(1, dt_iop_primaries_params_t)

static const float RAD_TO_DEG = 180.f / M_PI_F;

typedef struct dt_iop_primaries_params_t
{
  float achromatic_tint_hue;    // $MIN: -3.14 $MAX: 3.14 $DEFAULT: 0.0 $DESCRIPTION: "tint hue"
  float achromatic_tint_purity; // $MIN: 0.0 $MAX: 0.99 $DEFAULT: 0.0 $DESCRIPTION: "tint purity"
  float red_hue;                // $MIN: -3.14 $MAX: 3.14 $DEFAULT: 0.0 $DESCRIPTION: "red hue"
  float red_purity;             // $MIN: 0.01 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "red purity"
  float green_hue;              // $MIN: -3.14 $MAX: 3.14 $DEFAULT: 0.0 $DESCRIPTION: "green hue"
  float green_purity;           // $MIN: 0.01 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "green purity"
  float blue_hue;               // $MIN: -3.14 $MAX: 3.14 $DEFAULT: 0.0 $DESCRIPTION: "blue hue"
  float blue_purity;            // $MIN: 0.01 $MAX: 5.0 $DEFAULT: 1.0 $DESCRIPTION: "blue purity"
} dt_iop_primaries_params_t;

typedef struct dt_iop_primaries_gui_data_t
{
  GtkWidget *achromatic_tint_hue, *achromatic_tint_purity;
  GtkWidget *red_hue, *red_purity;
  GtkWidget *green_hue, *green_purity;
  GtkWidget *blue_hue, *blue_purity;
  const dt_iop_order_iccprofile_info_t *painted_work_profile, *painted_display_profile;
} dt_iop_primaries_gui_data_t;

typedef struct dt_iop_primaries_global_data_t
{
  int kernel_primaries;
} dt_iop_primaries_global_data_t;

const char *name()
{
  return _("rgb primaries");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("adjustment of the RGB color primaries for color grading"),
                                      _("corrective or creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("linear, RGB"),
                                      _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void _calculate_adjustment_matrix
  (const dt_iop_primaries_params_t *const params,
   const dt_iop_order_iccprofile_info_t *const pipe_work_profile,
   dt_colormatrix_t matrix)
{
  float custom_primaries[3][2];
  const float scaling[3] = { params->red_purity,
                             params->green_purity,
                             params->blue_purity };
  const float rotation[3] = { params->red_hue, params->green_hue, params->blue_hue };
  for(size_t i = 0; i < 3; i++)
    dt_rotate_and_scale_primary(pipe_work_profile,
                                scaling[i], rotation[i], i, custom_primaries[i]);

  float whitepoint[2];
  dt_rotate_and_scale_primary(pipe_work_profile, params->achromatic_tint_purity,
                              params->achromatic_tint_hue, 0,
                              whitepoint);

  dt_colormatrix_t RGB_TO_XYZ;
  dt_make_transposed_matrices_from_primaries_and_whitepoint(custom_primaries,
                                                            whitepoint, RGB_TO_XYZ);
  dt_colormatrix_mul(matrix, RGB_TO_XYZ, pipe_work_profile->matrix_out_transposed);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_primaries_params_t *params = piece->data;

  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self,
                                        piece->colors, ivoid, ovoid, roi_in,
                                        roi_out))
    return;

  const dt_iop_order_iccprofile_info_t *pipe_work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  dt_colormatrix_t matrix;
  _calculate_adjustment_matrix(params, pipe_work_profile, matrix);

  DT_OMP_FOR(shared(matrix))
  for(size_t k = 0; k < 4 * roi_out->width * roi_out->height; k += 4)
  {
    const float *const restrict in = ((const float *)ivoid) + k;
    float *const restrict out = ((float *)ovoid) + k;

    dt_apply_transposed_color_matrix(in, matrix, out);
    out[3] = in[3];
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_primaries_params_t *params = piece->data;
  dt_iop_primaries_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const dt_iop_order_iccprofile_info_t *pipe_work_profile =
    dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  dt_colormatrix_t transposed_matrix, matrix;
  _calculate_adjustment_matrix(params, pipe_work_profile, transposed_matrix);
  transpose_3xSSE(transposed_matrix, matrix);

  cl_mem dev_matrix = dt_opencl_copy_host_to_device_constant(devid, sizeof(matrix), matrix);
  if(dev_matrix == NULL)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_primaries] couldn't allocate memory!");
    return DT_OPENCL_DEFAULT_ERROR;
  }

  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_primaries,
                                                width, height, CLARG(dev_in),
                                                CLARG(dev_out), CLARG(width), CLARG(height),
                                                CLARG(dev_matrix));
  dt_opencl_release_mem_object(dev_matrix);
  return err;
}
#endif

static void _rotated_primary_to_display_RGB
  (const dt_iop_order_iccprofile_info_t *work_profile,
   const dt_iop_order_iccprofile_info_t *display_profile,
   const dt_iop_order_iccprofile_info_t *sRGB_profile,
   const size_t primary_index,
   const float angle,
   const float desaturate,
   dt_aligned_pixel_t display_RGB)
{
  dt_aligned_pixel_t xyY = { 0.f }, XYZ, RGB;
  dt_rotate_and_scale_primary(work_profile, 1.f, angle, primary_index, xyY);
  // Luminance doesn't matter - it will change in the normalization in
  // the end of the function.
  xyY[2] = 1.f;
  dt_xyY_to_XYZ(xyY, XYZ);
  dt_apply_transposed_color_matrix(XYZ, sRGB_profile->matrix_out_transposed, RGB);

  // Bring the value to the sRGB hull and desaturate a bit.
  // This is done in sRGB to avoid eyesore for those with wide-gamut displays.
  const float min_value = MIN(MIN(RGB[0], RGB[1]), RGB[2]);
  const float offset = -MIN(min_value, 0.f) + desaturate;
  for_each_channel(c) RGB[c] += offset;

  // To display RGB. Bring to the hull and normalize to 1.
  dt_apply_transposed_color_matrix(RGB, sRGB_profile->matrix_in_transposed, XYZ);
  dt_apply_transposed_color_matrix(XYZ, display_profile->matrix_out_transposed, RGB);
  const float display_min_value = MIN(MIN(RGB[0], RGB[1]), RGB[2]);
  const float display_offset = -MIN(display_min_value, 0.f);
  const float display_max_value = MAX(MAX(RGB[0], RGB[1]), RGB[2]) + display_offset;
  const float scale = 1.f / display_max_value;
  for_each_channel(c) display_RGB[c] = scale * (RGB[c] + display_offset);
}

static void _apply_trc_if_nonlinear(const dt_iop_order_iccprofile_info_t *display_profile,
                                    const dt_aligned_pixel_t linear_RGB,
                                    dt_aligned_pixel_t RGB)
{
  // Apply tone response curve if any
  if(display_profile->nonlinearlut)
    dt_ioppr_apply_trc(linear_RGB, RGB, display_profile->lut_out,
                       display_profile->unbounded_coeffs_out,
                       display_profile->lutsize);
  else
    copy_pixel(RGB, linear_RGB);
}

static void _paint_hue_slider(const dt_iop_order_iccprofile_info_t *work_profile,
                              const dt_iop_order_iccprofile_info_t *display_profile,
                              const dt_iop_order_iccprofile_info_t *sRGB_profile,
                              const size_t primary_index,
                              GtkWidget *slider)
{
  const float hard_min = dt_bauhaus_slider_get_hard_min(slider);
  const float hard_max = dt_bauhaus_slider_get_hard_max(slider);
  const float range = hard_max - hard_min;
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    const float angle = hard_min + stop * range;
    dt_aligned_pixel_t linear_RGB, RGB;
    _rotated_primary_to_display_RGB(work_profile, display_profile,
                                    sRGB_profile, primary_index, angle, 0.4f,
                                    linear_RGB);
    _apply_trc_if_nonlinear(display_profile, linear_RGB, RGB);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
  gtk_widget_queue_draw(GTK_WIDGET(slider));
}

static void _paint_purity_slider(const dt_iop_order_iccprofile_info_t *work_profile,
                                 const dt_iop_order_iccprofile_info_t *display_profile,
                                 const dt_iop_order_iccprofile_info_t *sRGB_profile,
                                 const size_t primary_index,
                                 const float saturation,
                                 GtkWidget *hue_slider,
                                 GtkWidget *purity_slider)
{
  const float angle = dt_bauhaus_slider_get(hue_slider);
  dt_aligned_pixel_t linear_RGB, RGB;
  // Map the chosen primary from the full purity to fit the display gamut.
  _rotated_primary_to_display_RGB(work_profile, display_profile, sRGB_profile,
                                  primary_index, angle, 0.0f,
                                  linear_RGB);
  const float hard_min = dt_bauhaus_slider_get_hard_min(purity_slider);
  const float hard_max = dt_bauhaus_slider_get_hard_max(purity_slider);
  const float range = hard_max - hard_min;
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    const float t = MIN(hard_min + stop * saturation * range, 1.f);
    dt_aligned_pixel_t stop_RGB = { 0.f };
    // Interpolate between white (1, 1, 1) and the chosen
    // primary. Not super accurate (since the display can't represent the Rec.2020 primaries)
    // but gives an idea of the effect of the purity adjustment.
    for_each_channel(c) stop_RGB[c] = 1.f - t + t * linear_RGB[c];
    _apply_trc_if_nonlinear(display_profile, stop_RGB, RGB);
    dt_bauhaus_slider_set_stop(purity_slider, stop, RGB[0], RGB[1], RGB[2]);
  }
  gtk_widget_queue_draw(GTK_WIDGET(purity_slider));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  if(!self->dev || !self->dev->full.pipe) return;

  dt_iop_primaries_gui_data_t *g = self->gui_data;

  const dt_iop_order_iccprofile_info_t *work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, self->dev->full.pipe);
  const dt_iop_order_iccprofile_info_t *display_profile =
    dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);
  if(!work_profile || !display_profile) return; // couldn't fetch
                                                // profiles, can't
                                                // paint the sliders

  const gboolean repaint_all_sliders =
    !w
    || work_profile != g->painted_work_profile
    || display_profile != g->painted_display_profile;

  const dt_iop_order_iccprofile_info_t *sRGB_profile =
    dt_ioppr_add_profile_info_to_list(self->dev, DT_COLORSPACE_SRGB, "",
                                      DT_INTENT_RELATIVE_COLORIMETRIC);

  if(repaint_all_sliders)
  {
    _paint_hue_slider(work_profile, display_profile, sRGB_profile, 0, g->red_hue);
    _paint_hue_slider(work_profile, display_profile, sRGB_profile, 1, g->green_hue);
    _paint_hue_slider(work_profile, display_profile, sRGB_profile, 2, g->blue_hue);
    // Achromatic tint angle is anchored at the red primary
    _paint_hue_slider(work_profile, display_profile, sRGB_profile, 0,
                      g->achromatic_tint_hue);

    g->painted_work_profile = work_profile;
    g->painted_display_profile = display_profile;
  }

  if(repaint_all_sliders || w == g->red_hue)
    _paint_purity_slider(work_profile, display_profile, sRGB_profile,
                         0, 1.f, g->red_hue, g->red_purity);
  if(repaint_all_sliders || w == g->green_hue)
    _paint_purity_slider(work_profile, display_profile, sRGB_profile,
                         1, 1.f, g->green_hue, g->green_purity);
  if(repaint_all_sliders || w == g->blue_hue)
    _paint_purity_slider(work_profile, display_profile, sRGB_profile,
                         2, 1.f, g->blue_hue, g->blue_purity);
  if(repaint_all_sliders || w == g->achromatic_tint_hue)
    _paint_purity_slider(work_profile, display_profile, sRGB_profile,
                         0, 5.f, g->achromatic_tint_hue,
                         g->achromatic_tint_purity);
}

static void _signal_profile_user_changed(gpointer instance,
                                         const uint8_t profile_type,
                                         dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
}

static void _signal_profile_changed(gpointer instance,
                                    dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
}

static GtkWidget *_setup_hue_slider(dt_iop_module_t *self,
                                    const char *param_name,
                                    const char *tooltip)
{
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_format(slider, "°");
  dt_bauhaus_slider_set_digits(slider, 1);
  dt_bauhaus_slider_set_factor(slider, RAD_TO_DEG);
  dt_bauhaus_slider_set_soft_range(slider, -20.f / RAD_TO_DEG, 20.f / RAD_TO_DEG);
  gtk_widget_set_tooltip_text(slider, tooltip);
  return slider;
}

static GtkWidget *_setup_purity_slider(dt_iop_module_t *self,
                                       const char *param_name,
                                       const char *tooltip)
{
  GtkWidget *slider = dt_bauhaus_slider_from_params(self, param_name);
  dt_bauhaus_slider_set_format(slider, "%");
  dt_bauhaus_slider_set_digits(slider, 1);
  dt_bauhaus_slider_set_factor(slider, 100.f);
  dt_bauhaus_slider_set_offset(slider, -100.f);
  dt_bauhaus_slider_set_soft_range(slider, 0.5f, 1.5f);
  gtk_widget_set_tooltip_text(slider, tooltip);
  return slider;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_primaries_gui_data_t *g = IOP_GUI_ALLOC(primaries);

  g->red_hue = _setup_hue_slider(self, "red_hue", _("shift red towards yellow or magenta"));
  g->red_purity = _setup_purity_slider(self, "red_purity", _("red primary purity"));
  g->green_hue = _setup_hue_slider(self, "green_hue", _("shift green towards cyan or yellow"));
  g->green_purity = _setup_purity_slider(self, "green_purity", _("green primary purity"));
  g->blue_hue = _setup_hue_slider(self, "blue_hue", _("shift blue towards magenta or cyan"));
  g->blue_purity = _setup_purity_slider(self, "blue_purity", _("blue primary purity"));

  g->achromatic_tint_hue = dt_bauhaus_slider_from_params(self, "achromatic_tint_hue");
  dt_bauhaus_slider_set_format(g->achromatic_tint_hue, "°");
  dt_bauhaus_slider_set_digits(g->achromatic_tint_hue, 1);
  dt_bauhaus_slider_set_factor(g->achromatic_tint_hue, RAD_TO_DEG);
  gtk_widget_set_tooltip_text(g->achromatic_tint_hue, _("tint hue"));

  g->achromatic_tint_purity = dt_bauhaus_slider_from_params(self, "achromatic_tint_purity");
  dt_bauhaus_slider_set_format(g->achromatic_tint_purity, "%");
  dt_bauhaus_slider_set_digits(g->achromatic_tint_purity, 1);
  dt_bauhaus_slider_set_factor(g->achromatic_tint_purity, 100.f);
  dt_bauhaus_slider_set_soft_range(g->achromatic_tint_purity, 0.f, 0.2f);
  gtk_widget_set_tooltip_text(g->achromatic_tint_purity, _("tint purity"));

  g->painted_work_profile = NULL;
  g->painted_display_profile = NULL;

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED, _signal_profile_user_changed, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_PROFILE_CHANGED, _signal_profile_changed, self);
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _signal_profile_changed, self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_signal_profile_user_changed, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_signal_profile_changed, self);

  IOP_GUI_FREE;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_primaries_global_data_t *gd = malloc(sizeof(dt_iop_primaries_global_data_t));
  self->data = gd;
  gd->kernel_primaries = dt_opencl_create_kernel(program, "primaries");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_primaries_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_primaries);
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
