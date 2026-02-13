/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include <stdint.h>

#include "bauhaus/bauhaus.h"
#include "common/atomic.h"
#include "common/color_harmony.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"
#include "common/splines.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

// # of gradations between each primary/secondary to draw the hue ring
// this is tuned to most degenerate cases: curve to blue primary in
// Luv in linear ProPhoto RGB and the widely spaced gradations of the
// PQ P3 RGB colorspace. This could be lowered to 32 with little
// visible consequence.
#define VECTORSCOPE_HUES 48
#define VECTORSCOPE_BASE_LOG 30

DT_MODULE(1)

typedef enum dt_lib_vectorscope_scale_t
{
  DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC = 0,
  DT_LIB_VECTORSCOPE_SCALE_LINEAR,
  DT_LIB_VECTORSCOPE_SCALE_N // needs to be the last one
} dt_lib_vectorscope_scale_t;

typedef enum dt_lib_vectorscope_type_t
{
  DT_LIB_VECTORSCOPE_TYPE_CIELUV = 0,   // CIE 1976 u*v*
  DT_LIB_VECTORSCOPE_TYPE_JZAZBZ,
  DT_LIB_VECTORSCOPE_TYPE_RYB,
  DT_LIB_VECTORSCOPE_TYPE_N // needs to be the last one
} dt_lib_vectorscope_type_t;

typedef struct dt_lib_vectorscope_color_harmony_t
{
  const char *name;
  const int sectors;      // how many sectors
  const float angle[4];   // the angle of the sector center, expressed in fractions of a full turn
  const float length[4];  // the radius of the sector, from 0. to 1., linear scale
} dt_lib_vectorscope_color_harmony_t;

dt_lib_vectorscope_color_harmony_t dt_color_harmonies[DT_COLOR_HARMONY_N] =
{
  {N_("none"),                    0                                                              },
  {N_("monochromatic"),           1, { 0./12.                         }, {0.80                  }},
  {N_("analogous"),               3, {-1./12., 0./12.,  1./12.        }, {0.50, 0.80, 0.50      }},
  {N_("analogous complementary"), 4, {-1./12., 0./12.,  1./12., 6./12.}, {0.50, 0.80, 0.50, 0.50}},
  {N_("complementary"),           2, { 0./12., 6./12                  }, {0.80, 0.50            }},
  {N_("split complementary"),     3, { 0./12., 5./12.,  7./12.        }, {0.80, 0.50, 0.50      }},
  {N_("dyad"),                    2, {-1./12., 1./12                  }, {0.80, 0.80            }},
  {N_("triad"),                   3, { 0./12., 4./12.,  8./12.        }, {0.80, 0.50, 0.50      }},
  {N_("tetrad"),                  4, {-1./12., 1./12.,  5./12., 7./12.}, {0.80, 0.80, 0.50, 0.50}},
  {N_("square"),                  4, { 0./12., 3./12.,  6./12., 9./12.}, {0.80, 0.50, 0.50, 0.50}},
};

const gchar *dt_lib_vectorscope_scale_names[DT_LIB_VECTORSCOPE_SCALE_N] =
  { "logarithmic",
    "linear"
  };

const gchar *dt_lib_vectorscope_type_names[DT_LIB_VECTORSCOPE_TYPE_N] =
  { "u*v*",
    "AzBz",
    "RYB"
  };

const gchar *dt_lib_vectorscope_color_harmony_width_names[DT_COLOR_HARMONY_WIDTH_N] =
  { "normal",
    "large",
    "narrow",
    "line"
  };

const float dt_lib_vectorscope_color_harmony_width[DT_COLOR_HARMONY_WIDTH_N] =
  { 0.5f/12.f, 0.75f/12.f, 0.25f/12.f, 0.0f };

typedef struct dt_lib_vectorscope_t
{
  uint8_t *vectorscope_graph, *vectorscope_bkgd;
  float vectorscope_pt[2];            // point colorpicker position
  GSList *vectorscope_samples;        // live samples position
  int selected_sample;                // position of the selected live sample in the list
  int vectorscope_diameter_px;
  float hue_ring[6][VECTORSCOPE_HUES][2] DT_ALIGNED_ARRAY;
  const dt_iop_order_iccprofile_info_t *hue_ring_prof;
  dt_lib_vectorscope_scale_t hue_ring_scale;
  dt_lib_vectorscope_type_t hue_ring_colorspace;
  double vectorscope_radius;
  dt_pthread_mutex_t lock;
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *button_box_main;          // GtkBox -- contains scope control buttons
  GtkWidget *button_box_opt;           // GtkBox -- contains options buttons
  GtkWidget *color_harmony_box;        // GtkBox -- contains color harmony buttons
  GtkWidget *color_harmony_fix;        // GtkFixed -- contains moveable color harmony buttons
  GtkWidget *scale_button;             // GtkButton -- how to render the current scope
  GtkWidget *colorspace_button;        // GtkButton -- vectorscope colorspace
  GtkWidget *color_harmony_button
    [DT_COLOR_HARMONY_N - 1];  // GtkButton -- RYB vectorscope color harmonies
  // state set by buttons
  dt_lib_vectorscope_type_t vectorscope_type;
  dt_lib_vectorscope_scale_t vectorscope_scale;
  double vectorscope_angle;
  float *rgb2ryb_ypp;
  float *ryb2rgb_ypp;
  dt_color_harmony_type_t color_harmony_old;
  dt_color_harmony_guide_t harmony_guide;
} dt_lib_vectorscope_t;

const char *name(dt_lib_module_t *self)
{
  return _("vectorscope");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM | DT_VIEW_TETHERING;
}

uint32_t container(dt_lib_module_t *self)
{
  return g_strcmp0
    (dt_conf_get_string_const("plugins/darkroom/vectorscope/panel_position"), "right")
    ? DT_UI_CONTAINER_PANEL_LEFT_TOP
    : DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}


// Inspired by "Paint Inspired Color Mixing and Compositing for Visualization" - Gossett
// http://vis.computer.org/vis2004/DVD/infovis/papers/gossett.pdf
// As the Gossett model is not reversible, we keep his cube hues
// and use them to transpose rgb <-> ryb by spline interpolation
// This model compensates the orange expansion by compressing from green to red
// unlike the model proposed by Junichi SUGITA & Tokiichiro TAKAHASHI,
// in "Computational RYB Color Model and its Applications",
// which compresses mainly the cyan colors (while also reversible)
// https://danielhaim.com/research/downloads/Computational%20RYB%20Color%20Model%20and%20its%20Applications.pdf

const float x_vtx[7] =     {0.0, 0.166667, 0.333333, 0.5, 0.666667, 0.833333, 1.0};
const float rgb_y_vtx[7] = {0.0, 0.083333, 0.166667, 0.383838, 0.586575, 0.833333, 1.0};
const float ryb_y_vtx[7] = {0.0, 0.333333, 0.472217, 0.611105, 0.715271, 0.833333, 1.0};

static void _ryb2rgb(const dt_aligned_pixel_t ryb,
                     dt_aligned_pixel_t rgb,
                     const float *ryb2rgb_ypp)
{
  dt_aligned_pixel_t HSV;
  dt_RGB_2_HSV(ryb, HSV);
  HSV[0] = interpolate_val(sizeof(x_vtx)/sizeof(float), (float *)x_vtx, HSV[0],
                           (float *)rgb_y_vtx, (float *)ryb2rgb_ypp, CUBIC_SPLINE);
  dt_HSV_2_RGB(HSV, rgb);
}

static void _rgb2ryb(const dt_aligned_pixel_t rgb,
                     dt_aligned_pixel_t ryb,
                     const float *rgb2ryb_ypp)
{
  dt_aligned_pixel_t HSV;
  dt_RGB_2_HSV(rgb, HSV);
  HSV[0] = interpolate_val(sizeof(x_vtx)/sizeof(float), (float *)x_vtx, HSV[0],
                           (float *)ryb_y_vtx, (float *)rgb2ryb_ypp, CUBIC_SPLINE);
  dt_HSV_2_RGB(HSV, ryb);
}

static inline float baselog(const float x,
                            const float bound)
{
  // FIXME: use dt's fastlog()?
  return log1pf((VECTORSCOPE_BASE_LOG - 1.f) * x / bound)
    / logf(VECTORSCOPE_BASE_LOG) * bound;
}

static inline void log_scale(float *x, float *y, const float r)
{
  const float h = dt_fast_hypotf(*x,*y);
  // Haven't seen a zero point in practice, but it is certainly
  // possible. Map these to zero, and CPU should predict that
  // this is unlikely.
  if(h >= FLT_MIN)
  {
    const float s = baselog(h, r) / h;
    *x *= s;
    *y *= s;
  }
}

