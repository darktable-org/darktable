/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* film grain: grain of silver halide film for the scene-referred pipeline.

   Runs on the linear working-profile RGB ahead of the tone mapper, and
   applies grain as an exposure change: each channel is multiplied by 2^ev,
   so black stays black, nothing changes sign, and the grain passes through
   whatever tone mapping follows the way the image itself does.

   Two methods:

   - simplex: the layered noise and photographic paper response of the
     display-referred grain module, placed on a log-exposure scale centred
     on middle grey instead of on Lab lightness.

   - spektrafilm: the emulsion particle model of the spektrafilm module.
     Each channel is read as the exposure of its own dye layer, turned into
     a density, and redrawn as a Poisson process over the crystals the pixel
     covers; the difference is the grain. Its amplitude follows from crystal
     size and output size rather than from a noise amplitude. */

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/gaussian.h"
#include "common/imagebuf.h"
#include "common/iop_profile.h"
#include "common/math.h"
#include "common/opencl.h"
/* sf_blur_plane1 is the host twin of blur_plane.h's gauss kernels, and
   spektra_core.h also brings in data/kernels/grain.h. Because grain.h is
   compiled here, this file carries SPEKTRA_FP_CONTRACT_FLAGS in
   src/CMakeLists.txt. */
#include "common/spektra_core.h"
#include "../data/kernels/filmgrain.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_filmgrain_params_t)

/* ---- simplex method --------------------------------------------------- */

#define FILMGRAIN_SIMPLEX_STRENGTH_SCALE 0.15f
#define FILMGRAIN_SCALE_FACTOR 213.2

#define FILMGRAIN_LUT_SIZE 128
#define FILMGRAIN_LUT_DELTA_MAX 2.0
#define FILMGRAIN_LUT_DELTA_MIN 0.0001
#define FILMGRAIN_LUT_PAPER_GAMMA 1.0

/* ---- spektrafilm method ------------------------------------------------

   Grain size, sub-layer scale and the texture controls mirror spektrafilm's
   grain tab and move the grain in the same directions, with the same ranges
   and defaults, so a look can be carried between the two modules. What 1.0
   means differs: there each slider scales a figure measured for the loaded
   film stock, while this module has no stock, so 1.0 is the reference
   emulsion defined by the constants below. All of them are tuning constants
   that place the defaults; none is measured against film scans.

   spektrafilm's uniformity, density floor and dye cloud size are fixed
   here. Uniformity and the floor only reshape how grain varies with tone,
   which the tone curve controls directly: the floor acts below the toe of
   the characteristic curve, where a multiplicative exposure change on a
   near-black value cannot be seen, and uniformity moves mid-tone grain by
   a few percent across its useful range. The dye cloud is sub-pixel at
   ordinary output sizes (0.16 px for the coarsest layer at a 6000 px long
   edge) and only reaches a blur worth running at coarse grain sizes, which
   it still does at its fixed size. */

/* Area, in um^2, of one developed crystal at grain size 1.0. Places the
   default at ~110 crystals per pixel on a 6000 px long edge. */
#define FILMGRAIN_CLUMP_UM2 0.32f

/* Long edge of the frame the crystal area above is stated for. With the
   output size it gives the physical size of a pixel, which sets how many
   crystals a pixel averages over, and so why grain gets finer relative to
   the frame as the output grows.

   Not exposed: the crystal count per pixel depends on frame size and grain
   size only through their ratio, so a second slider would add nothing grain
   size cannot already reach. */
#define FILMGRAIN_REF_MM 36.0f

/* Density floor per crystal. spektrafilm notes that real stocks measure
   0.03 to 0.06. */
#define FILMGRAIN_DMIN 0.04f

/* Crystal distribution uniformity. Kept below 1 because
   grain_layer_particle's saturation term 1 - p * unif turns negative for
   unif >= 1 in the densest areas. */
#define FILMGRAIN_UNIF 0.9f

/* A real emulsion layers coarse crystals over finer ones. spektrafilm reads
   the layering from the stock's fitted density curves; here it is a fixed
   ladder of three, the coarsest held constant and the finer two following
   the sub-layer slider. */
#define FILMGRAIN_SUBLAYERS 3

/* Number of dye layers, one per colour channel. */
#define FILMGRAIN_DYE_LAYERS 3

/* Texture stage. A per-pixel draw is spatially independent, which is white
   noise, not grain; the clump blur is what gives it the texture of
   clustered crystals. Constants match spektrafilm's. The clump blur is a
   fixed pixel sigma: physical scaling lives in the crystal count. */
#define FILMGRAIN_BLUR_FACTOR 0.8f
#define FILMGRAIN_BLUR_MIN 0.05f
/* Dye cloud reference spread in um, scaled per sub-layer by the square root
   of that sub-layer's per-crystal optical density. */
#define FILMGRAIN_DYE_BLUR_UM 2.0f

/* The strength slider is shared with the simplex method. This gain makes
   a slider position give the same luminance grain at middle grey in both
   methods, about 0.2 EV mean deviation at the default strength, for a
   6000 px long edge. The match holds at that size only: particle grain
   weakens as the output grows, since each pixel then covers fewer
   crystals, while simplex grain does not. It scales the result only:
   strength 0 stays an exact identity. */
#define FILMGRAIN_STRENGTH_GAIN 9.0f

/* Normalisations that keep luminance and colour grain at the deviation s of
   a single dye layer. The sum of three independent layers has deviation
   s * sqrt(3); one layer's departure from the three-layer mean has
   s * sqrt(2/3). */
#define FILMGRAIN_LUM_NORM 0.57735027f   /* 1/sqrt(3) */
#define FILMGRAIN_CHROMA_NORM 1.2247449f /* sqrt(3/2) */

typedef enum dt_iop_filmgrain_method_t
{
  DT_FILMGRAIN_METHOD_SIMPLEX = 0,     // $DESCRIPTION: "simplex"
  DT_FILMGRAIN_METHOD_SPEKTRAFILM = 1, // $DESCRIPTION: "spektrafilm"
} dt_iop_filmgrain_method_t;

typedef struct dt_iop_filmgrain_params_t
{
  dt_iop_filmgrain_method_t method; // $DEFAULT: DT_FILMGRAIN_METHOD_SPEKTRAFILM $DESCRIPTION: "method"
  float strength;        // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "strength"
  /* simplex */
  float scale;           /* $MIN: 20.0/FILMGRAIN_SCALE_FACTOR
                            $MAX: 6400.0/FILMGRAIN_SCALE_FACTOR
                            $DEFAULT: 1600.0/FILMGRAIN_SCALE_FACTOR
                            $DESCRIPTION: "coarseness" */
  float midtones_bias;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "mid-tones bias"
  /* spektrafilm */
  float grain_size;      // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "grain size"
  float chroma;          // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "chroma"
  float sublayer_scale;  // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "sub-layer particle scale"
  float grain_blur;      // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "grain blur"
  float grain_usm_sigma; // $MIN: 0.0 $MAX: 3.0 $DEFAULT: 0.7 $DESCRIPTION: "recovery radius"
  float grain_usm_amount;// $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.5 $DESCRIPTION: "recovery strength"
  /* both methods: share of the grain per tone, one node per EV from
     FILMGRAIN_TONE_EV_FIRST up, 1.0 = unchanged */
  float tone_curve[FILMGRAIN_TONE_NODES]; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "tone curve"
} dt_iop_filmgrain_params_t;

typedef struct dt_iop_filmgrain_gui_data_t
{
  GtkNotebook *notebook;
  GtkWidget *method, *strength;
  GtkWidget *scale, *midtones_bias;
  GtkWidget *grain_size, *chroma, *sublayer_scale;
  GtkWidget *node[FILMGRAIN_TONE_NODES];
  GtkDrawingArea *area;
  dt_gui_collapsible_section_t node_sliders;
  dt_gui_collapsible_section_t texture;
  GtkWidget *grain_blur, *grain_usm_sigma, *grain_usm_amount;

  /* tone graph state, GUI thread only */
  int active_node;   // node under the pointer or being dragged, -1 for none
  gboolean dragging;
  float graph_x, graph_y, graph_w, graph_h; // plot rectangle from the last draw
} dt_iop_filmgrain_gui_data_t;

/* The synthetic emulsion, per sub-layer. npart is stated for a pixel of
   SF_GRAIN_REF_UM and rescaled to the rendered pixel size at process time. */
typedef struct dt_iop_filmgrain_emulsion_t
{
  float share[FILMGRAIN_SUBLAYERS];
  float dmin[FILMGRAIN_SUBLAYERS];
  float dmax[FILMGRAIN_SUBLAYERS];
  float npart[FILMGRAIN_SUBLAYERS];
  float unif;
} dt_iop_filmgrain_emulsion_t;

typedef struct dt_iop_filmgrain_data_t
{
  dt_iop_filmgrain_params_t p;
  dt_iop_filmgrain_emulsion_t em;
  float grain_lut[FILMGRAIN_LUT_SIZE * FILMGRAIN_LUT_SIZE];
  float tone_lut[FILMGRAIN_TONE_LUT_SIZE];
} dt_iop_filmgrain_data_t;

