/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "common/imagebuf.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "libs/colorpicker.h"

#define HISTOGRAM_BINS 256
// # of gradations between each primary/secondary to draw the hue ring
// Note that this is gradations of CIE 1931 xy, when converted to
// RGB (or perceptual space), the spacing will be different
// FIXME: would fewer gradations still produce a nice hue ring? are this many gradations (32 * 6 = 192) slow to draw on the scope?
#define VECTORSCOPE_HUES 32
#define VECTORSCOPE_BASE_LOG 30

DT_MODULE(1)

typedef enum dt_lib_histogram_highlight_t
{
  DT_LIB_HISTOGRAM_HIGHLIGHT_NONE = 0,
  DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT,
  DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE
} dt_lib_histogram_highlight_t;

typedef enum dt_lib_histogram_scope_type_t
{
  DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM = 0,
  DT_LIB_HISTOGRAM_SCOPE_WAVEFORM,
  DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE,
  DT_LIB_HISTOGRAM_SCOPE_N // needs to be the last one
} dt_lib_histogram_scope_type_t;

typedef enum dt_lib_histogram_scale_t
{
  DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC = 0,
  DT_LIB_HISTOGRAM_SCALE_LINEAR,
  DT_LIB_HISTOGRAM_SCALE_N // needs to be the last one
} dt_lib_histogram_scale_t;

typedef enum dt_lib_histogram_waveform_type_t
{
  DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID = 0,
  DT_LIB_HISTOGRAM_WAVEFORM_PARADE,
  DT_LIB_HISTOGRAM_WAVEFORM_N // needs to be the last one
} dt_lib_histogram_waveform_type_t;

typedef enum dt_lib_histogram_vectorscope_type_t
{
  DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV = 0,   // CIE 1976 u*v*
  DT_LIB_HISTOGRAM_VECTORSCOPE_JZAZBZ,
  DT_LIB_HISTOGRAM_VECTORSCOPE_N // needs to be the last one
} dt_lib_histogram_vectorscope_type_t;

// FIXME: are these lists available from the enum/options in darktableconfig.xml?
const gchar *dt_lib_histogram_scope_type_names[DT_LIB_HISTOGRAM_SCOPE_N] = { "histogram", "waveform", "vectorscope" };
const gchar *dt_lib_histogram_scale_names[DT_LIB_HISTOGRAM_SCALE_N] = { "logarithmic", "linear" };
const gchar *dt_lib_histogram_waveform_type_names[DT_LIB_HISTOGRAM_WAVEFORM_N] = { "overlaid", "parade" };
const gchar *dt_lib_histogram_vectorscope_type_names[DT_LIB_HISTOGRAM_VECTORSCOPE_N] = { "u*v*", "AzBz" };

typedef struct dt_lib_histogram_t
{
  // histogram for display
  uint32_t *histogram;
  uint32_t histogram_max;
  // waveform histogram buffer and dimensions
  float *waveform_linear;
  uint8_t *waveform_8bit;
  int waveform_width, waveform_height, waveform_max_width;
  // FIXME: make dt_lib_histogram_vectorscope_t for all this data?
  uint8_t *vectorscope_graph;
  uint8_t *vectorscope_bkgd;
  float vectorscope_pt[2];            // point colorpicker position
  int vectorscope_diameter_px;
  // FIXME: These arrays could instead be alloc'd/free'd. Would the only concern about making dt_lib_histogram_t large so long be if it were stored in the DB?
  float hue_ring_rgb[6][VECTORSCOPE_HUES][4] DT_ALIGNED_ARRAY;
  float hue_ring_coord[6][VECTORSCOPE_HUES][2] DT_ALIGNED_ARRAY;
  const dt_iop_order_iccprofile_info_t *hue_ring_prof;
  dt_lib_histogram_scale_t hue_ring_scale;
  dt_lib_histogram_vectorscope_type_t hue_ring_colorspace;
  double vectorscope_radius;
  dt_pthread_mutex_t lock;
  GtkWidget *scope_draw;               // GtkDrawingArea -- scope, scale, and draggable overlays
  GtkWidget *button_box;               // GtkButtonBox -- contains scope control buttons
  GtkWidget *button_stack;             // GtkStack -- flips between red and colorspace buttons
  GtkWidget *scope_type_button;        // GtkButton -- histogram/waveform/vectorscope control
  GtkWidget *scope_view_button;        // GtkButton -- how to render the current scope
  GtkWidget *red_channel_button;       // GtkToggleButton -- enable/disable processing R channel
  GtkWidget *green_channel_button;     // GtkToggleButton -- enable/disable processing G channel
  GtkWidget *blue_channel_button;      // GtkToggleButton -- enable/disable processing B channel
  GtkWidget *colorspace_button;        // GtkButton -- vectorscope colorspace
  // drag to change parameters
  gboolean dragging;
  int32_t button_down_x, button_down_y;
  float button_down_value;
  // depends on mouse position
  dt_lib_histogram_highlight_t highlight;
  // state set by buttons
  dt_lib_histogram_scope_type_t scope_type;
  dt_lib_histogram_scale_t histogram_scale;
  dt_lib_histogram_waveform_type_t waveform_type;
  dt_lib_histogram_vectorscope_type_t vectorscope_type;
  dt_lib_histogram_scale_t vectorscope_scale;
  double vectorscope_angle;
  gboolean red, green, blue;
} dt_lib_histogram_t;

const char *name(dt_lib_module_t *self)
{
  return _("histogram");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", "tethering", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_TOP;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 1001;
}


static void _lib_histogram_process_histogram(dt_lib_histogram_t *const d, const float *const input,
                                             const dt_histogram_roi_t *const roi)
{
  dt_dev_histogram_collection_params_t histogram_params = { 0 };
  const dt_iop_colorspace_type_t cst = iop_cs_rgb;
  dt_dev_histogram_stats_t histogram_stats = { .bins_count = HISTOGRAM_BINS, .ch = 4, .pixels = 0 };
  uint32_t histogram_max[4] = { 0 };

  d->histogram_max = 0;
  memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);

  histogram_params.roi = roi;
  histogram_params.bins_count = HISTOGRAM_BINS;
  histogram_params.mul = histogram_params.bins_count - 1;

  // FIXME: for point sample, calculate whole graph and the point sample values, draw these on top of the graph
  // FIXME: set up "custom" histogram worker which can do colorspace conversion on fly -- in cases that we need to do that -- may need to add from colorspace to dt_dev_histogram_collection_params_t
  dt_histogram_helper(&histogram_params, &histogram_stats, cst, iop_cs_NONE, input, &d->histogram, FALSE, NULL);
  dt_histogram_max_helper(&histogram_stats, cst, iop_cs_NONE, &d->histogram, histogram_max);
  d->histogram_max = MAX(MAX(histogram_max[0], histogram_max[1]), histogram_max[2]);
}

static void _lib_histogram_process_waveform(dt_lib_histogram_t *const d, const float *const input,
                                            const dt_histogram_roi_t *const roi)
{
  const int sample_width = MAX(1, roi->width - roi->crop_width - roi->crop_x);
  const int sample_height = MAX(1, roi->height - roi->crop_height - roi->crop_y);

  // Note that, with current constants, the input buffer is from the
  // preview pixelpipe and should be <= 1440x900x4. The output buffer
  // will be <= 360x175x4. Hence process works with a relatively small
  // quantity of data.
  const float *const restrict in = DT_IS_ALIGNED((const float *const restrict)input);
  float *const restrict wf_linear = DT_IS_ALIGNED((float *const restrict)d->waveform_linear);
  uint8_t *const restrict wf_8bit = DT_IS_ALIGNED((uint8_t *const restrict)d->waveform_8bit);

  // Use integral sized bins for columns, as otherwise they will be
  // unequal and have banding. Rely on draw to smoothly do horizontal
  // scaling. For a 3:2 image, "landscape" orientation, bin_width will
  // generally be 4, for "portrait" it will generally be 3.
  // Note that waveform_stride is pre-initialized/hardcoded,
  // but waveform_width varies, depending on preview image
  // width and # of bins.
  const size_t bin_width = ceilf(sample_width / (float)d->waveform_max_width);
  const size_t wf_width = ceilf(sample_width / (float)bin_width);
  d->waveform_width = wf_width;
  const size_t wf_8bit_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, wf_width);
  const size_t wf_height = d->waveform_height;
  dt_iop_image_fill(wf_linear, 0.0f, wf_width, wf_height, 3);

  // Every bin_width x height portion of the image is being described
  // in a 1 pixel x waveform_height portion of the histogram.
  // NOTE: if constant is decreased, will brighten output
  const float brightness = wf_height / 40.0f;
  const float scale = brightness / (sample_height * bin_width);

  // 1.0 is at 8/9 of the height!
  const size_t height_i = wf_height-1;
  const float height_f = height_i;

  // FIXME: for point sample, calculate whole graph and the point sample values, draw these on top of a dimmer graph
  // count the colors
  // FIXME: could flip x/y axes here and when reading to make row-wise iteration?
  // FIXME: Try histogram-style worker threads to process by row and consolidate results. Have the workers do colorspace conversion per-pixel. As there will be no intermediate buffer, even 20 per-thread output buffers will still use less memory.
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, wf_linear, roi, wf_width, wf_height, bin_width, height_f, height_i, scale) \
  schedule(static)