static void _lib_vectorscope_bkgd
  (dt_lib_vectorscope_t *d,
   const dt_iop_order_iccprofile_info_t *const vs_prof)
{
  if(vs_prof == d->hue_ring_prof
     && d->vectorscope_scale == d->hue_ring_scale
     && d->vectorscope_type == d->hue_ring_colorspace)
    return;

  // Calculate "hue ring" by tracing along the edges of the "RGB cube"
  // which do not touch the white or black vertex. This should be the
  // maximum chromas. It's OK if some of the sampled points are
  // closer/further from each other. A hue ring in xy between
  // primaries and secondaries is larger than the RGB space clipped to
  // [0,1]. Note that hue ring calculation seems fast enough that it's
  // not worth caching, but the below math does not vary once it is
  // calculated for a profile.

  // To test if the hue ring represents RGB gamut of a histogram
  // profile with a given colorspace, use a test image: Set histogram
  // profile = input profile. The ideal test image is a hue/saturation
  // two dimensional gradient. This could simply be 7x3 px, bottom row
  // white, middle row R,Y,G,C,B,M,R, top row black,
  // scaled up via linear interpolation.

  const float vertex_rgb[6][4] DT_ALIGNED_PIXEL = {{1.f, 0.f, 0.f}, {1.f, 1.f, 0.f},
                                                   {0.f, 1.f, 0.f}, {0.f, 1.f, 1.f},
                                                   {0.f, 0.f, 1.f}, {1.f, 0.f, 1.f} };

  float max_radius = 0.f;
  const dt_lib_vectorscope_type_t vs_type = d->vectorscope_type;

  // chromaticities for drawing both hue ring and graph
  // NOTE: As ProPhoto's blue primary is very dark (and imaginary), it
  // maps to a very small radius in CIELuv.
  cairo_pattern_t *p = cairo_pattern_create_mesh();
  // initialize to make gcc-7 happy
  dt_aligned_pixel_t rgb_display = { 0.f };
  dt_aligned_pixel_t prev_rgb_display = { 0.f };
  dt_aligned_pixel_t first_rgb_display = { 0.f };

  double px = 0., py= 0.;

  for(int k=0; k<6; k++)
  {
    dt_aligned_pixel_t delta;
    for_each_channel(ch, aligned(vertex_rgb, delta:16))
      delta[ch] = (vertex_rgb[(k+1)%6][ch] - vertex_rgb[k][ch]) / VECTORSCOPE_HUES;
    for(int i=0; i < VECTORSCOPE_HUES; i++)
    {
      dt_aligned_pixel_t rgb_scope, XYZ_D50 = { 0 }, chromaticity = { 0 };
      for_each_channel(ch, aligned(vertex_rgb, delta, rgb_scope:16))
        rgb_scope[ch] = vertex_rgb[k][ch] + delta[ch] * i;
      switch(vs_type)
      {
        case DT_LIB_VECTORSCOPE_TYPE_CIELUV:
        {
          dt_ioppr_rgb_matrix_to_xyz(rgb_scope,
                                     XYZ_D50,
                                     vs_prof->matrix_in_transposed,
                                     vs_prof->lut_in,
                                     vs_prof->unbounded_coeffs_in,
                                     vs_prof->lutsize,
                                     vs_prof->nonlinearlut);
          dt_aligned_pixel_t xyY;
          dt_D50_XYZ_to_xyY(XYZ_D50, xyY);
          dt_xyY_to_Luv(xyY, chromaticity);
          dt_XYZ_to_Rec709_D50(XYZ_D50, rgb_display);
          break;
        }
        case DT_LIB_VECTORSCOPE_TYPE_JZAZBZ:
        {
          dt_ioppr_rgb_matrix_to_xyz(rgb_scope,
                                     XYZ_D50,
                                     vs_prof->matrix_in_transposed,
                                     vs_prof->lut_in,
                                     vs_prof->unbounded_coeffs_in,
                                     vs_prof->lutsize,
                                     vs_prof->nonlinearlut);
          dt_aligned_pixel_t XYZ_D65;
          dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
          dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
          dt_XYZ_to_Rec709_D50(XYZ_D50, rgb_display);
          break;
        }
        case DT_LIB_VECTORSCOPE_TYPE_RYB:
        {
          // get the color to be displayed
          _ryb2rgb(rgb_scope, rgb_display, d->ryb2rgb_ypp);
          const float alpha = M_PI_F * (0.33333f * ((float)k + (float)i / VECTORSCOPE_HUES));
          chromaticity[1] = cosf(alpha) * 0.01;
          chromaticity[2] = sinf(alpha) * 0.01;
          break;
        }
        case DT_LIB_VECTORSCOPE_TYPE_N:
          dt_unreachable_codepath();
      }

      d->hue_ring[k][i][0] = chromaticity[1];
      d->hue_ring[k][i][1] = chromaticity[2];
      const float h = dt_fast_hypotf(chromaticity[1], chromaticity[2]);
      max_radius = MAX(max_radius, h);

      // Try to represent hue in profile colorspace. Do crude gamut
      // clipping, and cairo_mesh_pattern_set_corner_color_rgb will
      // clamp.
      const float max_RGB = MAX(MAX(rgb_display[0], rgb_display[1]), rgb_display[2]);
      for_each_channel(ch, aligned(rgb_display:16))
        rgb_display[ch] = rgb_display[ch] / max_RGB;
      if(k==0 && i==0)
      {
        for_each_channel(ch, aligned(first_rgb_display, rgb_display:16))
          first_rgb_display[ch] = rgb_display[ch];
      }
      else
      {
        // Extend radii of the sectors of the mesh pattern so to the
        // way to the edge of the background. This matters
        // particularly for blue in ProPhoto, as there is a very small
        // chroma. By the time we reach the less intense colors,
        // max_radius is a reasonable value.
        if(h >= FLT_MIN)
        {
          chromaticity[1] *= max_radius / h;
          chromaticity[2] *= max_radius / h;
        }
        // triangle with 4th point set to make gradient
        cairo_mesh_pattern_begin_patch(p);
        cairo_mesh_pattern_move_to(p, 0., 0.);
        cairo_mesh_pattern_line_to(p, px, py);
        cairo_mesh_pattern_line_to(p, chromaticity[1], chromaticity[2]);
        cairo_mesh_pattern_set_corner_color_rgb(p,
                                                0,
                                                prev_rgb_display[0],
                                                prev_rgb_display[1],
                                                prev_rgb_display[2]);
        cairo_mesh_pattern_set_corner_color_rgb(p,
                                                1,
                                                prev_rgb_display[0],
                                                prev_rgb_display[1],
                                                prev_rgb_display[2]);
        cairo_mesh_pattern_set_corner_color_rgb(p,
                                                2,
                                                rgb_display[0],
                                                rgb_display[1],
                                                rgb_display[2]);
        cairo_mesh_pattern_set_corner_color_rgb(p,
                                                3,
                                                rgb_display[0],
                                                rgb_display[1],
                                                rgb_display[2]);
        cairo_mesh_pattern_end_patch(p);
      }

      px = chromaticity[1];
      py = chromaticity[2];
      for_each_channel(ch, aligned(prev_rgb_display, rgb_display:16))
        prev_rgb_display[ch] = rgb_display[ch];
    }
  }
  // last patch
  cairo_mesh_pattern_begin_patch(p);
  cairo_mesh_pattern_move_to(p, 0., 0.);
  cairo_mesh_pattern_line_to(p, px, py);
  cairo_mesh_pattern_line_to(p, d->hue_ring[0][0][0], d->hue_ring[0][0][1]);
  cairo_mesh_pattern_set_corner_color_rgb(p,
                                          0,
                                          prev_rgb_display[0],
                                          prev_rgb_display[1],
                                          prev_rgb_display[2]);
  cairo_mesh_pattern_set_corner_color_rgb(p,
                                          1,
                                          prev_rgb_display[0],
                                          prev_rgb_display[1],
                                          prev_rgb_display[2]);
  cairo_mesh_pattern_set_corner_color_rgb(p,
                                          2,
                                          first_rgb_display[0],
                                          first_rgb_display[1],
                                          first_rgb_display[2]);
  cairo_mesh_pattern_set_corner_color_rgb(p,
                                          3,
                                          first_rgb_display[0],
                                          first_rgb_display[1],
                                          first_rgb_display[2]);
  cairo_mesh_pattern_end_patch(p);

  const int diam_px = d->vectorscope_diameter_px;
  const double pattern_max_radius = hypotf(diam_px, diam_px);
  cairo_matrix_t matrix;
  cairo_matrix_init_scale(&matrix, max_radius / pattern_max_radius,
                          max_radius / pattern_max_radius);
  cairo_matrix_translate(&matrix, -0.5*diam_px, -0.5*diam_px);
  cairo_pattern_set_matrix(p, &matrix);

  // rasterize chromaticities pattern for drawing speed
  cairo_surface_t *bkgd_surface =
    cairo_image_surface_create_for_data
    (d->vectorscope_bkgd, CAIRO_FORMAT_RGB24,
     diam_px, diam_px,
     cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, diam_px));

  cairo_t *crt = cairo_create(bkgd_surface);
  cairo_set_operator(crt, CAIRO_OPERATOR_SOURCE);
  cairo_set_source(crt, p);
  cairo_paint(crt);
  cairo_surface_destroy(bkgd_surface);
  cairo_pattern_destroy(p);
  cairo_destroy(crt);

  if(d->vectorscope_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
    for(int k = 0; k < 6; k++)
      for(int i = 0; i < VECTORSCOPE_HUES; i++)
        // NOTE: hypotenuse is already calculated above, but not worth caching it
        log_scale(&d->hue_ring[k][i][0], &d->hue_ring[k][i][1], max_radius);

  d->vectorscope_radius = max_radius;
  d->hue_ring_prof = vs_prof;
  d->hue_ring_scale = d->vectorscope_scale;
  d->hue_ring_colorspace = d->vectorscope_type;
}