typedef struct dt_iop_filmgrain_global_data_t
{
  int kernel_density;
  int kernel_deviation;
  int kernel_accumulate;
  int kernel_recover;
  int kernel_apply;
  int kernel_gauss_row_1c;
  int kernel_gauss_col_1c;
} dt_iop_filmgrain_global_data_t;

const char *name()
{
  return _("film grain");
}

const char *aliases()
{
  return _("grain|noise|spektrafilm");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("simulate the grain of silver halide film"),
                                _("creative"),
                                _("linear, RGB, scene-referred"),
                                _("non-linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

/* ======================================================================
   tone curve
   ====================================================================== */

/* The nodes are joined by a monotone cubic (Fritsch-Carlson), which never
   overshoots between two nodes: a curve drawn between 50 % and 100 % stays
   in that range, and a flat stretch stays flat. */
static void _tone_tangents(const float *const y,
                           float *const m)
{
  const int n = FILMGRAIN_TONE_NODES;
  float delta[FILMGRAIN_TONE_NODES - 1];
  for(int k = 0; k < n - 1; k++) delta[k] = y[k + 1] - y[k];

  m[0] = delta[0];
  m[n - 1] = delta[n - 2];
  for(int k = 1; k < n - 1; k++)
    m[k] = delta[k - 1] * delta[k] <= 0.0f ? 0.0f : 0.5f * (delta[k - 1] + delta[k]);

  for(int k = 0; k < n - 1; k++)
  {
    if(delta[k] == 0.0f)
    {
      m[k] = m[k + 1] = 0.0f;
      continue;
    }
    const float a = m[k] / delta[k];
    const float b = m[k + 1] / delta[k];
    const float r2 = a * a + b * b;
    if(r2 > 9.0f)
    {
      const float tau = 3.0f / sqrtf(r2);
      m[k] = tau * a * delta[k];
      m[k + 1] = tau * b * delta[k];
    }
  }
}

/* The curve at `ev`, in EV relative to middle grey. Flat outside the
   outermost nodes. */
static float _tone_eval(const float *const y,
                        const float *const m,
                        const float ev)
{
  const float x = CLAMPF(ev - FILMGRAIN_TONE_EV_FIRST, 0.0f, (float)(FILMGRAIN_TONE_NODES - 1));
  const int k = MIN((int)x, FILMGRAIN_TONE_NODES - 2);
  // a flat segment is returned as is, so an untouched curve is exactly 1.0
  if(y[k] == y[k + 1] && m[k] == 0.0f && m[k + 1] == 0.0f) return y[k];
  const float t = x - (float)k;
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
  const float h10 = t3 - 2.0f * t2 + t;
  const float h01 = -2.0f * t3 + 3.0f * t2;
  const float h11 = t3 - t2;
  return fmaxf(h00 * y[k] + h10 * m[k] + h01 * y[k + 1] + h11 * m[k + 1], 0.0f);
}

/* EV relative to middle grey at a table position, the inverse of the
   characteristic curve's t = 0.5 + ev / latitude. */
static inline float _tone_lut_ev(const int i)
{
  const float t = (float)i / (float)(FILMGRAIN_TONE_LUT_SIZE - 1);
  return (t - 0.5f) * FILMGRAIN_LATITUDE_EV;
}

static void _tone_build_lut(const float *const nodes,
                            float *const lut)
{
  float m[FILMGRAIN_TONE_NODES];
  _tone_tangents(nodes, m);
  for(int i = 0; i < FILMGRAIN_TONE_LUT_SIZE; i++)
    lut[i] = _tone_eval(nodes, m, _tone_lut_ev(i));
}

/* Y row of the pipe's working profile. Returns FALSE for a non-linear
   profile, where a row of the matrix is not the luminance; the caller then
   falls back to dt_ioppr_get_rgb_matrix_luminance, which the device path
   does not have. */
static gboolean _luminance_coeffs(const dt_dev_pixelpipe_iop_t *const piece,
                                  float *const coeffs)
{
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(!work_profile)
  {
    // Rec. 2020, darktable's default working space
    coeffs[0] = 0.2627f;
    coeffs[1] = 0.6780f;
    coeffs[2] = 0.0593f;
    return TRUE;
  }
  for(int c = 0; c < 3; c++) coeffs[c] = work_profile->matrix_in[1][c];
  return !work_profile->nonlinearlut;
}

/* Tone curve factor for one input pixel, as both methods use it on the
   host. The linear case is the exact expression the kernel evaluates. */
static inline float _pixel_tone_factor(const dt_iop_filmgrain_data_t *const d,
                                       const dt_iop_order_iccprofile_info_t *const work_profile,
                                       const gboolean linear,
                                       const float *const coeffs,
                                       const float *const pin)
{
  const float lum = linear
      ? filmgrain_luminance(pin[0], pin[1], pin[2], coeffs[0], coeffs[1], coeffs[2])
      : dt_ioppr_get_rgb_matrix_luminance(pin, work_profile->matrix_in, work_profile->lut_in,
                                          work_profile->unbounded_coeffs_in,
                                          work_profile->lutsize, work_profile->nonlinearlut);
  return filmgrain_tone_factor(d->tone_lut, filmgrain_density(lum) / FILMGRAIN_DMAX);
}

/* ======================================================================
   simplex method
   ====================================================================== */

static const double grad3[12][3]
  = { { 1, 1, 0 },  { -1, 1, 0 },  { 1, -1, 0 },  { -1, -1, 0 },
      { 1, 0, 1 },  { -1, 0, 1 },  { 1, 0, -1 },  { -1, 0, -1 },
      { 0, 1, 1 },  { 0, -1, 1 },  { 0, 1, -1 },  { 0, -1, -1 } };

static const int permutation[]
    = { 151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,
        69,  142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,
        94,  252, 219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136,
        171, 168, 68,  175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122,
        60,  211, 133, 230, 220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161,
        1,   216, 80,  73,  209, 76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,
        164, 100, 109, 198, 173, 186, 3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126,
        255, 82,  85,  212, 207, 206, 59,  227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213,
        119, 248, 152, 2,   44,  154, 163, 70,  221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253,
        19,  98,  108, 110, 79,  113, 224, 232, 178, 185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193,
        238, 210, 144, 12,  191, 179, 162, 241, 81,  51,  145, 235, 249, 14,  239, 107, 49,  192, 214, 31,
        181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,  45,  127, 4,   150, 254, 138, 236, 205, 93,
        222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,  215, 61,  156, 180 };

static size_t perm[512];     // permutation lookup table
static size_t perm_mod[512]; // same, all values mod 12 to select from grad3

static void _simplex_noise_init()
{
  for(int i = 0; i < 512; i++)
  {
    perm[i] = permutation[i & 255];
    perm_mod[i] = perm[i] % 12;
  }
}

static inline double _dot(const double g[],
                          const double x,
                          const double y,
                          const double z)
{
  return g[0] * x + g[1] * y + g[2] * z;
}

#define FILMGRAIN_FASTFLOOR(x) ((x) > 0 ? (int)(x) : (int)(x) - 1)

/* 3D simplex noise, scaled to stay just inside [-1, 1]. */
static double _simplex_noise(const double xin,
                             const double yin,
                             const double zin)
{
  const double F3 = 1.0 / 3.0;
  const double s = (xin + yin + zin) * F3;
  const int i = FILMGRAIN_FASTFLOOR(xin + s);
  const int j = FILMGRAIN_FASTFLOOR(yin + s);
  const int k = FILMGRAIN_FASTFLOOR(zin + s);
  const double G3 = 1.0 / 6.0;
  const double t = (i + j + k) * G3;
  const double x0 = xin - (i - t);
  const double y0 = yin - (j - t);
  const double z0 = zin - (k - t);

  // which of the six tetrahedra of the skewed cube the point falls in
  int i1, j1, k1, i2, j2, k2;
  if(x0 >= y0)
  {
    if(y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    else if(x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
    else              { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
  }
  else
  {
    if(y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
    else if(x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
    else              { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
  }

  const double x1 = x0 - i1 + G3, y1 = y0 - j1 + G3, z1 = z0 - k1 + G3;
  const double x2 = x0 - i2 + 2.0 * G3, y2 = y0 - j2 + 2.0 * G3, z2 = z0 - k2 + 2.0 * G3;
  const double x3 = x0 - 1.0 + 3.0 * G3, y3 = y0 - 1.0 + 3.0 * G3, z3 = z0 - 1.0 + 3.0 * G3;

  const int ii = i & 255, jj = j & 255, kk = k & 255;
  const size_t gi0 = perm_mod[ii + perm[jj + perm[kk]]];
  const size_t gi1 = perm_mod[ii + i1 + perm[jj + j1 + perm[kk + k1]]];
  const size_t gi2 = perm_mod[ii + i2 + perm[jj + j2 + perm[kk + k2]]];
  const size_t gi3 = perm_mod[ii + 1 + perm[jj + 1 + perm[kk + 1]]];

  double n = 0.0;
  double t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
  if(t0 >= 0) { t0 *= t0; n += t0 * t0 * _dot(grad3[gi0], x0, y0, z0); }
  double t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
  if(t1 >= 0) { t1 *= t1; n += t1 * t1 * _dot(grad3[gi1], x1, y1, z1); }
  double t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
  if(t2 >= 0) { t2 *= t2; n += t2 * t2 * _dot(grad3[gi2], x2, y2, z2); }
  double t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
  if(t3 >= 0) { t3 *= t3; n += t3 * t3 * _dot(grad3[gi3], x3, y3, z3); }

  return 32.0 * n;
}

/* Three octaves whose frequencies and weights match the power spectrum of
   real grain scans. z sets the overall scale. */
static double _simplex_2d_noise(const double x,
                                const double y,
                                const double z)
{
  static const double f[] = { 0.4910, 0.9441, 1.7280 };
  static const double a[] = { 0.2340, 0.7850, 1.2150 };

  double total = 0.0;
  for(int octave = 0; octave < 3; octave++)
    total += _simplex_noise(x * f[octave] / z, y * f[octave] / z, octave) * a[octave];
  return total;
}

/* Photographic paper response and its inverse, over normalised exposure and
   density in [0, 1]. mb is the mid-tones bias, which steepens the fall-off
   toward both ends. */
static float _paper_resp(const float exposure,
                         const float mb,
                         const float gp)
{
  const float delta = FILMGRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(FILMGRAIN_LUT_DELTA_MIN));
  return (1.0f + 2.0f * delta)
             / (1.0f + expf((4.0f * gp * (0.5f - exposure)) / (1.0f + 2.0f * delta)))
         - delta;
}

static float _paper_resp_inverse(const float density,
                                 const float mb,
                                 const float gp)
{
  const float delta = FILMGRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(FILMGRAIN_LUT_DELTA_MIN));
  return -logf((1.0f + 2.0f * delta) / (density + delta) - 1.0f) * (1.0f + 2.0f * delta)
             / (4.0f * gp)
         + 0.5f;
}

/* Tabulate how far a noise value moves a tone after the paper response,
   indexed by noise (x) and by the tone's position on the log-exposure
   scale (y). Entries are in hundredths of that scale. */
static void _evaluate_grain_lut(float *const grain_lut,
                                const float mb)
{
  for(int i = 0; i < FILMGRAIN_LUT_SIZE; i++)
    for(int j = 0; j < FILMGRAIN_LUT_SIZE; j++)
    {
      const float gu = (float)i / (FILMGRAIN_LUT_SIZE - 1) - 0.5f;
      const float l = (float)j / (FILMGRAIN_LUT_SIZE - 1);
      grain_lut[j * FILMGRAIN_LUT_SIZE + i]
          = 100.0f * (_paper_resp(gu + _paper_resp_inverse(l, mb, FILMGRAIN_LUT_PAPER_GAMMA), mb,
                                  FILMGRAIN_LUT_PAPER_GAMMA)
                      - l);
    }
}

static inline float _lut_lookup_2d_1c(const float *const grain_lut,
                                      const float x,
                                      const float y)
{
  const float _x = CLAMPS((x + 0.5f) * (FILMGRAIN_LUT_SIZE - 1), 0, FILMGRAIN_LUT_SIZE - 1);
  const float _y = CLAMPS(y * (FILMGRAIN_LUT_SIZE - 1), 0, FILMGRAIN_LUT_SIZE - 1);

  const int _x0 = _x < FILMGRAIN_LUT_SIZE - 2 ? _x : FILMGRAIN_LUT_SIZE - 2;
  const int _y0 = _y < FILMGRAIN_LUT_SIZE - 2 ? _y : FILMGRAIN_LUT_SIZE - 2;
  const int _x1 = _x0 + 1;
  const int _y1 = _y0 + 1;

  const float x_diff = _x - _x0;
  const float y_diff = _y - _y0;

  const float l00 = grain_lut[_y0 * FILMGRAIN_LUT_SIZE + _x0];
  const float l01 = grain_lut[_y0 * FILMGRAIN_LUT_SIZE + _x1];
  const float l10 = grain_lut[_y1 * FILMGRAIN_LUT_SIZE + _x0];
  const float l11 = grain_lut[_y1 * FILMGRAIN_LUT_SIZE + _x1];

  const float xy0 = (1.0f - y_diff) * l00 + l10 * y_diff;
  const float xy1 = (1.0f - y_diff) * l01 + l11 * y_diff;
  return xy0 * (1.0f - x_diff) + xy1 * x_diff;
}

/* DJBX33X hash of the file name, taken from the end of the string so that
   numbered frames of a sequence, which differ in their last characters,
   get well-separated noise offsets and the grain does not stand still
   across a time-lapse. */
static unsigned int _hash_string(const char *const str)
{
  unsigned int hash = 5381;
  for(int i = (int)strlen(str) - 1; i >= 0; i--)
    hash = ((hash << 5) + hash) ^ (unsigned char)str[i];
  return hash;
}

static void _process_simplex(const dt_iop_filmgrain_data_t *const d,
                             const dt_dev_pixelpipe_iop_t *const piece,
                             const float *const in,
                             float *const out,
                             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  float coeffs[3];
  const gboolean linear = _luminance_coeffs(piece, coeffs);

  /* The offset is reduced to a fixed range rather than one tied to the
     region's width, so it does not change, and the pattern does not jump,
     when the region being rendered changes size. */
  const double hash = _hash_string(piece->pipe->image.filename) % 1024;

  const gboolean fastmode = dt_pipe_is_fast(piece->pipe);
  const float strength = d->p.strength / 100.0f;
  const double wd = fminf(piece->buf_in.width, piece->buf_in.height);
  const double zoom = (1.0 + 8 * d->p.scale / 100) / 800.0;
  // in fast pipe mode, skip the downsampling for zoomed-out views
  const gboolean filter = !fastmode && fabsf(roi_out->scale - 1.0f) > 0.01f;
  /* width of the downsampling filter in world space: undo the short-side
     normalisation, roi scale and the pipe's own input scale */
  const double filtermul = piece->iscale / (roi_out->scale * wd);
  const float fib1 = 34.0f, fib2 = 21.0f;
  const float fib1div2 = fib1 / fib2;
  const double scale = roi_out->scale;
  const double fib2inv = 1.0 / fib2;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *pin = in + (size_t)4 * roi_out->width * j;
    float *pout = out + (size_t)4 * roi_out->width * j;
    // resolution-independent coordinates, normalised to the short side
    const double y = (roi_out->y + j) / scale / wd;

    for(int i = 0; i < roi_out->width; i++, pin += 4, pout += 4)
    {
      const double x = (roi_out->x + i) / scale / wd;
      float noise = 0.0f;
      if(filter)
      {
        // zoomed out: rank-1 lattice downsampling
        for(int l = 0; l < fib2; l++)
        {
          const float px = l / fib2;
          float py = l * fib1div2;
          py -= (int)py;
          noise += fib2inv
                   * _simplex_2d_noise(x + px * filtermul + hash, y + py * filtermul, zoom);
        }
      }
      else
        noise = _simplex_2d_noise(x + hash, y, zoom);

      const float lum = linear
          ? filmgrain_luminance(pin[0], pin[1], pin[2], coeffs[0], coeffs[1], coeffs[2])
          : dt_ioppr_get_rgb_matrix_luminance(pin, work_profile->matrix_in, work_profile->lut_in,
                                              work_profile->unbounded_coeffs_in,
                                              work_profile->lutsize, work_profile->nonlinearlut);

      /* Position on the same log-exposure scale the spektrafilm method uses:
         middle grey in the middle, LATITUDE_EV across. The paper-response
         table works in hundredths of that scale, which convert back to EV. */
      const float tone = filmgrain_density(lum) / FILMGRAIN_DMAX;
      const float shift = _lut_lookup_2d_1c(d->grain_lut,
                                            noise * strength * FILMGRAIN_SIMPLEX_STRENGTH_SCALE,
                                            tone);
      const float factor = filmgrain_tone_factor(d->tone_lut, tone);
      const float gain = grain_exp2f(factor * shift / 100.0f * FILMGRAIN_LATITUDE_EV);

      pout[0] = pin[0] * gain;
      pout[1] = pin[1] * gain;
      pout[2] = pin[2] * gain;
      pout[3] = pin[3];
    }
  }
}

/* ======================================================================
   spektrafilm method
   ====================================================================== */

/* Build the synthetic emulsion from the sliders. Density is split evenly
   across the sub-layers; a real stock's split comes from its fitted
   curves, and with none to fit even is the neutral choice. */
static void _emulsion_build(dt_iop_filmgrain_emulsion_t *const em,
                            const dt_iop_filmgrain_params_t *const p)
{
  /* Coarsest first. Only the finer layers follow the slider, so at 0 they
     shrink away and the coarsest carries the grain alone, as in
     spektrafilm. */
  static const float ladder[FILMGRAIN_SUBLAYERS] = { 1.0f, 0.5f, 0.25f };
  const float ref_um2 = SF_GRAIN_REF_UM * SF_GRAIN_REF_UM;
  const float share = 1.0f / (float)FILMGRAIN_SUBLAYERS;

  for(int sl = 0; sl < FILMGRAIN_SUBLAYERS; sl++)
  {
    const float ps = sl == 0 ? ladder[sl] : ladder[sl] * p->sublayer_scale;
    /* A pixel covers its area divided by one crystal's, and crystal area
       goes with the square of its linear size. */
    const float lin = fmaxf(p->grain_size * ps, 1e-4f);
    const float clump_um2 = FILMGRAIN_CLUMP_UM2 * lin * lin;
    const float dmin = FILMGRAIN_DMIN * share;

    em->share[sl] = share;
    em->dmin[sl] = dmin;
    em->dmax[sl] = FILMGRAIN_DMAX * share + dmin;
    em->npart[sl] = fmaxf(ref_um2 / clump_um2, 1e-3f);
  }
  em->unif = FILMGRAIN_UNIF;
}

/* Output-dependent scales of the spektrafilm method, computed in one place
   so process(), process_cl(), modify_roi_in() and tiling_callback() cannot
   disagree about them. */
typedef struct dt_iop_filmgrain_scales_t
{
  float npart_scale;                    // crystal counts from SF_GRAIN_REF_UM to this pixel size
  float dye_sigma[FILMGRAIN_SUBLAYERS];  // per sub-layer dye cloud blur, px
  float clump_sigma;                    // clump blur, px
  float usm_sigma;                      // acutance recovery blur, px (0 when recovery is off)
  int halo;                             // padding the three blurs need, px
} dt_iop_filmgrain_scales_t;

/* Blurs narrower than SF_GAUSS_MIN_SIGMA are skipped rather than run with
   the 3-tap kernel dt_gaussian_kernel_1d floors at, on both paths alike. */
static inline float _blur_sigma(const float sigma)
{
  return sigma >= SF_GAUSS_MIN_SIGMA ? sigma : 0.0f;
}

/* Pixels a blur of this sigma reads beyond the pixel it writes. */
static inline int _blur_reach(const float sigma)
{
  return sigma > 0.0f ? (int)ceilf(3.0f * sigma) + 1 : 0;
}

static void _spektra_scales(const dt_iop_filmgrain_data_t *const d,
                            const dt_dev_pixelpipe_iop_t *const piece,
                            const float roi_scale,
                            dt_iop_filmgrain_scales_t *const s)
{
  memset(s, 0, sizeof(*s));
  if(d->p.method != DT_FILMGRAIN_METHOD_SPEKTRAFILM || d->p.strength <= 0.0f) return;

  /* Micrometres per pixel. The reference frame is stated by its long edge,
     so it pairs with the long edge of the full buffer at this scale. */
  const float long_edge
      = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * roi_scale, 1.0f);
  const float pixel_um = FILMGRAIN_REF_MM * 1000.0f / long_edge;
  s->npart_scale = pixel_um * pixel_um / (SF_GRAIN_REF_UM * SF_GRAIN_REF_UM);

  /* The pixel-sized blurs shrink with the view when zoomed out, as in
     spektrafilm, so a downscaled render matches a downscaled full-size one
     instead of showing full-size clumps on smaller pixels. */
  const float preview_scale = fminf(roi_scale, 1.0f);

  int dye_reach = 0;
  for(int sl = 0; sl < FILMGRAIN_SUBLAYERS; sl++)
  {
    const float od = d->em.dmax[sl] / fmaxf(d->em.npart[sl] * s->npart_scale, 1e-6f);
    s->dye_sigma[sl] = _blur_sigma(FILMGRAIN_DYE_BLUR_UM * sqrtf(fmaxf(od, 0.0f)) * preview_scale);
    dye_reach = MAX(dye_reach, _blur_reach(s->dye_sigma[sl]));
  }
  s->clump_sigma = _blur_sigma(FILMGRAIN_BLUR_FACTOR * fmaxf(d->p.grain_blur, FILMGRAIN_BLUR_MIN)
                               * preview_scale);
  s->usm_sigma = d->p.grain_usm_amount > 0.0f
                     ? _blur_sigma(d->p.grain_usm_sigma * preview_scale)
                     : 0.0f;

  /* The blurs run one after another, so the region an edge can corrupt
     grows by each one's reach in turn. */
  s->halo = dye_reach + _blur_reach(s->clump_sigma) + _blur_reach(s->usm_sigma);
}

/* Luminance and chroma gains for filmgrain_channel_ev: strength, the
   density-to-EV scale of the characteristic curve, and the two
   normalisations. */
static void _spektra_gains(const dt_iop_filmgrain_data_t *const d,
                           float *const k_lum,
                           float *const k_chroma)
{
  const float amount = d->p.strength / 100.0f * FILMGRAIN_STRENGTH_GAIN;
  const float ev_per_density = FILMGRAIN_LATITUDE_EV / FILMGRAIN_DMAX;
  *k_lum = amount * FILMGRAIN_LUM_NORM * ev_per_density;
  *k_chroma = amount * d->p.chroma * FILMGRAIN_CHROMA_NORM * ev_per_density;
}

/* Build the three dye layers' grain fields over the whole of roi_in into
   acc[0..2]. dens, dev, scratch and blurred are caller-owned work buffers
   (3, 1, 1 and 1 floats per pixel). */
static void _spektra_grain(const dt_iop_filmgrain_data_t *const d,
                           const dt_iop_filmgrain_scales_t *const sc,
                           const float *const in,
                           float *const dens,
                           float *const dev,
                           float *const scratch,
                           float *const blurred,
                           float *const acc[FILMGRAIN_DYE_LAYERS],
                           const dt_iop_roi_t *const roi_in)
{
  const int w = roi_in->width;
  const int h = roi_in->height;
  const size_t npix = (size_t)w * h;

  DT_OMP_FOR()
  for(size_t k = 0; k < npix; k++)
    for(int c = 0; c < 3; c++)
      dens[3 * k + c] = filmgrain_density(in[4 * k + c]);

  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++)
  {
    /* Each sub-layer is drawn, reduced to its deviation, spread by its own
       dye cloud and only then summed. Blurring per sub-layer keeps the
       coarse and fine layers at their own spread; one blur of the sum would
       give them all the same. */
    for(int sl = 0; sl < FILMGRAIN_SUBLAYERS; sl++)
    {
      const float share = d->em.share[sl];
      const float dmin = d->em.dmin[sl];
      const float dmax = d->em.dmax[sl];
      const float npart = d->em.npart[sl] * sc->npart_scale;
      const float unif = d->em.unif;

      DT_OMP_FOR()
      for(int y = 0; y < h; y++)
        for(int x = 0; x < w; x++)
        {
          const size_t k = (size_t)y * w + x;
          /* Absolute image coordinates, so the pattern does not crawl when
             panning and a tile draws what the untiled render draws. */
          const uint32_t seed = grain_pixel_seed((uint32_t)(x + roi_in->x),
                                                 (uint32_t)(y + roi_in->y),
                                                 (uint32_t)(c + sl * 10));
          dev[k] = filmgrain_sublayer_deviation(dens[3 * k + c], share, dmin, dmax, npart,
                                                unif, seed);
        }

      sf_blur_plane1(dev, w, h, sc->dye_sigma[sl], NULL, scratch);

      if(sl == 0)
        dt_iop_image_copy(acc[c], dev, npix);
      else
      {
        float *const a = acc[c];
        DT_OMP_FOR()
        for(size_t k = 0; k < npix; k++) a[k] += dev[k];
      }
    }

    // crystal clumping: turns per-pixel white noise into grain texture
    sf_blur_plane1(acc[c], w, h, sc->clump_sigma, NULL, scratch);

    /* Acutance recovery for the clump blur. Additive: acc is a zero-mean
       deviation, where a ratio breaks down at every zero crossing. */
    if(sc->usm_sigma > 0.0f)
    {
      const float amount = d->p.grain_usm_amount;
      float *const a = acc[c];
      dt_iop_image_copy(blurred, a, npix);
      sf_blur_plane1(blurred, w, h, sc->usm_sigma, NULL, scratch);
      DT_OMP_FOR()
      for(size_t k = 0; k < npix; k++) a[k] = a[k] + amount * (a[k] - blurred[k]);
    }
  }
}

static void _process_spektra(const dt_iop_filmgrain_data_t *const d,
                             const dt_dev_pixelpipe_iop_t *const piece,
                             const float *const in,
                             float *const out,
                             const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  /* modify_roi_in() pads the input by the blur reach, so `in` covers roi_in
     and `out` covers roi_out. Grain is built across all of roi_in, so the
     blurs see real data beyond the output edges, and only the output window
     is written at the end. */
  const int w = roi_in->width;
  const size_t npix = (size_t)w * roi_in->height;
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;

  dt_iop_filmgrain_scales_t sc;
  _spektra_scales(d, piece, roi_in->scale, &sc);

  float *acc[FILMGRAIN_DYE_LAYERS] = { NULL, NULL, NULL };
  float *const dens = dt_alloc_align_float(3 * npix);
  float *const dev = dt_alloc_align_float(npix);
  float *const scratch = dt_alloc_align_float(npix);
  float *const blurred = dt_alloc_align_float(npix);
  gboolean ok = dens && dev && scratch && blurred;
  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++)
  {
    acc[c] = dt_alloc_align_float(npix);
    ok = ok && acc[c];
  }

  if(!ok)
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out);
  else
  {
    _spektra_grain(d, &sc, in, dens, dev, scratch, blurred, acc, roi_in);

    float k_lum, k_chroma;
    _spektra_gains(d, &k_lum, &k_chroma);
    const dt_iop_order_iccprofile_info_t *const work_profile
        = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
    float coeffs[3];
    const gboolean linear = _luminance_coeffs(piece, coeffs);
    const float *const g0 = acc[0];
    const float *const g1 = acc[1];
    const float *const g2 = acc[2];

    DT_OMP_FOR()
    for(int y = 0; y < roi_out->height; y++)
    {
      const size_t row = (size_t)(y + dy) * w + dx;
      const float *pin = in + 4 * row;
      float *pout = out + (size_t)4 * y * roi_out->width;
      for(int x = 0; x < roi_out->width; x++, pin += 4, pout += 4)
      {
        const float a0 = g0[row + x];
        const float a1 = g1[row + x];
        const float a2 = g2[row + x];
        const float tone = _pixel_tone_factor(d, work_profile, linear, coeffs, pin);
        pout[0] = pin[0] * grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a0, k_lum, k_chroma));
        pout[1] = pin[1] * grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a1, k_lum, k_chroma));
        pout[2] = pin[2] * grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a2, k_lum, k_chroma));
        pout[3] = pin[3];
      }
    }
  }

  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++) dt_free_align(acc[c]);
  dt_free_align(dens);
  dt_free_align(dev);
  dt_free_align(scratch);
  dt_free_align(blurred);
}
/* ======================================================================
   pipe hooks
   ====================================================================== */

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  const dt_iop_filmgrain_data_t *const d = piece->data;
  if(!d) return;

  dt_iop_filmgrain_scales_t sc;
  _spektra_scales(d, piece, roi_out->scale, &sc);
  if(sc.halo <= 0) return;

  const int img_w = (int)roundf((float)piece->buf_in.width * roi_out->scale);
  const int img_h = (int)roundf((float)piece->buf_in.height * roi_out->scale);
  const int x0 = MAX(roi_out->x - sc.halo, 0);
  const int y0 = MAX(roi_out->y - sc.halo, 0);
  int x1 = roi_out->x + roi_out->width + sc.halo;
  int y1 = roi_out->y + roi_out->height + sc.halo;
  if(img_w > 0) x1 = MIN(x1, MAX(img_w, roi_out->x + roi_out->width));
  if(img_h > 0) y1 = MIN(y1, MAX(img_h, roi_out->y + roi_out->height));
  roi_in->x = x0;
  roi_in->y = y0;
  roi_in->width = x1 - x0;
  roi_in->height = y1 - y0;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_filmgrain_data_t *const d = piece->data;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->align = 1;
  tiling->factor = 2.0f;
  tiling->factor_cl = 2.0f;
  tiling->overlap = 0;

  if(!d || d->p.method != DT_FILMGRAIN_METHOD_SPEKTRAFILM) return;

  /* In units of the 4-float input pixel. Host: three densities, one
     sub-layer deviation, blur scratch, recovery copy and three accumulated
     layers, 9 floats. Device: the float4 density buffer, deviation, row-pass
     scratch, recovery copy and three layers, 10 floats. */
  tiling->factor = 2.0f + 9.0f / 4.0f;
  tiling->factor_cl = 2.0f + 10.0f / 4.0f;

  dt_iop_filmgrain_scales_t sc;
  _spektra_scales(d, piece, roi_in->scale, &sc);
  tiling->overlap = sc.halo;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const dt_iop_filmgrain_data_t *const d = piece->data;

  if(d->p.method == DT_FILMGRAIN_METHOD_SPEKTRAFILM)
  {
    if(d->p.strength <= 0.0f)
      dt_iop_copy_image_roi(ovoid, ivoid, 4, roi_in, roi_out);
    else
      _process_spektra(d, piece, ivoid, ovoid, roi_in, roi_out);
  }
  else
    _process_simplex(d, piece, ivoid, ovoid, roi_out);
}