#endif
  for(size_t out_x = 0; out_x < wf_width; out_x++)
  {
    const size_t x_from = out_x * bin_width + roi->crop_x;
    const size_t x_high = MIN(x_from + bin_width, roi->width - roi->crop_width);
    for(size_t in_x = x_from; in_x < x_high; in_x++)
      for(size_t in_y = roi->crop_y; in_y < roi->height - roi->crop_height; in_y++)
        for(size_t k = 0; k < 3; k++)
        {
          const float v = 1.0f - (8.0f / 9.0f) * in[4U * (roi->width * in_y + in_x) + k];
          const size_t out_y = isnan(v) ? 0 : MIN((size_t)fmaxf(v*height_f, 0.0f), height_i);
          wf_linear[(k * wf_height + out_y) * wf_width + out_x] += scale;
        }
  }

  // shortcut to change from linear to display gamma -- borrow hybrid log-gamma LUT
  const dt_iop_order_iccprofile_info_t *const profile =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  // lut for all three channels should be the same
  const float *const restrict lut = DT_IS_ALIGNED((const float *const restrict)profile->lut_out[0]);
  const float lutmax = profile->lutsize - 1;

  // loops are too small (3 * 360 * 175 max iterations) to need threads
  for(size_t ch = 0; ch < 3; ch++)
    for(size_t y = 0; y < wf_height; y++)
      for(size_t x = 0; x < wf_width; x++)
      {
        const float linear = MIN(1.f, wf_linear[(ch * wf_height + y) * wf_width + x]);
        const float display = lut[(int)(linear * lutmax)];
        wf_8bit[(ch * wf_height + y) * wf_8bit_stride + x] = display * 255.f;
      }
}

static inline float baselog(float x, float bound)
{
  // FIXME: use dt's fastlog()?
  return log1pf((VECTORSCOPE_BASE_LOG - 1.f) * x / bound) / log(VECTORSCOPE_BASE_LOG) * bound;
}

static inline void log_scale(const dt_lib_histogram_t *d, float *x, float *y, float r)
{
  if(d->vectorscope_scale == DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC)
  {
    const float h = hypotf(*x,*y);
    const float s = baselog(h, r);
    *x *= s / h;
    *y *= s / h;
  }
}

static void _lib_histogram_vectorscope_bkgd(dt_lib_histogram_t *d, const dt_iop_order_iccprofile_info_t *const vs_prof)
{
  if(vs_prof == d->hue_ring_prof &&
     d->vectorscope_scale == d->hue_ring_scale &&
     d->vectorscope_type == d->hue_ring_colorspace)
    return;

  // FIXME: as in colorbalancergb, repack matrix for SEE?
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
  float vertex_rgb[6][4] DT_ALIGNED_PIXEL = {{1.f, 0.f, 0.f}, {1.f, 1.f, 0.f},
                                             {0.f, 1.f, 0.f}, {0.f, 1.f, 1.f},
                                             {0.f, 0.f, 1.f}, {1.f, 0.f, 1.f} };
  float max_radius = 0.f;
  for(int k=0; k<6; k++)
  {
    float delta[4] DT_ALIGNED_PIXEL;
    for_each_channel(ch,aligned(vertex_rgb,delta:16))
      delta[ch]=(vertex_rgb[(k+1)%6][ch] - vertex_rgb[k][ch]) / VECTORSCOPE_HUES;
    for(int i=0; i < VECTORSCOPE_HUES; i++)
    {
      float rgb[4] DT_ALIGNED_PIXEL, XYZ_D50[4] DT_ALIGNED_PIXEL, intermed[4] DT_ALIGNED_PIXEL, chromaticity[4] DT_ALIGNED_PIXEL;
      for_each_channel(ch,aligned(vertex_rgb,delta,rgb:16))
        rgb[ch] = vertex_rgb[k][ch] + delta[ch] * i;
      dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D50, vs_prof->matrix_in, vs_prof->lut_in,
                                 vs_prof->unbounded_coeffs_in, vs_prof->lutsize, vs_prof->nonlinearlut);
      // Try to represent hue in profile colorspace. Values may be
      // outside [0,1] but cairo_set_source_rgba will clamp. Compare
      // to illuminant_xy_to_RGB.
      dt_XYZ_to_Rec709_D50(XYZ_D50, d->hue_ring_rgb[k][i]);
      // FIXME: keep d->vectorscope_type in local variable for speed?
      if(d->vectorscope_type == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
      {
        dt_XYZ_to_xyY(XYZ_D50, intermed);
        dt_xyY_to_Luv(intermed, chromaticity);
      }
      else
      {
        dt_XYZ_D50_2_XYZ_D65(XYZ_D50, intermed);
        dt_XYZ_2_JzAzBz(intermed, chromaticity);
      }
      // FIXME: log_scale these coords here rather than when drawing the hue ring
      d->hue_ring_coord[k][i][0] = chromaticity[1];
      d->hue_ring_coord[k][i][1] = chromaticity[2];
      // FIXME: max radius isn't log scaled?
      max_radius = MAX(max_radius, hypotf(chromaticity[1], chromaticity[2]));
    }
  }

  // chromaticities for drawing both hue ring and grpah
  const int diam_px = d->vectorscope_diameter_px;
  const dt_lib_histogram_vectorscope_type_t vs_type = d->vectorscope_type;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, diam_px);
  // loop appears to be too small to benefit w/OpenMP
  // FIXME: is this still true? -- all this will only run once per colorspace change, so doesn't need to be extra-fast
  for(size_t y = 0; y < diam_px; y++)
    for(size_t x = 0; x < diam_px; x++)
    {
      uint8_t *const restrict px = d->vectorscope_bkgd + y * stride + x * 4U;
      // FIXME: should be / (diam_px-1)? same in other places?
      float a = max_radius * 2.0f * (x / (float)(diam_px-1) - 0.5f);
      float b = max_radius * 2.0f * (y / (float)(diam_px-1) - 0.5f);
      // FIXME: should we be doing log_scale of [-1,1] rather than [-max_diam,max_diam]?
      log_scale(d, &a, &b, max_radius);
      float XYZ_D50[4] DT_ALIGNED_PIXEL, RGB[4] DT_ALIGNED_PIXEL;
      // FIXME: look at how hue controls on colorbalance rgb are drawn -- what lightness level -- use similar math
      if(vs_type == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
      {
        const float Luv[4] DT_ALIGNED_PIXEL = {65.0f, a, b};
        float xyY[4] DT_ALIGNED_PIXEL;
        dt_Luv_to_xyY(Luv, xyY);
        // FIXME: do have to worry about chromatic adaptation? this assumes that the histogram profile white point is the same as PCS whitepoint (D50) -- if we have a D65 whitepoint profile, how does the result change if we adapt to D65 then convert to L*u*v* with a D65 whitepoint?
        dt_xyY_to_XYZ(xyY, XYZ_D50);
      }
      else if(vs_type == DT_LIB_HISTOGRAM_VECTORSCOPE_JZAZBZ)
      {
        const float JzAzBz[4] DT_ALIGNED_PIXEL = {0.008f, a, b};
        // FIXME: can optimize the XYZ_D65 -> RGB conversion by pre-multiplying matrix?
        float XYZ_D65[4] DT_ALIGNED_PIXEL;
        dt_JzAzBz_2_XYZ(JzAzBz, XYZ_D65);
        dt_XYZ_D65_2_XYZ_D50(XYZ_D65, XYZ_D50);
      }
      else
      {
        dt_unreachable_codepath();
      }
      // FIXME: a custom matrix could do this flip and write directly to pixel buffer
      dt_XYZ_to_Rec709_D50(XYZ_D50, RGB);
      // BGR/RGB flip is for pixelpipe vs. Cairo color?
      for(int ch=0; ch<3; ch++)
        px[2U-ch] = CLAMP((int)(RGB[ch] * 255.0f), 0, 255);
    }

  d->vectorscope_radius = max_radius;
  d->hue_ring_prof = vs_prof;
  d->hue_ring_scale = d->vectorscope_scale;
  d->hue_ring_colorspace = d->vectorscope_type;
}