static void _get_chromaticity(const dt_aligned_pixel_t RGB,
                              dt_aligned_pixel_t chromaticity,
                              const dt_lib_vectorscope_type_t vs_type,
                              const dt_iop_order_iccprofile_info_t *vs_prof,
                              const float *rgb2ryb_ypp)
{
  switch(vs_type)
  {
    case DT_LIB_VECTORSCOPE_TYPE_CIELUV:
    {
      // NOTE: see for comparison/reference rgb_to_JzCzhz() in color_picker.c
      dt_aligned_pixel_t XYZ_D50;
      // this goes to the PCS which has standard illuminant D50
      dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50,
                                 vs_prof->matrix_in_transposed,
                                 vs_prof->lut_in,
                                 vs_prof->unbounded_coeffs_in,
                                 vs_prof->lutsize,
                                 vs_prof->nonlinearlut);
      // FIXME: do have to worry about chromatic adaptation? this
      // assumes that the histogram profile white point is the same as
      // PCS whitepoint (D50) -- if we have a D65 whitepoint profile,
      // how does the result change if we adapt to D65 then convert to
      // L*u*v* with a D65 whitepoint?
      dt_aligned_pixel_t xyY_D50;
      dt_D50_XYZ_to_xyY(XYZ_D50, xyY_D50);
      // using D50 correct u*v* (not u'v') to be relative to the
      // whitepoint (important for vectorscope) and as u*v* is more
      // evenly spaced
      dt_xyY_to_Luv(xyY_D50, chromaticity);
      break;
    }
    case DT_LIB_VECTORSCOPE_TYPE_JZAZBZ:
    {
      dt_aligned_pixel_t XYZ_D50;
      // this goes to the PCS which has standard illuminant D50
      dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50,
                                 vs_prof->matrix_in_transposed,
                                 vs_prof->lut_in,
      vs_prof->unbounded_coeffs_in, vs_prof->lutsize, vs_prof->nonlinearlut);
      // FIXME: can skip a hop by pre-multipying matrices: see
      // colorbalancergb and dt_develop_blendif_init_masking_profile()
      // for how to make hacked profile
      dt_aligned_pixel_t XYZ_D65;
      // If the profile whitepoint is D65, its RGB -> XYZ conversion
      // matrix has been adapted to D50 (PCS standard) via
      // Bradford. Using Bradford again to adapt back to D65 gives a
      // pretty clean reversal of the transform.
      // FIXME: if the profile whitepoint is D50 (ProPhoto...), then
      // should we use a nicer adaptation (CAT16?) to D65?
      dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
      // FIXME: The bulk of processing time is spent in the XYZ ->
      // JzAzBz conversion in the 2*3 powf() in X'Y'Z' ->
      // L'M'S'. Making a LUT for these, using _apply_trc() to do
      // powf() work. It only needs to be accurate enough to be about
      // on the right pixel for a diam_px x diam_px plot
      dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
      break;
    }
    case DT_LIB_VECTORSCOPE_TYPE_RYB:
    {
      dt_aligned_pixel_t RYB, rgb, HCV;
      dt_sRGB_to_linear_sRGB(RGB, rgb);
      _rgb2ryb(rgb, RYB, rgb2ryb_ypp);
      dt_RGB_2_HCV(RYB, HCV);
      const float alpha = 2.f * M_PI_F * HCV[0];
      chromaticity[1] = cosf(alpha) * HCV[1] * 0.01;
      chromaticity[2] = sinf(alpha) * HCV[1] * 0.01;
      break;
    }
    case DT_LIB_VECTORSCOPE_TYPE_N:
      dt_unreachable_codepath();
  }
}

