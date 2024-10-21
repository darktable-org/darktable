/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/math.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define GRAIN_LIGHTNESS_STRENGTH_SCALE 0.15f

// (m_pi/2)/4 = half hue colorspan
#define GRAIN_HUE_COLORRANGE 0.392699082

#define GRAIN_HUE_STRENGTH_SCALE 0.25
#define GRAIN_SATURATION_STRENGTH_SCALE 0.25
#define GRAIN_RGB_STRENGTH_SCALE 0.25

#define GRAIN_SCALE_FACTOR 213.2

#define GRAIN_LUT_SIZE 128
#define GRAIN_LUT_DELTA_MAX 2.0
#define GRAIN_LUT_DELTA_MIN 0.0001
#define GRAIN_LUT_PAPER_GAMMA 1.0

DT_MODULE_INTROSPECTION(2, dt_iop_grain_params_t)


typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE = 0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
} _dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel; // $DEFAULT: DT_GRAIN_CHANNEL_LIGHTNESS
  float scale;                     /* $MIN: 20.0/GRAIN_SCALE_FACTOR
                                      $MAX: 6400.0/GRAIN_SCALE_FACTOR
                                      $DEFAULT: 1600.0/GRAIN_SCALE_FACTOR
                                      $DESCRIPTION: "coarseness" */
  float strength;      // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0
  float midtones_bias; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "mid-tones bias"
} dt_iop_grain_params_t;

typedef struct dt_iop_grain_gui_data_t
{
  GtkWidget *scale, *strength, *midtones_bias; // scale, strength, midtones_bias
} dt_iop_grain_gui_data_t;

typedef struct dt_iop_grain_data_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
  float midtones_bias;
  float grain_lut[GRAIN_LUT_SIZE * GRAIN_LUT_SIZE];
} dt_iop_grain_data_t;


int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_grain_params_v2_t
  {
    _dt_iop_grain_channel_t channel;
    float scale;
    float strength;
    float midtones_bias;
  } dt_iop_grain_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_grain_params_v1_t
    {
      _dt_iop_grain_channel_t channel;
      float scale;
      float strength;
    } dt_iop_grain_params_v1_t;

    const dt_iop_grain_params_v1_t *o = old_params;
    dt_iop_grain_params_v2_t *n = malloc(sizeof(dt_iop_grain_params_v2_t));

    n->channel = o->channel;
    n->scale = o->scale;
    n->strength = o->strength;
    n->midtones_bias = 0.0; // it produces the same results as the old version

    *new_params = n;
    *new_params_size = sizeof(dt_iop_grain_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
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
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
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

  const gboolean fastmode = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;
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

void init_global(dt_iop_module_so_t *self)
{
  _simplex_noise_init();
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_grain_gui_data_t *g = IOP_GUI_ALLOC(grain);

  /* courseness */
  g->scale = dt_bauhaus_slider_from_params(self, "scale");
  dt_bauhaus_slider_set_factor(g->scale, GRAIN_SCALE_FACTOR);
  dt_bauhaus_slider_set_digits(g->scale, 0);
  dt_bauhaus_slider_set_format(g->scale, " ISO");
  gtk_widget_set_tooltip_text(g->scale, _("the grain size (~ISO of the film)"));

  g->strength = dt_bauhaus_slider_from_params(self, N_("strength"));
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("the strength of applied grain"));

  g->midtones_bias = dt_bauhaus_slider_from_params(self, "midtones_bias");
  dt_bauhaus_slider_set_format(g->midtones_bias, "%");
  gtk_widget_set_tooltip_text(g->midtones_bias, _("amount of mid-tones bias from the photographic paper response modeling. the greater the bias, the more pronounced the fall off of the grain in shadows and highlights"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