static void _lib_histogram_process_vectorscope(dt_lib_histogram_t *d, const float *const input,
                                               dt_histogram_roi_t *const roi,
                                               const dt_iop_order_iccprofile_info_t *vs_prof)
{
  const int diam_px = d->vectorscope_diameter_px;
  const dt_lib_histogram_vectorscope_type_t vs_type = d->vectorscope_type;

  if(!vs_prof || isnan(vs_prof->matrix_in[0]))
  {
    fprintf(stderr, "[histogram] unsupported vectorscope profile %i %s, it will be replaced with linear rec2020\n", vs_prof->type, vs_prof->filename);
    vs_prof = dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_LIN_REC2020, "", DT_INTENT_RELATIVE_COLORIMETRIC);
  }

  _lib_histogram_vectorscope_bkgd(d, vs_prof);
  // FIXME: particularly for u*v*, center on hue ring bounds rather than plot center, to be able to show a larger plot?
  const float max_radius = d->vectorscope_radius;
  const float max_diam = max_radius * 2.f;

  int sample_width = MAX(1, roi->width - roi->crop_width - roi->crop_x);
  int sample_height = MAX(1, roi->height - roi->crop_height - roi->crop_y);
  size_t pt_sample_x = SIZE_MAX, pt_sample_y = SIZE_MAX;
  if(sample_width == 1 && sample_height == 1)
  {
    // point sample still calculates graph based on whole image
    pt_sample_x = roi->crop_x - (roi->crop_x % 2);
    pt_sample_y = roi->crop_y - (roi->crop_y % 2);
    sample_width = roi->width;
    sample_height = roi->height;
    roi->crop_x = roi->crop_y = 0;
  }
  else
  {
    d->vectorscope_pt[0] = NAN;
  }

  // RGB -> chromaticity (processor-heavy), count into bins by chromaticity
  // FIXME: if we do convert to histogram RGB, should it be an absolute colorimetric conversion (would mean knowing the histogram profile whitepoint and un-adapting its matrices) and then we have a meaningful whitepoint and could plot spectral locus -- or the reverse, adapt the spectral locus to the histogram profile PCS (always D50)?
  // FIXME: pre-allocate? -- use the same buffer as for waveform?
  dt_atomic_int *const restrict binned = __builtin_assume_aligned(dt_alloc_align(64, sizeof(int) * diam_px * diam_px), 64);
  memset(binned, 0, sizeof(int) * diam_px * diam_px);
  // FIXME: move verbosed interleaved comments into a method note at the start, as the code itself is succinct and clear
  // FIXME: even with getting rid of the extra profile conversion hop there's no noticeable speedup -- maybe this loop is memory bound -- if can get rid of one of the output buffers and still no speedup, consider doing more work in this loop, such as atomic binning
  // FIXME: make 2x2 averaging be conditional on preprocessor define
  // FIXME: average neighboring pixels on x but not y -- may be enough of an optimization
  const int sample_max_x = sample_width - (sample_width % 2);
  const int sample_max_y = sample_height - (sample_height % 2);
  // FIXME: if decimate/downsample, should blur before this
  // FIXME: instead of scaling, if chromaticity really depends only on XY, then make a lookup on startup of for each grid cell on graph output the minimum XY to populate that cell, then either brute-force scan that LUT, or start from position of last pixel and scan, or do an optimized search (1/2, 1/2, 1/2, etc.) -- would also find point sample pixel this way
#if defined(_OPENMP)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(input, binned, sample_max_x, sample_max_y, roi, pt_sample_x, pt_sample_y, d, diam_px, max_radius, max_diam, vs_prof, vs_type) \
  schedule(static) collapse(2)
#endif
  for(size_t y=0; y<sample_max_y; y+=2)
    for(size_t x=0; x<sample_max_x; x+=2)
    {
      // FIXME: There are unnecessary color math hops. Right now the data
      // comes into dt_lib_histogram_process() in a known profile
      // (usually from pixelpipe). Then (usually) it gets converted to
      // the histogram profile. Here it gets converted to XYZ D50 before
      // making its way to L*u*v* or JzAzBz:
      //   RGB (pixelpipe) -> XYZ(PCS, D50) -> RGB (histogram) -> XYZ (PCS, D50) -> chromaticity
      // Given that the histogram profile is "well behaved" and the
      // conversion to histogram profile is relative colorimetric, could
      // instead:
      //   RGB (pixelpipe) -> XYZ(PCS, D50) -> chromaticity
      // A catch is that pixelpipe RGB may be a CLUT profile, hence would
      // need to have an LCMS path unless histogram moves to before colorout.
      float RGB[4] DT_ALIGNED_PIXEL = {0.f}, XYZ_D50[4] DT_ALIGNED_PIXEL, chromaticity[4] DT_ALIGNED_PIXEL;
      // FIXME: for speed, downsample 2x2 -> 1x1 here, which still should produce enough chromaticity data -- Question: AVERAGE(RGBx4) -> chromaticity, or AVERAGE((RGB -> chromaticity)x4)?
      // FIXME: could compromise and downsample to 2x1 -- may also be a bit faster than skipping rows
      const float *const restrict px = DT_IS_ALIGNED((const float *const restrict)input +
                                                     4U * ((y + roi->crop_y) * roi->width + x + roi->crop_x));
      for(size_t xx=0; xx<2; xx++)
        for(size_t yy=0; yy<2; yy++)
          for_each_channel(ch,aligned(px,RGB:16))
            RGB[ch] += px[4U * (yy * roi->width + xx) + ch] * 0.25f;

      // this goes to the PCS which has standard illuminant D50
      dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, vs_prof->matrix_in, vs_prof->lut_in,
                                 vs_prof->unbounded_coeffs_in, vs_prof->lutsize, vs_prof->nonlinearlut);
      // NOTE: see for comparison/reference rgb_to_JzCzhz() in color_picker.c
      if(vs_type == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
      {
        // FIXME: do have to worry about chromatic adaptation? this assumes that the histogram profile white point is the same as PCS whitepoint (D50) -- if we have a D65 whitepoint profile, how does the result change if we adapt to D65 then convert to L*u*v* with a D65 whitepoint?
        float xyY_D50[4] DT_ALIGNED_PIXEL;
        dt_XYZ_to_xyY(XYZ_D50, xyY_D50);
        // using D50 correct u*v* (not u'v') to be relative to the whitepoint (important for vectorscope) and as u*v* is more evenly spaced
        dt_xyY_to_Luv(xyY_D50, chromaticity);
      }
      else
      {
        // FIXME: can skip a hop by pre-multipying matrices: see colorbalancergb and dt_develop_blendif_init_masking_profile() for how to make hacked profile
        float XYZ_D65[4] DT_ALIGNED_PIXEL;
        // If the profile whitepoint is D65, its RGB -> XYZ conversion
        // matrix has been adapted to D50 (PCS standard) via
        // Bradford. Using Bradford again to adapt back to D65 gives a
        // pretty clean reversal of the transform.
        // FIXME: if the profile whitepoint is D50 (ProPhoto...), then should we use a nicer adaptation (CAT16?) to D65?
        dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
        // FIXME: The bulk of processing time is spent in the XYZ -> JzAzBz conversion in the 2*3 powf() in X'Y'Z' -> L'M'S'. Making a LUT for these, using _apply_trc() to do powf() work. It only needs to be accurate enough to be about on the right pixel for a diam_px x diam_px plot
        dt_XYZ_2_JzAzBz(XYZ_D65, chromaticity);
      }
      // FIXME: we ignore the L or Jz components -- do they optimize out of the above code, or would in particular a XYZ_2_AzBz but helpful?
      log_scale(d, chromaticity+1, chromaticity+2, max_radius);
      if(x == pt_sample_x && y == pt_sample_y)
      {
        d->vectorscope_pt[0] = chromaticity[1];
        d->vectorscope_pt[1] = chromaticity[2];
      }

      // FIXME: make cx,cy which are float, check 0 <= cx < 1, then multiply by diam_px
      const int out_x = diam_px * (chromaticity[1] / max_diam + 0.5f);
      const int out_y = diam_px * (chromaticity[2] / max_diam + 0.5f);

      // clip any out-of-scale values, so there aren't light edges
      if(out_x >= 0 && out_x <= diam_px-1 && out_y >= 0 && out_y <= diam_px-1)
        dt_atomic_add_int(binned + out_y * diam_px + out_x, 1);
    }

  // shortcut to change from linear to display gamma
  const dt_iop_order_iccprofile_info_t *const profile =
    dt_ioppr_add_profile_info_to_list(darktable.develop, DT_COLORSPACE_HLG_REC2020, "", DT_INTENT_PERCEPTUAL);
  const float *const restrict lut = DT_IS_ALIGNED((const float *const restrict)profile->lut_out[0]);
  const float lutmax = profile->lutsize - 1;
  const int out_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam_px);
  uint8_t *const graph = d->vectorscope_graph;

  // FIXME: should count the max bin size, and vary the scale such that it is always 1?
  const float gain = 1.f / 75.f;
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