static void _lib_vectorscope_process
  (dt_lib_vectorscope_t *d,
   const float *const input,
   dt_histogram_roi_t *const roi,
   const dt_iop_order_iccprofile_info_t *vs_prof)
{
  const int diam_px = d->vectorscope_diameter_px;
  const dt_lib_vectorscope_type_t vs_type = d->vectorscope_type;
  const dt_lib_vectorscope_scale_t vs_scale = d->vectorscope_scale;

  _lib_vectorscope_bkgd(d, vs_prof);
  // FIXME: particularly for u*v*, center on hue ring bounds rather
  // than plot center, to be able to show a larger plot?
  const float max_radius = d->vectorscope_radius;
  const float max_diam = max_radius * 2.f;

  int sample_width = MAX(1, roi->width - roi->crop_right - roi->crop_x);
  int sample_height = MAX(1, roi->height - roi->crop_bottom - roi->crop_y);
  if(sample_width == 1 && sample_height == 1)
  {
    // point sample still calculates graph based on whole image
    sample_width = roi->width;
    sample_height = roi->height;
    roi->crop_x = roi->crop_y = 0;
  }

  const float *rgb2ryb_ypp = d->rgb2ryb_ypp;
  // RGB -> chromaticity (processor-heavy), count into bins by chromaticity
  //
  // FIXME: if we do convert to histogram RGB, should it be an
  // absolute colorimetric conversion (would mean knowing the
  // histogram profile whitepoint and un-adapting its matrices) and
  // then we have a meaningful whitepoint and could plot spectral
  // locus -- or the reverse, adapt the spectral locus to the
  // histogram profile PCS (always D50)?
  //
  // FIXME: pre-allocate? -- use the same buffer as for waveform?
  dt_atomic_int *const restrict binned = (dt_atomic_int*)dt_calloc_align_int(diam_px * diam_px);
  // FIXME: move verbosed interleaved comments into a method note at
  // the start, as the code itself is succinct and clear
  //
  // FIXME: even with getting rid of the extra profile conversion hop
  // there's no noticeable speedup -- maybe this loop is memory bound
  // -- if can get rid of one of the output buffers and still no
  // speedup, consider doing more work in this loop, such as atomic
  // binning
  //
  // FIXME: make 2x2 averaging be conditional on preprocessor define
  //
  // FIXME: average neighboring pixels on x but not y -- may be enough of an optimization
  const int sample_max_x = sample_width - (sample_width % 2);
  const int sample_max_y = sample_height - (sample_height % 2);
  // FIXME: if decimate/downsample, should blur before this
  //
  // FIXME: instead of scaling, if chromaticity really depends only on
  // XY, then make a lookup on startup of for each grid cell on graph
  // output the minimum XY to populate that cell, then either
  // brute-force scan that LUT, or start from position of last pixel
  // and scan, or do an optimized search (1/2, 1/2, 1/2, etc.) --
  // would also find point sample pixel this way

  DT_OMP_FOR(collapse(2))
  for(size_t y=0; y<sample_max_y; y+=2)
    for(size_t x=0; x<sample_max_x; x+=2)
    {
      // FIXME: There are unnecessary color math hops. Right now the
      // data comes into dt_lib_vectorscope_process() in a known profile
      // (usually from pixelpipe). Then (usually) it gets converted to
      // the histogram profile. Here it gets converted to XYZ D50
      // before making its way to L*u*v* or JzAzBz:
      //   RGB (pixelpipe) -> XYZ(PCS, D50) -> RGB (histogram) -> XYZ (PCS, D50) -> chromaticity
      // Given that the histogram profile is "well behaved" and the
      // conversion to histogram profile is relative colorimetric, could
      // instead:
      //   RGB (pixelpipe) -> XYZ(PCS, D50) -> chromaticity
      // A catch is that pixelpipe RGB may be a CLUT profile, hence would
      // need to have an LCMS path unless histogram moves to before colorout.
      dt_aligned_pixel_t RGB = {0.f}, chromaticity;
      // FIXME: for speed, downsample 2x2 -> 1x1 here, which still
      // should produce enough chromaticity data -- Question:
      // AVERAGE(RGBx4) -> chromaticity, or AVERAGE((RGB ->
      // chromaticity)x4)?
      //
      // FIXME: could compromise and downsample to 2x1 -- may also be
      // a bit faster than skipping rows
      const float *const restrict px =
        DT_IS_ALIGNED((const float *const restrict)input +
                      4U * ((y + roi->crop_y) * roi->width + x + roi->crop_x));
      for(size_t xx=0; xx<2; xx++)
        for(size_t yy=0; yy<2; yy++)
          for_each_channel(ch, aligned(px,RGB:16))
            RGB[ch] += px[4U * (yy * roi->width + xx) + ch] * 0.25f;

      _get_chromaticity(RGB, chromaticity, vs_type, vs_prof, rgb2ryb_ypp);
      // FIXME: we ignore the L or Jz components -- do they optimize
      // out of the above code, or would in particular a XYZ_2_AzBz
      // but helpful?
      if(vs_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
        log_scale(&chromaticity[1], &chromaticity[2], max_radius);

      // FIXME: make cx,cy which are float, check 0 <= cx < 1, then multiply by diam_px
      const int out_x = (diam_px-1) * (chromaticity[1] / max_diam + 0.5f);
      const int out_y = (diam_px-1) * (chromaticity[2] / max_diam + 0.5f);

      // clip any out-of-scale values, so there aren't light edges
      if(out_x >= 0 && out_x <= diam_px-1 && out_y >= 0 && out_y <= diam_px-1)
        dt_atomic_add_int(binned + out_y * diam_px + out_x, 1);
    }

  dt_aligned_pixel_t RGB = {0.f}, chromaticity;
  const dt_lib_colorpicker_statistic_t statistic =
    darktable.lib->proxy.colorpicker.statistic;
  dt_colorpicker_sample_t *sample;

  // find position of the primary sample
  sample = darktable.lib->proxy.colorpicker.primary_sample;
  memcpy(RGB, sample->scope[statistic], sizeof(dt_aligned_pixel_t));

  _get_chromaticity(RGB, chromaticity, vs_type, vs_prof, rgb2ryb_ypp);

  if(vs_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
    log_scale(&chromaticity[1], &chromaticity[2], max_radius);

  d->vectorscope_pt[0] = chromaticity[1];
  d->vectorscope_pt[1] = chromaticity[2];

  // if live simple visualized, find their position
  if(d->vectorscope_samples && darktable.lib->proxy.colorpicker.display_samples)
  {
    g_slist_free_full((GSList *)d->vectorscope_samples, free);
    d->vectorscope_samples = NULL;
    d->selected_sample = -1;
  }
  GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
  if(samples)
  {
    const dt_colorpicker_sample_t *selected =
      darktable.lib->proxy.colorpicker.selected_sample;

    int pos = 0;
    for(; samples; samples = g_slist_next(samples))
    {
      sample = samples->data;
      if(sample == selected) d->selected_sample = pos;
      pos++;

      //find coordinates
      memcpy(RGB, sample->scope[statistic], sizeof(dt_aligned_pixel_t));

      _get_chromaticity(RGB, chromaticity, vs_type, vs_prof, rgb2ryb_ypp);

      if(vs_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
        log_scale(&chromaticity[1], &chromaticity[2], max_radius);

      float *sample_xy = (float *)calloc(2, sizeof(float));

      sample_xy[0] = chromaticity[1];
      sample_xy[1] = chromaticity[2];

      d->vectorscope_samples = g_slist_append(d->vectorscope_samples, sample_xy);
    }
  }

  // shortcut to change from linear to display gamma
  const dt_iop_order_iccprofile_info_t *const profile =
    dt_ioppr_add_profile_info_to_list(darktable.develop,
                                      DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  const float *const restrict lut =
    DT_IS_ALIGNED((const float *const restrict)profile->lut_out[0]);
  const float lutmax = profile->lutsize - 1;
  const int out_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam_px);
  uint8_t *const graph = d->vectorscope_graph;

  // FIXME: should count the max bin size, and vary the scale such that it is always 1?
  const float gain = 1.f / 30.f;
  const float scale = gain * (diam_px * diam_px) / (sample_width * sample_height);

  // loop appears to be too small to benefit w/OpenMP
  // FIXME: is this still true?
  for(size_t out_y = 0; out_y < diam_px; out_y++)
    for(size_t out_x = 0; out_x < diam_px; out_x++)
    {
      const int count = binned[out_y * diam_px + out_x];
      const float intensity = lut[(int)(MIN(1.f, scale * count) * lutmax)];
      graph[out_y * out_stride + out_x] = intensity * 255.0f;
    }

  dt_free_align(binned);
}

static void dt_lib_vectorscope_process
  (struct dt_lib_module_t *self,
   const float *const input,
   int width,
   int height,
   const dt_iop_order_iccprofile_info_t *const profile_info_from,
   const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_lib_vectorscope_t *d = self->data;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    d->vectorscope_radius = 0.f;
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }

  // FIXME: scope goes black when click histogram lib colorpicker on
  // -- is this meant to happen?
  //
  // FIXME: scope doesn't redraw when click histogram lib colorpicker
  // off -- is this meant to happen?
  dt_histogram_roi_t roi = { .width = width,
                             .height = height,
                             .crop_x = 0,
                             .crop_y = 0,
                             .crop_right = 0,
                             .crop_bottom = 0 };

  // Constraining the area if the colorpicker is active in area mode
  //
  // FIXME: only need to do colorspace conversion below on roi
  //
  // FIXME: if the only time we use roi in histogram to limit area is
  // here, and whenever we use tether there is no colorpicker (true?),
  // and if we're always doing a colorspace transform in darkroom and
  // clip to roi during conversion, then can get rid of all roi code
  // for common/histogram?  when darkroom colorpicker is active,
  // gui_module is set to colorout
  if(dt_view_get_current() == DT_VIEW_DARKROOM
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    const dt_colorpicker_sample_t *const sample =
      darktable.lib->proxy.colorpicker.primary_sample;
    const dt_iop_color_picker_t *proxy = darktable.lib->proxy.colorpicker.picker_proxy;
    if(proxy && !proxy->module)
    {
      // FIXME: for histogram process whole image, then pull point
      // sample #'s from primary_picker->scope_mean (point) or _mean,
      // _min, _max (as in rgb curve) and draw them as an overlay
      //
      // FIXME: for waveform point sample, could process whole image,
      // then do an overlay of the point sample from
      // primary_picker->scope_mean as red/green/blue dots (or short
      // lines) at appropriate position at the horizontal/vertical
      // position of sample
      dt_boundingbox_t pos;
      const gboolean isbox = sample->size == DT_LIB_COLORPICKER_SIZE_BOX;
      const gboolean ispoint = sample->size == DT_LIB_COLORPICKER_SIZE_POINT;
      if(ispoint || isbox)
      {
        dt_color_picker_transform_box(darktable.develop,
                                     isbox ? 2 : 1,
                                     isbox ? sample->box : sample->point,
                                     pos, TRUE);
        roi.crop_x = MIN(width, MAX(0, pos[0] * width));
        roi.crop_y = MIN(height, MAX(0, pos[1] * height));
        roi.crop_right = width -    MIN(width,  MAX(0, (isbox ? pos[2] : pos[0]) * width));
        roi.crop_bottom = height -  MIN(height, MAX(0, (isbox ? pos[3] : pos[1]) * height));
      }
    }
  }

  // Convert pixelpipe output in display RGB to histogram profile. If
  // in tether view, then the image is already converted by the
  // caller.

  float *img_display = dt_alloc_align_float((size_t)4 * width * height);
  if(!img_display) return;

  // FIXME: we might get called with profile_info_to == NULL due to caller errors
  if(!profile_info_to)
  {
    dt_print(DT_DEBUG_ALWAYS,
       "[histogram] no histogram profile, replaced with linear Rec2020");
    dt_control_log(_("unsupported profile selected for histogram,"
                     " it will be replaced with linear Rec2020"));
  }

  const dt_iop_order_iccprofile_info_t *fallback =
    dt_ioppr_add_profile_info_to_list(darktable.develop,
      DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);

  const dt_iop_order_iccprofile_info_t *profile_info_out = !profile_info_to ? fallback : profile_info_to;

  dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height,
                                            profile_info_from, profile_info_out, "final histogram");

  dt_pthread_mutex_lock(&d->lock);
  // if using a non-rgb profile_info_out as in cmyk softproofing we pass DT_COLORSPACE_LIN_REC2020
  //   for calculating the vertex_rgb data.
  _lib_vectorscope_process(d, img_display, &roi, profile_info_out->type ? profile_info_out : fallback);
  dt_pthread_mutex_unlock(&d->lock);

  dt_free_align(img_display);

  dt_show_times_f(&start, "[vectorscope]", "final");
}

static void _lib_vectorscope_draw(const dt_lib_vectorscope_t *d, cairo_t *cr,
                                            const int width, const int height)
{
  const float vs_radius = d->vectorscope_radius;
  const int diam_px = d->vectorscope_diameter_px;
  const double node_radius = DT_PIXEL_APPLY_DPI(2.);
  const int min_size = MIN(width, height) - node_radius * 2.0;
  const double scale = min_size / (vs_radius * 2.);

  cairo_save(cr);

  // background
  cairo_pattern_t *p = cairo_pattern_create_radial
    (0.5 * width, 0.5 * height, 0.5 * min_size,
     0.5 * width, 0.5 * height, 0.5 * hypot(min_size, min_size));
  cairo_pattern_add_color_stop_rgb(p, 0.,
                                   darktable.bauhaus->graph_bg.red,
                                   darktable.bauhaus->graph_bg.green,
                                   darktable.bauhaus->graph_bg.blue);
  cairo_pattern_add_color_stop_rgb(p, 1.,
                                   darktable.bauhaus->graph_exterior.red,
                                   darktable.bauhaus->graph_exterior.green,
                                   darktable.bauhaus->graph_exterior.blue);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);

  // FIXME: the areas to left/right of the scope could have some data
  // (primaries, whitepoint, scale, etc.)
  cairo_translate(cr, width / 2., height / 2.);
  cairo_rotate(cr, d->vectorscope_angle);

  // traditional video editor's vectorscope is oriented with x-axis Y
  // -> B, y-axis C -> R but CIE 1976 UCS is graphed x-axis as u (G ->
  // M), y-axis as v (B -> Y), so do that and keep to the proper color
  // math
  cairo_scale(cr, 1., -1.);

  // concentric circles as a scale
  set_color(cr, darktable.bauhaus->graph_grid);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  const float grid_radius = d->hue_ring_colorspace == DT_LIB_VECTORSCOPE_TYPE_CIELUV
    ? 100. : 0.01;
  for(int i = 1; i < 1.f + ceilf(vs_radius/grid_radius); i++)
  {
    float r = grid_radius * i;
    if(d->vectorscope_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
      r = baselog(r, vs_radius);
    cairo_arc(cr, 0., 0., r * scale, 0., M_PI * 2.);
    cairo_stroke(cr);
  }

  // chromaticities for drawing both hue ring and graph
  cairo_surface_t *bkgd_surface =
    dt_cairo_image_surface_create_for_data
    (d->vectorscope_bkgd, CAIRO_FORMAT_RGB24,
     diam_px, diam_px,
     cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, diam_px));
  cairo_pattern_t *bkgd_pat = cairo_pattern_create_for_surface(bkgd_surface);
  // primary nodes circles may extend to outside of pattern
  cairo_pattern_set_extend(bkgd_pat, CAIRO_EXTEND_PAD);

  cairo_matrix_t matrix;
  cairo_matrix_init_translate(&matrix,
                              0.5*diam_px/darktable.gui->ppd,
                              0.5*diam_px/darktable.gui->ppd);
  cairo_matrix_scale(&matrix,
                     (double)diam_px / min_size / darktable.gui->ppd,
                     (double)diam_px / min_size / darktable.gui->ppd);
  cairo_pattern_set_matrix(bkgd_pat, &matrix);

  // FIXME: also add hue rings (monochrome/dotted) for input/work/output profiles
  // from Sobotka:

  // 1. The input encoding primaries. How dd the image start out life?
  // What is valid data within that? What is invalid introduced by
  // error of camera virtual primaries solving or math such as
  // resampling an image such that negative lobes result?
  //
  // 2. The working reference primaries. How did 1. end up in 2.? Are
  // there negative and therefore nonsensical values in the working
  // space? Should a gamut mapping pass be applied before work,
  // between 1. and 2.?
  //
  // 3. The output primaries rendition. From a selection of gamut
  // mappings, is one required between 2. and 3.?"

  // graticule: histogram profile hue ring
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_push_group(cr);
  cairo_set_source(cr, bkgd_pat);
  for(int n = 0; n < 6; n++)
    for(int h=0; h<VECTORSCOPE_HUES; h++)
    {
      // note that hue_ring coords are calculated as float but
      // converted here to double
      const float x = d->hue_ring[n][h][0];
      const float y = d->hue_ring[n][h][1];
      cairo_line_to(cr, x*scale, y*scale);
    }
  cairo_close_path(cr);
  cairo_stroke(cr);
  cairo_pop_group_to_source(cr);
  cairo_paint_with_alpha(cr, 0.4);

  // primary/secondary nodes
  for(int n = 0; n < 6; n++)
  {
    const float x = d->hue_ring[n][0][0];
    const float y = d->hue_ring[n][0][1];
    cairo_arc(cr, x*scale, y*scale, node_radius, 0., M_PI * 2.);
    cairo_set_source(cr, bkgd_pat);
    cairo_fill_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_grid);
    cairo_stroke(cr);
  }

  // vectorscope graph
  // FIXME: use cairo_pattern_set_filter()?
  cairo_surface_t *graph_surface =
    dt_cairo_image_surface_create_for_data
    (d->vectorscope_graph, CAIRO_FORMAT_A8,
     diam_px, diam_px,
     cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam_px));
  cairo_pattern_t *graph_pat = cairo_pattern_create_for_surface(graph_surface);
  cairo_pattern_set_matrix(graph_pat, &matrix);

  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);

  const gboolean display_primary_sample =
    darktable.lib->proxy.colorpicker.restrict_histogram
    && darktable.lib->proxy.colorpicker.primary_sample->size == DT_LIB_COLORPICKER_SIZE_POINT;
  const gboolean display_live_samples = d->vectorscope_samples
    && darktable.lib->proxy.colorpicker.display_samples;

  // we draw the color harmony guidelines
  if(d->vectorscope_type == DT_LIB_VECTORSCOPE_TYPE_RYB
     && d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
  {
    cairo_save(cr);

    const float hw = dt_lib_vectorscope_color_harmony_width[d->harmony_guide.width];
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    const dt_lib_vectorscope_color_harmony_t hm = dt_color_harmonies[d->harmony_guide.type];
    for(int i = 0; i < hm.sectors; i++)
    {
      float hr = vs_radius * hm.length[i];
      if(d->vectorscope_scale == DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC)
        hr = baselog(hr, vs_radius);
      const float span1 = (i > 0
                           ? MIN(hw, (hm.angle[i] - hm.angle[i-1]) / 2.f)
                           : hw); // avoid sectors overlap
      const float span2 = (i < hm.sectors - 1
                           ? MIN(hw, (hm.angle[i+1] - hm.angle[i]) / 2.f)
                           : hw);
      const float angle1 =
        (hm.angle[i] - span1) * 2.f * M_PI_F + deg2radf((float)d->harmony_guide.rotation);
      const float angle2 =
        (hm.angle[i] + span2) * 2.f * M_PI_F + deg2radf((float)d->harmony_guide.rotation);
      cairo_arc(cr, 0., 0., hr * scale, angle1, angle2);
      cairo_line_to(cr, 0., 0.);
    }
    cairo_close_path(cr);
    cairo_set_source(cr, bkgd_pat);
    set_color(cr, darktable.bauhaus->graph_fg);
    if(d->harmony_guide.width == DT_COLOR_HARMONY_WIDTH_LINE)
      cairo_stroke(cr);
    else
    {
      // we dim the histogram graph outside the harmony sectors
      cairo_stroke_preserve(cr);
      cairo_push_group(cr);
      cairo_paint_with_alpha
        (cr,
         dt_conf_get_float("plugins/darkroom/vectorscope/harmony/dim"));
      cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
      cairo_fill(cr);
      cairo_pattern_t *harmony_pat = cairo_pop_group(cr);

      cairo_set_source(cr, graph_pat);
      cairo_push_group(cr);
      cairo_mask(cr, harmony_pat);
      cairo_pattern_destroy(harmony_pat);
      cairo_pattern_destroy(graph_pat);
      graph_pat = cairo_pop_group(cr);
    }

    if(gtk_widget_get_visible(d->button_box_main))
    {
      // draw information about current selected harmony
      PangoLayout *layout;
      PangoRectangle ink;
      PangoFontDescription *desc =
        pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
      pango_font_description_set_weight(desc, PANGO_WEIGHT_NORMAL);
      pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(16) * PANGO_SCALE);
      layout = pango_cairo_create_layout(cr);
      pango_layout_set_font_description(layout, desc);
      pango_layout_set_alignment(layout, PANGO_ALIGN_RIGHT);

      gchar *text = g_strdup_printf("%d°\n%s", d->harmony_guide.rotation, _(hm.name));

      set_color(cr, darktable.bauhaus->graph_fg);
      pango_layout_set_text(layout, text, -1);
      pango_layout_get_pixel_extents(layout, NULL, &ink);
      cairo_scale(cr, 1., -1.);
      cairo_rotate(cr, -d->vectorscope_angle);
      cairo_move_to(cr,
                    0.48f * width - ink.width - ink.x,
                    0.48 * height - ink.height - ink.y);
      pango_cairo_show_layout(cr, layout);
      cairo_stroke(cr);
      pango_font_description_free(desc);
      g_object_unref(layout);
      g_free(text);
    }
    cairo_restore(cr);
  }

  if(display_primary_sample || display_live_samples)
    cairo_push_group(cr);
  cairo_set_source(cr, bkgd_pat);
  cairo_mask(cr, graph_pat);
  cairo_set_operator(cr, CAIRO_OPERATOR_HARD_LIGHT);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.55);
  cairo_mask(cr, graph_pat);

  cairo_pattern_destroy(bkgd_pat);
  cairo_surface_destroy(bkgd_surface);
  cairo_pattern_destroy(graph_pat);
  cairo_surface_destroy(graph_surface);

  if(display_primary_sample || display_live_samples)
  {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, 0.5);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  // overlay central circle
  set_color(cr, darktable.bauhaus->graph_grid);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0., 0., DT_PIXEL_APPLY_DPI(3.), 0., M_PI * 2.);
  cairo_fill(cr);

  if(display_primary_sample)
  {
    // point sample
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_arc(cr, scale*d->vectorscope_pt[0], scale*d->vectorscope_pt[1],
              DT_PIXEL_APPLY_DPI(3.), 0., M_PI * 2.);
    cairo_fill(cr);
  }

   // live samples
  if(display_live_samples)
  {
    GSList *samples = d->vectorscope_samples;
    const float *sample_xy = NULL;
    int pos = 0;
    for( ; samples; samples = g_slist_next(samples))
    {
      sample_xy = samples->data;
      if(pos == d->selected_sample)
      {
        set_color(cr, darktable.bauhaus->graph_fg_active);
        cairo_arc(cr,
                  scale * sample_xy[0],
                  scale * sample_xy[1],
                  DT_PIXEL_APPLY_DPI(6.),
                  0., M_PI * 2.);
        cairo_fill(cr);
      }
      else
      {
        set_color(cr, darktable.bauhaus->graph_fg);
        cairo_arc(cr,
                  scale * sample_xy[0],
                  scale * sample_xy[1],
                  DT_PIXEL_APPLY_DPI(4.), 0., M_PI * 2.);
        cairo_stroke(cr);
      }
      pos++;
    }
  }

  cairo_restore(cr);
}

