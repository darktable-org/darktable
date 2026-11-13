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
#include "common/iop_profile.h"
#include "develop/develop.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <omp.h>

static dt_colorspaces_t _registry;
static dt_develop_t _dev;
static dt_iop_module_t _module = { .op = "xyz experiment" };
static int _pixels = 131072;

static void _check(const int condition, const char *message)
{
  if(!condition)
  {
    fprintf(stderr, "FAIL: %s\n", message);
    exit(1);
  }
}

static void _register(const dt_colorspaces_color_profile_type_t type,
                      const cmsCIExyYTRIPLE *primaries)
{
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  cmsToneCurve *linear = cmsBuildGamma(NULL, 1.0);
  cmsToneCurve *curves[3] = { linear, linear, linear };
  dt_colorspaces_color_profile_t *p = calloc(1, sizeof(*p));
  p->type = type;
  p->profile = cmsCreateRGBProfile(&white, primaries, curves);
  _check(p->profile != NULL, "create profile");
  cmsFreeToneCurve(linear);
  _registry.profiles = g_list_append(_registry.profiles, p);
}

static void _convert(const float *in, float *out, const int n,
                     const dt_iop_colorspace_type_t from,
                     const dt_iop_colorspace_type_t to,
                     const dt_iop_order_iccprofile_info_t *profile)
{
  dt_iop_colorspace_type_t converted = from;
  dt_ioppr_transform_image_colorspace(&_module, in, out, n, 1, from, to, &converted, profile);
  _check(converted == to, "pipeline conversion succeeded");
}

static inline void _tone(const float *in, float *out)
{
  // luminance-only Reinhard curve, preserving xy for positive luminance
  const float scale = in[1] > 0.f ? 1.f / (1.f + in[1]) : 0.f;
  for(int c = 0; c < 3; c++) out[c] = in[c] * scale;
  out[3] = in[3];
}

static inline float _gamut(float *rgb, const dt_iop_order_iccprofile_info_t *profile)
{
  // contract toward the profile's neutral at constant luminance, in linear RGB
  const float sum = profile->matrix_in[1][0] + profile->matrix_in[1][1]
                  + profile->matrix_in[1][2];
  const float y = profile->matrix_in[1][0] * rgb[0]
                + profile->matrix_in[1][1] * rgb[1]
                + profile->matrix_in[1][2] * rgb[2];
  const float gray = y / sum;
  float t = 1.f;
  for(int c = 0; c < 3; c++)
  {
    const float delta = rgb[c] - gray;
    if(rgb[c] < 0.f) t = fminf(t, -gray / delta);
    if(rgb[c] > 1.f) t = fminf(t, (1.f - gray) / delta);
  }
  for(int c = 0; c < 3; c++) rgb[c] = gray + t * (rgb[c] - gray);
  return t;
}

static void _local(const float *in, float *out, const int n,
                   const dt_iop_order_iccprofile_info_t *profile)
{
  DT_OMP_FOR()
  for(int k = 0; k < n; k++)
  {
    dt_aligned_pixel_t xyz, toned;
    dt_ioppr_rgb_to_xyz(in + 4*k, xyz, profile);
    xyz[3] = in[4*k+3];
    _tone(xyz, toned);
    dt_apply_transposed_color_matrix(toned, profile->matrix_out_transposed, out + 4*k);
    out[4*k+3] = in[4*k+3];
    _gamut(out + 4*k, profile);
  }
}

static void _pipeline(const float *in, float *out, float *a, float *b, const int n,
                      const dt_iop_order_iccprofile_info_t *profile)
{
  _convert(in, a, n, IOP_CS_RGB, IOP_CS_XYZ, profile);
  DT_OMP_FOR()
  for(int k = 0; k < n; k++) _tone(a + 4*k, b + 4*k);
  _convert(b, out, n, IOP_CS_XYZ, IOP_CS_RGB, profile);
  DT_OMP_FOR()
  for(int k = 0; k < n; k++) _gamut(out + 4*k, profile);
}

