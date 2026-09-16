/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/math.h"
/* sf_grain_delta_ml draws the grain for the particle method; it is the same
   entry point spektrafilm uses, called here with mono set. sf_grain_layers_t
   is the emulsion it draws from. Both pull in data/kernels/grain.h,
   so iop/grain.c carries SPEKTRA_FP_CONTRACT_FLAGS in src/CMakeLists.txt. */
#include "common/spektra_core.h"
#include "common/spektra_sim.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "common/gaussian.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#define GRAIN_LIGHTNESS_STRENGTH_SCALE 0.15f

// (m_pi/2)/4 = half hue colorspan
#define GRAIN_HUE_COLORRANGE 0.392699082

#define GRAIN_HUE_STRENGTH_SCALE 0.25
#define GRAIN_SATURATION_STRENGTH_SCALE 0.25
#define GRAIN_RGB_STRENGTH_SCALE 0.25

#define GRAIN_SCALE_FACTOR 213.2

/* ---- particle method --------------------------------------------------

   The controls mirror the emulsion section of spektrafilm's grain tab and
   move the grain in the same directions. They differ in what 1.0 means:
   there each slider scales a figure measured for the loaded film stock,
   while this module has no stock and no pack to load one from, so 1.0 is
   the reference emulsion defined by the four constants below.

   All four are tuning constants that place the defaults; none is measured
   against film scans. */

/* Maximum density of the modelled emulsion. Not exposed: it only fixes the
   scale that grain strength then works against. */
#define GRAIN_PARTICLE_DMAX 2.0f

/* Area, in um^2, of one developed crystal at grain size 1.0, measured
   against GRAIN_PARTICLE_REF_MM below. Places the default at ~110 particles
   per pixel on a 6000px long edge. */
#define GRAIN_PARTICLE_CLUMP_UM2 0.32f

/* Long edge of the frame the crystal area above is stated for. With the
   output resolution it gives the physical size of a pixel, which is what
   sets how many crystals a pixel averages over -- and therefore why grain
   gets finer, relative to the frame, as the export grows.

   Fixed rather than exposed. The count works out to
   (REF_MM*1000)^2 / (long_edge * size * particle_scale)^2 / CLUMP_UM2, so
   the frame size and the grain size enter only as a ratio: a slider for
   each would be two controls over one degree of freedom, and every setting
   reachable with both is reachable with grain size alone. */
#define GRAIN_PARTICLE_REF_MM 36.0f

/* Density floor per crystal at density floor 1.0. spektrafilm notes real
   stocks measure 0.03 to 0.06. */
#define GRAIN_PARTICLE_DMIN 0.04f

/* Crystal distribution uniformity at uniformity 1.0. Below 1 by necessity:
   grain_layer_particle computes a saturation term 1 - p*unif, which goes
   negative for unif >= 1 in the densest areas. spektrafilm's slider reaches
   1.03 because it scales a stock figure that is itself below 1. */
#define GRAIN_PARTICLE_UNIF 0.9f

/* A real emulsion layers coarse crystals over finer ones. spektrafilm reads
   the layer count and each layer's particle scale from the stock's own
   fitted density curves; with no stock to read, this is a fixed ladder of
   three, the coarsest held constant and the finer two following the slider.
   The structure is synthetic: the slider shifts noise between coarse and
   fine as spektrafilm's does, but it describes no real film. Must not exceed
   SF_GRAIN_MAX_SUBLAYERS. */
#define GRAIN_PARTICLE_SUBLAYERS 3

/* ---- texture stage ----------------------------------------------------
   A per-pixel draw has no spatial correlation: neighbouring pixels are
   independent, which is white noise, not grain. Film crystals clump, and
   the blur below is what produces that clumping -- measured lag-1
   autocorrelation of the raw draw is 0.00 and rises to 0.67 once the clump
   blur runs. The stage is therefore not optional polish; without it the
   particle method does not read as grain at all.

   Constants match spektrafilm's. The clump blur is a FIXED pixel sigma,
   independent of pixel_um: physical scaling lives in the particle density,
   not in this smoothing pass. */
#define GRAIN_BLUR_FACTOR 0.8f
#define GRAIN_BLUR_MIN 0.05f
/* Dye cloud reference spread in um, scaled per sub-layer by the square root
   of that sub-layer's per-particle optical density. */
#define GRAIN_DYE_BLUR_UM 2.0f
/* Gaussian support used when padding the ROI for the blurs above. */
#define GRAIN_HALO_SIGMAS 3.0f

/* Each dye layer is drawn independently, and the three fields are combined on
   CIE Lab's opponent axes: their mean carries lightness, their differences
   carry a and b. That is what makes the grain coloured -- three layers that
   grain in step would only move L.

   The two norms keep the result comparable to a single field of standard
   deviation s. The sum of three independent fields has deviation s*sqrt(3),
   so 1/sqrt(3) returns lightness grain to s and leaves it unchanged by the
   chroma control. A difference of two has deviation s*sqrt(2), so 1/sqrt(2)
   puts a and b at s as well, which makes chroma 1.0 mean "as much colour
   grain as lightness grain". */
#define GRAIN_LUMA_NORM 0.57735027f   /* 1/sqrt(3) */
#define GRAIN_CHROMA_NORM 0.70710678f /* 1/sqrt(2) */
#define GRAIN_DYE_LAYERS 3

/* The strength slider is shared with the simplex method, which reaches a
   usable amount of grain far lower on the scale. A draw whose deviation is
   fixed by the crystal statistics needs this much gain on top to land in the
   same range, so that a given slider position means roughly the same amount
   of grain whichever method is selected. Purely a scale on the delta: it
   changes no distribution and leaves strength 0 an exact identity. */
#define GRAIN_PARTICLE_STRENGTH_GAIN 4.0f

#define GRAIN_LUT_SIZE 128
#define GRAIN_LUT_DELTA_MAX 2.0
#define GRAIN_LUT_DELTA_MIN 0.0001
#define GRAIN_LUT_PAPER_GAMMA 1.0

DT_MODULE_INTROSPECTION(3, dt_iop_grain_params_t)


typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE = 0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
} _dt_iop_grain_channel_t;