static gboolean _drawable_draw_callback(GtkWidget *widget,
                                        cairo_t *crf,
                                        const gpointer user_data)
{
  dt_times_t start;
  dt_get_perf_times(&start);

  dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;
  const dt_develop_t *dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  // FIXME: should set histogram buffer to black if have just entered
  // tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  if((dt_view_get_current() == DT_VIEW_TETHERING
      || dev->image_storage.id == dev->preview_pipe->output_imgid)
     && (d->vectorscope_radius != 0.f))
  {
    _lib_vectorscope_draw(d, cr, width, height);
  }
  dt_pthread_mutex_unlock(&d->lock);

  // finally a thin border
  cairo_rectangle(cr, 0, 0, width, height);
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);

  dt_show_times_f(&start, "[vectorscope]", "scope draw");
  return FALSE;
}

static void _vectorscope_update_tooltip(const dt_lib_vectorscope_t *d)
{
  // FIXME: set the tooltip on scope type change
  gchar *tip = g_strdup_printf("%s\n(%s)\n%s\n%s",
                               _("vectorscope"),
                               _("use buttons at top of graph to change type"),
                               _("click on ❓ and then graph for documentation"),
                               _("use color picker module to restrict area"));
  if(d->vectorscope_type == DT_LIB_VECTORSCOPE_TYPE_RYB &&
     d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
  {
    dt_util_str_cat(&tip, "\n%s\n%s\n%s\n%s",
                    _("scroll to coarse-rotate"),
                    _("ctrl+scroll to fine rotate"),
                    _("shift+scroll to change width"),
                    _("alt+scroll to cycle"));
  }
  gtk_widget_set_tooltip_text(d->scope_draw, tip);
  g_free(tip);
}

static void _color_harmony_button_on(const dt_lib_vectorscope_t *d)
{
  const dt_color_harmony_type_t on = d->harmony_guide.type;

  for(dt_color_harmony_type_t c = DT_COLOR_HARMONY_MONOCHROMATIC;
      c < DT_COLOR_HARMONY_N;
      c++)
  {
    gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(d->color_harmony_button[c-1]), c == on);
  }
}