static void dt_lib_histogram_process(struct dt_lib_module_t *self, const float *const input,
                                     int width, int height,
                                     const dt_iop_order_iccprofile_info_t *const profile_info_from,
                                     const dt_iop_order_iccprofile_info_t *const profile_info_to)
{
  dt_times_t start;
  dt_get_times(&start);

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_develop_t *dev = darktable.develop;

  // special case, clear the scopes
  if(!input)
  {
    dt_pthread_mutex_lock(&d->lock);
    memset(d->histogram, 0, sizeof(uint32_t) * 4 * HISTOGRAM_BINS);
    d->waveform_width = 0;
    d->vectorscope_radius = 0.f;
    dt_pthread_mutex_unlock(&d->lock);
    return;
  }

  // FIXME: scope goes black when click histogram lib colorpicker on -- is this meant to happen?
  // FIXME: scope doesn't redraw when click histogram lib colorpicker off -- is this meant to happen?
  dt_histogram_roi_t roi = { .width = width, .height = height,
                             .crop_x = 0, .crop_y = 0, .crop_width = 0, .crop_height = 0 };

  // Constraining the area if the colorpicker is active in area mode
  // FIXME: only need to do colorspace conversion below on roi
  // FIXME: if the only time we use roi in histogram to limit area is here, and whenever we use tether there is no colorpicker (true?), and if we're always doing a colorspace transform in darkroom and clip to roi during conversion, then can get rid of all roi code for common/histogram?
  // when darkroom colorpicker is active, gui_module is set to colorout
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM &&
     dev->gui_module && !strcmp(dev->gui_module->op, "colorout")
     && dev->gui_module->request_color_pick != DT_REQUEST_COLORPICK_OFF
     && darktable.lib->proxy.colorpicker.restrict_histogram)
  {
    if(darktable.lib->proxy.colorpicker.size == DT_COLORPICKER_SIZE_BOX)
    {
      roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_box[0] * width));
      roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_box[1] * height));
      roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_box[2] * width));
      roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_box[3] * height));
    }
    else
    {
      roi.crop_x = MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      roi.crop_y = MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
      roi.crop_width = width - MIN(width, MAX(0, dev->gui_module->color_picker_point[0] * width));
      roi.crop_height = height - MIN(height, MAX(0, dev->gui_module->color_picker_point[1] * height));
    }
  }

  // Convert pixelpipe output in display RGB to histogram profile. If
  // in tether view, then the image is already converted by the
  // caller.
  // FIXME: do conversion in-place in the processing to save an extra buffer? -- at least for waveform, which already has to touch each pixel -- will need logic from _transform_matrix_rgb() -- or better yet a per-pixel callback within _transform_matrix_rgb()-ish code
  // FIXME: in case of vectorscope, it needs XYZ data, so skip this conversion and instead it's enough that it has input & profile_info_from -- though then we don't see the result of a relative colorimetric conversion to the histogram profile...
  float *img_display = dt_alloc_align_float((size_t)4 * width * height);
  if(!img_display) return;
  dt_ioppr_transform_image_colorspace_rgb(input, img_display, width, height,
                                          profile_info_from, profile_info_to, "final histogram");
  dt_pthread_mutex_lock(&d->lock);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      _lib_histogram_process_histogram(d, img_display, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      _lib_histogram_process_waveform(d, img_display, &roi);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      _lib_histogram_process_vectorscope(d, img_display, &roi, profile_info_to);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
      break;
  }
  dt_pthread_mutex_unlock(&d->lock);
  dt_free_align(img_display);

  dt_show_times_f(&start, "[histogram]", "final %s", dt_lib_histogram_scope_type_names[d->scope_type]);
}


static void _lib_histogram_draw_histogram(dt_lib_histogram_t *d, cairo_t *cr,
                                          int width, int height, const uint8_t mask[3])
{
  if(!d->histogram_max) return;
  const float hist_max = d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR ? d->histogram_max
                                                                             : logf(1.0 + d->histogram_max);
  // The alpha of each histogram channel is 1, hence the primaries and
  // overlaid secondaries and neutral colors should be about the same
  // brightness. The combined group is then drawn with an alpha, which
  // dims things down.
  cairo_save(cr);
  cairo_push_group_with_content(cr, CAIRO_CONTENT_COLOR);
  cairo_translate(cr, 0, height);
  cairo_scale(cr, width / 255.0, -(height - 10) / hist_max);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < 3; k++)
    if(mask[k])
    {
      set_color(cr, darktable.bauhaus->graph_colors[k]);
      dt_draw_histogram_8(cr, d->histogram, 4, k, d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR);
    }
  cairo_pop_group_to_source(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_paint_with_alpha(cr, 0.5);
  cairo_restore(cr);
}

static void _lib_histogram_draw_waveform_channel(dt_lib_histogram_t *d, cairo_t *cr, int ch, double alpha)
{
  // FIXME: force a recalc/redraw when colors have changed via user entering new CSS in preferences -- is there a signal for this?
  // waveform data is BGR, need to flip to RGB
  const GdkRGBA primary = darktable.bauhaus->graph_colors[2-ch];
  cairo_set_source_rgba(cr, primary.red, primary.green, primary.blue, alpha);
  const size_t wf_8bit_stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->waveform_width);
  cairo_surface_t *surface
    = dt_cairo_image_surface_create_for_data(d->waveform_8bit + (2-ch) * d->waveform_height * wf_8bit_stride,
                                             CAIRO_FORMAT_A8,
                                             d->waveform_width, d->waveform_height, wf_8bit_stride);
  cairo_mask_surface(cr, surface, 0., 0.);
  cairo_surface_destroy(surface);
}

static void _lib_histogram_draw_waveform(dt_lib_histogram_t *d, cairo_t *cr,
                                         int width, int height,
                                         const uint8_t mask[3])
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/d->waveform_width,
              darktable.gui->ppd*height/d->waveform_height);

  for(int ch = 0; ch < 3; ch++)
    if(mask[2-ch])
      _lib_histogram_draw_waveform_channel(d, cr, ch, 0.6);
  cairo_restore(cr);
}

static void _lib_histogram_draw_rgb_parade(dt_lib_histogram_t *d, cairo_t *cr, int width, int height)
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_scale(cr, darktable.gui->ppd*width/(d->waveform_width*3),
              darktable.gui->ppd*height/d->waveform_height);
  for(int ch = 2; ch >= 0; ch--)
  {
    _lib_histogram_draw_waveform_channel(d, cr, ch, 0.9);
    cairo_translate(cr, d->waveform_width/darktable.gui->ppd, 0);
  }
  cairo_restore(cr);
}