#ifdef HAVE_OPENCL
/* One separable gaussian over a single-channel device buffer, the twin of
   sf_blur_plane1: the same dt_gaussian_kernel_1d weights, clamp-to-edge and
   accumulation order. `tmp` takes the row pass; the column pass writes back
   into `buf`. */
static cl_int _blur_cl(const int devid,
                       const dt_iop_filmgrain_global_data_t *const gd,
                       cl_mem buf,
                       cl_mem tmp,
                       const int w,
                       const int h,
                       const float sigma)
{
  if(sigma < 1e-6f) return CL_SUCCESS;

  float weights[2 * SF_GAUSS_MAX_RADIUS + 1];
  const int radius = dt_gaussian_kernel_1d(sigma, weights, SF_GAUSS_MAX_RADIUS);

  cl_mem dev_w = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(float) * (size_t)(2 * radius + 1), weights);
  if(!dev_w) return DT_OPENCL_SYSMEM_ALLOCATION;

  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_row_1c, w, h,
      CLARG(buf), CLARG(tmp), CLARG(w), CLARG(h), CLARG(dev_w), CLARG(radius));
  if(err == CL_SUCCESS)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_col_1c, w, h,
        CLARG(tmp), CLARG(buf), CLARG(w), CLARG(h), CLARG(dev_w), CLARG(radius));

  dt_opencl_release_mem_object(dev_w);
  return err;
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmgrain_data_t *const d = piece->data;
  const dt_iop_filmgrain_global_data_t *const gd = self->global_data;
  const int devid = piece->pipe->devid;

  /* Fast OpenCL mode compiles kernels with -cl-fast-relaxed-math, which
     marks the whole translation unit approximate: built-ins may be answered
     to a few ULP and multiply-adds may fuse. grain_poisson's accept/reject
     loop turns a one-ULP difference into a whole grain quantum, so the
     device would draw different grain from the CPU and a preview would not
     match its export. The CPU path renders the same picture, slower. */
  if(dt_opencl_running_fast()) return DT_OPENCL_PROCESS_CL;

  /* The simplex method has no device path, and the kernel only knows the
     luminance of a linear working profile; both are handed to the CPU. */
  float coeffs[3];
  if(d->p.method != DT_FILMGRAIN_METHOD_SPEKTRAFILM || !_luminance_coeffs(piece, coeffs))
    return DT_OPENCL_PROCESS_CL;

  if(d->p.strength <= 0.0f)
  {
    size_t origin[] = { roi_out->x - roi_in->x, roi_out->y - roi_in->y, 0 };
    size_t region[] = { roi_out->width, roi_out->height, 1 };
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, CLIMG_ORIGIN, region);
  }

  const int w = roi_in->width;
  const int h = roi_in->height;
  const size_t npix = (size_t)w * h;
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  const int roi_x = roi_in->x;
  const int roi_y = roi_in->y;

  dt_iop_filmgrain_scales_t sc;
  _spektra_scales(d, piece, roi_in->scale, &sc);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem acc[FILMGRAIN_DYE_LAYERS] = { NULL, NULL, NULL };
  cl_mem tone_lut = dt_opencl_copy_host_to_device_constant(devid, sizeof(d->tone_lut),
                                                           (float *)d->tone_lut);
  cl_mem dens = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * npix);
  cl_mem dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
  cl_mem tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
  cl_mem blurred = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
  gboolean ok = tone_lut && dens && dev && tmp && blurred;
  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++)
  {
    acc[c] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
    ok = ok && acc[c];
  }
  if(!ok)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto cleanup;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_density, w, h,
      CLARG(dev_in), CLARG(dens), CLARG(w), CLARG(h));
  if(err != CL_SUCCESS) goto cleanup;

  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++)
  {
    for(int sl = 0; sl < FILMGRAIN_SUBLAYERS; sl++)
    {
      const float npart = d->em.npart[sl] * sc.npart_scale;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_deviation, w, h,
          CLARG(dens), CLARG(dev), CLARG(w), CLARG(h), CLARG(roi_x), CLARG(roi_y),
          CLARG(c), CLARG(sl), CLARG(d->em.share[sl]), CLARG(d->em.dmin[sl]),
          CLARG(d->em.dmax[sl]), CLARG(npart), CLARG(d->em.unif));
      if(err != CL_SUCCESS) goto cleanup;

      err = _blur_cl(devid, gd, dev, tmp, w, h, sc.dye_sigma[sl]);
      if(err != CL_SUCCESS) goto cleanup;

      const int reset = sl == 0;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_accumulate, w, h,
          CLARG(acc[c]), CLARG(dev), CLARG(w), CLARG(h), CLARG(reset));
      if(err != CL_SUCCESS) goto cleanup;
    }

    err = _blur_cl(devid, gd, acc[c], tmp, w, h, sc.clump_sigma);
    if(err != CL_SUCCESS) goto cleanup;

    if(sc.usm_sigma > 0.0f)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, acc[c], blurred, 0, 0,
                                                    sizeof(float) * npix);
      if(err != CL_SUCCESS) goto cleanup;
      err = _blur_cl(devid, gd, blurred, tmp, w, h, sc.usm_sigma);
      if(err != CL_SUCCESS) goto cleanup;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_recover, w, h,
          CLARG(acc[c]), CLARG(blurred), CLARG(w), CLARG(h), CLARG(d->p.grain_usm_amount));
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

  float k_lum, k_chroma;
  _spektra_gains(d, &k_lum, &k_chroma);
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_apply,
      roi_out->width, roi_out->height,
      CLARG(dev_in), CLARG(dev_out), CLARG(acc[0]), CLARG(acc[1]), CLARG(acc[2]),
      CLARG(tone_lut), CLARGINT(roi_out->width), CLARGINT(roi_out->height), CLARG(w),
      CLARG(dx), CLARG(dy), CLARG(k_lum), CLARG(k_chroma),
      CLARG(coeffs[0]), CLARG(coeffs[1]), CLARG(coeffs[2]));