static void _rgb_native(const float *in, float *out, const int n,
                        const dt_iop_order_iccprofile_info_t *profile)
{
  DT_OMP_FOR()
  for(int k = 0; k < n; k++)
  {
    const float y = dt_ioppr_rgb_luminance(in + 4*k, profile);
    const float scale = y > 0.f ? 1.f / (1.f + y) : 0.f;
    for(int c = 0; c < 3; c++) out[4*k+c] = in[4*k+c] * scale;
    out[4*k+3] = in[4*k+3];
  }
  DT_OMP_FOR()
  for(int k = 0; k < n; k++) _gamut(out + 4*k, profile);
}

static void _buffered_local(const float *in, float *out, float *a, float *b, const int n,
                            const dt_iop_order_iccprofile_info_t *profile)
{
  DT_OMP_FOR()
  for(int k = 0; k < n; k++)
  {
    dt_ioppr_rgb_to_xyz(in + 4*k, a + 4*k, profile);
    a[4*k+3] = in[4*k+3];
  }
  DT_OMP_FOR()
  for(int k = 0; k < n; k++) _tone(a + 4*k, b + 4*k);
  DT_OMP_FOR()
  for(int k = 0; k < n; k++)
  {
    dt_apply_transposed_color_matrix(b + 4*k, profile->matrix_out_transposed, out + 4*k);
    out[4*k+3] = in[4*k+3];
  }
  DT_OMP_FOR()
  for(int k = 0; k < n; k++) _gamut(out + 4*k, profile);
}