static void _lib_histogram_draw_vectorscope(dt_lib_histogram_t *d, cairo_t *cr,
                                            int width, int height)
{
  const float vs_radius = d->vectorscope_radius;
  const int diam_px = d->vectorscope_diameter_px;
  const int min_size = MIN(width, height);
  const double scale = min_size / (vs_radius * 2.);

  cairo_save(cr);

  // background
  cairo_pattern_t *p = cairo_pattern_create_radial(0.5 * width, 0.5 * height, 0.5 * min_size,
                                                   0.5 * width, 0.5 * height, 0.5 * hypot(min_size, min_size));
  cairo_pattern_add_color_stop_rgb(p, 0., darktable.bauhaus->graph_bg.red, darktable.bauhaus->graph_bg.green, darktable.bauhaus->graph_bg.blue);
  cairo_pattern_add_color_stop_rgb(p, 1., darktable.bauhaus->graph_exterior.red, darktable.bauhaus->graph_exterior.green, darktable.bauhaus->graph_exterior.blue);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_set_source(cr, p);
  cairo_fill(cr);
  cairo_pattern_destroy(p);

  // FIXME: the areas to left/right of the scope could have some data (primaries, whitepoint, scale, etc.)
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
  const float grid_radius = d->hue_ring_colorspace == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV ? 100. : 0.01;
  for(int i = 1; i < 1.f + ceilf(vs_radius/grid_radius); i++)
  {
    float r = grid_radius * i;
    if(d->vectorscope_scale == DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC)
      r = baselog(r, vs_radius);
    cairo_arc(cr, 0., 0., r * scale, 0., M_PI * 2.);
    cairo_stroke(cr);
  }

  // FIXME: draw hue ring as a mask on the background, then don't calculate hue ring colors
  // FIXME: also add hue rings (monochrome/dotted) for input/work/output profiles
  // from Sobotka:
  // 1. The input encoding primaries. How dd the image start out life? What is valid data within that? What is invalid introduced by error of camera virtual primaries solving or math such as resampling an image such that negative lobes result?
  // 2. The working reference primaries. How did 1. end up in 2.? Are there negative and therefore nonsensical values in the working space? Should a gamut mapping pass be applied before work, between 1. and 2.?
  // 3. The output primaries rendition. From a selection of gamut mappings, is one required between 2. and 3.?"

  // graticule: histogram profile hue ring
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  float x = d->hue_ring_coord[5][VECTORSCOPE_HUES-1][0];
  float y = d->hue_ring_coord[5][VECTORSCOPE_HUES-1][1];
  log_scale(d, &x, &y, vs_radius);
  for(int n=0; n<6; n++)
  {
    for(int h=0; h<VECTORSCOPE_HUES; h++)
    {
      cairo_move_to(cr, x*scale, y*scale);
      // FIXME: can we pre-make a pattern with the hues radiating out, and use it as the "ink" to draw the hue ring and -- if in false color mode -- the vectorscope? will this be faster then drawing lots of lines each with their own color? will it allow for drawing the hue ring with splines and calculating fewer points? -- we might need a color pattern of the colorspace, then masked once to increase saturation and once for alpha?
      // note that hue_ring_rgb and hue_ring_coord are calculated as float but converted here to double
      cairo_set_source_rgba(cr, d->hue_ring_rgb[n][h][0], d->hue_ring_rgb[n][h][1], d->hue_ring_rgb[n][h][2], 0.5);
      x = d->hue_ring_coord[n][h][0];
      y = d->hue_ring_coord[n][h][1];
      log_scale(d, &x, &y, vs_radius);
      cairo_line_to(cr, x*scale, y*scale);
      cairo_stroke(cr);
      if(h==0)
      {
        cairo_arc(cr, x*scale, y*scale, DT_PIXEL_APPLY_DPI(2.), 0., M_PI * 2.);
        cairo_set_source_rgba(cr, d->hue_ring_rgb[n][h][0], d->hue_ring_rgb[n][h][1], d->hue_ring_rgb[n][h][2], 1.);
        cairo_fill_preserve(cr);
        set_color(cr, darktable.bauhaus->graph_grid);
        cairo_stroke(cr);
      }
    }
  }

  // vectorscope graph
  // FIXME: use cairo_pattern_set_filter()?
  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  // FIXME: use cairo_pattern_set_extend() with CAIRO_EXTEND_PAD?
  cairo_surface_t *graph_surface =
    dt_cairo_image_surface_create_for_data(d->vectorscope_graph, CAIRO_FORMAT_A8,
                                           diam_px, diam_px,
                                           cairo_format_stride_for_width(CAIRO_FORMAT_A8, diam_px));
  cairo_surface_t *bkgd_surface =
    dt_cairo_image_surface_create_for_data(d->vectorscope_bkgd, CAIRO_FORMAT_RGB24,
                                           diam_px, diam_px,
                                           cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, diam_px));

  // FIXME: do one pass in color with bkgd, one pass with hard light with gray
  cairo_pattern_t *graph_pat = cairo_pattern_create_for_surface(graph_surface);
  cairo_pattern_t *bkgd_pat = cairo_pattern_create_for_surface(bkgd_surface);
  // FIXME: is there an easier way to do this work?
  cairo_matrix_t matrix;
  cairo_matrix_init_translate(&matrix, 0.5*diam_px/darktable.gui->ppd, 0.5*diam_px/darktable.gui->ppd);
  cairo_matrix_scale(&matrix, (double)diam_px / min_size / darktable.gui->ppd,
                     (double)diam_px / min_size / darktable.gui->ppd);
  cairo_pattern_set_matrix(graph_pat, &matrix);
  cairo_pattern_set_matrix(bkgd_pat, &matrix);
  cairo_set_source(cr, bkgd_pat);
  //cairo_paint(cr);
  cairo_mask(cr, graph_pat);
  //cairo_mask_surface(cr, graph_surface, 0., 0.);
  // FIXME: how will handle fading background for point sample? draw to another surface?
  /*
  if(isnan(d->vectorscope_pt[0]))
    cairo_paint(cr);
  else
    cairo_paint_with_alpha(cr, 0.5);
  */
  cairo_pattern_destroy(bkgd_pat);
  cairo_surface_destroy(bkgd_surface);
  cairo_pattern_destroy(graph_pat);
  cairo_surface_destroy(graph_surface);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  if(!isnan(d->vectorscope_pt[0]))
  {
    // point sample
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_arc(cr, scale*d->vectorscope_pt[0], scale*d->vectorscope_pt[1],
              DT_PIXEL_APPLY_DPI(3.), 0., M_PI * 2.);
    cairo_fill(cr);
  }

  // overlay central circle
  set_color(cr, darktable.bauhaus->graph_overlay);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0., 0., DT_PIXEL_APPLY_DPI(3.), 0., M_PI * 2.);
  cairo_stroke(cr);

  cairo_restore(cr);
}

// FIXME: have different drawable for each scope in a stack -- simplifies this function from being a swath of conditionals -- then esentially draw callbacks _lib_histogram_draw_waveform, _lib_histogram_draw_rgb_parade, etc.
// FIXME: if exposure change regions are separate widgets, then we could have a menu to swap in different overlay widgets (sort of like basic adjustments) to adjust other things about the image, e.g. tone equalizer, color balance, etc.
static gboolean _drawable_draw_callback(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_times_t start;
  dt_get_times(&start);

  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width, height = allocation.height;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.5)); // borders width

  // Draw frame and background
  if(d->scope_type != DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE)
  {
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, width, height);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);
    cairo_restore(cr);
  }

  // exposure change regions
  if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 7.0/9.0 * height, width, height);
    else
      cairo_rectangle(cr, 0, 0, 0.2 * width, height);
    cairo_fill(cr);
  }
  else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
  {
    set_color(cr, darktable.bauhaus->graph_overlay);
    if(d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM)
      cairo_rectangle(cr, 0, 0, width, 7.0/9.0 * height);
    else
      cairo_rectangle(cr, 0.2 * width, 0, width, height);
    cairo_fill(cr);
  }

  // draw grid
  set_color(cr, darktable.bauhaus->graph_grid);
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      // FIXME: now that vectorscope grid represents log scale, should histogram grid do the same?
      dt_draw_grid(cr, 4, 0, 0, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      dt_draw_waveform_lines(cr, 0, 0, width, height);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      // grid is drawn with scope, as it depends on chromaticity scale
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }

  // FIXME: should set histogram buffer to black if have just entered tether view and nothing is displayed
  dt_pthread_mutex_lock(&d->lock);
  // darkroom view: draw scope so long as preview pipe is finished
  // tether view: draw whatever has come in from tether
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_TETHERING || dev->image_storage.id == dev->preview_pipe->output_imgid)
  {
    const uint8_t mask[3] = { d->red, d->green, d->blue };
    switch(d->scope_type)
    {
      case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
        _lib_histogram_draw_histogram(d, cr, width, height, mask);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
        if(!d->waveform_width) break;
        if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
          _lib_histogram_draw_waveform(d, cr, width, height, mask);
        else
          _lib_histogram_draw_rgb_parade(d, cr, width, height);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
        if(d->vectorscope_radius != 0.f)
          _lib_histogram_draw_vectorscope(d, cr, width, height);
        break;
      case DT_LIB_HISTOGRAM_SCOPE_N:
        dt_unreachable_codepath();
    }
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

  dt_show_times_f(&start, "[histogram]", "scope draw");
  return TRUE;
}

static gboolean _drawable_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event,
                                                 gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  if(d->dragging)
  {
    // FIXME: dragging the vectorscope could change white balance
    const float diff = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? d->button_down_y - event->y
                                                                        : event->x - d->button_down_x;
    const int range = d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM ? allocation.height
                                                                       : allocation.width;
    // FIXME: see dt_bauhaus_slider_postponed_value_change(): delay processing until the pixelpipe can update based on dev->preview_average_delay for smoother interaction
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float exposure = d->button_down_value + diff * 4.0f / (float)range;
      dt_dev_exposure_set_exposure(dev, exposure);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float black = d->button_down_value - diff * .1f / (float)range;
      dt_dev_exposure_set_black(dev, black);
    }
  }
  else
  {
    const float x = event->x;
    const float y = event->y;
    const float posx = x / (float)(allocation.width);
    const float posy = y / (float)(allocation.height);
    const dt_lib_histogram_highlight_t prior_highlight = d->highlight;
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    const gboolean hooks_available = (cv->view(cv) == DT_VIEW_DARKROOM) && dt_dev_exposure_hooks_available(dev);

    // FIXME: make just one tooltip for the widget depending on whether it is draggable or not, and set it when enter the view
    if(!hooks_available || d->scope_type == DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE)
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
      gtk_widget_set_tooltip_text(widget, _("ctrl+scroll to change display height"));
    }
    // FIXME: could a GtkRange be used to do this work?
    else if((posx < 0.2f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM) ||
            (posy > 7.0f/9.0f && d->scope_type == DT_LIB_HISTOGRAM_SCOPE_WAVEFORM))
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT;
      gtk_widget_set_tooltip_text(widget, _("drag to change black point,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    else
    {
      d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE;
      gtk_widget_set_tooltip_text(widget, _("drag to change exposure,\ndoubleclick resets\nctrl+scroll to change display height"));
    }
    if(prior_highlight != d->highlight)
    {
      dt_control_queue_redraw_widget(widget);
      if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
      {
        // FIXME: should really use named cursors, and differentiate between "grab" and "grabbing"
        dt_control_change_cursor(GDK_HAND1);
      }
    }
  }

  //bubble event to eventbox to update the button tooltip
  return FALSE;
}