typedef enum dt_iop_grain_method_t
{
  /* Three octaves of simplex noise, tuned to the power spectrum of real
     grain scans, reshaped by a photographic paper response curve. The only
     method before v3, and still the default. */
  DT_GRAIN_METHOD_SIMPLEX = 0,  // $DESCRIPTION: "simplex"
  /* Per-pixel draw from spektrafilm's emulsion particle model: the pixel's
     lightness is read as a film density, the developed density is redrawn
     as a Poisson process over the grains the pixel covers, and the
     difference is the grain. Grain amplitude then follows from physical
     grain size and frame size rather than from a noise amplitude. */
  DT_GRAIN_METHOD_PARTICLE = 1, // $DESCRIPTION: "particle"
} dt_iop_grain_method_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel; // $DEFAULT: DT_GRAIN_CHANNEL_LIGHTNESS
  float scale;                     /* $MIN: 20.0/GRAIN_SCALE_FACTOR
                                      $MAX: 6400.0/GRAIN_SCALE_FACTOR
                                      $DEFAULT: 1600.0/GRAIN_SCALE_FACTOR
                                      $DESCRIPTION: "coarseness" */
  float strength;      // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0
  float midtones_bias; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "mid-tones bias"
  dt_iop_grain_method_t method; // $DEFAULT: DT_GRAIN_METHOD_SIMPLEX $DESCRIPTION: "method"
  /* All of the following are particle-method only, and carry the same
     ranges and defaults as the matching sliders in spektrafilm's grain tab
     so a look can be moved between the two modules. */
  float grain_size;      // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "grain size"
  float uniformity;      // $MIN: 0.5 $MAX: 1.03 $DEFAULT: 1.0 $DESCRIPTION: "uniformity"
  float sublayer_scale;  // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "sublayer particle scale"
  float density_min;     // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "density floor"
  float grain_blur;      // $MIN: 0.2 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "grain blur"
  float grain_dye_cloud; // $MIN: 0.0 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "dye cloud size"
  float grain_usm_sigma; // $MIN: 0.0 $MAX: 3.0 $DEFAULT: 0.7 $DESCRIPTION: "recovery radius"
  float grain_usm_amount;// $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.5 $DESCRIPTION: "recovery strength"
  float chroma;          // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "chroma"
} dt_iop_grain_params_t;

typedef struct dt_iop_grain_gui_data_t
{
  GtkWidget *scale, *strength, *midtones_bias;
  GtkWidget *method, *grain_size;
  GtkWidget *uniformity, *sublayer_scale, *density_min, *chroma;
  dt_gui_collapsible_section_t texture;
  GtkWidget *grain_blur, *grain_dye_cloud, *grain_usm_sigma, *grain_usm_amount;
} dt_iop_grain_gui_data_t;

typedef struct dt_iop_grain_data_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
  float midtones_bias;
  dt_iop_grain_method_t method;
  float grain_size;
  float uniformity;
  float sublayer_scale;
  float density_min;
  float grain_blur;
  float grain_dye_cloud;
  float grain_usm_sigma;
  float grain_usm_amount;
  float chroma;
  float grain_lut[GRAIN_LUT_SIZE * GRAIN_LUT_SIZE];

  /* The emulsion handed to sf_grain_delta_ml. Its two curve members are
     pointers into the tables below, which live here so they outlive
     commit_params and stay put for the pipe's lifetime. */
  sf_grain_layers_t layers;
  float layer_curve[SF_NLE][SF_GRAIN_MAX_SUBLAYERS][3];
  float layer_curve_total[SF_NLE][3];
  float dmin_c[3];
  float unif_c[3];
} dt_iop_grain_data_t;

typedef struct dt_iop_grain_global_data_t
{
  int kernel_grain_gen_deviation;
  int kernel_grain_accumulate;
  int kernel_gauss_row_1c;
  int kernel_gauss_col_1c;
  int kernel_grain_recover;
  int kernel_grain_combine;
} dt_iop_grain_global_data_t;


int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_grain_params_v1_t
  {
    _dt_iop_grain_channel_t channel;
    float scale;
    float strength;
  } dt_iop_grain_params_v1_t;

  typedef struct dt_iop_grain_params_v2_t
  {
    _dt_iop_grain_channel_t channel;
    float scale;
    float strength;
    float midtones_bias;
  } dt_iop_grain_params_v2_t;

  typedef struct dt_iop_grain_params_v3_t
  {
    _dt_iop_grain_channel_t channel;
    float scale;
    float strength;
    float midtones_bias;
    dt_iop_grain_method_t method;
    float grain_size;
    float uniformity;
    float sublayer_scale;
    float density_min;
    float grain_blur;
    float grain_dye_cloud;
    float grain_usm_sigma;
    float grain_usm_amount;
    float chroma;
  } dt_iop_grain_params_v3_t;

  if(old_version != 1 && old_version != 2) return 1;

  dt_iop_grain_params_v2_t v2;
  if(old_version == 1)
  {
    const dt_iop_grain_params_v1_t *o = old_params;
    v2.channel = o->channel;
    v2.scale = o->scale;
    v2.strength = o->strength;
    v2.midtones_bias = 0.0f; // it produces the same results as the old version
  }
  else
    v2 = *(const dt_iop_grain_params_v2_t *)old_params;

  dt_iop_grain_params_v3_t *n = malloc(sizeof(dt_iop_grain_params_v3_t));

  n->channel = v2.channel;
  n->scale = v2.scale;
  n->strength = v2.strength;
  n->midtones_bias = v2.midtones_bias;
  /* Pre-v3 edits were rendered with the simplex generator and must keep
     that appearance, so the method is pinned here instead of inheriting the
     struct default. The particle method is opt-in. */
  n->method = DT_GRAIN_METHOD_SIMPLEX;
  /* Unused while the method is simplex; these match the introspection
     defaults so switching methods starts from a sane place. */
  n->grain_size = 1.0f;
  n->uniformity = 1.0f;
  n->sublayer_scale = 1.0f;
  n->density_min = 1.0f;
  n->grain_blur = 1.0f;
  n->grain_dye_cloud = 1.0f;
  n->grain_usm_sigma = 0.7f;
  n->grain_usm_amount = 1.5f;
  n->chroma = 1.0f;

  *new_params = n;
  *new_params_size = sizeof(dt_iop_grain_params_v3_t);
  *new_version = 3;
  return 0;
}


static const double grad3[12][3]
  = { { 1, 1, 0 },
      { -1, 1, 0 },
      { 1, -1, 0 },
      { -1, -1, 0 },
      { 1, 0, 1 },
      { -1, 0, 1 },
      { 1, 0, -1 },
      { -1, 0, -1 },
      { 0, 1, 1 },
      { 0, -1, 1 },
      { 0, 1, -1 },
      { 0, -1, -1 } };

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

static size_t perm[512];	// permutation lookup table
static size_t perm_mod[512];	// same as above, but all values mod 12 for selection from grad3

static void _simplex_noise_init()
{
  for(int i = 0; i < 512; i++)
  {
    perm[i] = permutation[i & 255];
    perm_mod[i] = perm[i] % 12;
  }
}
static double dot(const double g[], const double x, const double y, const double z)
{
  return g[0] * x + g[1] * y + g[2] * z;
}

#define FASTFLOOR(x) (x > 0 ? (int)(x) : (int)(x)-1)

