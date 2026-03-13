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

#include "common/atomic.h"
#include "common/color_harmony.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "gui/accelerators.h"
#include "libs/colorpicker.h"
#include "scopes.h"

// # of gradations between each primary/secondary to draw the hue ring
// this is tuned to most degenerate cases: curve to blue primary in
// Luv in linear ProPhoto RGB and the widely spaced gradations of the
// PQ P3 RGB colorspace. This could be lowered to 32 with little
// visible consequence.
#define VECTORSCOPE_HUES 48
#define VECTORSCOPE_BASE_LOG 30

typedef enum dt_scopes_vec_scale_t
{
  DT_SCOPES_VEC_SCALE_LOGARITHMIC = 0,
  DT_SCOPES_VEC_SCALE_LINEAR,
  DT_SCOPES_VEC_SCALE_N // needs to be the last one
} dt_scopes_vec_scale_t;

typedef enum dt_scopes_vec_vectorscope_type_t
{
  DT_SCOPES_VEC_VECTORSCOPE_CIELUV = 0,   // CIE 1976 u*v*
  DT_SCOPES_VEC_VECTORSCOPE_JZAZBZ,
  DT_SCOPES_VEC_VECTORSCOPE_RYB,
  DT_SCOPES_VEC_VECTORSCOPE_N // needs to be the last one
} dt_scopes_vec_vectorscope_type_t;

typedef struct dt_scopes_vec_color_harmony_t
{
  const char *name;
  const int sectors;      // how many sectors
  const float angle[4];   // the angle of the sector center, expressed in fractions of a full turn
  const float length[4];  // the radius of the sector, from 0. to 1., linear scale
} dt_scopes_vec_color_harmony_t;

dt_scopes_vec_color_harmony_t _vec_color_harmonies[DT_COLOR_HARMONY_N] =
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

const gchar *dt_scopes_vec_scale_names[DT_SCOPES_VEC_SCALE_N] =
  { "logarithmic",
    "linear"
  };

const gchar *dt_scopes_vec_vectorscope_type_names[DT_SCOPES_VEC_VECTORSCOPE_N] =
  { "u*v*",
    "AzBz",
    "RYB"
  };

const gchar *dt_scopes_vec_color_harmony_width_names[DT_COLOR_HARMONY_WIDTH_N] =
  { "normal",
    "large",
    "narrow",
    "line"
  };

const float dt_scopes_vec_color_harmony_width[DT_COLOR_HARMONY_WIDTH_N] =
  { 0.5f/12.f, 0.75f/12.f, 0.25f/12.f, 0.0f };

typedef struct dt_scopes_vec_t
{
  uint8_t *vectorscope_graph, *vectorscope_bkgd;
  float vectorscope_pt[2];            // point colorpicker position
  GSList *vectorscope_samples;        // live samples position
  int selected_sample;                // position of the selected live sample in the list
  int vectorscope_diameter_px;
  float hue_ring[6][VECTORSCOPE_HUES][2] DT_ALIGNED_ARRAY;
  const dt_iop_order_iccprofile_info_t *hue_ring_prof;
  dt_scopes_vec_scale_t hue_ring_scale;
  dt_scopes_vec_vectorscope_type_t hue_ring_colorspace;
  double vectorscope_radius;

  GtkWidget *color_harmony_box;        // GtkBox -- contains color harmony buttons
  GtkWidget *color_harmony_fix;        // GtkFixed -- contains moveable color harmony buttons
  GtkWidget *vec_scale_button;         // GtkButton -- linear or logarithmic vectorscope
  GtkWidget *colorspace_button;        // GtkButton -- vectorscope colorspace
  GtkWidget *color_harmony_button
    [DT_COLOR_HARMONY_N];              // GtkButton -- RYB vectorscope color harmonies
  gulong toggle_signal_handler[DT_COLOR_HARMONY_N];

  // state set by buttons
  dt_scopes_vec_vectorscope_type_t vectorscope_type;
  dt_scopes_vec_scale_t vectorscope_scale;
  double vectorscope_angle;
  gboolean red, green, blue;
  float *rgb2ryb_ypp;
  float *ryb2rgb_ypp;
  dt_color_harmony_guide_t harmony_guide;
  dt_color_harmony_type_t harmony_prelight;
  dt_color_harmony_type_t ignore_prelight;
} dt_scopes_vec_t;