static gboolean _drawable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  dt_develop_t *dev = darktable.develop;

  if(d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    if(event->type == GDK_2BUTTON_PRESS)
    {
      dt_dev_exposure_reset_defaults(dev);
    }
    else
    {
      // FIXME: should change cursor from "grab" to "grabbing", but this would mean rewriting dt_control_change_cursor() to use gdk_cursor_new_from_name()
      if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
      {
        d->button_down_value = dt_dev_exposure_get_exposure(dev);
      }
      else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
      {
        d->button_down_value = dt_dev_exposure_get_black(dev);
      }
      d->dragging = TRUE;
      d->button_down_x = event->x;
      d->button_down_y = event->y;
    }
  }

  return TRUE;
}

static gboolean _drawable_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    // bubble to adjusting the overall widget size
    return FALSE;
  }
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  int delta_y;
  // note are using unit rather than smooth scroll events, as
  // exposure changes can get laggy if handling a multitude of smooth
  // scroll events
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y) &&
     d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    dt_develop_t *dev = darktable.develop;
    // FIXME: see dt_bauhaus_slider_postponed_value_change(): delay processing until the pixelpipe can update based on dev->preview_average_delay for smoother interaction
    if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_EXPOSURE)
    {
      const float ce = dt_dev_exposure_get_exposure(dev);
      dt_dev_exposure_set_exposure(dev, ce - 0.15f * delta_y);
    }
    else if(d->highlight == DT_LIB_HISTOGRAM_HIGHLIGHT_BLACK_POINT)
    {
      const float cb = dt_dev_exposure_get_black(dev);
      dt_dev_exposure_set_black(dev, cb + 0.001f * delta_y);
    }
  }

  return TRUE;
}

static gboolean _drawable_button_release_callback(GtkWidget *widget, GdkEventButton *event,
                                                  gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  d->dragging = FALSE;
  // hack to recalculate the highlight as mouse may be over a different part of the widget
  // FIXME: generate an event instead?
  _drawable_motion_notify_callback(widget, (GdkEventMotion *)event, user_data);
  return TRUE;
}

static gboolean _drawable_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  // if dragging, gtk keeps up motion notifications until mouse button
  // is released, at which point we'll get another leave event for
  // drawable if pointer is still outside of the widget
  if(!d->dragging && d->highlight != DT_LIB_HISTOGRAM_HIGHLIGHT_NONE)
  {
    d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
    dt_control_change_cursor(GDK_LEFT_PTR);
    dt_control_queue_redraw_widget(widget);
  }
  // event should bubble up to the eventbox
  return FALSE;
}

static void _histogram_scale_update(const dt_lib_histogram_t *d)
{
  switch(d->histogram_scale)
  {
    case DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_N:
      dt_unreachable_codepath();
  }
  // FIXME: this should really redraw current iop if its background is a histogram (check request_histogram)
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR;
}

static void _waveform_view_update(const dt_lib_histogram_t *d)
{
  // FIXME: add other waveform types -- RGB parade overlaid top-to-bottom rather than left to right, possibly waveform calculated sideways (another button?)
  switch(d->waveform_type)
  {
    case DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to RGB parade"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_waveform_overlaid, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, TRUE);
      gtk_widget_set_sensitive(d->green_channel_button, TRUE);
      gtk_widget_set_sensitive(d->blue_channel_button, TRUE);
      break;
    case DT_LIB_HISTOGRAM_WAVEFORM_PARADE:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set view to waveform"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_rgb_parade, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, FALSE);
      gtk_widget_set_sensitive(d->green_channel_button, FALSE);
      gtk_widget_set_sensitive(d->blue_channel_button, FALSE);
      break;
    case DT_LIB_HISTOGRAM_WAVEFORM_N:
      dt_unreachable_codepath();
  }
}

static void _vectorscope_view_update(dt_lib_histogram_t *d)
{
  switch(d->vectorscope_scale)
  {
    case DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to linear"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_logarithmic_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_LINEAR:
      gtk_widget_set_tooltip_text(d->scope_view_button, _("set scale to logarithmic"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_view_button),
                             dtgtk_cairo_paint_linear_scale, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_SCALE_N:
      dt_unreachable_codepath();
  }
  switch(d->vectorscope_type)
  {
    case DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to AzBz"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_luv, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_VECTORSCOPE_JZAZBZ:
      gtk_widget_set_tooltip_text(d->colorspace_button, _("set view to u*v*"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->colorspace_button),
                             dtgtk_cairo_paint_jzazbz, CPF_NONE, NULL);
      break;
    case DT_LIB_HISTOGRAM_VECTORSCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _scope_type_update(dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to waveform"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_histogram_scope, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, TRUE);
      gtk_widget_set_sensitive(d->green_channel_button, TRUE);
      gtk_widget_set_sensitive(d->blue_channel_button, TRUE);
      gtk_stack_set_visible_child(GTK_STACK(d->button_stack), d->red_channel_button);
      _histogram_scale_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to vectorscope"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_waveform_scope, CPF_NONE, NULL);
      gtk_stack_set_visible_child(GTK_STACK(d->button_stack), d->red_channel_button);
      // handles setting RGB channel button sensitive state
      _waveform_view_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      gtk_widget_set_tooltip_text(d->scope_type_button, _("set mode to histogram"));
      dtgtk_button_set_paint(DTGTK_BUTTON(d->scope_type_button),
                             dtgtk_cairo_paint_vectorscope, CPF_NONE, NULL);
      gtk_widget_set_sensitive(d->red_channel_button, FALSE);
      gtk_widget_set_sensitive(d->green_channel_button, FALSE);
      gtk_widget_set_sensitive(d->blue_channel_button, FALSE);
      gtk_stack_set_visible_child(GTK_STACK(d->button_stack), d->colorspace_button);
      _vectorscope_view_update(d);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _scope_type_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  // NOTE: this isn't a "real" button but more of a tri-state toggle button
  d->scope_type = (d->scope_type + 1) % DT_LIB_HISTOGRAM_SCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/mode", dt_lib_histogram_scope_type_names[d->scope_type]);
  _scope_type_update(d);

  // generate data for changed scope and trigger widget redraw
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

static void _scope_view_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      d->histogram_scale = (d->histogram_scale + 1) % DT_LIB_HISTOGRAM_SCALE_N;
      dt_conf_set_string("plugins/darkroom/histogram/histogram",
                         dt_lib_histogram_scale_names[d->histogram_scale]);
      _histogram_scale_update(d);
      dt_control_queue_redraw_widget(d->scope_draw);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      d->waveform_type = (d->waveform_type + 1) % DT_LIB_HISTOGRAM_WAVEFORM_N;
      dt_conf_set_string("plugins/darkroom/histogram/waveform",
                         dt_lib_histogram_waveform_type_names[d->waveform_type]);
      _waveform_view_update(d);
      dt_control_queue_redraw_widget(d->scope_draw);
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      d->vectorscope_scale = (d->vectorscope_scale + 1) % DT_LIB_HISTOGRAM_SCALE_N;
      dt_conf_set_string("plugins/darkroom/histogram/vectorscope/scale",
                         dt_lib_histogram_scale_names[d->vectorscope_scale]);
      _vectorscope_view_update(d);
      // trigger new process from scratch depending on whether linear or logarithmic
      // FIXME: it would be nice as with other scopes to make the initial processing independent of the view
      const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
      if(cv->view(cv) == DT_VIEW_DARKROOM)
        dt_dev_process_preview(darktable.develop);
      else
        dt_control_queue_redraw_center();
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }
}

static void _colorspace_clicked(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->vectorscope_type = (d->vectorscope_type + 1) % DT_LIB_HISTOGRAM_VECTORSCOPE_N;
  dt_conf_set_string("plugins/darkroom/histogram/vectorscope",
                     dt_lib_histogram_vectorscope_type_names[d->vectorscope_type]);
  _vectorscope_view_update(d);
  // trigger new process from scratch depending on whether CIELuv or JzAzBz
  // FIXME: it would be nice as with other scopes to make the initial processing independent of the view
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view(cv) == DT_VIEW_DARKROOM)
    dt_dev_process_preview(darktable.develop);
  else
    dt_control_queue_redraw_center();
}

// FIXME: these all could be the same function with different user_data
static void _red_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->red = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_red", d->red);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _green_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->green = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_green", d->green);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static void _blue_channel_toggle(GtkWidget *button, dt_lib_histogram_t *d)
{
  d->blue = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));
  gtk_widget_set_tooltip_text(button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  dt_conf_set_bool("plugins/darkroom/histogram/show_blue", d->blue);
  dt_control_queue_redraw_widget(d->scope_draw);
}

static gboolean _eventbox_enter_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  gtk_widget_show(d->button_box);
  return TRUE;
}