cleanup:
  for(int c = 0; c < FILMGRAIN_DYE_LAYERS; c++) dt_opencl_release_mem_object(acc[c]);
  dt_opencl_release_mem_object(tone_lut);
  dt_opencl_release_mem_object(dens);
  dt_opencl_release_mem_object(dev);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blurred);
  return err;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  _simplex_noise_init();

  const int program = 44; // filmgrain.cl in data/kernels/programs.conf
  dt_iop_filmgrain_global_data_t *gd = malloc(sizeof(dt_iop_filmgrain_global_data_t));
  self->data = gd;
  gd->kernel_density = dt_opencl_create_kernel(program, "filmgrain_to_density");
  gd->kernel_deviation = dt_opencl_create_kernel(program, "filmgrain_deviation");
  gd->kernel_accumulate = dt_opencl_create_kernel(program, "filmgrain_accumulate");
  gd->kernel_recover = dt_opencl_create_kernel(program, "filmgrain_recover");
  gd->kernel_apply = dt_opencl_create_kernel(program, "filmgrain_apply");
  gd->kernel_gauss_row_1c = dt_opencl_create_kernel(program, "gauss_row_1c");
  gd->kernel_gauss_col_1c = dt_opencl_create_kernel(program, "gauss_col_1c");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_filmgrain_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_density);
  dt_opencl_free_kernel(gd->kernel_deviation);
  dt_opencl_free_kernel(gd->kernel_accumulate);
  dt_opencl_free_kernel(gd->kernel_recover);
  dt_opencl_free_kernel(gd->kernel_apply);
  dt_opencl_free_kernel(gd->kernel_gauss_row_1c);
  dt_opencl_free_kernel(gd->kernel_gauss_col_1c);
  free(self->data);
  self->data = NULL;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_filmgrain_params_t *const p = (dt_iop_filmgrain_params_t *)p1;
  dt_iop_filmgrain_data_t *const d = piece->data;

  d->p = *p;
  _emulsion_build(&d->em, p);
  _evaluate_grain_lut(d->grain_lut, p->midtones_bias);
  _tone_build_lut(p->tone_curve, d->tone_lut);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_filmgrain_data_t);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