static double _simplex_noise(double xin, double yin, double zin)
{
  double n0, n1, n2, n3; // Noise contributions from the four corners
                         // Skew the input space to determine which simplex cell we're in
  const double F3 = 1.0 / 3.0;
  const double s = (xin + yin + zin) * F3; // Very nice and simple skew factor for 3D
  const int i = FASTFLOOR(xin + s);
  const int j = FASTFLOOR(yin + s);
  const int k = FASTFLOOR(zin + s);
  const double G3 = 1.0 / 6.0; // Very nice and simple unskew factor, too
  const double t = (i + j + k) * G3;
  const double X0 = i - t; // Unskew the cell origin back to (x,y,z) space
  const double Y0 = j - t;
  const double Z0 = k - t;
  const double x0 = xin - X0; // The x,y,z distances from the cell origin
  const double y0 = yin - Y0;
  const double z0 = zin - Z0;
  // For the 3D case, the simplex shape is a slightly irregular tetrahedron.
  // Determine which simplex we are in.
  int i1, j1, k1; // Offsets for second corner of simplex in (i,j,k) coords
  int i2, j2, k2; // Offsets for third corner of simplex in (i,j,k) coords
  if(x0 >= y0)
  {
    if(y0 >= z0)
    {
      i1 = 1; // X Y Z order
      j1 = 0;
      k1 = 0;
      i2 = 1;
      j2 = 1;
      k2 = 0;
    }
    else if(x0 >= z0)
    {
      i1 = 1; // X Z Y order
      j1 = 0;
      k1 = 0;
      i2 = 1;
      j2 = 0;
      k2 = 1;
    }
    else
    {
      i1 = 0; // Z X Y order
      j1 = 0;
      k1 = 1;
      i2 = 1;
      j2 = 0;
      k2 = 1;
    }
  }
  else // x0<y0
  {
    if(y0 < z0)
    {
      i1 = 0; // Z Y X order
      j1 = 0;
      k1 = 1;
      i2 = 0;
      j2 = 1;
      k2 = 1;
    }
    else if(x0 < z0)
    {
      i1 = 0; // Y Z X order
      j1 = 1;
      k1 = 0;
      i2 = 0;
      j2 = 1;
      k2 = 1;
    }
    else
    {
      i1 = 0; // Y X Z order
      j1 = 1;
      k1 = 0;
      i2 = 1;
      j2 = 1;
      k2 = 0;
    }
  }
  //  A step of (1,0,0) in (i,j,k) means a step of (1-c,-c,-c) in (x,y,z),
  //  a step of (0,1,0) in (i,j,k) means a step of (-c,1-c,-c) in (x,y,z), and
  //  a step of (0,0,1) in (i,j,k) means a step of (-c,-c,1-c) in (x,y,z), where
  //  c = 1/6.
  const double x1 = x0 - i1 + G3; // Offsets for second corner in (x,y,z) coords
  const double y1 = y0 - j1 + G3;
  const double z1 = z0 - k1 + G3;
  const double x2 = x0 - i2 + 2.0 * G3; // Offsets for third corner in (x,y,z) coords
  const double y2 = y0 - j2 + 2.0 * G3;
  const double z2 = z0 - k2 + 2.0 * G3;
  const double x3 = x0 - 1.0 + 3.0 * G3; // Offsets for last corner in (x,y,z) coords
  const double y3 = y0 - 1.0 + 3.0 * G3;
  const double z3 = z0 - 1.0 + 3.0 * G3;
  // Work out the hashed gradient indices of the four simplex corners
  const size_t ii = i & 255;
  const size_t jj = j & 255;
  const size_t kk = k & 255;
  const size_t gi0 = perm_mod[ii + perm[jj + perm[kk]]];
  const size_t gi1 = perm_mod[ii + i1 + perm[jj + j1 + perm[kk + k1]]];
  const size_t gi2 = perm_mod[ii + i2 + perm[jj + j2 + perm[kk + k2]]];
  const size_t gi3 = perm_mod[ii + 1 + perm[jj + 1 + perm[kk + 1]]];
  // Calculate the contribution from the four corners
  double t0 = 0.6 - x0 * x0 - y0 * y0 - z0 * z0;
  if(t0 < 0)
    n0 = 0.0;
  else
  {
    t0 *= t0;
    n0 = t0 * t0 * dot(grad3[gi0], x0, y0, z0);
  }
  double t1 = 0.6 - x1 * x1 - y1 * y1 - z1 * z1;
  if(t1 < 0)
    n1 = 0.0;
  else
  {
    t1 *= t1;
    n1 = t1 * t1 * dot(grad3[gi1], x1, y1, z1);
  }
  double t2 = 0.6 - x2 * x2 - y2 * y2 - z2 * z2;
  if(t2 < 0)
    n2 = 0.0;
  else
  {
    t2 *= t2;
    n2 = t2 * t2 * dot(grad3[gi2], x2, y2, z2);
  }
  double t3 = 0.6 - x3 * x3 - y3 * y3 - z3 * z3;
  if(t3 < 0)
    n3 = 0.0;
  else
  {
    t3 *= t3;
    n3 = t3 * t3 * dot(grad3[gi3], x3, y3, z3);
  }
  // Add contributions from each corner to get the final noise value.
  // The result is scaled to stay just inside [-1,1]
  return 32.0 * (n0 + n1 + n2 + n3);
}

static double _simplex_2d_noise(double x, double y, double z)
{
  double total = 0;

  // parametrization of octaves to match power spectrum of real grain scans
  static const double f[] = {0.4910, 0.9441, 1.7280};
  static const double a[] = {0.2340, 0.7850, 1.2150};

  for(uint32_t octave = 0; octave < 3; octave++)
  {
    total += (_simplex_noise(x * f[octave] / z, y * f[octave] / z, octave) * a[octave]);
  }
  return total;
}

static float paper_resp(float exposure, float mb, float gp)
{
  const float delta = GRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(GRAIN_LUT_DELTA_MIN));
  const float density = (1.0f + 2.0f * delta) / (1.0f + expf( (4.0f * gp * (0.5f - exposure)) / (1.0f + 2.0f * delta) )) - delta;
  return density;
}

static float paper_resp_inverse(float density, float mb, float gp)
{
  const float delta = GRAIN_LUT_DELTA_MAX * expf((mb / 100.0f) * logf(GRAIN_LUT_DELTA_MIN));
  const float exposure = -logf((1.0f + 2.0f * delta) / (density + delta) - 1.0f) * (1.0f + 2.0f * delta) / (4.0f * gp) + 0.5f;
  return exposure;
}

static void evaluate_grain_lut(float *grain_lut, const float mb)
{
  for(int i = 0; i < GRAIN_LUT_SIZE; i++)
  {
    for(int j = 0; j < GRAIN_LUT_SIZE; j++)
    {
      const float gu = (float)i / (GRAIN_LUT_SIZE - 1) - 0.5;
      const float l = (float)j / (GRAIN_LUT_SIZE - 1);
      grain_lut[j * GRAIN_LUT_SIZE + i] = 100.0f * (paper_resp(gu + paper_resp_inverse(l, mb, GRAIN_LUT_PAPER_GAMMA), mb, GRAIN_LUT_PAPER_GAMMA) - l);
    }
  }
}

static float dt_lut_lookup_2d_1c(const float *grain_lut, const float x, const float y)
{
  const float _x = CLAMPS((x + 0.5f) * (GRAIN_LUT_SIZE - 1), 0, GRAIN_LUT_SIZE - 1);
  const float _y = CLAMPS(y * (GRAIN_LUT_SIZE - 1), 0, GRAIN_LUT_SIZE - 1);

  const int _x0 = _x < GRAIN_LUT_SIZE - 2 ? _x : GRAIN_LUT_SIZE - 2;
  const int _y0 = _y < GRAIN_LUT_SIZE - 2 ? _y : GRAIN_LUT_SIZE - 2;

  const int _x1 = _x0 + 1;
  const int _y1 = _y0 + 1;

  const float x_diff = _x - _x0;
  const float y_diff = _y - _y0;

  const float l00 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x0];
  const float l01 = grain_lut[_y0 * GRAIN_LUT_SIZE + _x1];
  const float l10 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x0];
  const float l11 = grain_lut[_y1 * GRAIN_LUT_SIZE + _x1];

  const float xy0 = (1.0f - y_diff) * l00 + l10 * y_diff;
  const float xy1 = (1.0f - y_diff) * l01 + l11 * y_diff;
  return xy0 * (1.0f - x_diff) + xy1 * x_diff;
}