static gboolean _eventbox_motion_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  //This is required in order to correctly display the button tooltips
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
  gtk_widget_set_tooltip_text(d->green_channel_button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  gtk_widget_set_tooltip_text(d->blue_channel_button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  gtk_widget_set_tooltip_text(d->red_channel_button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  _scope_type_update(d);
  return TRUE;
}

static gboolean _eventbox_leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event,
                                                 gpointer user_data)
{
  // when click between buttons on the buttonbox a leave event is generated -- ignore it
  if(!(event->mode == GDK_CROSSING_UNGRAB && event->detail == GDK_NOTIFY_INFERIOR))
  {
    dt_lib_histogram_t *d = (dt_lib_histogram_t *)user_data;
    gtk_widget_hide(d->button_box);
  }
  return TRUE;
}

static gboolean _lib_histogram_scroll_callback(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y) &&
     dt_modifier_is(event->state, GDK_CONTROL_MASK) && !darktable.gui->reset)
  {
    /* set size of navigation draw area */
    const float hmin = (float)dt_confgen_get_int("plugins/darkroom/histogram/height", DT_MIN);
    const float hmax = (float)dt_confgen_get_int("plugins/darkroom/histogram/height", DT_MAX);
    const float histheight = clamp_range_f(dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f + 10 * delta_y, hmin, hmax);
    dt_conf_set_int("plugins/darkroom/histogram/height", histheight);
    gtk_widget_set_size_request(widget, -1, DT_PIXEL_APPLY_DPI(histheight));
  }
  return TRUE;
}

static gboolean _lib_histogram_collapse_callback(GtkAccelGroup *accel_group,
                                                 GObject *acceleratable, guint keyval,
                                                 GdkModifierType modifier, gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;

  // Get the state
  const gint visible = dt_lib_is_visible(self);

  // Inverse the visibility
  dt_lib_set_visible(self, !visible);

  return TRUE;
}