/* ======================================================================
   tone graph
   ====================================================================== */

/* Top of the graph's value axis: 200 %, the parameter's maximum. */
#define FILMGRAIN_GRAPH_MAX 2.0f

/* Height of the tone graph, before DPI scaling: room for the plot plus the
   gradient, EV labels and axis title under it. */
#define FILMGRAIN_GRAPH_HEIGHT 200

static inline float _node_ev(const int node)
{
  return FILMGRAIN_TONE_EV_FIRST + (float)node;
}

static inline float _graph_x_of_ev(const dt_iop_filmgrain_gui_data_t *const g,
                                   const float ev)
{
  const float span = (float)(FILMGRAIN_TONE_NODES - 1);
  return g->graph_x + (ev - FILMGRAIN_TONE_EV_FIRST) / span * g->graph_w;
}

static inline float _graph_y_of_value(const dt_iop_filmgrain_gui_data_t *const g,
                                      const float value)
{
  return g->graph_y + (1.0f - value / FILMGRAIN_GRAPH_MAX) * g->graph_h;
}

static inline float _graph_value_of_y(const dt_iop_filmgrain_gui_data_t *const g,
                                      const float y)
{
  return CLAMPF((1.0f - (y - g->graph_y) / g->graph_h) * FILMGRAIN_GRAPH_MAX,
                0.0f, FILMGRAIN_GRAPH_MAX);
}