const char *name()
{
  return _("grain");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate silver grains from film"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING
         | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

// This is a modified Bernstein hash known as DJBX33X (hash x 33 with bitwise XOR).
// However, we calculate the hash from the end of the string to the beginning. Why?
// We hash the image filename. This allows us to get rid of static grain when
// creating a video from a sequence of images, the names of which will usually
// differ by the last characters. Therefore, we start hashing from the changed
// characters so that these changes have a greater impact on the resulting hash.
static unsigned int _hash_string(char *str)
{
  unsigned int hash = 5381;

  for(int i = strlen(str) - 1; i >= 0; i--)
    hash = ((hash << 5) + hash) ^ str[i];

  return hash;
}

/* Fill the emulsion sf_grain_delta_ml draws from.

   spektrafilm gets this from sf_sim_grain_layers(), which reads the loaded
   stock's fitted density curves. There is no stock here, so the same struct
   is built from the sliders and the four constants above.

   The two curve members are what let a synthetic emulsion use the real
   sampler. sf_grain_delta_ml resolves a total density to a position along
   layer_curve_total, then reads each sub-layer's share of that density off
   layer_curve at the same position. Filling both with straight ramps --
   total rising to DMAX, each sub-layer rising to its weighted share -- makes
   that lookup return weight[sl] * density, which is the even split this
   module wants.

   layer_npart holds the crystal count for a pixel of SF_GRAIN_REF_UM, so it
   is independent of pipe resolution; sf_grain_delta_ml scales it by the
   npart_scale process() passes. */
static void _particle_build(dt_iop_grain_data_t *d)
{
  /* Coarsest first. Only the finer sub-layers follow the slider, so at 0
     they shrink away and the coarsest carries the noise alone, matching the
     end behaviour of spektrafilm's control. */
  static const float ladder[GRAIN_PARTICLE_SUBLAYERS] = { 1.0f, 0.5f, 0.25f };
  const float ref_um2 = SF_GRAIN_REF_UM * SF_GRAIN_REF_UM;
  /* Density is split evenly. A real stock's split comes out of its fitted
     curves; with none to fit, even is the neutral choice. */
  const float w = 1.0f / (float)GRAIN_PARTICLE_SUBLAYERS;

  d->layers.n = GRAIN_PARTICLE_SUBLAYERS;
  d->layers.layer_curve = d->layer_curve;
  d->layers.layer_curve_total = d->layer_curve_total;

  float dmin_total = 0.0f;
  for(int sl = 0; sl < GRAIN_PARTICLE_SUBLAYERS; sl++)
  {
    const float ps = (sl == 0) ? ladder[sl] : ladder[sl] * d->sublayer_scale;
    /* Crystal AREA goes as the square of the linear size, and the number of
       crystals a pixel covers is its area divided by theirs. */
    const float lin = fmaxf(d->grain_size * ps, 1e-4f);
    const float clump_um2 = GRAIN_PARTICLE_CLUMP_UM2 * lin * lin;
    const float dmin = GRAIN_PARTICLE_DMIN * d->density_min * w;

    d->layers.particle_scale[sl] = ps;
    dmin_total += dmin;
    for(int c = 0; c < 3; c++)
    {
      d->layers.layer_dmin[sl][c] = dmin;
      d->layers.layer_dmax[sl][c] = GRAIN_PARTICLE_DMAX * w + dmin;
      d->layers.layer_npart[sl][c] = fmaxf(ref_um2 / clump_um2, 1e-3f);
    }
  }

  for(int i = 0; i < SF_NLE; i++)
  {
    const float t = (float)i / (float)(SF_NLE - 1);
    for(int c = 0; c < 3; c++)
    {
      d->layer_curve_total[i][c] = GRAIN_PARTICLE_DMAX * t;
      for(int sl = 0; sl < GRAIN_PARTICLE_SUBLAYERS; sl++)
        d->layer_curve[i][sl][c] = GRAIN_PARTICLE_DMAX * w * t;
      /* Sub-layers past the ladder are unused: n caps the loop in
         sf_grain_delta_ml. Zeroed so the table holds no stale values. */
      for(int sl = GRAIN_PARTICLE_SUBLAYERS; sl < SF_GRAIN_MAX_SUBLAYERS; sl++)
        d->layer_curve[i][sl][c] = 0.0f;
    }
  }

  /* sf_grain_delta_ml's mono path reads channel 1 throughout. */
  for(int c = 0; c < 3; c++)
  {
    d->dmin_c[c] = dmin_total;
    d->unif_c[c] = CLAMPF(d->uniformity * GRAIN_PARTICLE_UNIF, 0.0f, 0.999f);
  }
}

/* Widest gaussian sigma the texture stage dispatches, in pixels. Shared by
   process(), modify_roi_in() and tiling_callback() so the halo each assumes
   cannot drift from the blurs actually run. */
static float _particle_max_sigma(const dt_iop_grain_data_t *d,
                                 const float npart_scale)
{
  if(d->method != DT_GRAIN_METHOD_PARTICLE || d->strength <= 0.0f) return 0.0f;

  float s = GRAIN_BLUR_FACTOR * fmaxf(d->grain_blur, GRAIN_BLUR_MIN);
  if(d->grain_usm_amount > 0.0f) s = fmaxf(s, d->grain_usm_sigma);

  for(int sl = 0; sl < d->layers.n; sl++)
  {
    const float npart_c = (float)d->layers.layer_npart[sl][1] * npart_scale;
    const float od = (float)d->layers.layer_dmax[sl][1] / fmaxf(npart_c, 1e-6f);
    s = fmaxf(s, GRAIN_DYE_BLUR_UM * d->grain_dye_cloud * sqrtf(fmaxf(od, 0.0f)));
  }
  return s;
}

/* Physical micrometres per pixel at a given pipe scale. The reference frame
   is stated by its long edge, so it pairs with the long edge of the full
   buffer; width alone under-scales portrait images. */
static float _particle_pixel_um(const dt_dev_pixelpipe_iop_t *piece,
                                const float scale)
{
  const float full_long_edge
    = fmaxf(fmaxf((float)piece->buf_in.width, (float)piece->buf_in.height) * scale, 1.0f);
  return GRAIN_PARTICLE_REF_MM * 1000.0f / full_long_edge;
}

static float _particle_npart_scale(const float pixel_um)
{
  /* layer_npart is stated for a SF_GRAIN_REF_UM pixel; this carries it to the
     pixel size actually being rendered. */
  return pixel_um * pixel_um / (SF_GRAIN_REF_UM * SF_GRAIN_REF_UM);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  const dt_iop_grain_data_t *const d = piece->data;
  if(!d) return;

  const float pixel_um = _particle_pixel_um(piece, roi_out->scale);
  const int halo
    = (int)ceilf(GRAIN_HALO_SIGMAS * _particle_max_sigma(d, _particle_npart_scale(pixel_um)));
  if(halo <= 0) return;

  const int img_w = (int)roundf((float)piece->buf_in.width * roi_out->scale);
  const int img_h = (int)roundf((float)piece->buf_in.height * roi_out->scale);
  int x0 = roi_out->x - halo, y0 = roi_out->y - halo;
  int x1 = roi_out->x + roi_out->width + halo, y1 = roi_out->y + roi_out->height + halo;
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(img_w > 0 && x1 > img_w) x1 = img_w;
  if(img_h > 0 && y1 > img_h) y1 = img_h;
  roi_in->x = x0;
  roi_in->y = y0;
  roi_in->width = x1 - x0;
  roi_in->height = y1 - y0;
}

#ifdef HAVE_OPENCL
/* One separable gaussian pass over a single-channel device buffer, matching
   the host's _sf_gauss_convolve_1d: the same dt_gaussian_kernel_1d weights,
   clamp-to-edge, and accumulation order. `tmp` receives the row pass and the
   column pass writes back into `buf`. */
static cl_int _grain_blur_cl(const int devid,
                             const dt_iop_grain_global_data_t *gd,
                             cl_mem buf,
                             cl_mem tmp,
                             const int w,
                             const int h,
                             const float sigma)
{
  if(sigma < 1e-6f) return CL_SUCCESS;

  float kernel[2 * SF_GAUSS_MAX_RADIUS + 1];
  const int radius = dt_gaussian_kernel_1d(sigma, kernel, SF_GAUSS_MAX_RADIUS);

  cl_mem dev_k = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(float) * (size_t)(2 * radius + 1), kernel);
  if(!dev_k) return DT_OPENCL_SYSMEM_ALLOCATION;

  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_row_1c, w, h,
      CLARG(buf), CLARG(tmp), CLARG(w), CLARG(h), CLARG(dev_k), CLARG(radius));
  if(err == CL_SUCCESS)
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_gauss_col_1c, w, h,
        CLARG(tmp), CLARG(buf), CLARG(w), CLARG(h), CLARG(dev_k), CLARG(radius));

  dt_opencl_release_mem_object(dev_k);
  return err;
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_grain_data_t *const d = piece->data;
  const dt_iop_grain_global_data_t *const gd = self->global_data;
  const int devid = piece->pipe->devid;

  /* Only the particle method has a device path. Returning this hands the
     simplex method back to the CPU, which is where its LUT already lives. */
  if(d->method != DT_GRAIN_METHOD_PARTICLE) return DT_OPENCL_PROCESS_CL;

  const int w = roi_in->width, h = roi_in->height;
  const int dx = roi_out->x - roi_in->x;
  const int dy = roi_out->y - roi_in->y;
  const int nsub = d->layers.n;

  const float pixel_um = _particle_pixel_um(piece, roi_in->scale);
  const float npart_scale = _particle_npart_scale(pixel_um);
  const float amount = d->strength / 100.0f * GRAIN_PARTICLE_STRENGTH_GAIN;
  const float dmax_full = GRAIN_PARTICLE_DMAX;

  cl_mem acc[GRAIN_DYE_LAYERS] = { NULL, NULL, NULL };
  cl_mem dev_dev = NULL, dev_tmp = NULL, dev_blur = NULL;
  cl_mem dev_dmax = NULL, dev_npart = NULL, dev_dmin = NULL;
  cl_mem dev_curve = NULL, dev_curve_total = NULL;

  cl_int err = DT_OPENCL_SYSMEM_ALLOCATION;

  for(int c = 0; c < GRAIN_DYE_LAYERS; c++)
  {
    acc[c] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)w * h);
    if(!acc[c]) goto error;
  }
  dev_dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)w * h);
  dev_tmp = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)w * h);
  dev_blur = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)w * h);
  if(!dev_dev || !dev_tmp || !dev_blur) goto error;

  /* The emulsion tables. layer_* are double on the host and float on the
     device, so they are narrowed here rather than uploaded raw; the values
     come from constants and slider positions, so the narrowing is exact for
     everything the sampler then does with them. */
  {
    const int n3 = SF_GRAIN_MAX_SUBLAYERS * 3;
    float fdmax[SF_GRAIN_MAX_SUBLAYERS * 3];
    float fnpart[SF_GRAIN_MAX_SUBLAYERS * 3];
    float fdmin[SF_GRAIN_MAX_SUBLAYERS * 3];
    for(int sl = 0; sl < SF_GRAIN_MAX_SUBLAYERS; sl++)
      for(int c = 0; c < 3; c++)
      {
        const int i = sl * 3 + c;
        fdmax[i] = (float)d->layers.layer_dmax[sl][c];
        fnpart[i] = (float)d->layers.layer_npart[sl][c];
        fdmin[i] = (float)d->layers.layer_dmin[sl][c];
      }
    dev_dmax = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * n3, fdmax);
    dev_npart = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * n3, fnpart);
    dev_dmin = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * n3, fdmin);
  }
  dev_curve = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(d->layer_curve), (void *)d->layer_curve);
  dev_curve_total = dt_opencl_copy_host_to_device_constant(
      devid, sizeof(d->layer_curve_total), (void *)d->layer_curve_total);
  if(!dev_dmax || !dev_npart || !dev_dmin || !dev_curve || !dev_curve_total) goto error;

  for(int c = 0; c < GRAIN_DYE_LAYERS; c++)
  {
    for(int sl = 0; sl < nsub; sl++)
    {
      const float unif_ch = d->unif_c[c];
      const int sl_idx = sl, chan = c;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_gen_deviation, w, h,
          CLARG(dev_in), CLARG(dev_dev), CLARG(w), CLARG(h),
          CLARGINT(roi_in->x), CLARGINT(roi_in->y), CLARG(chan), CLARG(sl_idx),
          CLARGINT(SF_NLE), CLARGINT(SF_GRAIN_MAX_SUBLAYERS), CLARG(dmax_full),
          CLARG(unif_ch), CLARG(npart_scale),
          CLARG(dev_dmax), CLARG(dev_npart), CLARG(dev_dmin),
          CLARG(dev_curve_total), CLARG(dev_curve));
      if(err != CL_SUCCESS) goto error;

      const float npart_c = (float)d->layers.layer_npart[sl][c] * npart_scale;
      const float od = (float)d->layers.layer_dmax[sl][c] / fmaxf(npart_c, 1e-6f);
      err = _grain_blur_cl(devid, gd, dev_dev, dev_tmp, w, h,
                           GRAIN_DYE_BLUR_UM * d->grain_dye_cloud * sqrtf(fmaxf(od, 0.0f)));
      if(err != CL_SUCCESS) goto error;

      const int reset = (sl == 0);
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_accumulate, w, h,
          CLARG(acc[c]), CLARG(dev_dev), CLARG(w), CLARG(h), CLARG(reset));
      if(err != CL_SUCCESS) goto error;
    }

    err = _grain_blur_cl(devid, gd, acc[c], dev_tmp, w, h,
                         GRAIN_BLUR_FACTOR * fmaxf(d->grain_blur, GRAIN_BLUR_MIN));
    if(err != CL_SUCCESS) goto error;

    if(d->grain_usm_sigma > 0.0f && d->grain_usm_amount > 0.0f)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, acc[c], dev_blur, 0, 0,
                                                    sizeof(float) * (size_t)w * h);
      if(err != CL_SUCCESS) goto error;
      err = _grain_blur_cl(devid, gd, dev_blur, dev_tmp, w, h, d->grain_usm_sigma);
      if(err != CL_SUCCESS) goto error;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_recover, w, h,
          CLARG(acc[c]), CLARG(dev_blur), CLARG(w), CLARG(h),
          CLARGFLOAT(d->grain_usm_amount));
      if(err != CL_SUCCESS) goto error;
    }
  }

  {
    const float k_l = amount * GRAIN_LUMA_NORM * 100.0f / GRAIN_PARTICLE_DMAX;
    const float k_c = amount * d->chroma * GRAIN_CHROMA_NORM * 100.0f / GRAIN_PARTICLE_DMAX;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_grain_combine,
        roi_out->width, roi_out->height,
        CLARG(dev_in), CLARG(dev_out), CLARG(acc[0]), CLARG(acc[1]), CLARG(acc[2]),
        CLARGINT(roi_out->width), CLARGINT(roi_out->height), CLARG(w),
        CLARG(dx), CLARG(dy), CLARG(k_l), CLARG(k_c));
  }