static void _color_harmony_changed(const dt_lib_vectorscope_t *d);
static void _color_harmony_changed_record(const dt_lib_vectorscope_t *d);

static gboolean _eventbox_scroll_callback(GtkWidget *widget,
                                          GdkEventScroll *event,
                                          dt_lib_vectorscope_t *d)
{
  int delta_y = 0;
  if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_MOD1_MASK))
  {
    // bubble to adjusting the overall widget size
    gtk_widget_event(d->scope_draw, (GdkEvent*)event);
  }
  else if(dt_gui_get_scroll_unit_delta(event, &delta_y) && delta_y != 0)
  {
    if(dt_modifier_is(event->state, GDK_SHIFT_MASK)) // SHIFT+SCROLL
    {
      if(d->harmony_guide.width == 0 && delta_y < 0)
        d->harmony_guide.width = DT_COLOR_HARMONY_WIDTH_N - 1;
      else
        d->harmony_guide.width = (d->harmony_guide.width +delta_y) % DT_COLOR_HARMONY_WIDTH_N;
    }
    else if(dt_modifier_is(event->state, GDK_MOD1_MASK)) // ALT+SCROLL
    {
      if(d->color_harmony_old == DT_COLOR_HARMONY_NONE && delta_y < 0)
        d->harmony_guide.type = DT_COLOR_HARMONY_N - 1;
      else
        d->harmony_guide.type = (d->color_harmony_old + delta_y) % DT_COLOR_HARMONY_N;
      _color_harmony_button_on(d);
      d->color_harmony_old = d->harmony_guide.type;
      _vectorscope_update_tooltip(d);
    }
    else
    {
      int a;
      if(dt_modifier_is(event->state, GDK_CONTROL_MASK)) // CTRL+SCROLL
        a = d->harmony_guide.rotation + delta_y;
      else // SCROLL
      {
        d->harmony_guide.rotation = (int)(d->harmony_guide.rotation / 15.) * 15;
        a = d->harmony_guide.rotation + 15 * delta_y;
      }
      a %= 360;
      if(a < 0) a += 360;
      d->harmony_guide.rotation = a;
    }
    _color_harmony_changed_record(d);
  }
  return TRUE;
}

static void _vectorscope_view_update(const dt_lib_vectorscope_t *d)
{
  switch(d->vectorscope_scale)
  {
    case DT_LIB_VECTORSCOPE_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scale_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scale_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_VECTORSCOPE_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->scale_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scale_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_VECTORSCOPE_SCALE_N:
      dt_unreachable_codepath();
  }
  switch(d->vectorscope_type)
  {
    case DT_LIB_VECTORSCOPE_TYPE_CIELUV:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to AzBz"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_luv, CPF_NONE, NULL);
      gtk_widget_hide(d->color_harmony_box);
      break;
    case DT_LIB_VECTORSCOPE_TYPE_JZAZBZ:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to RYB"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_jzazbz, CPF_NONE, NULL);
      gtk_widget_hide(d->color_harmony_box);
      break;
    case DT_LIB_VECTORSCOPE_TYPE_RYB:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to u*v*"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_ryb, CPF_NONE, NULL);
      gtk_widget_show(d->color_harmony_box);
      break;
    case DT_LIB_VECTORSCOPE_TYPE_N:
      dt_unreachable_codepath();
  }
}