/* The node whose column the pointer is in, or -1 outside the plot. Each
   node owns the half spacing either side of it, so the whole width of the
   plot is live and a node never has to be hit exactly. */
static int _graph_node_at(const dt_iop_filmgrain_gui_data_t *const g,
                          const float x,
                          const float y)
{
  if(g->graph_w <= 0.0f || g->graph_h <= 0.0f) return -1;
  const float slack = DT_PIXEL_APPLY_DPI(6);
  if(y < g->graph_y - slack || y > g->graph_y + g->graph_h + slack) return -1;

  const float spacing = g->graph_w / (float)(FILMGRAIN_TONE_NODES - 1);
  const int node = (int)roundf((x - g->graph_x) / spacing);
  return node >= 0 && node < FILMGRAIN_TONE_NODES ? node : -1;
}

/* Draw `text` with its ink box anchored at (x, y): ax and ay pick the
   anchor point inside the box, 0 for left/top, 0.5 for the centre and 1 for
   right/bottom. */
static void _graph_text(cairo_t *cr,
                        PangoLayout *layout,
                        const char *const text,
                        const double x,
                        const double y,
                        const double ax,
                        const double ay)
{
  PangoRectangle ink;
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, x - ax * ink.width - ink.x, y - ay * ink.height - ink.y);
  pango_cairo_show_layout(cr, layout);
}

static void _graph_text_size(PangoLayout *layout,
                             const char *const text,
                             int *const width,
                             int *const height)
{
  PangoRectangle ink;
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  if(width) *width = ink.width;
  if(height) *height = ink.height;
}

static void _ev_label(char *const buf,
                      const size_t size,
                      const int ev)
{
  if(ev == 0)
    snprintf(buf, size, "0");
  else
    snprintf(buf, size, "%+d", ev);
}

/* Paint the whole graph into `cr`, a surface of width x height, and keep
   the plot rectangle in `g` for hit-testing. */