error:
  for(int c = 0; c < GRAIN_DYE_LAYERS; c++) dt_opencl_release_mem_object(acc[c]);
  dt_opencl_release_mem_object(dev_dev);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_blur);
  dt_opencl_release_mem_object(dev_dmax);
  dt_opencl_release_mem_object(dev_npart);
  dt_opencl_release_mem_object(dev_dmin);
  dt_opencl_release_mem_object(dev_curve);
  dt_opencl_release_mem_object(dev_curve_total);
  return err;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  /* The simplex method's permutation tables, needed whether or not OpenCL
     is available. */
  _simplex_noise_init();

  const int program = 44; // grain.cl, from programs.conf
  dt_iop_grain_global_data_t *gd = malloc(sizeof(dt_iop_grain_global_data_t));
  self->data = gd;
  gd->kernel_grain_gen_deviation = dt_opencl_create_kernel(program, "grain_gen_deviation");
  gd->kernel_grain_accumulate = dt_opencl_create_kernel(program, "grain_accumulate");
  gd->kernel_gauss_row_1c = dt_opencl_create_kernel(program, "gauss_row_1c");
  gd->kernel_gauss_col_1c = dt_opencl_create_kernel(program, "gauss_col_1c");
  gd->kernel_grain_recover = dt_opencl_create_kernel(program, "grain_recover");
  gd->kernel_grain_combine = dt_opencl_create_kernel(program, "grain_combine");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_grain_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_grain_gen_deviation);
  dt_opencl_free_kernel(gd->kernel_grain_accumulate);
  dt_opencl_free_kernel(gd->kernel_gauss_row_1c);
  dt_opencl_free_kernel(gd->kernel_gauss_col_1c);
  dt_opencl_free_kernel(gd->kernel_grain_recover);
  dt_opencl_free_kernel(gd->kernel_grain_combine);
  free(self->data);
  self->data = NULL;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_grain_data_t *const d = piece->data;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->align = 1;

  /* Simplex needs nothing beyond input and output, and with no piece data
     there are no parameters to size a halo from. */
  if(!d || d->method != DT_GRAIN_METHOD_PARTICLE)
  {
    tiling->factor = 2.0f;
    tiling->factor_cl = 2.0f;
    tiling->overlap = 0;
    return;
  }

  /* Input and output are RGBA, so one tile's worth is 4 floats per pixel.
     The particle method holds six single-channel planes at once -- one
     accumulated grain field per dye layer, one sub-layer's deviation, the
     blur scratch and the recovery's blurred copy -- which is 6/4 of a
     tile. */
  tiling->factor = 2.0f + 6.0f / 4.0f;
  /* The device holds the same six single-channel planes plus the row-pass
     scratch the separable blur writes through. */
  tiling->factor_cl = 2.0f + 7.0f / 4.0f;
  const float pixel_um = _particle_pixel_um(piece, roi_in->scale);
  tiling->overlap
    = (int)ceilf(GRAIN_HALO_SIGMAS * _particle_max_sigma(d, _particle_npart_scale(pixel_um)));
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  dt_iop_grain_data_t *data = piece->data;

  unsigned int hash = _hash_string(piece->pipe->image.filename) % (int)fmax(roi_out->width * 0.3, 1.0);

  if(data->method == DT_GRAIN_METHOD_PARTICLE)
  {
    /* modify_roi_in() pads the input by the blur halo, so ivoid is roi_IN
       sized while ovoid is roi_out sized. The grain fields are built and
       blurred across the whole padded input -- that is what the padding is
       for, so the blurs have real data either side of the output region --
       and only the output window is read back out at the end. */
    const int w = roi_in->width, h = roi_in->height;
    const size_t npix = (size_t)w * h;
    const int dx = roi_out->x - roi_in->x;
    const int dy = roi_out->y - roi_in->y;

    const float pixel_um = _particle_pixel_um(piece, roi_in->scale);
    const float npart_scale = _particle_npart_scale(pixel_um);
    const float amount = data->strength / 100.0f * GRAIN_PARTICLE_STRENGTH_GAIN;
    const int nsub = data->layers.n;

    /* one accumulated field per dye layer, plus a sub-layer's deviation, the
       blur scratch and the recovery's copy */
    float *acc[GRAIN_DYE_LAYERS] = { NULL, NULL, NULL };
    float *const dev = dt_alloc_align_float(npix);
    float *const scratch = dt_alloc_align_float(npix);
    float *const blurred = dt_alloc_align_float(npix);
    gboolean ok = dev && scratch && blurred;
    for(int c = 0; c < GRAIN_DYE_LAYERS; c++)
    {
      acc[c] = dt_alloc_align_float(npix);
      if(!acc[c]) ok = FALSE;
    }

    if(!ok)
    {
      for(int c = 0; c < GRAIN_DYE_LAYERS; c++) dt_free_align(acc[c]);
      dt_free_align(dev);
      dt_free_align(scratch);
      dt_free_align(blurred);
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 4);
      return;
    }

    /* Every dye layer is drawn from the same density -- L is all this module
       has -- and decorrelated only by its seed channel, which is what makes
       the three fields independent and so gives the combination below
       something to put on a and b. */
    for(int c = 0; c < GRAIN_DYE_LAYERS; c++)
    {
      memset(acc[c], 0, npix * sizeof(float));

      /* Each sub-layer is drawn, reduced to its DEVIATION from the density it
         was drawn against, spread by its own dye cloud and only then added in.

         Subtracting the expected density first is what keeps this a grain
         module. grain_layer_particle's mean is exactly the absolute density it
         was given, so the remainder carries no image content at all and every
         blur below acts on grain alone. Blurring the raw draw instead would
         carry blur(density) - density with it, which is a negative unsharp
         mask on the photograph.

         Per sub-layer rather than on the sum, because one blur of the summed
         draw would give every sub-layer the same spread and erase the
         coarse/fine structure the ladder exists for. sf_grain_raw_samples_ml
         returns the draws unsummed for this reason. */
      for(int sl = 0; sl < nsub; sl++)
      {
        const float wgt = (float)data->layers.layer_dmax[sl][c];
        const float dmin = (float)data->layers.layer_dmin[sl][c];

        DT_OMP_FOR()
        for(int y = 0; y < h; y++)
          for(int x = 0; x < w; x++)
          {
            const size_t k = (size_t)y * w + x;
            const float L = ((const float *)ivoid)[4 * k];
            const float dens = GRAIN_PARTICLE_DMAX * CLAMPF(L, 0.0f, 100.0f) / 100.0f;
            float samples[SF_GRAIN_MAX_SUBLAYERS];
            /* Absolute buffer coordinates, so the pattern does not crawl when
               panning and a tile draws the same grain as the untiled render. */
            sf_grain_raw_samples_ml(&data->layers, dens, c, c,
                                    (uint32_t)(roi_in->x + x), (uint32_t)(roi_in->y + y),
                                    data->unif_c[c], npart_scale, samples);
            /* The density this sub-layer was drawn against: its share of the
               total plus its floor, which is the draw's mean. */
            const float expected
              = dens * (wgt - dmin) / GRAIN_PARTICLE_DMAX + dmin;
            dev[k] = samples[sl] - expected;
          }

        const float npart_c = (float)data->layers.layer_npart[sl][c] * npart_scale;
        const float od = (float)data->layers.layer_dmax[sl][c] / fmaxf(npart_c, 1e-6f);
        sf_blur_plane1(dev, w, h,
                       GRAIN_DYE_BLUR_UM * data->grain_dye_cloud * sqrtf(fmaxf(od, 0.0f)),
                       NULL, scratch);

        DT_OMP_FOR()
        for(size_t k = 0; k < npix; k++) acc[c][k] += dev[k];
      }

      /* Crystal clumping. A per-pixel draw is spatially independent, which is
         white noise; this is what gives grain its texture. */
      sf_blur_plane1(acc[c], w, h,
                     GRAIN_BLUR_FACTOR * fmaxf(data->grain_blur, GRAIN_BLUR_MIN),
                     NULL, scratch);
      /* Acutance recovery for the clump blur. Additive, not the
         multiplicative mask: `acc` is the zero-mean grain deviation, where a
         ratio is ill-conditioned wherever the field crosses zero. */
      sf_unsharp_mask1(acc[c], w, h, data->grain_usm_sigma, data->grain_usm_amount,
                       blurred, scratch);
    }

    const float k_l = amount * GRAIN_LUMA_NORM * 100.0f / GRAIN_PARTICLE_DMAX;
    const float k_c = amount * data->chroma * GRAIN_CHROMA_NORM * 100.0f / GRAIN_PARTICLE_DMAX;

    DT_OMP_FOR()
    for(int y = 0; y < roi_out->height; y++)
    {
      const float *in = ((const float *)ivoid) + (size_t)4 * ((size_t)(y + dy) * w + dx);
      float *out = ((float *)ovoid) + (size_t)4 * y * roi_out->width;
      const size_t row = (size_t)(y + dy) * w + dx;
      for(int x = 0; x < roi_out->width; x++)
      {
        const float g0 = acc[0][row + x], g1 = acc[1][row + x], g2 = acc[2][row + x];
        /* Left unclamped, as the simplex path leaves `in[0] + lut(...)`. A
           clamp to [0,100] truncates the upper half of the draw wherever L is
           already 100, biasing flat white areas darker -- grain that can only
           subtract. */
        out[0] = in[0] + (g0 + g1 + g2) * k_l;
        out[1] = in[1] + (g0 - g1) * k_c;
        out[2] = in[2] + (g1 - g2) * k_c;
        out[3] = in[3];
        out += 4;
        in += 4;
      }
    }

    for(int c = 0; c < GRAIN_DYE_LAYERS; c++) dt_free_align(acc[c]);
    dt_free_align(dev);
    dt_free_align(scratch);
    dt_free_align(blurred);
    return;
  }

  const gboolean fastmode = dt_pipe_is_fast(piece->pipe);
  // Apply grain to image
  const float strength = (data->strength / 100.0f);
  // double zoom=1.0+(8*(data->scale/100.0));
  const double wd = fminf(piece->buf_in.width, piece->buf_in.height);
  const double zoom = (1.0 + 8 * data->scale / 100) / 800.0;
  // in fastpipe mode, skip the downsampling for zoomed-out views
  const int filter = !fastmode && fabsf(roi_out->scale - 1.0f) > 0.01f;
  // filter width depends on world space (i.e. reverse wd norm and roi->scale, as well as buffer input to
  // pixelpipe iscale)
  const double filtermul = piece->iscale / (roi_out->scale * wd);
  const float fib1 = 34.0f, fib2 = 21.0f;
  const float fib1div2 = fib1 / fib2;
  const double scale = roi_out->scale;	// is only used in double expressions, so avoid conversion
  const double fib2inv = 1.0 / fib2;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = ((float *)ivoid) + (size_t)4 * roi_out->width * j;
    float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;
    const double wy = (roi_out->y + j) / scale;
    const double y = wy / wd;
    // y: normalized to shorter side of image, so with pixel aspect = 1.

    for(int i = 0; i < roi_out->width; i++)
    {
      // calculate x, y in a resolution independent way:
      // wx,wy: worldspace in full image pixel coords:
      const double wx = (roi_out->x + i) / scale;
      // x: normalized to shorter side of image, so with pixel aspect = 1.
      const double x = wx / wd;
      float noise = 0.0;
      if(filter)
      {
        // if zoomed out a lot, use rank-1 lattice downsampling
        for(int l = 0; l < fib2; l++)
        {
          float px = l / fib2, py = l * fib1div2;
          py -= (int)py;
          float dx = px * filtermul, dy = py * filtermul;
          noise += fib2inv * _simplex_2d_noise(x + dx + hash, y + dy, zoom);
        }
      }
      else
      {
        noise = _simplex_2d_noise(x + hash, y, zoom);
      }

      out[0] = in[0] + dt_lut_lookup_2d_1c(data->grain_lut, (noise * strength) * GRAIN_LIGHTNESS_STRENGTH_SCALE, in[0] / 100.0f);
      out[1] = in[1];
      out[2] = in[2];

      out += 4;
      in += 4;
    }
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)p1;
  dt_iop_grain_data_t *d = piece->data;

  d->channel = p->channel;
  d->scale = p->scale;
  d->strength = p->strength;
  d->midtones_bias = p->midtones_bias;
  d->method = p->method;
  d->grain_size = p->grain_size;
  d->uniformity = p->uniformity;
  d->sublayer_scale = p->sublayer_scale;
  d->density_min = p->density_min;
  d->grain_blur = p->grain_blur;
  d->grain_dye_cloud = p->grain_dye_cloud;
  d->grain_usm_sigma = p->grain_usm_sigma;
  d->grain_usm_amount = p->grain_usm_amount;
  d->chroma = p->chroma;
  _particle_build(d);

  evaluate_grain_lut(d->grain_lut, d->midtones_bias);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_grain_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