const char* _vec_name(const dt_scopes_mode_t *const self)
{
  return N_("vectorscope");
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

static void _lib_histogram_vectorscope_bkgd(dt_scopes_vec_t *d,
                                            const dt_iop_order_iccprofile_info_t *const vs_prof)
{
  // FIXME: we use vectorscope_radius == 0.f as a flag to force recalc, but really should distinguish between full recalc needed (background + process data) or just needing to reprocess data
  if(vs_prof == d->hue_ring_prof
     && d->vectorscope_scale == d->hue_ring_scale
     && d->vectorscope_type == d->hue_ring_colorspace
     && d->vectorscope_radius != 0.f)
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
  const dt_scopes_vec_vectorscope_type_t vs_type = d->vectorscope_type;

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
        case DT_SCOPES_VEC_VECTORSCOPE_CIELUV:
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
        case DT_SCOPES_VEC_VECTORSCOPE_JZAZBZ:
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
        case DT_SCOPES_VEC_VECTORSCOPE_RYB:
        {
          // get the color to be displayed
          _ryb2rgb(rgb_scope, rgb_display, d->ryb2rgb_ypp);
          const float alpha = M_PI_F * (0.33333f * ((float)k + (float)i / VECTORSCOPE_HUES));
          chromaticity[1] = cosf(alpha) * 0.01;
          chromaticity[2] = sinf(alpha) * 0.01;
          break;
        }
        case DT_SCOPES_VEC_VECTORSCOPE_N:
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

  if(d->vectorscope_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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
                              const dt_scopes_vec_vectorscope_type_t vs_type,
                              const dt_iop_order_iccprofile_info_t *vs_prof,
                              const float *rgb2ryb_ypp)
{
  switch(vs_type)
  {
    case DT_SCOPES_VEC_VECTORSCOPE_CIELUV:
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
    case DT_SCOPES_VEC_VECTORSCOPE_JZAZBZ:
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
    case DT_SCOPES_VEC_VECTORSCOPE_RYB:
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
    case DT_SCOPES_VEC_VECTORSCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _vec_process(dt_scopes_mode_t *const self,
                         const float *const input,
                         dt_histogram_roi_t *const roi,
                         const dt_iop_order_iccprofile_info_t *vs_prof)
{
  dt_scopes_vec_t *const d = self->data;

  const int diam_px = d->vectorscope_diameter_px;
  const dt_scopes_vec_vectorscope_type_t vs_type = d->vectorscope_type;
  const dt_scopes_vec_scale_t vs_scale = d->vectorscope_scale;

  _lib_histogram_vectorscope_bkgd(d, vs_prof);
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
      // data comes into dt_lib_histogram_process() in a known profile
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
      if(vs_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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

  if(vs_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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

      if(vs_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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
  self->update_counter = self->scopes->update_counter;
}

static void _vec_clear(dt_scopes_mode_t *const self)
{
  // FIXME: make sure this is actually called appropriately in histogram.c
  dt_scopes_vec_t *const d = self->data;
  d->vectorscope_radius = 0.f;
}

static void _vec_draw(const dt_scopes_mode_t *const self,
                      cairo_t *cr,
                      const int width,
                      const int height)
{
  dt_scopes_vec_t *const d = self->data;

  const float vs_radius = d->vectorscope_radius;
  const int diam_px = d->vectorscope_diameter_px;
  const double node_radius = DT_PIXEL_APPLY_DPI(2.);
  const int min_size = MIN(width, height) - node_radius * 2.0;
  const double scale = min_size / (vs_radius * 2.);

  if(vs_radius == 0.f)
    return;

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
  const float grid_radius = d->hue_ring_colorspace == DT_SCOPES_VEC_VECTORSCOPE_CIELUV
    ? 100. : 0.01;
  for(int i = 1; i < 1.f + ceilf(vs_radius/grid_radius); i++)
  {
    float r = grid_radius * i;
    if(d->vectorscope_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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
  dt_color_harmony_type_t cur_harmony =
    (d->vectorscope_type == DT_SCOPES_VEC_VECTORSCOPE_RYB
     ? (d->harmony_prelight != DT_COLOR_HARMONY_NONE ? d->harmony_prelight : d->harmony_guide.type)
     : DT_COLOR_HARMONY_NONE);
  if(cur_harmony)
  {
    cairo_save(cr);

    const float hw = dt_scopes_vec_color_harmony_width[d->harmony_guide.width];
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    const dt_scopes_vec_color_harmony_t hm = _vec_color_harmonies[cur_harmony];
    for(int i = 0; i < hm.sectors; i++)
    {
      float hr = vs_radius * hm.length[i];
      if(d->vectorscope_scale == DT_SCOPES_VEC_SCALE_LOGARITHMIC)
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
         dt_conf_get_float("plugins/darkroom/histogram/vectorscope/harmony/dim"));
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

    // FIXME: is there a less awkward way to check if the mouse is over this, or could this even be another widget in the overlay?
    if(gtk_widget_get_visible(self->scopes->button_box_main))
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

static void _vec_update_buttons(const dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *d = self->data;
  switch(d->vectorscope_scale)
  {
    case DT_SCOPES_VEC_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->vec_scale_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->vec_scale_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_SCOPES_VEC_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->vec_scale_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->vec_scale_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_SCOPES_VEC_SCALE_N:
      dt_unreachable_codepath();
  }
  switch(d->vectorscope_type)
  {
    case DT_SCOPES_VEC_VECTORSCOPE_CIELUV:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to AzBz"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_luv, CPF_NONE, NULL);
      gtk_widget_hide(d->color_harmony_box);
      break;
    case DT_SCOPES_VEC_VECTORSCOPE_JZAZBZ:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to RYB"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_jzazbz, CPF_NONE, NULL);
      gtk_widget_hide(d->color_harmony_box);
      break;
    case DT_SCOPES_VEC_VECTORSCOPE_RYB:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to u*v*"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_ryb, CPF_NONE, NULL);
      gtk_widget_show(d->color_harmony_box);
      break;
    case DT_SCOPES_VEC_VECTORSCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _color_harmony_changed_record(dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *const d = self->data;
  dt_conf_set_string("plugins/darkroom/histogram/vectorscope/harmony_type",
                     _vec_color_harmonies[d->harmony_guide.type].name);
  // if color harmony unset, still keep the rotation/width as default
  if(d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
  {
    dt_conf_set_int("plugins/darkroom/histogram/vectorscope/harmony_width",
                    d->harmony_guide.width);
    dt_conf_set_int("plugins/darkroom/histogram/vectorscope/harmony_rotation",
                    d->harmony_guide.rotation);
  }

  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  dt_image_t *img = dt_image_cache_get(imgid, 'w');
  if(img)
    memcpy(&img->color_harmony_guide,
           &d->harmony_guide,
           sizeof(dt_color_harmony_guide_t));
  dt_image_cache_write_release_info(img, DT_IMAGE_CACHE_SAFE, "histogram color_harmony_changed_record");

  dt_scopes_refresh(self->scopes);
}

static void _color_harmony_state_changed(GtkWidget *widget,
                                         GtkStateFlags flags,
                                         dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *const d = self->data;
  GtkStateFlags new_flags = gtk_widget_get_state_flags(widget);
  const dt_color_harmony_type_t prior = d->harmony_prelight;
  if(new_flags & GTK_STATE_FLAG_PRELIGHT)
  {
    for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE+1; i < DT_COLOR_HARMONY_N - 1; i++)
      if(d->color_harmony_button[i] == widget && i != d->ignore_prelight)
      {
        d->harmony_prelight = i;
        d->ignore_prelight = DT_COLOR_HARMONY_NONE;
      }
  }
  else
  {
    // FIXME: ideal would be to leave harmony preview on until leave
    // the container of harmony buttons, so don't flicker as move
    // between buttons
    d->harmony_prelight = DT_COLOR_HARMONY_NONE;
    d->ignore_prelight = DT_COLOR_HARMONY_NONE;
  }
  if(d->harmony_prelight != prior)
    dt_scopes_refresh(self->scopes);
}

static void _color_harmony_toggled(GtkButton *button,
                                   dt_scopes_mode_t *const self)
{
  // this toggle handler is the way to set d->harmony_guide.type:
  // updating the UI widget will update internal data structures, but
  // not vice versa
  dt_scopes_vec_t *const d = self->data;
  // find positions of clicked button
  const dt_color_harmony_type_t prior = d->harmony_guide.type;
  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE+1; i < DT_COLOR_HARMONY_N; i++)
  {
    if(d->color_harmony_button[i] == GTK_WIDGET(button))
    {
      if(d->harmony_guide.type == i)
      {
        // clicked on active button, remove guidelines
        if(!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button)))
        {
          d->harmony_guide.type = DT_COLOR_HARMONY_NONE;
          d->harmony_prelight = DT_COLOR_HARMONY_NONE;
          // don't immediately turn back on scope preview
          d->ignore_prelight = i;
        }
      }
      else
      {
        // clicked on an inactive button, activate guidelines
        if(prior != DT_COLOR_HARMONY_NONE)
        {
          g_signal_handler_block(d->color_harmony_button[prior], d->toggle_signal_handler[prior]);
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_harmony_button[prior]), FALSE);
          g_signal_handler_unblock(d->color_harmony_button[prior], d->toggle_signal_handler[prior]);
        }
        d->harmony_guide.type = i;
        d->ignore_prelight = DT_COLOR_HARMONY_NONE;
      }
    }
  }
  _color_harmony_changed_record(self);
}

static void _vec_append_to_tooltip(const dt_scopes_mode_t *const self,
                                   gchar **tip)
{
  const dt_scopes_vec_t *const d = self->data;
  if(d->vectorscope_type == DT_SCOPES_VEC_VECTORSCOPE_RYB &&
     d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
    dt_util_str_cat(tip, "\n%s\n%s\n%s\n%s",
                    _("scroll to coarse-rotate"),
                    _("ctrl+scroll to fine rotate"),
                    _("shift+scroll to change width"),
                    _("alt+scroll to cycle"));
}

static void _vec_eventbox_scroll(dt_scopes_mode_t *const self,
                                 gdouble x, gdouble y,
                                 gdouble delta_x, gdouble delta_y,
                                 GdkModifierType state)
{
  dt_scopes_vec_t *const d = self->data;

  // FIXME: if have own drawable for vectorscope can set scroll handler directly
  // clamp as mouse wheel scrolls sometimes report a delta of 2
  const int delta = CLAMP(delta_y, -1.0, 1.0);
  if(dt_modifier_is(state, GDK_SHIFT_MASK)) //( SHIFT+SCROLL
  {
    d->harmony_guide.width = (d->harmony_guide.width + delta + DT_COLOR_HARMONY_WIDTH_N)
                             % DT_COLOR_HARMONY_WIDTH_N;
  }
  else if(dt_modifier_is(state, GDK_MOD1_MASK)) // ALT+SCROLL
  {
    const dt_color_harmony_type_t new_type =
      (d->harmony_guide.type + delta + DT_COLOR_HARMONY_N) % DT_COLOR_HARMONY_N;
    if(new_type == DT_COLOR_HARMONY_NONE)
      // turn all buttons off
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(d->color_harmony_button[d->harmony_guide.type]), FALSE);
    else
      // will automatically turn off the prior button
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(d->color_harmony_button[new_type]), TRUE);
  }
  else
  {
    if(dt_modifier_is(state, GDK_CONTROL_MASK)) // CTRL+SCROLL
      d->harmony_guide.rotation += delta;
    else // SCROLL
    {
      d->harmony_guide.rotation = ((d->harmony_guide.rotation + 7) / 15) * 15;
      d->harmony_guide.rotation += 15 * delta;
    }
    d->harmony_guide.rotation = (d->harmony_guide.rotation + 360) % 360;
  }
  _color_harmony_changed_record(self);
}

static void _vec_eventbox_motion(dt_scopes_mode_t *const self,
                                 GtkEventControllerMotion *controller,
                                 double x,
                                 double y)
{
  dt_scopes_vec_t *const d = self->data;
  // FIXME: replace the color harmony box buttons with a widget with a combobox, then get rid of the eventbox motion callback
  // FIXME: this shouldn't do anything unless in RYB mode
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
  GtkAllocation fix_alloc;
  // FIXME: why use gtk_widget_get_allocation vs. gtk_widget_get_allocated_height?
  gtk_widget_get_allocation(d->color_harmony_fix, &fix_alloc);
  const int full_height = gtk_widget_get_allocated_height(widget);
  const int excess =
    gtk_widget_get_allocated_height(d->color_harmony_box) + fix_alloc.y - full_height;
  const int shift = excess * MAX(y - fix_alloc.y, 0) / (full_height - fix_alloc.y);
  gtk_fixed_move(GTK_FIXED(d->color_harmony_fix), d->color_harmony_box, 0, - MAX(shift, 0));
}

static void _vec_scale_clicked(GtkWidget *button, dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *d = self->data;

  d->vectorscope_scale = (d->vectorscope_scale + 1) % DT_SCOPES_VEC_SCALE_N;
  dt_conf_set_string("plugins/darkroom/histogram/vectorscope/scale",
                     dt_scopes_vec_scale_names[d->vectorscope_scale]);
  _vec_update_buttons(self);
  dt_scopes_reprocess();
}

static void _vec_colorspace_clicked(GtkWidget *button, dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *d = self->data;
  d->vectorscope_type = (d->vectorscope_type + 1) % DT_SCOPES_VEC_VECTORSCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/vectorscope",
                     dt_scopes_vec_vectorscope_type_names[d->vectorscope_type]);
  _vec_update_buttons(self);
  lib_histogram_update_tooltip(self->scopes);
  dt_scopes_reprocess();
}

static void _lib_histogram_cycle_harmony_callback(dt_action_t *action)
{
  const dt_lib_module_t *self = darktable.lib->proxy.histogram.module;
  dt_scopes_t *s = self->data;
  // we might be called with current mode as split, but want action to happen on vectorscope
  dt_scopes_mode_t *vec_mode = &s->modes[DT_SCOPES_MODE_VECTORSCOPE];
  dt_scopes_vec_t *d = vec_mode->data;

  const dt_color_harmony_type_t new_type = (d->harmony_guide.type + 1) % DT_COLOR_HARMONY_N;
  if(new_type == DT_COLOR_HARMONY_NONE)
    // turn all buttons off
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_harmony_button[d->harmony_guide.type]), FALSE);
  else
    // will automatically turn off the prior button
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_harmony_button[new_type]), TRUE);
  _color_harmony_changed_record(vec_mode);
}

void _vec_signal_image_changed(gpointer instance, dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *const d = self->data;
  dt_color_harmony_guide_t new_guide;
  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  dt_color_harmony_init(&new_guide);
  if(img)
  {
    memcpy(&new_guide, &img->color_harmony_guide, sizeof(dt_color_harmony_guide_t));
    dt_image_cache_read_release(img);
  }

  // FIXME: changing type toggle button calls _color_harmony_changed_record() which saves back to image the harmony data which we just loaded
  if(new_guide.type == DT_COLOR_HARMONY_NONE)
  {
    if(d->harmony_guide.type != DT_COLOR_HARMONY_NONE)
      // deselect prior guide
      gtk_toggle_button_set_active
        (GTK_TOGGLE_BUTTON(d->color_harmony_button[d->harmony_guide.type]), FALSE);
    // restore rotation/width default
    d->harmony_guide.rotation =
      dt_conf_get_int("plugins/darkroom/histogram/vectorscope/harmony_rotation");
    d->harmony_guide.width =
      dt_conf_get_int("plugins/darkroom/histogram/vectorscope/harmony_width");
  }
  else
  {
    gtk_toggle_button_set_active
      (GTK_TOGGLE_BUTTON(d->color_harmony_button[new_guide.type]), TRUE);
    d->harmony_guide.rotation = new_guide.rotation;
    d->harmony_guide.width = new_guide.width;
  }

  dt_scopes_refresh(self->scopes);
}

static void _vec_mode_enter(dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *const d = self->data;
  gtk_widget_show(d->vec_scale_button);
  gtk_widget_show(d->colorspace_button);
}

static void _vec_mode_leave(const dt_scopes_mode_t *const self)
{
  const dt_scopes_vec_t *const d = self->data;
  gtk_widget_hide(d->vec_scale_button);
  gtk_widget_hide(d->colorspace_button);
  gtk_widget_hide(d->color_harmony_box);
}

static void _vec_gui_init(dt_scopes_mode_t *const self,
                          dt_scopes_t *const scopes)
{
  dt_scopes_vec_t *d = dt_calloc1_align_type(dt_scopes_vec_t);
  self->data = (void *)d;

  const char *str = dt_conf_get_string_const("plugins/darkroom/histogram/vectorscope");
  for(dt_scopes_vec_vectorscope_type_t i=0; i<DT_SCOPES_VEC_VECTORSCOPE_N; i++)
    if(g_strcmp0(str, dt_scopes_vec_vectorscope_type_names[i]) == 0)
      d->vectorscope_type = i;

  str = dt_conf_get_string_const("plugins/darkroom/histogram/vectorscope/scale");
  for(dt_scopes_vec_scale_t i=0; i<DT_SCOPES_VEC_SCALE_N; i++)
    if(g_strcmp0(str, dt_scopes_vec_scale_names[i]) == 0)
      d->vectorscope_scale = i;

  const int a = dt_conf_get_int("plugins/darkroom/histogram/vectorscope/angle");
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
  d->hue_ring_scale = DT_SCOPES_VEC_SCALE_N;
  d->hue_ring_colorspace = DT_SCOPES_VEC_VECTORSCOPE_N;
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
  // FIXME: this is always overwritten when load the first image
  str = dt_conf_get_string_const("plugins/darkroom/histogram/vectorscope/harmony_type");
  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_NONE; i < DT_COLOR_HARMONY_N; i++)
    if(g_strcmp0(str, _vec_color_harmonies[i].name) == 0)
      d->harmony_guide.type = i;
  d->harmony_guide.rotation =
    dt_conf_get_int("plugins/darkroom/histogram/vectorscope/harmony_rotation");
  d->harmony_guide.width =
    dt_conf_get_int("plugins/darkroom/histogram/vectorscope/harmony_width");
  d->harmony_prelight = d->ignore_prelight = DT_COLOR_HARMONY_NONE;
}

static void _vec_add_to_main_box(dt_scopes_mode_t *const self,
                                 dt_action_t *dark,
                                 GtkWidget *box)
{
  dt_scopes_vec_t *const d = self->data;
  d->color_harmony_box = dt_gui_vbox();
  gtk_widget_set_valign(d->color_harmony_box, GTK_ALIGN_START);
  gtk_widget_set_halign(d->color_harmony_box, GTK_ALIGN_START);
  d->color_harmony_fix = gtk_fixed_new();
  gtk_fixed_put(GTK_FIXED(d->color_harmony_fix), d->color_harmony_box, 0, 0);
  dt_gui_box_add(box, d->color_harmony_fix);

  // a series of buttons for color harmony guide lines
  for(dt_color_harmony_type_t i = DT_COLOR_HARMONY_MONOCHROMATIC;
      i < DT_COLOR_HARMONY_N;
      i++)
  {
    GtkWidget *rb = dtgtk_togglebutton_new(dtgtk_cairo_paint_color_harmony, CPF_NONE,
                                           &(_vec_color_harmonies[i]));
    dt_action_define(dark, N_("color harmonies"),
                     _vec_color_harmonies[i].name, rb, &dt_action_def_toggle);
    d->toggle_signal_handler[i] =
      g_signal_connect_data(G_OBJECT(rb), "toggled",
                            G_CALLBACK(_color_harmony_toggled), self, NULL, 0);
    g_signal_connect(G_OBJECT(rb), "state_flags_changed",
                     G_CALLBACK(_color_harmony_state_changed), self);

    dt_gui_box_add(d->color_harmony_box, rb);
    d->color_harmony_button[i] = rb;
  }

  // FIXME: do we need this action, or is it vestigial?
  dt_action_register(dark, N_("cycle color harmonies"),
                     _lib_histogram_cycle_harmony_callback, 0, 0);
}

// FIXME: s/gui_init_options/gui_add_to_options/
static void _vec_gui_init_options(dt_scopes_mode_t *const self,
                                  dt_action_t *dark,
                                  GtkWidget *box)
{
  dt_scopes_vec_t *const d = self->data;

  d->colorspace_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  dt_action_define(dark, NULL, N_("cycle vectorscope types"),
                   d->colorspace_button, &dt_action_def_button);
  d->vec_scale_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  dt_action_define(dark, NULL, N_("switch vectorscope scale"),
                   d->vec_scale_button, &dt_action_def_button);
  dt_gui_box_add(box, d->colorspace_button, d->vec_scale_button);

  /* connect callbacks */
  g_signal_connect(G_OBJECT(d->vec_scale_button), "clicked",
                   G_CALLBACK(_vec_scale_clicked), self);
  g_signal_connect(G_OBJECT(d->colorspace_button), "clicked",
                   G_CALLBACK(_vec_colorspace_clicked), self);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _vec_signal_image_changed);
}