static void _scope_scale_clicked(GtkWidget *button, dt_lib_vectorscope_t *d)
{
  d->vectorscope_scale = (d->vectorscope_scale + 1) % DT_LIB_VECTORSCOPE_SCALE_N;
  dt_conf_set_string("plugins/darkroom/vectorscope/scale",
                     dt_lib_vectorscope_scale_names[d->vectorscope_scale]);
  _vectorscope_view_update(d);

  // trigger new process from scratch
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

static void _colorspace_clicked(GtkWidget *button, dt_lib_vectorscope_t *d)
{
  d->vectorscope_type = (d->vectorscope_type + 1) % DT_LIB_VECTORSCOPE_TYPE_N;
  dt_conf_set_string("plugins/darkroom/vectorscope/type",
                     dt_lib_vectorscope_type_names[d->vectorscope_type]);
  _vectorscope_view_update(d);
  _vectorscope_update_tooltip(d);
  // trigger new process from scratch depending on whether CIELuv or JzAzBz
  // FIXME: it would be nice as with other scopes to make the initial
  // processing independent of the view
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

static void _update_color_harmony_gui(const dt_lib_module_t *self)
{
  dt_lib_vectorscope_t *d = self->data;

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');

  dt_color_harmony_init(&d->harmony_guide);

  if(img)
  {
    memcpy(&d->harmony_guide, &img->color_harmony_guide, sizeof(dt_color_harmony_guide_t));
    dt_image_cache_read_release(img);
  }

  // restore rotation/width default
  if(d->harmony_guide.type == DT_COLOR_HARMONY_NONE)
  {
    d->harmony_guide.rotation =
      dt_conf_get_int("plugins/darkroom/vectorscope/harmony_rotation");
    d->harmony_guide.width =
      dt_conf_get_int("plugins/darkroom/vectorscope/harmony_width");
  }

  _color_harmony_button_on(d);
  _color_harmony_changed(d);
  _vectorscope_update_tooltip(d);
}

void _signal_image_changed(gpointer instance, const dt_lib_module_t *self)
{

  _update_color_harmony_gui(self);
}

static void _color_harmony_changed(const dt_lib_vectorscope_t *d)
{
  gtk_widget_queue_draw(d->scope_draw);
}

static void _color_harmony_changed_record(const dt_lib_vectorscope_t *d)
{
  dt_conf_set_string("plugins/darkroom/vectorscope/harmony_type",
                     dt_color_harmonies[d->harmony_guide.type].name);
  // if color harmony unset, still keep the rotation/width as default
  if(d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
  {
    dt_conf_set_int("plugins/darkroom/vectorscope/harmony_width",
                    d->harmony_guide.width);
    dt_conf_set_int("plugins/darkroom/vectorscope/harmony_rotation",
                    d->harmony_guide.rotation);
  }

  _color_harmony_changed(d);
  _vectorscope_update_tooltip(d);

  const dt_imgid_t imgid = darktable.develop->image_storage.id;

  dt_image_t *img = dt_image_cache_get(imgid, 'w');

  memcpy(&img->color_harmony_guide,
         &d->harmony_guide,
         sizeof(dt_color_harmony_guide_t));

  dt_image_cache_write_release_info(img, DT_IMAGE_CACHE_SAFE, "histogram color_harmony_changed_record");
}

static gboolean _color_harmony_clicked(GtkWidget *button,
                                       GdkEventButton *event,
                                       dt_lib_vectorscope_t *d)
{
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
  {
    // clicked on active button, we remove guidelines
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), FALSE);
    d->harmony_guide.type = d->color_harmony_old = DT_COLOR_HARMONY_NONE;
  }
  else
  {
    // find positions of clicked button
    for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE; i < DT_COLOR_HARMONY_N - 1; i++)
      if(d->color_harmony_button[i] == button)
      {
        d->harmony_guide.type = d->color_harmony_old = i + 1;
        break;
      }
    _color_harmony_button_on(d);
  }
  _color_harmony_changed_record(d);
  return TRUE;
}

static gboolean _color_harmony_enter_notify_callback(const GtkWidget *widget,
                                                     GdkEventCrossing *event,
                                                     const gpointer user_data)
{
  dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;
  // find positions of entered button

  d->color_harmony_old = d->harmony_guide.type;

  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE; i < DT_COLOR_HARMONY_N - 1; i++)
    if(d->color_harmony_button[i] == widget)
    {
      d->harmony_guide.type = i + 1;
      break;
    }

  gtk_widget_queue_draw(d->scope_draw);
  return FALSE;
}

static gboolean _color_harmony_leave_notify_callback(GtkWidget *widget,
                                                     GdkEventCrossing *event,
                                                     const gpointer user_data)
{
  dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;
  d->harmony_guide.type = d->color_harmony_old;
  gtk_widget_queue_draw(d->scope_draw);
  return FALSE;
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget,
                                                GdkEventCrossing *event,
                                                const gpointer user_data)
{
  const dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;
  _vectorscope_view_update(d);
  gtk_widget_show(d->button_box_main);
  gtk_widget_show(d->button_box_opt);
  return FALSE;
}

static gboolean _eventbox_motion_notify_callback(GtkWidget *widget,
                                                 const GdkEventMotion *event,
                                                 const gpointer user_data)
{
  //This is required in order to correctly display the button tooltips
  const dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;

  GtkAllocation fix_alloc;
  gtk_widget_get_allocation(d->color_harmony_fix, &fix_alloc);
  const int full_height = gtk_widget_get_allocated_height(widget);
  const int excess =
    gtk_widget_get_allocated_height(d->color_harmony_box) + fix_alloc.y - full_height;
  const int shift = excess * MAX(event->y - fix_alloc.y, 0) / (full_height - fix_alloc.y);
  gtk_fixed_move(GTK_FIXED(d->color_harmony_fix), d->color_harmony_box, 0, - MAX(shift, 0));

  return FALSE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget,
                                                const GdkEventCrossing *event,
                                                const gpointer user_data)
{
  // when click between buttons on the buttonbox a leave event is generated -- ignore it
  if(!(event->mode == GDK_CROSSING_UNGRAB && event->detail == GDK_NOTIFY_INFERIOR))
  {
    const dt_lib_vectorscope_t *d = (dt_lib_vectorscope_t *)user_data;
    gtk_widget_hide(d->button_box_main);
    gtk_widget_hide(d->button_box_opt);
  }
  return FALSE;
}

static void _lib_vectorscope_collapse_callback(dt_action_t *action)
{
  dt_lib_module_t *self = darktable.lib->proxy.vectorscope.module;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);
}

static void _lib_vectorscope_change_type_callback(dt_action_t *action)
{
  const dt_lib_module_t *self = darktable.lib->proxy.vectorscope.module;
  dt_lib_vectorscope_t *d = self->data;
  _scope_scale_clicked(d->scale_button, d);
}

static void _lib_vectorscope_cycle_harmony_callback(dt_action_t *action)
{
  const dt_lib_module_t *self = darktable.lib->proxy.vectorscope.module;
  dt_lib_vectorscope_t *d = self->data;
  d->harmony_guide.type = (d->color_harmony_old + 1) % DT_COLOR_HARMONY_N;
  _color_harmony_button_on(d);
  d->color_harmony_old = d->harmony_guide.type;
  _color_harmony_changed_record(d);
}

// this is only called in darkroom view
static void _lib_vectorscope_preview_updated_callback(gpointer instance,
                                                    const dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  //
  // FIXME: it would be nice if process() just queued a redraw if not
  // in live view, but then our draw code would have to have some
  // other way to assure that the histogram image is current besides
  // checking the pixelpipe to see if it has processed the current
  // image
  const dt_lib_vectorscope_t *d = self->data;
  gtk_widget_queue_draw(d->scope_draw);
}