static void _graph_paint(dt_iop_filmgrain_gui_data_t *const g,
                         const dt_iop_filmgrain_params_t *const p,
                         cairo_t *cr,
                         PangoLayout *layout,
                         const int width,
                         const int height)
{
  /* Layout, from the size of the widest labels so nothing is clipped at
     any font size: value labels on the left, a tone gradient, the EV
     labels and the axis title below. */
  const float pad = DT_PIXEL_APPLY_DPI(4);
  int y_label_w, line_h, x_label_w;
  _graph_text_size(layout, "200%", &y_label_w, NULL);
  _graph_text_size(layout, "+5", &x_label_w, &line_h);
  const float bar_h = 0.6f * line_h;

  g->graph_x = y_label_w + 2.0f * pad;
  g->graph_y = 0.5f * line_h + pad;
  g->graph_w = width - g->graph_x - 0.5f * x_label_w - pad;
  g->graph_h = height - g->graph_y - (bar_h + 2.0f * line_h + 4.0f * pad);

  if(g->graph_w < 2.0f * FILMGRAIN_TONE_NODES || g->graph_h < 2.0f * line_h)
  {
    // too small to plot; also turns off hit-testing in _graph_node_at()
    g->graph_w = g->graph_h = 0.0f;
    return;
  }

  const float gx0 = g->graph_x;
  const float gy0 = g->graph_y;
  const float gx1 = gx0 + g->graph_w;
  const float gy1 = gy0 + g->graph_h;

  // plot background
  cairo_rectangle(cr, gx0, gy0, g->graph_w, g->graph_h);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);

  // grid: one column per node, one row per 50 %
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_grid);
  for(int k = 1; k < FILMGRAIN_TONE_NODES - 1; k++)
  {
    const float x = _graph_x_of_ev(g, _node_ev(k));
    cairo_move_to(cr, x, gy0);
    cairo_line_to(cr, x, gy1);
  }
  for(int k = 1; k < 4; k++)
  {
    const float y = _graph_y_of_value(g, 0.5f * k);
    cairo_move_to(cr, gx0, y);
    cairo_line_to(cr, gx1, y);
  }
  cairo_stroke(cr);

  // middle grey column and the 100 % line, where the curve leaves grain as it is
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  set_color(cr, darktable.bauhaus->graph_border);
  const float x_grey = _graph_x_of_ev(g, 0.0f);
  cairo_move_to(cr, x_grey, gy0);
  cairo_line_to(cr, x_grey, gy1);
  cairo_stroke(cr);
  set_color(cr, darktable.bauhaus->graph_fg);
  const float y_unity = _graph_y_of_value(g, 1.0f);
  cairo_move_to(cr, gx0, y_unity);
  cairo_line_to(cr, gx1, y_unity);
  cairo_stroke(cr);

  // frame
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, gx0, gy0, g->graph_w, g->graph_h);
  cairo_stroke(cr);

  // node bars, from the 100 % line to each node
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
  set_color(cr, darktable.bauhaus->color_fill);
  for(int k = 0; k < FILMGRAIN_TONE_NODES; k++)
  {
    const float x = _graph_x_of_ev(g, _node_ev(k));
    cairo_move_to(cr, x, y_unity);
    cairo_line_to(cr, x, _graph_y_of_value(g, p->tone_curve[k]));
  }
  cairo_stroke(cr);

  // the curve, drawn from the same interpolation the pipe tabulates
  float m[FILMGRAIN_TONE_NODES];
  _tone_tangents(p->tone_curve, m);
  cairo_save(cr);
  cairo_rectangle(cr, gx0, gy0, g->graph_w, g->graph_h);
  cairo_clip(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));
  set_color(cr, darktable.bauhaus->graph_fg);
  const int samples = MAX((int)g->graph_w, 2);
  for(int i = 0; i <= samples; i++)
  {
    const float x = gx0 + g->graph_w * (float)i / (float)samples;
    const float ev = FILMGRAIN_TONE_EV_FIRST
                     + (float)(FILMGRAIN_TONE_NODES - 1) * (float)i / (float)samples;
    const float y = _graph_y_of_value(g, _tone_eval(p->tone_curve, m, ev));
    if(i == 0)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);
  cairo_restore(cr);

  // nodes
  for(int k = 0; k < FILMGRAIN_TONE_NODES; k++)
  {
    const float x = _graph_x_of_ev(g, _node_ev(k));
    const float y = _graph_y_of_value(g, p->tone_curve[k]);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x, y, DT_PIXEL_APPLY_DPI(4), 0, 2.0 * M_PI);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);
    set_color(cr, k == g->active_node ? darktable.bauhaus->graph_fg
                                      : darktable.bauhaus->graph_bg);
    cairo_fill(cr);
  }

  char text[64];
  set_color(cr, darktable.bauhaus->graph_fg);

  // value readout for the node under the pointer, away from that node
  if(g->active_node >= 0)
  {
    const int ev = (int)_node_ev(g->active_node);
    char evs[16];
    _ev_label(evs, sizeof(evs), ev);
    snprintf(text, sizeof(text), _("%s EV: %.0f%%"), evs,
             100.0f * p->tone_curve[g->active_node]);
    const gboolean right = g->active_node < FILMGRAIN_TONE_NODES / 2;
    _graph_text(cr, layout, text, right ? gx1 - pad : gx0 + pad, gy0 + pad,
                right ? 1.0 : 0.0, 0.0);
  }

  // value axis labels
  for(int k = 0; k <= 4; k++)
  {
    snprintf(text, sizeof(text), "%d%%", 50 * k);
    _graph_text(cr, layout, text, gx0 - pad, _graph_y_of_value(g, 0.5f * k), 1.0, 0.5);
  }

  // tone gradient under the plot, dark to bright like the EV axis
  const float bar_y = gy1 + pad;
  cairo_pattern_t *grad = cairo_pattern_create_linear(gx0, 0.0, gx1, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_rectangle(cr, gx0, bar_y, g->graph_w, bar_h);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // EV labels, thinned to every other one when they would touch; the
  // middle grey label is always kept
  set_color(cr, darktable.bauhaus->graph_fg);
  const float spacing = g->graph_w / (float)(FILMGRAIN_TONE_NODES - 1);
  const int step = spacing < x_label_w + pad ? 2 : 1;
  const int grey_node = (int)-FILMGRAIN_TONE_EV_FIRST;
  const float label_y = bar_y + bar_h + pad;
  for(int k = 0; k < FILMGRAIN_TONE_NODES; k++)
  {
    if((k - grey_node) % step) continue;
    _ev_label(text, sizeof(text), (int)_node_ev(k));
    _graph_text(cr, layout, text, _graph_x_of_ev(g, _node_ev(k)), label_y, 0.5, 0.0);
  }

  // axis title
  _graph_text(cr, layout, _("EV relative to middle gray"), gx0 + 0.5f * g->graph_w,
              label_y + line_h + pad, 0.5, 0.0);
}

static gboolean _area_draw(GtkWidget *widget,
                           cairo_t *crf,
                           dt_iop_module_t *self)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  const dt_iop_filmgrain_params_t *p = self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;
  if(width <= 0 || height <= 0) return FALSE;

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  PangoFontDescription *desc = dt_gui_get_font();
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0, width, height);

  _graph_paint(g, p, cr, layout, width, height);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return FALSE;
}

/* Show a node's value on its slider without the slider writing it back as
   a second history item. */
static void _node_slider_sync(dt_iop_filmgrain_gui_data_t *const g,
                              const dt_iop_filmgrain_params_t *const p,
                              const int node)
{
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->node[node], p->tone_curve[node]);
  DT_LEAVE_GUI_UPDATE();
}

/* Graph edits go through here: snap to the sliders' whole-percent steps so
   graph and slider always show the same value, set the node, move its
   slider along, redraw and record the change. */