static void _vec_gui_cleanup(dt_scopes_mode_t *const self)
{
  dt_scopes_vec_t *d = self->data;

  dt_free_align(d->vectorscope_graph);
  dt_free_align(d->vectorscope_bkgd);
  if(d->vectorscope_samples)
    g_slist_free_full((GSList *)d->vectorscope_samples, free);
  d->vectorscope_samples = NULL;
  d->selected_sample = -1;
  g_free(d->rgb2ryb_ypp);
  g_free(d->ryb2rgb_ypp);

  dt_free_align(self->data);
  self->data = NULL;
}

// The function table for vectorscope mode. This must be public, i.e. no "static" keyword.
const dt_scopes_functions_t dt_scopes_functions_vectorscope = {
  .name = _vec_name,
  .process = _vec_process,
  .clear = _vec_clear,
  // grid is drawn with scope, as it depends on chromaticity scale
  // FIXME: now that there is no auto-scale but logarithmic/linear, can draw grid here again?
  .draw_bkgd = NULL,
  .draw_grid = NULL,
  .draw_highlight = NULL,
  .draw_scope = _vec_draw,
  .draw_scope_channels = NULL,
  .get_highlight = NULL,
  .get_exposure_pos = NULL,
  .append_to_tooltip = _vec_append_to_tooltip,
  .eventbox_scroll = _vec_eventbox_scroll,
  .eventbox_motion = _vec_eventbox_motion,
  .update_buttons = _vec_update_buttons,
  .mode_enter = _vec_mode_enter,
  .mode_leave = _vec_mode_leave,
  .gui_init = _vec_gui_init,
  .add_to_main_box = _vec_add_to_main_box,
  .add_to_options_box = _vec_gui_init_options,
  .gui_cleanup = _vec_gui_cleanup
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
