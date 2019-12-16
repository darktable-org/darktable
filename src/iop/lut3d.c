/*
    This file is part of darktable,
    copyright (c) 2019 Philippe Weyland

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
#include "common/imageio_png.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <libgen.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)

DT_MODULE_INTROSPECTION(1, dt_iop_lut3d_params_t)

typedef enum dt_iop_lut3d_colorspace_t
{
  DT_IOP_SRGB = 0,
  DT_IOP_REC709 = 1,
  DT_IOP_LIN_REC709 = 2,
  DT_IOP_LIN_REC2020 = 3,
} dt_iop_lut3d_colorspace_t;

typedef enum dt_iop_lut3d_interpolation_t
{
  DT_IOP_TETRAHEDRAL = 0,
  DT_IOP_TRILINEAR = 1,
  DT_IOP_PYRAMID = 2,
} dt_iop_lut3d_interpolation_t;

typedef struct dt_iop_lut3d_params_t
{
  char filepath[512];
  int colorspace;
  int interpolation;
} dt_iop_lut3d_params_t;

typedef struct dt_iop_lut3d_gui_data_t
{
  GtkWidget *hbox;
  GtkWidget *filepath;
  GtkWidget *colorspace;
  GtkWidget *interpolation;
} dt_iop_lut3d_gui_data_t;

typedef struct dt_iop_lut3d_data_t
{
  dt_iop_lut3d_params_t params;
  float *clut;  // cube lut pointer
  uint16_t level; // cube_size
} dt_iop_lut3d_data_t;

typedef struct dt_iop_lut3d_global_data_t
{
  int kernel_lut3d_tetrahedral;
  int kernel_lut3d_trilinear;
  int kernel_lut3d_pyramid;
  int kernel_lut3d_none;
} dt_iop_lut3d_global_data_t;

const char *name()
{
  return _("lut 3D");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

// From `HaldCLUT_correct.c' by Eskil Steenberg (http://www.quelsolaar.com) (BSD licensed)
void correct_pixel_trilinear(const float *const in, float *const out,
                             const size_t pixel_nb, const float *const restrict clut, const uint16_t level)
{
  const int level2 = level * level;
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(clut, in, level, level2, out, pixel_nb) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k+=4)
  {
    float *input = ((float *)in) + k;
    float *output = ((float *)out) + k;

    int rgbi[3], i, j;
    float tmp[6];
    float rgbd[3];

    for(int c = 0; c < 3; ++c) input[c] = fminf(fmaxf(input[c], 0.0f), 1.0f);

    rgbd[0] = input[0] * (float)(level - 1);
    rgbd[1] = input[1] * (float)(level - 1);
    rgbd[2] = input[2] * (float)(level - 1);

    rgbi[0] = CLAMP((int)rgbd[0], 0, level - 2);
    rgbi[1] = CLAMP((int)rgbd[1], 0, level - 2);
    rgbi[2] = CLAMP((int)rgbd[2], 0, level - 2);

    rgbd[0] = rgbd[0] - rgbi[0]; // delta red
    rgbd[1] = rgbd[1] - rgbi[1]; // delta green
    rgbd[2] = rgbd[2] - rgbi[2]; // delta blue

    // indexes of P000 to P111 in clut

    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level * level;
    i = color * 3;  // P000
    j = (color + 1) * 3;  // P100

    tmp[0] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[1] = clut[i+1] * (1 - rgbd[0]) + clut[j+1] * rgbd[0];
    tmp[2] = clut[i+2] * (1 - rgbd[0]) + clut[j+2] * rgbd[0];

    i = (color + level) * 3;  // P010
    j = (color + level + 1) * 3;  //P110

    tmp[3] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[4] = clut[i+1] * (1 - rgbd[0]) + clut[j+1] * rgbd[0];
    tmp[5] = clut[i+2] * (1 - rgbd[0]) + clut[j+2] * rgbd[0];

    output[0] = tmp[0] * (1 - rgbd[1]) + tmp[3] * rgbd[1];
    output[1] = tmp[1] * (1 - rgbd[1]) + tmp[4] * rgbd[1];
    output[2] = tmp[2] * (1 - rgbd[1]) + tmp[5] * rgbd[1];

    i = (color + level2) * 3;  // P001
    j = (color + level2 + 1) * 3;  // P101

    tmp[0] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[1] = clut[i+1] * (1 - rgbd[0]) + clut[j+1] * rgbd[0];
    tmp[2] = clut[i+2] * (1 - rgbd[0]) + clut[j+2] * rgbd[0];

    i = (color + level + level2) * 3;  // P011
    j = (color + level + level2 + 1) * 3;  // P111

    tmp[3] = clut[i] * (1 - rgbd[0]) + clut[j] * rgbd[0];
    tmp[4] = clut[i+1] * (1 - rgbd[0]) + clut[j+1] * rgbd[0];
    tmp[5] = clut[i+2] * (1 - rgbd[0]) + clut[j+2] * rgbd[0];

    tmp[0] = tmp[0] * (1 - rgbd[1]) + tmp[3] * rgbd[1];
    tmp[1] = tmp[1] * (1 - rgbd[1]) + tmp[4] * rgbd[1];
    tmp[2] = tmp[2] * (1 - rgbd[1]) + tmp[5] * rgbd[1];

    output[0] = output[0] * (1 - rgbd[2]) + tmp[0] * rgbd[2];
    output[1] = output[1] * (1 - rgbd[2]) + tmp[1] * rgbd[2];
    output[2] = output[2] * (1 - rgbd[2]) + tmp[2] * rgbd[2];
 }
}

// from OpenColorIO
// https://github.com/imageworks/OpenColorIO/blob/master/src/OpenColorIO/ops/Lut3D/Lut3DOp.cpp
void correct_pixel_tetrahedral(const float *const in, float *const out,
                               const size_t pixel_nb, const float *const restrict clut, const uint16_t level)
{
  const int level2 = level * level;
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(clut, in, level, level2, out, pixel_nb) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k+=4)
  {
    float *const input = ((float *const)in) + k;
    float *const output = ((float *const)out) + k;
    int rgbi[3];

    float rgbd[3];
    for(int c = 0; c < 3; ++c) input[c] = fminf(fmaxf(input[c], 0.0f), 1.0f);

    rgbd[0] = input[0] * (float)(level - 1);
    rgbd[1] = input[1] * (float)(level - 1);
    rgbd[2] = input[2] * (float)(level - 1);

    rgbi[0] = CLAMP((int)rgbd[0], 0, level - 2);
    rgbi[1] = CLAMP((int)rgbd[1], 0, level - 2);
    rgbi[2] = CLAMP((int)rgbd[2], 0, level - 2);

    rgbd[0] = rgbd[0] - rgbi[0]; // delta red
    rgbd[1] = rgbd[1] - rgbi[1]; // delta green
    rgbd[2] = rgbd[2] - rgbi[2]; // delta blue

    // indexes of P000 to P111 in clut
    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level * level;
    const int i000 = color * 3;                     // P000
    const int i100 = i000 + 3;                      // P100
    const int i010 = (color + level) * 3;           // P010
    const int i110 = i010 + 3;                      // P110
    const int i001 = (color + level2) * 3;          // P001
    const int i101 = i001 + 3;                      // P101
    const int i011 = (color + level + level2) * 3;  // P011
    const int i111 = i011 + 3;                      // P111

    if (rgbd[0] > rgbd[1])
    {
      if (rgbd[1] > rgbd[2])
      {
        output[0] = (1-rgbd[0])*clut[i000] + (rgbd[0]-rgbd[1])*clut[i100] + (rgbd[1]-rgbd[2])*clut[i110] + rgbd[2]*clut[i111];
        output[1] = (1-rgbd[0])*clut[i000+1] + (rgbd[0]-rgbd[1])*clut[i100+1] + (rgbd[1]-rgbd[2])*clut[i110+1] + rgbd[2]*clut[i111+1];
        output[2] = (1-rgbd[0])*clut[i000+2] + (rgbd[0]-rgbd[1])*clut[i100+2] + (rgbd[1]-rgbd[2])*clut[i110+2] + rgbd[2]*clut[i111+2];
      }
      else if (rgbd[0] > rgbd[2])
      {
        output[0] = (1-rgbd[0])*clut[i000] + (rgbd[0]-rgbd[2])*clut[i100] + (rgbd[2]-rgbd[1])*clut[i101] + rgbd[1]*clut[i111];
        output[1] = (1-rgbd[0])*clut[i000+1] + (rgbd[0]-rgbd[2])*clut[i100+1] + (rgbd[2]-rgbd[1])*clut[i101+1] + rgbd[1]*clut[i111+1];
        output[2] = (1-rgbd[0])*clut[i000+2] + (rgbd[0]-rgbd[2])*clut[i100+2] + (rgbd[2]-rgbd[1])*clut[i101+2] + rgbd[1]*clut[i111+2];
      }
      else
      {
        output[0] = (1-rgbd[2])*clut[i000] + (rgbd[2]-rgbd[0])*clut[i001] + (rgbd[0]-rgbd[1])*clut[i101] + rgbd[1]*clut[i111];
        output[1] = (1-rgbd[2])*clut[i000+1] + (rgbd[2]-rgbd[0])*clut[i001+1] + (rgbd[0]-rgbd[1])*clut[i101+1] + rgbd[1]*clut[i111+1];
        output[2] = (1-rgbd[2])*clut[i000+2] + (rgbd[2]-rgbd[0])*clut[i001+2] + (rgbd[0]-rgbd[1])*clut[i101+2] + rgbd[1]*clut[i111+2];
      }
    }
    else
    {
      if (rgbd[2] > rgbd[1])
      {
        output[0] = (1-rgbd[2])*clut[i000] + (rgbd[2]-rgbd[1])*clut[i001] + (rgbd[1]-rgbd[0])*clut[i011] + rgbd[0]*clut[i111];
        output[1] = (1-rgbd[2])*clut[i000+1] + (rgbd[2]-rgbd[1])*clut[i001+1] + (rgbd[1]-rgbd[0])*clut[i011+1] + rgbd[0]*clut[i111+1];
        output[2] = (1-rgbd[2])*clut[i000+2] + (rgbd[2]-rgbd[1])*clut[i001+2] + (rgbd[1]-rgbd[0])*clut[i011+2] + rgbd[0]*clut[i111+2];
      }
      else if (rgbd[2] > rgbd[0])
      {
        output[0] = (1-rgbd[1])*clut[i000] + (rgbd[1]-rgbd[2])*clut[i010] + (rgbd[2]-rgbd[0])*clut[i011] + rgbd[0]*clut[i111];
        output[1] = (1-rgbd[1])*clut[i000+1] + (rgbd[1]-rgbd[2])*clut[i010+1] + (rgbd[2]-rgbd[0])*clut[i011+1] + rgbd[0]*clut[i111+1];
        output[2] = (1-rgbd[1])*clut[i000+2] + (rgbd[1]-rgbd[2])*clut[i010+2] + (rgbd[2]-rgbd[0])*clut[i011+2] + rgbd[0]*clut[i111+2];
      }
      else
      {
        output[0] = (1-rgbd[1])*clut[i000] + (rgbd[1]-rgbd[0])*clut[i010] + (rgbd[0]-rgbd[2])*clut[i110] + rgbd[2]*clut[i111];
        output[1] = (1-rgbd[1])*clut[i000+1] + (rgbd[1]-rgbd[0])*clut[i010+1] + (rgbd[0]-rgbd[2])*clut[i110+1] + rgbd[2]*clut[i111+1];
        output[2] = (1-rgbd[1])*clut[i000+2] + (rgbd[1]-rgbd[0])*clut[i010+2] + (rgbd[0]-rgbd[2])*clut[i110+2] + rgbd[2]*clut[i111+2];
      }
    }
  }
}

// from Study on the 3D Interpolation Models Used in Color Conversion
// http://ijetch.org/papers/318-T860.pdf
void correct_pixel_pyramid(const float *const in, float *const out,
                           const size_t pixel_nb, const float *const restrict clut, const uint16_t level)
{
  const int level2 = level * level;
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(clut, in, level, level2, out, pixel_nb) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)(pixel_nb * 4); k+=4)
  {
    float *const input = ((float *const)in) + k;
    float *const output = ((float *const)out) + k;

    int rgbi[3];
    float rgbd[3];

    for(int c = 0; c < 3; ++c) input[c] = fminf(fmaxf(input[c], 0.0f), 1.0f);

    rgbd[0] = input[0] * (float)(level - 1);
    rgbd[1] = input[1] * (float)(level - 1);
    rgbd[2] = input[2] * (float)(level - 1);

    rgbi[0] = CLAMP((int)rgbd[0], 0, level - 2);
    rgbi[1] = CLAMP((int)rgbd[1], 0, level - 2);
    rgbi[2] = CLAMP((int)rgbd[2], 0, level - 2);

    rgbd[0] = rgbd[0] - rgbi[0]; // delta red
    rgbd[1] = rgbd[1] - rgbi[1]; // delta green
    rgbd[2] = rgbd[2] - rgbi[2]; // delta blue

    // indexes of P000 to P111 in clut
    const int color = rgbi[0] + rgbi[1] * level + rgbi[2] * level * level;
    const int i000 = color * 3;                     // P000
    const int i100 = i000 + 3;                      // P100
    const int i010 = (color + level) * 3;           // P010
    const int i110 = i010 + 3;                      // P110
    const int i001 = (color + level2) * 3;          // P001
    const int i101 = i001 + 3;                      // P101
    const int i011 = (color + level + level2) * 3;  // P011
    const int i111 = i011 + 3;                      // P111

    if (rgbd[1] > rgbd[0] && rgbd[2] > rgbd[0])
    {
      output[0] = clut[i000] + (clut[i111]-clut[i011])*rgbd[0] + (clut[i010]-clut[i000])*rgbd[1] + (clut[i001]-clut[i000])*rgbd[2]
        + (clut[i011]-clut[i001]-clut[i010]+clut[i000])*rgbd[1]*rgbd[2];
      output[1] = clut[i000+1] + (clut[i111+1]-clut[i011+1])*rgbd[0] + (clut[i010+1]-clut[i000+1])*rgbd[1] + (clut[i001+1]-clut[i000+1])*rgbd[2]
        + (clut[i011+1]-clut[i001+1]-clut[i010+1]+clut[i000+1])*rgbd[1]*rgbd[2];
      output[2] = clut[i000+2] + (clut[i111+2]-clut[i011+2])*rgbd[0] + (clut[i010+2]-clut[i000+2])*rgbd[1] + (clut[i001+2]-clut[i000+2])*rgbd[2]
        + (clut[i011+2]-clut[i001+2]-clut[i010+2]+clut[i000+2])*rgbd[1]*rgbd[2];
    }
    else if (rgbd[0] > rgbd[1] && rgbd[2] > rgbd[1])
    {
      output[0] = clut[i000] + (clut[i100]-clut[i000])*rgbd[0] + (clut[i111]-clut[i101])*rgbd[1] + (clut[i001]-clut[i000])*rgbd[2]
        + (clut[i101]-clut[i001]-clut[i100]+clut[i000])*rgbd[0]*rgbd[2];
      output[1] = clut[i000+1] + (clut[i100+1]-clut[i000+1])*rgbd[0] + (clut[i111+1]-clut[i101+1])*rgbd[1] + (clut[i001+1]-clut[i000+1])*rgbd[2]
        + (clut[i101+1]-clut[i001+1]-clut[i100+1]+clut[i000+1])*rgbd[0]*rgbd[2];
      output[2] = clut[i000+2] + (clut[i100+2]-clut[i000+2])*rgbd[0] + (clut[i111]-clut[i101+2])*rgbd[1] + (clut[i001+2]-clut[i000+2])*rgbd[2]
        + (clut[i101+2]-clut[i001+2]-clut[i100+2]+clut[i000+2])*rgbd[0]*rgbd[2];
    }
    else
    {
      output[0] = clut[i000] + (clut[i100]-clut[i000])*rgbd[0] + (clut[i010]-clut[i000])*rgbd[1] + (clut[i111]-clut[i110])*rgbd[2]
        + (clut[i110]-clut[i100]-clut[i010]+clut[i000])*rgbd[0]*rgbd[1];
      output[1] = clut[i000+1] + (clut[i100+1]-clut[i000+1])*rgbd[0] + (clut[i010+1]-clut[i000+1])*rgbd[1] + (clut[i111+1]-clut[i110+1])*rgbd[2]
        + (clut[i110+1]-clut[i100+1]-clut[i010+1]+clut[i000+1])*rgbd[0]*rgbd[1];
      output[2] = clut[i000+2] + (clut[i100+2]-clut[i000+2])*rgbd[0] + (clut[i010+2]-clut[i000+2])*rgbd[1] + (clut[i111+2]-clut[i110+2])*rgbd[2]
        + (clut[i110+2]-clut[i100+2]-clut[i010+2]+clut[i000+2])*rgbd[0]*rgbd[1];
    }
  }
}

uint16_t calculate_clut_haldclut(char *filepath, float **clut)
{
  dt_imageio_png_t png;
  if(read_header(filepath, &png))
  {
    fprintf(stderr, "[lut3d] invalid png header from %s\n", filepath);
    dt_control_log(_("invalid png header from %s"), filepath);
    return 0;
  }
  dt_print(DT_DEBUG_DEV, "[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);
  if (png.bit_depth !=8 && png.bit_depth != 16)
  {
    fprintf(stderr, "[lut3d] png bit-depth %d not supported\n", png.bit_depth);
    dt_control_log(_("png bit-depth %d not supported"), png.bit_depth);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }

  uint16_t level = 2;
  while(level * level * level < png.width) ++level;

  if(level * level * level != png.width)
  {
    fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
    dt_control_log(_("invalid level in png file %d %d"), level, png.width);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  level *= level;  // to be equivalent to cube level
  if(level > 256)
  {
    fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
    dt_control_log(_("error - lut 3D size %d exceeds the maximum supported"), level);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  const size_t buf_size = (size_t)png.height * png_get_rowbytes(png.png_ptr, png.info_ptr);
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for png file\n", buf_size);
  uint8_t *buf = NULL;
  buf = dt_alloc_align(16, buf_size);
  if(!buf)
  {
    fprintf(stderr, "[lut3d] error - allocating buffer for png lut\n");
    dt_control_log(_("error - allocating buffer for png lut"));
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  if (read_image(&png, buf))
  {
    fprintf(stderr, "[lut3d] error - could not read png image `%s'\n", filepath);
    dt_control_log(_("error - could not read png image %s"), filepath);
    dt_free_align(buf);
    return 0;
  }
  const size_t buf_size_lut = (size_t)png.height * png.height * 3;
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu floats for png lut - level %d\n", buf_size_lut, level);
  float *lclut = dt_alloc_align(16, buf_size_lut * sizeof(float));
  if(!lclut)
  {
    fprintf(stderr, "[lut3d] error - allocating buffer for png lut\n");
    dt_control_log(_("error - allocating buffer for png lut"));
    dt_free_align(buf);
    return 0;
  }
  const float norm = 1.0f / (powf(2.f, png.bit_depth) - 1.0f);
  if (png.bit_depth == 8)
  {
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (float)buf[i] * norm;
  }
  else
  {
    for (size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (256.0f * (float)buf[2*i] + (float)buf[2*i+1]) * norm;
  }
  dt_free_align(buf);
  *clut = lclut;
  return level;
}

// provided by @rabauke, atof replaces strtod & sccanf which are locale dependent
double dt_atof(const char *str)
{
  if (strncmp(str, "nan", 3) == 0 || strncmp(str, "NAN", 3) == 0)
    return NAN;
  double integral_result = 0;
  double fractional_result = 0;
  double sign = 1;

  if (*str == '+')
  {
    str++;
    sign = +1;
  }
  else if (*str == '-')
  {
    str++;
    sign = -1;
  }
  if (strncmp(str, "inf", 3) == 0 || strncmp(str, "INF", 3) == 0)
    return sign * INFINITY;

  // search for end of integral part and parse from
  // right to left for numerical stability
  const char * istr_back = str;
  while (*str >= '0' && *str <= '9')
    str++;
  const char * istr_2 = str;
  double imultiplier = 1;

  while (istr_2 != istr_back)
  {
    --istr_2;
    integral_result += (*istr_2 - '0') * imultiplier;
    imultiplier *= 10;
  }

  if (*str == '.')
  {
    str++;
    // search for end of fractional part and parse from
    // right to left for numerical stability
    const char * fstr_back = str;
    while (*str >= '0' && *str <= '9')
      str++;
    const char * fstr_2 = str;
    double fmultiplier = 1;

    while (fstr_2 != fstr_back)
    {
      --fstr_2;
      fractional_result += (*fstr_2 - '0') * fmultiplier;
      fmultiplier *= 10;
    }
    fractional_result /= fmultiplier;
  }
  double result = sign * (integral_result + fractional_result);
  if (*str == 'e' || *str == 'E')
  {
    str++;
    double power_sign = 1;
    if (*str == '+')
    {
      str++;
      power_sign = +1;
    }
    else if (*str == '-')
    {
      str++;
      power_sign = -1;
    }
    double power = 0;
    while (*str >= '0' && *str <= '9')
    {
      power *= 10;
      power += *str - '0';
      str++;
    }
    if (power_sign > 0)
      result *= pow(10, power);
    else
      result /= pow(10, power);
  }
  return result;
}

// return max 3 tokens from the line (separator = ' ' and token length = 50)
// todo get the LUT name (between " ")
uint8_t parse_cube_line(char *line, char *token)
{
  uint8_t i, c;
  i = c = 0;
  char *t = token;
  char *l = line;

  while (*l != 0 && c < 3 && i < 50)
  {
    if (*l == '#' || *l == '\n' || *l == '\r')
    { // end of useful part of the line
      if (i > 0)
      {
        *t = 0;
        c++;
        return c;
      }
      else
      {
        *t = 0;
        return c;
      }
    }
    if (*l == ' ' || *l == '\t')
    { // separator
      if (i > 0)
      {
        *t = 0;
        c++;
        i = 0;
        t = token + (50 * c);
      }
    }
    else
    { // capture info
      *t = *l;
      t++;
      i++;
    }
    l++;
  }
  return c;
}

uint16_t calculate_clut_cube(char *filepath, float **clut)
{
  FILE *cube_file;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint16_t level = 0;
  float *lclut = NULL;
  uint32_t i = 0;
  size_t buf_size = 0;

  if(!(cube_file = g_fopen(filepath, "r")))
  {
    fprintf(stderr, "[lut3d] invalid cube file: %s\n", filepath);
    dt_control_log(_("error - invalid cube file: %s"), filepath);
    return 0;
  }

  while ((read = getline(&line, &len, cube_file)) != -1)
  {
    uint8_t nb_token = parse_cube_line(line, &token[0][0]);
    if (nb_token)
    {
      if (token[0][0] == 'T')
        continue;
      else if (strcmp("DOMAIN_MIN", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 0.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MIN <> 0.0 is not supported\n");
          dt_control_log(_("DOMAIN MIN <> 0.0 is not supported"));
          if (lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
      }
      else if (strcmp("DOMAIN_MAX", token[0]) == 0)
      {
        if (strtod(token[1], NULL) != 1.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MAX <> 1.0 is not supported\n");
          dt_control_log(_("DOMAIN MAX <> 1.0 is not supported"));
          if (lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
      }
      else if (strcmp("LUT_1D_SIZE", token[0]) == 0)
      {
        fprintf(stderr, "[lut3d] 1D cube lut is not supported\n");
        dt_control_log(_("1D cube lut is not supported"));
        free(line);
        fclose(cube_file);
        return 0;
      }
      else if (strcmp("LUT_3D_SIZE", token[0]) == 0)
      {
        level = atoll(token[1]);
        if(level > 256)
        {
          fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
          dt_control_log(_("error - lut 3D size %d exceeds the maximum supported"), level);
          free(line);
          fclose(cube_file);
          return 0;
        }
        buf_size = level * level * level * 3;
        dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for cube lut - level %d\n", buf_size, level);
        lclut = dt_alloc_align(16, buf_size * sizeof(float));
        if(!lclut)
        {
          fprintf(stderr, "[lut3d] error - allocating buffer for cube lut\n");
          dt_control_log(_("error - allocating buffer for cube lut"));
          free(line);
          fclose(cube_file);
          return 0;
        }
      }
      else
      {
        if (!level)
        {
          fprintf(stderr, "[lut3d] error - cube lut size is not defined\n");
          dt_control_log(_("error - cube lut size is not defined"));
          free(line);
          fclose(cube_file);
          return 0;
        }
        for (int j=0; j < 3; j++)
        {
          lclut[i+j] = dt_atof(token[j]);
        }
        i += 3;
      }
    }
  }

  if (i != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - cube lut lines number is not correct\n");
    dt_control_log(_("error - cube lut lines number is not correct"));
    dt_free_align(lclut);
    free(line);
    fclose(cube_file);
    return 0;
  }
  *clut = lclut;
  free(line);
  fclose(cube_file);
  return level;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  dt_iop_lut3d_global_data_t *gd = (dt_iop_lut3d_global_data_t *)self->global_data;
  cl_int err = CL_SUCCESS;
  const float *const clut = (float *)d->clut;
  const int level = d->level;
  const int kernel = (d->params.interpolation == DT_IOP_TETRAHEDRAL) ? gd->kernel_lut3d_tetrahedral
    : (d->params.interpolation == DT_IOP_TRILINEAR) ? gd->kernel_lut3d_trilinear
    : gd->kernel_lut3d_pyramid;
  const int colorspace
    = (d->params.colorspace == DT_IOP_SRGB) ? DT_COLORSPACE_SRGB
    : (d->params.colorspace == DT_IOP_REC709) ? DT_COLORSPACE_REC709
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  gboolean transform = (work_profile != NULL && lut_profile != NULL) ? TRUE : FALSE;
  cl_mem clut_cl = NULL;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if (clut && level)
  {
    clut_cl = dt_opencl_copy_host_to_device_constant(devid, level * level * level * 3 * sizeof(float), (void *)clut);
    if(clut_cl == NULL)
    {
      fprintf(stderr, "[lut3d process_cl] error allocating memory\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    if (transform)
    {
      const int success = dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_in, dev_out, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      if (!success)
       transform = FALSE;
    }
    if (transform)
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_out);
    else
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&clut_cl);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&level);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if (transform)
      dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_out, dev_out, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
  }
  else
  { // no lut: identity kernel
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lut3d_none, 3, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lut3d_none, sizes);
  }
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[lut3d process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  if(clut_cl) dt_opencl_release_mem_object(clut_cl);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_lut3d] couldn't enqueue kernel! %d\n", err);
  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ibuf, void *const obuf,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  const float *const clut = (float *)d->clut;
  const uint16_t level = d->level;
  const int interpolation = d->params.interpolation;
  const int colorspace
    = (d->params.colorspace == DT_IOP_SRGB) ? DT_COLORSPACE_SRGB
    : (d->params.colorspace == DT_IOP_REC709) ? DT_COLORSPACE_REC709
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  const gboolean transform = (work_profile != NULL && lut_profile != NULL) ? TRUE : FALSE;
  if (clut)
  {
    if (transform)
    {
      dt_ioppr_transform_image_colorspace_rgb(ibuf, obuf, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      if (interpolation == DT_IOP_TETRAHEDRAL)
        correct_pixel_tetrahedral(obuf, obuf, width * height, clut, level);
      else if (interpolation == DT_IOP_TRILINEAR)
        correct_pixel_trilinear(obuf, obuf, width * height, clut, level);
      else
        correct_pixel_pyramid(obuf, obuf, width * height, clut, level);
      dt_ioppr_transform_image_colorspace_rgb(obuf, obuf, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
    }
    else
    {
      if (interpolation == DT_IOP_TETRAHEDRAL)
        correct_pixel_tetrahedral(ibuf, obuf, width * height, clut, level);
      else if (interpolation == DT_IOP_TRILINEAR)
        correct_pixel_trilinear(ibuf, obuf, width * height, clut, level);
      else
        correct_pixel_pyramid(ibuf, obuf, width * height, clut, level);
    }
  }
  else  // no clut
  {
    memcpy(obuf, ibuf, width * height * ch * sizeof(float));
  }
}

void filepath_set_unix_separator(char *filepath)
{ // use the unix separator as it works also on windows
  const int len = strlen(filepath);
  for(int i=0; i<len; ++i)
    if (filepath[i]=='\\') filepath[i] = '/';
}

void init(dt_iop_module_t *self)
{
  self->global_data = NULL;
  self->params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_lut3d_params_t));
  self->default_enabled = 0;
  self->params_size = sizeof(dt_iop_lut3d_params_t);
  self->gui_data = NULL;
  dt_iop_lut3d_params_t tmp = (dt_iop_lut3d_params_t)
    {
      { "" },
      DT_IOP_SRGB,
      DT_IOP_TETRAHEDRAL
    };
  memcpy(self->params, &tmp, sizeof(dt_iop_lut3d_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_lut3d_params_t));
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 28; // rgbcurve.cl, from programs.conf
  dt_iop_lut3d_global_data_t *gd
      = (dt_iop_lut3d_global_data_t *)malloc(sizeof(dt_iop_lut3d_global_data_t));
  module->data = gd;
  gd->kernel_lut3d_tetrahedral = dt_opencl_create_kernel(program, "lut3d_tetrahedral");
  gd->kernel_lut3d_trilinear = dt_opencl_create_kernel(program, "lut3d_trilinear");
  gd->kernel_lut3d_pyramid = dt_opencl_create_kernel(program, "lut3d_pyramid");
  gd->kernel_lut3d_none = dt_opencl_create_kernel(program, "lut3d_none");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lut3d_global_data_t *gd = (dt_iop_lut3d_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_lut3d_tetrahedral);
  dt_opencl_free_kernel(gd->kernel_lut3d_trilinear);
  dt_opencl_free_kernel(gd->kernel_lut3d_pyramid);
  dt_opencl_free_kernel(gd->kernel_lut3d_none);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)p1;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  if (strcmp(p->filepath, d->params.filepath) != 0)
  {
    if (d->clut)
    {
      dt_free_align(d->clut);
      d->clut = NULL;
      d->level = 0;
    }
  }
  memcpy(&d->params, p, sizeof(dt_iop_lut3d_params_t));
  const gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");

  if (p->filepath[0] && lutfolder[0] && !d->clut)
  {
    char *fullpath = g_build_filename(lutfolder, p->filepath, NULL);
    if (g_str_has_suffix (p->filepath, ".png") || g_str_has_suffix (p->filepath, ".PNG"))
    {
      d->level = calculate_clut_haldclut(fullpath, &d->clut);
    }
    else if (g_str_has_suffix (p->filepath, ".cube") || g_str_has_suffix (p->filepath, ".CUBE"))
    {
      d->level = calculate_clut_cube(fullpath, &d->clut);
    }
    g_free(fullpath);
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  piece->data = malloc(sizeof(dt_iop_lut3d_data_t));
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  d->clut = NULL;
  d->level = 0;
  d->params.filepath[0] = '\0';
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  if (d->clut)
  {
    dt_free_align(d->clut);
    d->clut = NULL;
  }
  free(piece->data);
  piece->data = NULL;
}

static void filepath_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  snprintf(p->filepath, sizeof(p->filepath), "%s", dt_bauhaus_combobox_get_text(widget));
  filepath_set_unix_separator(p->filepath);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorspace_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->colorspace = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void interpolation_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  p->interpolation = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// remove root lut folder from path
static void remove_root_from_path(const char *const lutfolder, char *const filepath)
{
  const int j = strlen(lutfolder) + 1;
  int i;
  for(i = 0; filepath[i+j] != '\0'; i++)
    filepath[i] = filepath[i+j];
  filepath[i] = '\0';
}

gboolean check_same_extension(char *filename, char *ext)
{
  gboolean res = FALSE;
  if (strlen(filename) < strlen(ext)) return res;  // crash Pascal
  char *p = g_strrstr(filename,".");
  if (!p) return res;
  char *fext = g_ascii_strdown(g_strdup(p), -1);
  if (!g_strcmp0(fext, ext)) res = TRUE;
  g_free(fext);
  return res;
}

static gint list_str_cmp(gconstpointer a, gconstpointer b)
{
  return g_strcmp0(((dt_bauhaus_combobox_entry_t *)a)->label, ((dt_bauhaus_combobox_entry_t *)b)->label);
}

// update filepath combobox with all files in the current folder
static void update_filepath_combobox(dt_iop_lut3d_gui_data_t *g, char *filepath, char *lutfolder)
{
  if (!filepath[0])
    dt_bauhaus_combobox_clear(g->filepath);
  else if (!dt_bauhaus_combobox_set_from_text(g->filepath, filepath))
  { // new folder -> update the files list
    char *relativepath = g_path_get_dirname(filepath);
    char *folder = g_build_filename(lutfolder, relativepath, NULL);
    struct dirent *dir;
    DIR *d = opendir(folder);
    if (d)
    {
      dt_bauhaus_combobox_clear(g->filepath);
      char *ext = g_ascii_strdown(g_strdup(g_strrstr(filepath,".")), -1);
      if (ext && ext[0])
      {
        while ((dir = readdir(d)) != NULL)
        {
          char *file = dir->d_name;
          if (check_same_extension(file, ext))
          {
            char *ofilepath = (strcmp(relativepath, ".") != 0)
                  ? g_build_filename(relativepath, file, NULL)
                  : g_strdup(file);
            filepath_set_unix_separator(ofilepath);
            dt_bauhaus_combobox_add(g->filepath, ofilepath);
            g_free(ofilepath);
          }
        }
        dt_bauhaus_widget_t *w = DT_BAUHAUS_WIDGET(g->filepath);
        dt_bauhaus_combobox_data_t *combo_data = &w->data.combobox;
        combo_data->entries = g_list_sort(combo_data->entries, list_str_cmp);
      }
      g_free(ext);
      closedir(d);
    }
    dt_bauhaus_combobox_set_from_text(g->filepath, filepath);
    g_free(relativepath);
    g_free(folder);
  }
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  int filetype;
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar* lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if (strlen(lutfolder) == 0)
  {
    fprintf(stderr, "[lut3d] Lut root folder not defined\n");
    dt_control_log(_("Lut root folder not defined"));
    g_free(lutfolder);
    return;
  }
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("select lut file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_select"), GTK_RESPONSE_ACCEPT, (char *)NULL);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  char *composed = g_build_filename(lutfolder, p->filepath, NULL);
  if (strlen(p->filepath) == 0 || access(composed, F_OK) == -1)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), lutfolder);
  else
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), composed);
  g_free(composed);

  filetype = 1;
  if (p->filepath[0] && (g_str_has_suffix (p->filepath, ".cube") || g_str_has_suffix (p->filepath, ".CUBE")))
    filetype = 2;

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.png");
  gtk_file_filter_add_pattern(filter, "*.PNG");
  gtk_file_filter_set_name(filter, _("hald cluts (png)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  if (filetype == 1) gtk_file_chooser_set_filter (GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_add_pattern(filter, "*.CUBE");
  gtk_file_filter_set_name(filter, _("3D lut (cube)"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  if (filetype == 2) gtk_file_chooser_set_filter (GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    if (strcmp(lutfolder, filepath) < 0)
    {
      remove_root_from_path(lutfolder, filepath);
      filepath_set_unix_separator(filepath);
      update_filepath_combobox(g, filepath, lutfolder);
    }
    else if (!filepath[0])// file chosen outside of root folder
    {
      fprintf(stderr, "[lut3d] Select file outside Lut root folder is not allowed\n");
      dt_control_log(_("Select file outside Lut root folder is not allowed"));
    }
    g_free(filepath);
    gtk_widget_set_sensitive(g->filepath, p->filepath[0]);
  }
  g_free(lutfolder);
  gtk_widget_destroy(filechooser);
}

void gui_reset(dt_iop_module_t *self)
{
  memcpy(self->params, self->default_params, sizeof(dt_iop_lut3d_params_t));
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if (!lutfolder[0])
  {
    gtk_widget_set_sensitive(g->hbox, FALSE);
    gtk_widget_set_sensitive(g->filepath, FALSE);
    dt_bauhaus_combobox_clear(g->filepath);
  }
  else
  {
    gtk_widget_set_sensitive(g->hbox, TRUE);
    gtk_widget_set_sensitive(g->filepath, p->filepath[0]);
    update_filepath_combobox(g, p->filepath, lutfolder);
  }
  dt_bauhaus_combobox_set(g->colorspace, p->colorspace);
  dt_bauhaus_combobox_set(g->interpolation, p->interpolation);
  g_free(lutfolder);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lut3d_gui_data_t));
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(button, DT_PIXEL_APPLY_DPI(18), DT_PIXEL_APPLY_DPI(18));
  gtk_widget_set_tooltip_text(button, _("select a png (haldclut) or a cube file"));
  gtk_box_pack_start(GTK_BOX(g->hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  g->filepath = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(g->hbox), g->filepath, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->filepath,
                              _("the file path (relative to lut folder) is saved with image (and not the lut data themselves)\n"
                                "CAUTION: lut folder must be set in preferences/core options/miscellaneous before choosing the lut file"));
  g_signal_connect(G_OBJECT(g->filepath), "value-changed", G_CALLBACK(filepath_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox), TRUE, TRUE, 0);

  g->colorspace = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->colorspace, NULL, _("application color space"));
  dt_bauhaus_combobox_add(g->colorspace, _("sRGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("gamma rec709 RGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("linear rec709 RGB"));
  dt_bauhaus_combobox_add(g->colorspace, _("linear rec2020 RGB"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->colorspace) , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->colorspace, _("select the color space in which the LUT has to be applied"));
  g_signal_connect(G_OBJECT(g->colorspace), "value-changed", G_CALLBACK(colorspace_callback), self);

  g->interpolation = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->interpolation, NULL, _("interpolation"));
  dt_bauhaus_combobox_add(g->interpolation, _("tetrahedral"));
  dt_bauhaus_combobox_add(g->interpolation, _("trilinear"));
  dt_bauhaus_combobox_add(g->interpolation, _("pyramid"));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->interpolation) , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->interpolation, _("select the interpolation method"));
  g_signal_connect(G_OBJECT(g->interpolation), "value-changed", G_CALLBACK(interpolation_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