static double _seconds(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

static void _run(const char *name, const dt_colorspaces_color_profile_type_t type)
{
  const dt_iop_order_iccprofile_info_t *profile = dt_ioppr_add_profile_info_to_list(
    &_dev, type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
  _check(profile != NULL && profile->nonlinearlut == 0, "linear matrix working profile");
  const int n = _pixels;
  float *in = dt_alloc_align_float(4*n), *local = dt_alloc_align_float(4*n);
  float *pipe = dt_alloc_align_float(4*n), *a = dt_alloc_align_float(4*n);
  float *b = dt_alloc_align_float(4*n);
  float *native = dt_alloc_align_float(4*n);
  float *buffered = dt_alloc_align_float(4*n);
  _check(in && local && pipe && a && b && native && buffered, "allocate images");
  unsigned int seed = 12345;
  for(int k = 0; k < n; k++)
  {
    for(int c = 0; c < 3; c++)
    {
      seed = seed * 1664525u + 1013904223u;
      in[4*k+c] = (seed >> 8) * (20.f / 16777215.f) - 2.f;
    }
    in[4*k+3] = (k % 101) / 100.f;
  }
  // neutral ramp includes black and HDR; XYZ stress samples are deliberately not a gamut
  const float levels[] = { 0.f, 1e-6f, 0.001f, 0.18f, 1.f, 4.f, 64.f };
  for(int k = 0; k < 7; k++)
    for(int c = 0; c < 3; c++) in[4*k+c] = levels[k];
  const DT_ALIGNED_PIXEL float stress[5][4] = {
    { 1.f, 1.f, 1.f, 0.1f }, { -0.2f, 0.5f, 1.4f, 0.2f },
    { 0.1f, -0.1f, 0.3f, 0.3f }, { 1.f, 0.f, 1.f, 0.4f },
    { 30.f, 16.f, -3.f, 0.5f }
  };
  _convert(stress[0], in + 28, 5, IOP_CS_XYZ, IOP_CS_RGB, profile);
  _local(in, local, n, profile);
  _pipeline(in, pipe, a, b, n, profile);
  _rgb_native(in, native, n, profile);
  _buffered_local(in, buffered, a, b, n, profile);
  float difference = 0.f, neutral = 0.f, luminance = 0.f, chromaticity = 0.f;
  float native_difference = 0.f;
  float buffered_difference = 0.f;
  int mapped = 0, nonpositive = 0;
  for(int k = 0; k < n; k++)
  {
    const float expected_y = a[4*k+1] > 0.f ? a[4*k+1] / (1.f + a[4*k+1]) : 0.f;
    const float actual_y = dt_ioppr_rgb_luminance(pipe + 4*k, profile);
    luminance = fmaxf(luminance, fabsf(actual_y - expected_y));
    if(a[4*k+1] <= 0.f) nonpositive++;
    if(a[4*k+1] > 1e-5f)
      for(int c = 0; c < 3; c++)
        chromaticity = fmaxf(chromaticity, fabsf(b[4*k+c] * a[4*k+1]
                         - a[4*k+c] * b[4*k+1]) / fmaxf(1.f, fabsf(a[4*k+c] * b[4*k+1])));
    dt_aligned_pixel_t unmapped;
    dt_apply_transposed_color_matrix(b + 4*k, profile->matrix_out_transposed, unmapped);
    if(_gamut(unmapped, profile) < 0.99999f) mapped++;
    for(int c = 0; c < 3; c++)
    {
      const float values[4] = { pipe[4*k+c], local[4*k+c], native[4*k+c], buffered[4*k+c] };
      for(int method = 0; method < 4; method++)
      {
        _check(isfinite(values[method]), "finite output");
        _check(values[method] >= -3e-5f && values[method] <= 1.00003f, "working RGB gamut bounds");
      }
      difference = fmaxf(difference, fabsf(local[4*k+c] - pipe[4*k+c]));
      native_difference = fmaxf(native_difference, fabsf(native[4*k+c] - pipe[4*k+c]));
      buffered_difference = fmaxf(buffered_difference, fabsf(buffered[4*k+c] - pipe[4*k+c]));
    }
    _check(memcmp(in + 4*k+3, pipe + 4*k+3, sizeof(float)) == 0, "pipeline preserves alpha");
    _check(memcmp(in + 4*k+3, local + 4*k+3, sizeof(float)) == 0, "local preserves alpha");
    _check(memcmp(in + 4*k+3, native + 4*k+3, sizeof(float)) == 0, "RGB native preserves alpha");
    _check(memcmp(in + 4*k+3, buffered + 4*k+3, sizeof(float)) == 0, "buffered local preserves alpha");
  }
  for(int k = 0; k < 7; k++)
    for(int c = 0; c < 3; c++) neutral = fmaxf(neutral, fabsf(pipe[4*k+c] - pipe[4*k]));
  _check(neutral < 3e-5f, "neutral ramp");
  _check(difference < 3e-5f, "local versus pipeline");
  _check(native_difference < 3e-5f, "RGB native versus pipeline");
  _check(buffered_difference < 3e-5f, "buffered local versus pipeline");
  _check(luminance < 3e-5f, "luminance preserved by gamut mapping");
  _check(chromaticity < 3e-5f, "chromaticity preserved before gamut mapping");
  _check(mapped > 100 && nonpositive > 100, "exercise gamut and nonpositive policies");
  // D50 neutrality follows the profile white, rather than equal XYZ coordinates
  const float white_x = profile->matrix_in[0][0] + profile->matrix_in[0][1] + profile->matrix_in[0][2];
  const float white_y = profile->matrix_in[1][0] + profile->matrix_in[1][1] + profile->matrix_in[1][2];
  const float white_z = profile->matrix_in[2][0] + profile->matrix_in[2][1] + profile->matrix_in[2][2];
  _check(fabsf(white_x / white_y - 0.9642f) < 2e-4f, "D50 neutral X/Y");
  _check(fabsf(white_z / white_y - 0.8249f) < 2e-4f, "D50 neutral Z/Y");
  const float equal_xyz_spread = fmaxf(pipe[28], fmaxf(pipe[29], pipe[30]))
                             - fminf(pipe[28], fminf(pipe[29], pipe[30]));
  _check(equal_xyz_spread > 0.01f, "equal XYZ is not neutral");
  double lt[15], pt[15], nt[15], bt[15];
  for(int r = 0; r < 15; r++)
  {
    for(int s = 0; s < 4; s++)
    {
      const int method = (r + s) % 4;
      const double t = _seconds();
      switch(method)
      {
        case 0: _local(in, local, n, profile); lt[r] = _seconds() - t; break;
        case 1: _pipeline(in, pipe, a, b, n, profile); pt[r] = _seconds() - t; break;
        case 2: _rgb_native(in, native, n, profile); nt[r] = _seconds() - t; break;
        case 3: _buffered_local(in, buffered, a, b, n, profile); bt[r] = _seconds() - t; break;
      }
    }
  }
  printf("{\"profile\":\"%s\",\"pixels\":%d,\"mapped\":%d,\"nonpositive\":%d,"
         "\"max_path_difference\":%.9g,\"max_neutral_spread\":%.9g,"
         "\"max_Y_error\":%.9g,\"max_xy_crossproduct_error\":%.9g,"
         "\"max_rgb_native_difference\":%.9g,\"max_buffered_difference\":%.9g,"
         "\"equal_XYZ_rgb_spread\":%.9g,\"white_XYZ\":[%.9g,%.9g,%.9g],\"local_seconds\":[",
         name, n, mapped, nonpositive, difference, neutral, luminance, chromaticity,
         native_difference, buffered_difference, equal_xyz_spread, white_x, white_y, white_z);
  for(int r = 0; r < 15; r++) printf("%s%.9g", r ? "," : "", lt[r]);
  printf("],\"pipeline_seconds\":[");
  for(int r = 0; r < 15; r++) printf("%s%.9g", r ? "," : "", pt[r]);
  printf("],\"rgb_native_seconds\":[");
  for(int r = 0; r < 15; r++) printf("%s%.9g", r ? "," : "", nt[r]);
  printf("],\"buffered_local_seconds\":[");
  for(int r = 0; r < 15; r++) printf("%s%.9g", r ? "," : "", bt[r]);
  printf("]}\n");
  dt_free_align(in); dt_free_align(local); dt_free_align(pipe); dt_free_align(a); dt_free_align(b);
  dt_free_align(native);
  dt_free_align(buffered);
}

int main(int argc, char **argv)
{
  omp_set_num_threads(argc > 1 ? atoi(argv[1]) : 1);
  if(argc > 2) _pixels = atoi(argv[2]);
  _check(_pixels >= 12 && _pixels <= 4194304, "supported experiment size");
  darktable.color_profiles = &_registry;
  const cmsCIExyYTRIPLE rec2020 = {
    { 0.708, 0.292, 1.0 }, { 0.170, 0.797, 1.0 }, { 0.131, 0.046, 1.0 }
  };
  const cmsCIExyYTRIPLE rec709 = {
    { 0.640, 0.330, 1.0 }, { 0.300, 0.600, 1.0 }, { 0.150, 0.060, 1.0 }
  };
  _register(DT_COLORSPACE_LIN_REC2020, &rec2020);
  _register(DT_COLORSPACE_LIN_REC709, &rec709);
  _run("linear Rec2020", DT_COLORSPACE_LIN_REC2020);
  _run("linear Rec709", DT_COLORSPACE_LIN_REC709);
  for(GList *l = _dev.allprofile_info; l; l = l->next)
  {
    dt_ioppr_cleanup_profile_info(l->data);
    dt_free_align(l->data);
  }
  g_list_free(_dev.allprofile_info);
  for(GList *l = _registry.profiles; l; l = l->next)
  {
    dt_colorspaces_color_profile_t *p = l->data;
    cmsCloseProfile(p->profile);
    free(p);
  }
  g_list_free(_registry.profiles);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