static gboolean _lib_histogram_cycle_mode_callback(GtkAccelGroup *accel_group,
                                                   GObject *acceleratable, guint keyval,
                                                   GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  // The cycle order is Hist log -> lin -> waveform -> parade -> vectorscope (update logic on more scopes)
  // FIXME: When switch modes, there is currently a hack to turn off the highlight and turn the cursor back to pointer, as we don't know what/if the new highlight is going to be. Right solution would be to have a highlight update function which takes cursor x,y and is called either here or on pointer motion. Really right solution is probably separate widgets for the drag areas which generate enter/leave events.
  // FIXME: can simplify this code: for view then type increment & compare enum max
  switch(d->scope_type)
  {
    case DT_LIB_HISTOGRAM_SCOPE_HISTOGRAM:
      if(d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->dragging = FALSE;
        d->waveform_type = DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID;
        dt_conf_set_string("plugins/darkroom/histogram/waveform",
                           dt_lib_histogram_waveform_type_names[d->waveform_type]);
        _scope_type_clicked(d->scope_type_button, d);
        d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
        dt_control_change_cursor(GDK_LEFT_PTR);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_WAVEFORM:
      if(d->waveform_type == DT_LIB_HISTOGRAM_WAVEFORM_OVERLAID)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else
      {
        d->dragging = FALSE;
        d->vectorscope_type = DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV;
        dt_conf_set_string("plugins/darkroom/histogram/vectorscope",
                           dt_lib_histogram_vectorscope_type_names[d->vectorscope_type]);
        d->vectorscope_scale = DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC;
        dt_conf_set_string("plugins/darkroom/histogram/vectorscope/scale",
                           dt_lib_histogram_scale_names[d->vectorscope_scale]);
        _scope_type_clicked(d->scope_type_button, d);
        d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
        dt_control_change_cursor(GDK_LEFT_PTR);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE:
      if(d->vectorscope_scale == DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC)
      {
        _scope_view_clicked(d->scope_view_button, d);
      }
      else if(d->vectorscope_type == DT_LIB_HISTOGRAM_VECTORSCOPE_CIELUV)
      {
        d->vectorscope_scale = DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC;
        dt_conf_set_string("plugins/darkroom/histogram/vectorscope/scale",
                           dt_lib_histogram_scale_names[d->vectorscope_scale]);
        _colorspace_clicked(d->colorspace_button, d);
      }
      else
      {
        d->histogram_scale = DT_LIB_HISTOGRAM_SCALE_LOGARITHMIC;
        dt_conf_set_string("plugins/darkroom/histogram/histogram",
                           dt_lib_histogram_scale_names[d->histogram_scale]);
        // don't need to cancel dragging or lose highlight so long as vectorscope isn't draggable
        _scope_type_clicked(d->scope_type_button, d);
      }
      break;
    case DT_LIB_HISTOGRAM_SCOPE_N:
      dt_unreachable_codepath();
  }

  return TRUE;
}

static gboolean _lib_histogram_change_mode_callback(GtkAccelGroup *accel_group,
                                                    GObject *acceleratable, guint keyval,
                                                    GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  d->dragging = FALSE;
  d->highlight = DT_LIB_HISTOGRAM_HIGHLIGHT_NONE;
  dt_control_change_cursor(GDK_LEFT_PTR);
  _scope_type_clicked(d->scope_type_button, d);
  return TRUE;
}

static gboolean _lib_histogram_change_type_callback(GtkAccelGroup *accel_group,
                                                    GObject *acceleratable, guint keyval,
                                                    GdkModifierType modifier, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  _scope_view_clicked(d->scope_view_button, d);
  return TRUE;
}

// this is only called in darkroom view
static void _lib_histogram_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  // preview pipe has already given process() the high quality
  // pre-gamma image. Now that preview pipe is complete, draw it
  // FIXME: it would be nice if process() just queued a redraw if not in live view, but then our draw code would have to have some other way to assure that the histogram image is current besides checking the pixelpipe to see if it has processed the current image
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  dt_control_queue_redraw_widget(d->scope_draw);
}

void view_enter(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;
  if(new_view->view(new_view) == DT_VIEW_DARKROOM)
  {
    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(_lib_histogram_preview_updated_callback), self);
  }
  // button box should be hidden when enter view, unless mouse is over
  // histogram, in which case gtk kindly generates enter events
  gtk_widget_hide(d->button_box);

  // FIXME: set histogram data to blank if enter tether with no active image
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals,
                               G_CALLBACK(_lib_histogram_preview_updated_callback),
                               self);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)g_malloc0(sizeof(dt_lib_histogram_t));
  self->data = (void *)d;

  dt_pthread_mutex_init(&d->lock, NULL);

  d->red = dt_conf_get_bool("plugins/darkroom/histogram/show_red");
  d->green = dt_conf_get_bool("plugins/darkroom/histogram/show_green");
  d->blue = dt_conf_get_bool("plugins/darkroom/histogram/show_blue");

  gchar *str = dt_conf_get_string("plugins/darkroom/histogram/mode");
  for(dt_lib_histogram_scope_type_t i=0; i<DT_LIB_HISTOGRAM_SCOPE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scope_type_names[i]) == 0)
      d->scope_type = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/histogram");
  for(dt_lib_histogram_scale_t i=0; i<DT_LIB_HISTOGRAM_SCALE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scale_names[i]) == 0)
      d->histogram_scale = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/waveform");
  for(dt_lib_histogram_waveform_type_t i=0; i<DT_LIB_HISTOGRAM_WAVEFORM_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_waveform_type_names[i]) == 0)
      d->waveform_type = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/vectorscope");
  for(dt_lib_histogram_vectorscope_type_t i=0; i<DT_LIB_HISTOGRAM_VECTORSCOPE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_vectorscope_type_names[i]) == 0)
      d->vectorscope_type = i;
  g_free(str);

  str = dt_conf_get_string("plugins/darkroom/histogram/vectorscope/scale");
  for(dt_lib_histogram_scale_t i=0; i<DT_LIB_HISTOGRAM_SCALE_N; i++)
    if(g_strcmp0(str, dt_lib_histogram_scale_names[i]) == 0)
      d->vectorscope_scale = i;
  g_free(str);

  int a = dt_conf_get_int("plugins/darkroom/histogram/vectorscope/angle");
  d->vectorscope_angle = a * M_PI / 180.0;

  d->histogram = (uint32_t *)calloc(4 * HISTOGRAM_BINS, sizeof(uint32_t));
  d->histogram_max = 0;

  // Waveform buffer doesn't need to be coupled with the histogram
  // widget size. The waveform is almost always scaled when
  // drawn. Choose buffer dimensions which produces workable detail,
  // don't use too much CPU/memory, and allow reasonable gradations
  // of tone.

  // Don't use absurd amounts of memory, exceed width of DT_MIPMAP_F
  // (which will be darktable.mipmap_cache->max_width[DT_MIPMAP_F]*2
  // for mosaiced images), nor make it too slow to calculate
  // (regardless of ppd). Try to get enough detail for a (default)
  // 350px panel, possibly 2x that on hidpi.  The actual buffer
  // width will vary with integral binning of image.
  // FIXME: increasing waveform_max_width increases processing speed less than increasing waveform_height -- tune these better?
  d->waveform_max_width = darktable.mipmap_cache->max_width[DT_MIPMAP_F]/2;
  // initially no waveform to draw
  d->waveform_width = 0;
  // 175 rows is the default histogram widget height. It's OK if the
  // widget height changes from this, as the width will almost always
  // be scaled. 175 rows is reasonable CPU usage and represents plenty
  // of tonal gradation. 256 would match the # of bins in a regular
  // histogram.
  d->waveform_height  = 175;
  // FIXME: combine with an intermediate buffer for vectorscope, as only use one or the other
  d->waveform_linear  = dt_iop_image_alloc(d->waveform_max_width, d->waveform_height, 3);
  // FIXME: combine waveform_8bit and vectorscope_graph, as only ever use one or the other
  d->waveform_8bit    = dt_alloc_align(64, sizeof(uint8_t) * 3 * d->waveform_height *
                                       cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->waveform_max_width));

  // FIXME: what is the appropriate resolution for this: balance memory use, processing speed, helpful resolution
  // FIXME: make this and waveform params #DEFINEd or const above, rather than set here?
  d->vectorscope_diameter_px = 384;
  d->vectorscope_graph = dt_alloc_align(64, sizeof(uint8_t) * d->vectorscope_diameter_px *
                                        cairo_format_stride_for_width(CAIRO_FORMAT_A8, d->vectorscope_diameter_px));
  // FIXME: note that the background can be lower resolution than the graph -- test/compare?
  d->vectorscope_bkgd = dt_alloc_align(64, sizeof(uint8_t) * 4U * d->vectorscope_diameter_px *
                                       cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, d->vectorscope_diameter_px));
  d->hue_ring_prof = NULL;
  d->hue_ring_scale = DT_LIB_HISTOGRAM_SCALE_N;
  d->hue_ring_colorspace = DT_LIB_HISTOGRAM_VECTORSCOPE_N;
  // initially no vectorscope to draw
  d->vectorscope_radius = 0.f;

  // proxy functions and data so that pixelpipe or tether can
  // provide data for a histogram
  // FIXME: do need to pass self, or can wrap a callback as a lambda
  darktable.lib->proxy.histogram.module = self;
  darktable.lib->proxy.histogram.process = dt_lib_histogram_process;
  darktable.lib->proxy.histogram.is_linear = d->histogram_scale == DT_LIB_HISTOGRAM_SCALE_LINEAR;

  // create widgets
  GtkWidget *overlay = gtk_overlay_new();

  // shows the scope, scale, and has draggable areas
  d->scope_draw = gtk_drawing_area_new();
  gtk_widget_set_tooltip_text(d->scope_draw, _("ctrl+scroll to change display height"));

  // a row of control buttons
  d->button_box = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_button_box_set_layout(GTK_BUTTON_BOX(d->button_box), GTK_BUTTONBOX_EXPAND);
  gtk_widget_set_valign(d->button_box, GTK_ALIGN_START);
  gtk_widget_set_halign(d->button_box, GTK_ALIGN_END);

  // FIXME: should histogram/waveform/vectorscope each be its own widget, and a GtkStack to switch between them?

  // First two buttons choose scope type and view of that scope (if
  // applicable). On click dt_lib_histogram_t data is updated,
  // icons/tooltips are updated, and button sensitivity is set as
  // needed.

  // FIXME: the button transitions when they appear on mouseover (mouse enters scope widget) or change (mouse click) cause redraws of the entire scope -- is there a way to avoid this?
  // FIXME: this could be a combobox to allow for more types and not to have to swap the icon on click
  // icons will be filled in by _scope_type_update()
  d->scope_type_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->scope_type_button, FALSE, FALSE, 0);
  d->scope_view_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->scope_view_button, FALSE, FALSE, 0);

  // the red togglebutton turns into colorspace button in vectorscope
  d->button_stack = gtk_stack_new();
  gtk_box_pack_start(GTK_BOX(d->button_box), d->button_stack, FALSE, FALSE, 0);

  // red/green/blue channel on/off
  // these are toggle boxes with a meaningful active state, unlike the type/view buttons
  d->red_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->red_channel_button, "red-channel-button");
  gtk_widget_set_tooltip_text(d->red_channel_button, d->red ? _("click to hide red channel") : _("click to show red channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->red_channel_button), d->red);
  // FIXME: just use gtk_container_add() as don't care about name?
  gtk_stack_add_named(GTK_STACK(d->button_stack), d->red_channel_button, "red");

  d->green_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->green_channel_button, "green-channel-button");
  gtk_widget_set_tooltip_text(d->green_channel_button, d->green ? _("click to hide green channel") : _("click to show green channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->green_channel_button), d->green);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->green_channel_button, FALSE, FALSE, 0);

  d->blue_channel_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_color, CPF_NONE, NULL);
  gtk_widget_set_name(d->blue_channel_button, "blue-channel-button");
  gtk_widget_set_tooltip_text(d->blue_channel_button, d->blue ? _("click to hide blue channel") : _("click to show blue channel"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->blue_channel_button), d->blue);
  gtk_box_pack_start(GTK_BOX(d->button_box), d->blue_channel_button, FALSE, FALSE, 0);

  d->colorspace_button = dtgtk_button_new(dtgtk_cairo_paint_empty, CPF_NONE, NULL);
  // FIXME: just use gtk_container_add() as don't care about name?
  gtk_stack_add_named(GTK_STACK(d->button_stack), d->colorspace_button, "colorspace");

  // will change sensitivity of channel buttons, hence must run after all buttons are declared
  _scope_type_update(d);

  // FIXME: add a brightness control (via GtkScaleButton?)

  // assemble the widgets

  // The main widget is an overlay which has no window, and hence
  // can't catch events. We need something on top to catch events to
  // show/hide the buttons. The drawable is below the buttons, and
  // hence won't catch motion events for the buttons, and gets a leave
  // event when the cursor moves over the buttons.

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
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), d->button_box);
  gtk_container_add(GTK_CONTAINER(eventbox), overlay);
  self->widget = eventbox;

  gtk_widget_set_name(self->widget, "main-histogram");
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  /* connect callbacks */

  g_signal_connect(G_OBJECT(d->scope_type_button), "clicked", G_CALLBACK(_scope_type_clicked), d);
  g_signal_connect(G_OBJECT(d->scope_view_button), "clicked", G_CALLBACK(_scope_view_clicked), d);
  g_signal_connect(G_OBJECT(d->colorspace_button), "clicked", G_CALLBACK(_colorspace_clicked), d);

  g_signal_connect(G_OBJECT(d->red_channel_button), "toggled", G_CALLBACK(_red_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->green_channel_button), "toggled", G_CALLBACK(_green_channel_toggle), d);
  g_signal_connect(G_OBJECT(d->blue_channel_button), "toggled", G_CALLBACK(_blue_channel_toggle), d);

  gtk_widget_add_events(d->scope_draw, GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
                                       GDK_BUTTON_RELEASE_MASK | darktable.gui->scroll_mask);
  // FIXME: why does cursor motion over buttons trigger multiple draw callbacks?
  g_signal_connect(G_OBJECT(d->scope_draw), "draw", G_CALLBACK(_drawable_draw_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "leave-notify-event",
                   G_CALLBACK(_drawable_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-press-event",
                   G_CALLBACK(_drawable_button_press_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "button-release-event",
                   G_CALLBACK(_drawable_button_release_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "motion-notify-event",
                   G_CALLBACK(_drawable_motion_notify_callback), d);
  g_signal_connect(G_OBJECT(d->scope_draw), "scroll-event",
                   G_CALLBACK(_drawable_scroll_callback), d);

  gtk_widget_add_events(eventbox, GDK_LEAVE_NOTIFY_MASK | GDK_ENTER_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(eventbox), "enter-notify-event",
                   G_CALLBACK(_eventbox_enter_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "leave-notify-event",
                   G_CALLBACK(_eventbox_leave_notify_callback), d);
  g_signal_connect(G_OBJECT(eventbox), "motion-notify-event",
                   G_CALLBACK(_eventbox_motion_notify_callback), d);

  // handles scroll-to-resize behavior
  gtk_widget_add_events(self->widget, darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(self->widget), "scroll-event",
                   G_CALLBACK(_lib_histogram_scroll_callback), NULL);

  /* set size of histogram draw area */
  const float histheight = dt_conf_get_int("plugins/darkroom/histogram/height") * 1.0f;
  gtk_widget_set_size_request(self->widget, -1, DT_PIXEL_APPLY_DPI(histheight));
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_histogram_t *d = (dt_lib_histogram_t *)self->data;

  free(d->histogram);
  dt_free_align(d->waveform_linear);
  dt_free_align(d->waveform_8bit);
  dt_free_align(d->vectorscope_graph);
  dt_pthread_mutex_destroy(&d->lock);

  g_free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "hide histogram"), GDK_KEY_H, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "cycle histogram modes"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram mode"), 0, 0);
  dt_accel_register_lib_as_view("darkroom", NC_("accel", "histogram/switch histogram type"), 0, 0);
  dt_accel_register_lib_as_view("tethering", NC_("accel", "switch histogram type"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "hide histogram",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_collapse_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "cycle histogram modes",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_cycle_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram mode",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_mode_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "darkroom", "histogram/switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
  dt_accel_connect_lib_as_view(self, "tethering", "switch histogram type",
                     g_cclosure_new(G_CALLBACK(_lib_histogram_change_type_callback), self, NULL));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