static void _graph_set_node(dt_iop_module_t *self,
                            const int node,
                            const float value)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  dt_iop_filmgrain_params_t *p = self->params;
  const float v = CLAMPF(roundf(value * 100.0f) / 100.0f, 0.0f, FILMGRAIN_GRAPH_MAX);
  if(p->tone_curve[node] == v) return;
  p->tone_curve[node] = v;
  _node_slider_sync(g, p, node);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _area_motion(GtkEventControllerMotion *controller,
                         gdouble x,
                         gdouble y,
                         dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;

  if(g->dragging && g->active_node >= 0)
  {
    _graph_set_node(self, g->active_node, _graph_value_of_y(g, y));
    return;
  }

  const int node = _graph_node_at(g, x, y);
  if(node != g->active_node)
  {
    g->active_node = node;
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

static void _area_leave(GtkEventControllerMotion *controller,
                        dt_iop_module_t *self)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  if(g->dragging) return;
  g->active_node = -1;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void _area_press(GtkGestureSingle *gesture,
                        gint n_press,
                        gdouble x,
                        gdouble y,
                        dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  dt_iop_filmgrain_params_t *p = self->params;
  const dt_iop_filmgrain_params_t *const d = self->default_params;
  const guint button = gtk_gesture_single_get_current_button(gesture);

  dt_iop_request_focus(self);

  if(button == GDK_BUTTON_PRIMARY && n_press == 2)
  {
    g->dragging = FALSE;
    memcpy(p->tone_curve, d->tone_curve, sizeof(p->tone_curve));
    for(int k = 0; k < FILMGRAIN_TONE_NODES; k++) _node_slider_sync(g, p, k);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  const int node = _graph_node_at(g, x, y);
  if(node < 0) return;

  if(button == GDK_BUTTON_SECONDARY)
  {
    _graph_set_node(self, node, d->tone_curve[node]);
  }
  else if(button == GDK_BUTTON_PRIMARY)
  {
    /* The node follows the pointer's height once it moves, so a click
       alone never changes the curve. */
    g->active_node = node;
    g->dragging = TRUE;
  }
}

static void _area_release(GtkGestureSingle *gesture,
                          gint n_press,
                          gdouble x,
                          gdouble y,
                          dt_iop_module_t *self)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  if(!g->dragging) return;
  g->dragging = FALSE;
  g->active_node = _graph_node_at(g, x, y);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void _area_scroll(GtkEventControllerScroll *controller,
                         gdouble dx,
                         gdouble dy,
                         dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  const dt_iop_filmgrain_params_t *p = self->params;

  if(g->dragging || g->active_node < 0 || dy == 0.0) return;

  dt_iop_request_focus(self);
  const float step
      = 0.05f * dt_accel_get_speed_multiplier(dt_gui_get_widget(controller), dt_key_modifier_state());
  _graph_set_node(self, g->active_node, p->tone_curve[g->active_node] - step * dy);
}

/* dt_gui_new_collapsible_section() packs a section at the end of its
   parent, which pins it to the bottom of a notebook page that is taller
   than its contents. Packing it at the start instead keeps it directly
   under the widgets above it. Returns the section's container for its
   contents. */
/* Reset the widgets a section holds, leaving the rest of the module alone.
   darktable resets whole modules or single widgets and nothing in between.

   Not wrapped in darktable.gui->reset: each widget's own value-changed
   handler is what writes the param, so suppressing it would move the
   sliders without changing the render. The undo record around the loop is
   what makes the click one step; opening it also clears the stored undo
   target, so resetting a slider just dragged is not taken for a
   continuation of that drag. Toggles go last, as _reset_all_bauhaus() in
   gui/gtk.c does. */
static void _section_reset_clicked(GtkButton *button,
                                   dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  GtkWidget *box = g_object_get_data(G_OBJECT(button), "filmgrain_section");
  if(!box) return;

  dt_dev_undo_start_record(darktable.develop);

  for(int toggles_pass = 0; toggles_pass < 2; toggles_pass++)
    for(GList *c = gtk_container_get_children(GTK_CONTAINER(box));
        c;
        c = g_list_delete_link(c, c))
    {
      if(DT_IS_BAUHAUS_WIDGET(c->data)
         && (dt_bauhaus_widget_get_type(c->data) == DT_BAUHAUS_TOGGLE) == (toggles_pass == 1))
        dt_bauhaus_widget_reset(GTK_WIDGET(c->data));
    }

  dt_dev_undo_end_record(darktable.develop);
}

static GtkWidget *_new_top_section(dt_iop_module_t *self,
                                   dt_gui_collapsible_section_t *const cs,
                                   const char *const confname,
                                   const char *const label,
                                   GtkWidget *const parent)
{
  dt_gui_new_collapsible_section(cs, confname, label, GTK_BOX(parent), DT_ACTION(self));
  gtk_container_child_set(GTK_CONTAINER(parent), cs->expander,
                          "pack-type", GTK_PACK_START, NULL);

  GtkWidget *hdr = dtgtk_expander_get_header(DTGTK_EXPANDER(cs->expander));
  GtkWidget *btn = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
  gtk_widget_set_tooltip_text(btn, _("reset only this section"));
  g_object_set_data(G_OBJECT(btn), "filmgrain_section", cs->container);
  g_signal_connect(G_OBJECT(btn), "clicked", G_CALLBACK(_section_reset_clicked), self);
  dt_gui_box_add(hdr, btn);

  return GTK_WIDGET(cs->container);
}

/* Each method only shows the controls it reads. Strength and the tone
   curve drive both. The node sliders write the curve through their own
   params binding, so every change also redraws the graph. */
void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  const dt_iop_filmgrain_params_t *p = self->params;
  const gboolean spektra = p->method == DT_FILMGRAIN_METHOD_SPEKTRAFILM;

  gtk_widget_set_visible(g->scale, !spektra);
  gtk_widget_set_visible(g->midtones_bias, !spektra);

  gtk_widget_set_visible(g->grain_size, spektra);
  gtk_widget_set_visible(g->chroma, spektra);
  gtk_widget_set_visible(g->sublayer_scale, spektra);
  // hiding the expander takes the sliders inside it along
  gtk_widget_set_visible(g->texture.expander, spektra);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_filmgrain_gui_data_t *g = self->gui_data;
  g->active_node = -1;
  g->dragging = FALSE;
  dt_gui_update_collapsible_section(&g->node_sliders);
  dt_gui_update_collapsible_section(&g->texture);
  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_filmgrain_gui_data_t *g = IOP_GUI_ALLOC(filmgrain);

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // first tab: how the grain is made
  GtkWidget *const grain_page
      = dt_ui_notebook_page(g->notebook, N_("grain"), _("grain generation and texture"));
  self->widget = grain_page;

  g->method = dt_bauhaus_combobox_from_params(self, "method");
  gtk_widget_set_tooltip_text(g->method,
    _("how the grain is generated.\n"
      "\n"
      "simplex: layered noise shaped by a photographic paper response.\n"
      "\n"
      "spektrafilm: the emulsion crystal model of the spektrafilm module.\n"
      "each channel is grained as its own dye layer, and the grain follows\n"
      "from the crystal size and the output size the way it does on film."));

  /* Built in reading order; gui_changed() hides whatever the current
     method does not use. */
  g->scale = dt_bauhaus_slider_from_params(self, "scale");
  dt_bauhaus_slider_set_factor(g->scale, FILMGRAIN_SCALE_FACTOR);
  dt_bauhaus_slider_set_digits(g->scale, 0);
  dt_bauhaus_slider_set_format(g->scale, _(" ISO"));
  gtk_widget_set_tooltip_text(g->scale, _("the grain size (~ISO of the film)"));

  g->grain_size = dt_bauhaus_slider_from_params(self, "grain_size");
  dt_bauhaus_slider_set_soft_range(g->grain_size, 0.25f, 2.5f);
  gtk_widget_set_tooltip_text(g->grain_size,
    _("how coarse the film's crystals are. larger crystals mean fewer of\n"
      "them in each pixel, so the grain gets stronger as well as coarser,\n"
      "the way a fast film differs from a slow one.\n"
      "\n"
      "the size is relative to the frame, not the pixel, so a larger\n"
      "export shows finer grain relative to the picture, as a larger\n"
      "print from the same negative would."));

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("the strength of applied grain"));

  g->chroma = dt_bauhaus_slider_from_params(self, "chroma");
  gtk_widget_set_tooltip_text(g->chroma,
    _("how much color the grain carries. each channel is grained as a\n"
      "separate dye layer, and where the layers disagree the grain is\n"
      "colored. at 1 there is as much color grain as luminance grain.\n"
      "\n"
      "set it to 0 for the neutral grain of a black & white film. the\n"
      "luminance grain stays the same either way."));

  g->sublayer_scale = dt_bauhaus_slider_from_params(self, "sublayer_scale");
  gtk_widget_set_tooltip_text(g->sublayer_scale,
    _("a real emulsion layers coarse crystals over finer ones. this scales\n"
      "the finer layers against the coarsest, which stays fixed. lower\n"
      "values leave the coarse layer to dominate; at 0 only it is left."));

  g->midtones_bias = dt_bauhaus_slider_from_params(self, "midtones_bias");
  dt_bauhaus_slider_set_format(g->midtones_bias, "%");
  gtk_widget_set_tooltip_text(g->midtones_bias,
    _("amount of mid-tones bias from the photographic paper response\n"
      "modeling. the greater the bias, the more pronounced the fall off of\n"
      "the grain in shadows and highlights"));

  /* The texture section's sliders are built with self->widget pointing
     into the section. */
  self->widget = _new_top_section(self, &g->texture, "plugins/darkroom/filmgrain/expand_texture",
                                  C_("section", "texture"), grain_page);

  g->grain_blur = dt_bauhaus_slider_from_params(self, "grain_blur");
  gtk_widget_set_tooltip_text(g->grain_blur,
    _("how far the developed crystals clump together. this is what turns\n"
      "per-pixel noise into grain, so very low values stop it looking like\n"
      "film. it costs grain contrast, which the recovery below returns."));

  g->grain_usm_sigma = dt_bauhaus_slider_from_params(self, "grain_usm_sigma");
  gtk_widget_set_tooltip_text(g->grain_usm_sigma,
    _("radius of the acutance recovery that follows the grain blur. the two\n"
      "are a tuned pair: recovery without blur over-defines the grain."));

  g->grain_usm_amount = dt_bauhaus_slider_from_params(self, "grain_usm_amount");
  gtk_widget_set_tooltip_text(g->grain_usm_amount,
    _("how much of the contrast the grain blur removed is put back. at 0\n"
      "the grain keeps the softness the blur gave it."));

  // second tab: how much of it each tone gets
  GtkWidget *const tone_page
      = dt_ui_notebook_page(g->notebook, N_("tonal range"), _("grain per tone"));
  self->widget = tone_page;
  g->active_node = -1;
  g->area = GTK_DRAWING_AREA(
      dtgtk_drawing_area_new_with_height((int)DT_PIXEL_APPLY_DPI(FILMGRAIN_GRAPH_HEIGHT)));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("tone graph"), GTK_WIDGET(g->area), NULL);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area),
    _("how much grain each tone gets, from the shadows on the left to the\n"
      "highlights on the right. 100% leaves the grain as it is.\n"
      "\n"
      "drag a node up or down to change it, or scroll over it for fine steps.\n"
      "right-click a node to reset it, double-click to reset the curve."));
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_area_draw), self);
  dt_gui_connect_click(g->area, _area_press, _area_release, self);
  dt_gui_connect_motion(g->area, _area_motion, NULL, _area_leave, self);
  dt_gui_connect_scroll(g->area, GTK_EVENT_CONTROLLER_SCROLL_VERTICAL
                                 | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE,
                        _area_scroll, self);
  dt_gui_box_add(self->widget, g->area);

  /* One slider per node, shadows first as on the graph, in a section of
     their own so the graph can be used alone. Labels are fixed strings so
     they can serve as shortcut names. */
  self->widget = _new_top_section(self, &g->node_sliders,
                                  "plugins/darkroom/filmgrain/expand_tone_sliders",
                                  C_("section", "grain per EV"), tone_page);
  static const char *const node_labels[FILMGRAIN_TONE_NODES]
      = { N_("-5 EV"), N_("-4 EV"), N_("-3 EV"), N_("-2 EV"), N_("-1 EV"), N_("0 EV"),
          N_("+1 EV"), N_("+2 EV"), N_("+3 EV"), N_("+4 EV"), N_("+5 EV") };
  for(int k = 0; k < FILMGRAIN_TONE_NODES; k++)
  {
    char param[32];
    snprintf(param, sizeof(param), "tone_curve[%d]", k);
    g->node[k] = dt_bauhaus_slider_from_params(self, param);
    dt_bauhaus_widget_set_label(g->node[k], NULL, node_labels[k]);
    /* "%" on a slider whose range stays within 10 scales the display by
       100 and drops two digits, giving whole-percent steps; setting a
       factor or digits as well would be applied twice. */
    dt_bauhaus_slider_set_format(g->node[k], "%");
    gtk_widget_set_tooltip_text(g->node[k],
                                _("how much grain pixels at this exposure get.\n"
                                  "100% leaves the grain as it is."));
  }

  // the module's widget is the notebook holding both tabs
  self->widget = GTK_WIDGET(g->notebook);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