void view_enter(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  const dt_lib_vectorscope_t *d = self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _lib_vectorscope_preview_updated_callback);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->button_box_main);
  gtk_widget_hide(d->button_box_opt);

  _update_color_harmony_gui(self);
  // FIXME: set vectorscope data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self,
                struct dt_view_t *old_view,
                struct dt_view_t *new_view)
{
  DT_CONTROL_SIGNAL_DISCONNECT(_lib_vectorscope_preview_updated_callback, self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_vectorscope_t *d = dt_calloc1_align_type(dt_lib_vectorscope_t);
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  const char *str = dt_conf_get_string_const("plugins/darkroom/vectorscope/type");
  for(dt_lib_vectorscope_type_t i=0; i<DT_LIB_VECTORSCOPE_TYPE_N; i++)
    if(g_strcmp0(str, dt_lib_vectorscope_type_names[i]) == 0)
      d->vectorscope_type = i;

  str = dt_conf_get_string_const("plugins/darkroom/vectorscope/scale");
  for(dt_lib_vectorscope_scale_t i=0; i<DT_LIB_VECTORSCOPE_SCALE_N; i++)
    if(g_strcmp0(str, dt_lib_vectorscope_scale_names[i]) == 0)
      d->vectorscope_scale = i;

  const int a = dt_conf_get_int("plugins/darkroom/vectorscope/angle");
  d->vectorscope_angle = deg2rad(a);

  // FIXME: what is the appropriate resolution for this: balance
  // memory use, processing speed, helpful resolution
  //
  // FIXME: make this and waveform params #DEFINEd or const above,
  // rather than set here?
  d->vectorscope_diameter_px = 384;
  d->vectorscope_graph = dt_alloc_align_uint8(d->vectorscope_diameter_px *
     cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->vectorscope_diameter_px));
  d->vectorscope_bkgd =
    dt_alloc_align_uint8(4U * d->vectorscope_diameter_px *
                         cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, d->vectorscope_diameter_px));
  d->hue_ring_prof = NULL;
  d->hue_ring_scale = DT_LIB_VECTORSCOPE_SCALE_N;
  d->hue_ring_colorspace = DT_LIB_VECTORSCOPE_TYPE_N;
  // initially no vectorscope to draw
  d->vectorscope_radius = 0.f;

  // initially no live samples
  d->vectorscope_samples = NULL;
  d->selected_sample = -1;

  d->rgb2ryb_ypp = interpolate_set(sizeof(x_vtx)/sizeof(float),
                                   (float *)x_vtx, (float *)ryb_y_vtx, CUBIC_SPLINE);
  d->ryb2rgb_ypp = interpolate_set(sizeof(x_vtx)/sizeof(float),
                                   (float *)x_vtx, (float *)rgb_y_vtx, CUBIC_SPLINE);

  // set the default harmony (last used), the actual harmony for the image
  // will be restored later.
  str = dt_conf_get_string_const("plugins/darkroom/vectorscope/harmony_type");
  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE; i < DT_COLOR_HARMONY_N; i++)
    if(g_strcmp0(str, dt_color_harmonies[i].name) == 0)
      d->color_harmony_old = d->harmony_guide.type = i;
  d->harmony_guide.rotation =
    dt_conf_get_int("plugins/darkroom/vectorscope/harmony_rotation");
  d->harmony_guide.width =
    dt_conf_get_int("plugins/darkroom/vectorscope/harmony_width");

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.vectorscope.module = self;
  darktable.lib->proxy.vectorscope.process = dt_lib_vectorscope_process;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();
  dt_action_t *dark =
    dt_action_section(&darktable.view_manager->proxy.darkroom.view->actions,
                      N_("vectorscope"));
  dt_action_t *ac = NULL;

  // shows the scope, scale, and has draggable areas
  // FIXME: this should have its own separate conf
  d->scope_draw = dt_ui_resize_wrap(NULL,
                                    0,
                                    "plugins/darkroom/vectorscope/graphheight");
  ac = dt_action_define(dark, NULL, N_("hide vectorscope"), d->scope_draw, NULL);
  dt_action_register(ac, NULL, _lib_vectorscope_collapse_callback,
                     GDK_KEY_V, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  gtk_widget_set_events(d->scope_draw, GDK_ENTER_NOTIFY_MASK);
  _vectorscope_update_tooltip(d);

  // a row of control buttons, split in two button boxes, on left and right side
  d->button_box_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(d->button_box_main, "button_box");
  gtk_widget_set_valign(d->button_box_main, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box_main, GTK_ALIGN_START);

  GtkWidget *box_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_left, GTK_ALIGN_START);
  gtk_widget_set_halign(box_left, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->button_box_main), box_left, FALSE, FALSE, 0);

  d->color_harmony_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(d->color_harmony_box, GTK_ALIGN_START);
  gtk_widget_set_halign(d->color_harmony_box, GTK_ALIGN_START);
  d->color_harmony_fix = gtk_fixed_new();
  gtk_fixed_put(GTK_FIXED(d->color_harmony_fix), d->color_harmony_box, 0, 0);
  gtk_box_pack_start(GTK_BOX(d->button_box_main), d->color_harmony_fix, FALSE, FALSE, 0);

  d->button_box_opt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  dt_gui_add_class(d->button_box_opt, "button_box");
  gtk_widget_set_valign(d->button_box_opt, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box_opt, GTK_ALIGN_END);

  // this intermediate box is needed to make the actions on buttons work
  GtkWidget *box_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_valign(box_right, GTK_ALIGN_START);
  gtk_widget_set_halign(box_right, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(d->button_box_opt), box_right, FALSE, FALSE, 0);

  // FIXME: the button transitions when they appear on mouseover
  // (mouse enters scope widget) or change (mouse click) cause redraws
  // of the entire scope -- is there a way to avoid this?

  dt_action_t *teth = &darktable.view_manager->proxy.tethering.view->actions;
  if(teth)
  {
    dt_action_register(teth, N_("hide vectorscope"),
                       _lib_vectorscope_collapse_callback,
                       GDK_KEY_V, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    dt_action_register(teth, N_("switch vectorscope view"),
                       _lib_vectorscope_change_type_callback, 0, 0);
  }

  d->scale_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  ac = dt_action_define(dark, NULL, N_("switch vectorscope scale"),
                        d->scale_button, &dt_action_def_button);
  gtk_box_pack_end(GTK_BOX(box_right), d->scale_button, FALSE, FALSE, 0);

  d->colorspace_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  dt_action_define(dark, NULL, N_("cycle vectorscope types"),
                   d->colorspace_button, &dt_action_def_button);
  gtk_box_pack_end(GTK_BOX(box_right), d->colorspace_button, FALSE, FALSE, 0);

  // a series of buttons for color harmony guide lines
  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_MONOCHROMATIC;
      i < DT_COLOR_HARMONY_N;
      i++)
  {
    GtkWidget *rb = dtgtk_togglebutton_new(dtgtk_cairo_paint_color_harmony, CPF_NONE,
                                           &(dt_color_harmonies[i]));
    dt_action_define(dark, N_("color harmonies"),
                     dt_color_harmonies[i].name, rb, &dt_action_def_toggle);
    g_signal_connect(G_OBJECT(rb), "button-press-event",
                     G_CALLBACK(_color_harmony_clicked), d);
    g_signal_connect(G_OBJECT(rb), "enter-notify-event",
                     G_CALLBACK(_color_harmony_enter_notify_callback), d);
    g_signal_connect(G_OBJECT(rb), "leave-notify-event",
                     G_CALLBACK(_color_harmony_leave_notify_callback), d);

    gtk_box_pack_start(GTK_BOX(d->color_harmony_box), rb, FALSE, FALSE, 0);
    d->color_harmony_button[i-1] = rb;
  }
  _color_harmony_button_on(d);

  dt_action_register(dark, N_("cycle color harmonies"),
                     _lib_vectorscope_cycle_harmony_callback, 0, 0);

  // FIXME: add a brightness control (via GtkScaleButton?). Different per each mode?

  // assemble the widgets

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.
  //
  // |----- EventBox -----|
  // |                    |
  // |  |-- Overlay  --|  |
  // |  |              |  |
  // |  |  ButtonBox   |  |
  // |  |              |  |
  // |  |--------------|  |
  // |  |              |  |
  // |  |  DrawingArea |  |
  // |  |              |  |
  // |  |--------------|  |
  // |                    |
  // |--------------------|

  GtkWidget *eventbox = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(overlay), d->scope_draw);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box_main);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box_opt);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-vectorscope");

  /* connect callbacks */
  g_signal_connect(G_OBJECT(d->scale_button), "clicked",
                   G_CALLBACK(_scope_scale_clicked), d);
  g_signal_connect(G_OBJECT(d->colorspace_button), "clicked",
                   G_CALLBACK(_colorspace_clicked), d);

  gtk_widget_add_events(d->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK
                                     | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_drawable_draw_callback), d);

  gtk_widget_add_events
    (eventbox,
     GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK
     | GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(eventbox), "scroll-event",
                   G_CALLBACK(_eventbox_scroll_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "motion-notify-event",
                   G_CALLBACK(_eventbox_motion_notify_callback), d);

  gtk_widget_show_all(self->widget);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _signal_image_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_vectorscope_t *d = self->data;

  dt_free_align(d->vectorscope_graph);
  dt_free_align(d->vectorscope_bkgd);
  if(d->vectorscope_samples)
    g_slist_free_full((GSList *)d->vectorscope_samples, free);
  d->vectorscope_samples = NULL;
  d->selected_sample = -1;
  dt_pthread_mutex_destroy(&d->lock);
  g_free(d->rgb2ryb_ypp);
  g_free(d->ryb2rgb_ypp);
  dt_free_align(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