/* `midtones_bias` shapes the paper response curve the simplex method runs
   its noise through, and the two particle controls describe an emulsion the
   simplex method knows nothing about, so each set is only shown for the
   method that reads it. `coarseness` and `strength` drive both. */
void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_grain_gui_data_t *g = self->gui_data;
  const dt_iop_grain_params_t *p = self->params;
  const gboolean particle = p->method == DT_GRAIN_METHOD_PARTICLE;

  /* Coarseness is the simplex noise's own frequency control and mid-tones
     bias shapes the paper response curve it runs that noise through;
     neither has a counterpart in the particle model, which takes its size
     from grain size and the frame. Strength drives both methods. */
  gtk_widget_set_visible(g->scale, !particle);
  gtk_widget_set_visible(g->midtones_bias, !particle);

  gtk_widget_set_visible(g->grain_size, particle);
  gtk_widget_set_visible(g->uniformity, particle);
  gtk_widget_set_visible(g->sublayer_scale, particle);
  gtk_widget_set_visible(g->density_min, particle);
  gtk_widget_set_visible(g->chroma, particle);

  /* The expander carries its own widgets, so hiding it takes the whole
     section with it; the sliders inside need no separate call. */
  gtk_widget_set_visible(g->texture.expander, particle);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_grain_gui_data_t *g = self->gui_data;
  dt_gui_update_collapsible_section(&g->texture);
  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_grain_gui_data_t *g = IOP_GUI_ALLOC(grain);

  g->method = dt_bauhaus_combobox_from_params(self, "method");
  gtk_widget_set_tooltip_text(g->method,
    _("how the grain is generated.\n"
      "\n"
      "simplex: layered noise shaped by a photographic paper response.\n"
      "the strength is yours to set.\n"
      "\n"
      "particle: a draw from an emulsion crystal model. the strength follows\n"
      "from the crystal size and the frame, so grain scales with the export\n"
      "size the way film does."));

  /* Construction order is the order the sliders read once gui_changed has
     hidden the ones the current method does not use: coarseness / strength
     / mid-tones bias for simplex, and grain size / strength / chroma /
     uniformity / sublayer / floor for particle. */

  /* courseness */
  g->scale = dt_bauhaus_slider_from_params(self, "scale");
  dt_bauhaus_slider_set_factor(g->scale, GRAIN_SCALE_FACTOR);
  dt_bauhaus_slider_set_digits(g->scale, 0);
  dt_bauhaus_slider_set_format(g->scale, " ISO");
  gtk_widget_set_tooltip_text(g->scale, _("the grain size (~ISO of the film)"));

  g->grain_size = dt_bauhaus_slider_from_params(self, "grain_size");
  dt_bauhaus_slider_set_soft_range(g->grain_size, 0.25f, 2.5f);
  gtk_widget_set_tooltip_text(g->grain_size,
    _("how coarse the film's crystals are. raising it grows the crystals, so\n"
      "fewer of them fall inside each pixel and the grain becomes stronger\n"
      "as well as coarser, the way a fast film differs from a slow one.\n"
      "\n"
      "the size is relative to the frame, not to the pixel, so grain stays\n"
      "the same size in the picture however large you export it."));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("the strength of applied grain"));

  g->chroma = dt_bauhaus_slider_from_params(self, "chroma");
  gtk_widget_set_tooltip_text(g->chroma,
    _("how much colour the grain carries. each dye layer is drawn\n"
      "separately, and what they disagree on becomes colour speckle.\n"
      "at 1.0 there is as much colour grain as lightness grain.\n"
      "\n"
      "set it to 0 for the achromatic grain of a black & white film,\n"
      "which leaves lightness untouched either way."));

  g->uniformity = dt_bauhaus_slider_from_params(self, "uniformity");
  dt_bauhaus_slider_set_soft_range(g->uniformity, 0.9f, 1.02f);
  gtk_widget_set_tooltip_text(g->uniformity,
    _("how evenly the crystals are distributed. raising it bends the noise\n"
      "toward a bell: grain that peaks in the midtones and eases off again\n"
      "in the densest areas. lowering it lets grain keep growing all the way\n"
      "into the densest areas instead."));

  g->sublayer_scale = dt_bauhaus_slider_from_params(self, "sublayer_scale");
  gtk_widget_set_tooltip_text(g->sublayer_scale,
    _("a real emulsion layers coarse crystals over finer ones. this scales\n"
      "the finer sub-layers against the coarsest, which stays fixed. lower\n"
      "makes the fine layers finer still, so the coarse layer dominates; at\n"
      "0 only the coarsest layer is left."));

  g->density_min = dt_bauhaus_slider_from_params(self, "density_min");
  dt_bauhaus_slider_set_soft_range(g->density_min, 0.25f, 2.5f);
  gtk_widget_set_tooltip_text(g->density_min,
    _("the density each crystal sits at even where the film received no\n"
      "light, which is why grain does not disappear entirely in clear\n"
      "areas. raise it for grain that carries into the blacks."));

  /* Collapsed by default and remembered per user, as spektrafilm's matching
     section is: these four shape the grain's texture rather than its
     strength, and the defaults are a tuned set most edits never touch. */
  /* Taken here, not at the top of gui_init: self->widget does not exist yet
     on entry, it is created by the first dt_bauhaus_*_from_params call
     (imageop_gui.c). Capturing it earlier stores NULL, and restoring that
     below would drop every widget built so far. */
  GtkWidget *const page = self->widget;
  dt_gui_new_collapsible_section(&g->texture, "plugins/darkroom/grain/expand_texture",
                                 C_("section", "texture"), GTK_BOX(page),
                                 DT_ACTION(self));
  /* Widgets are added to self->widget, so it points at the section while its
     sliders are built and is restored afterwards. */
  self->widget = GTK_WIDGET(g->texture.container);

  g->grain_blur = dt_bauhaus_slider_from_params(self, "grain_blur");
  gtk_widget_set_tooltip_text(g->grain_blur,
    _("how far the developed crystals clump together. a single crystal per\n"
      "pixel is white noise; this is what gives grain its texture, so very\n"
      "low values stop it looking like film at all.\n"
      "\n"
      "it costs grain contrast, which the recovery below is tuned to return."));

  g->grain_dye_cloud = dt_bauhaus_slider_from_params(self, "grain_dye_cloud");
  gtk_widget_set_tooltip_text(g->grain_dye_cloud,
    _("how far the dye spreads around each crystal. applied per sub-layer\n"
      "and scaled by that layer's crystal size, so it softens the fine\n"
      "layers more than the coarse ones."));

  g->grain_usm_sigma = dt_bauhaus_slider_from_params(self, "grain_usm_sigma");
  gtk_widget_set_tooltip_text(g->grain_usm_sigma,
    _("radius of the acutance recovery that follows the grain blur. the two\n"
      "are a tuned pair: recovery without blur over-defines the grain."));

  g->grain_usm_amount = dt_bauhaus_slider_from_params(self, "grain_usm_amount");
  gtk_widget_set_tooltip_text(g->grain_usm_amount,
    _("how much of the contrast the grain blur removed is put back. at 0 the\n"
      "grain keeps the softness the blur gave it."));

  /* back to the module box: mid-tones bias belongs to the simplex method and
     must not land inside the texture section */
  self->widget = page;

  g->midtones_bias = dt_bauhaus_slider_from_params(self, "midtones_bias");
  dt_bauhaus_slider_set_format(g->midtones_bias, "%");
  gtk_widget_set_tooltip_text(g->midtones_bias, _("amount of mid-tones bias from the photographic paper response modeling. the greater the bias, the more pronounced the fall off of the grain in shadows and highlights"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
