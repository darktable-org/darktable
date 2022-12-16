/*
    This file is part of darktable,
    Copyright (C) 2019-2022 darktable developers.

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
#include "common/imagebuf.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/file_location.h"
#include "common/iop_profile.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
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
#include "win/scandir.h"
#endif // defined (_WIN32)

DT_MODULE_INTROSPECTION(3, dt_iop_lut3d_params_t)

#define DT_IOP_LUT3D_MAX_PATHNAME 512
#define DT_IOP_LUT3D_MAX_LUTNAME 128
#define DT_IOP_LUT3D_CLUT_LEVEL 48
#define DT_IOP_LUT3D_MAX_KEYPOINTS 2048

typedef enum dt_iop_lut3d_colorspace_t
{
  DT_IOP_SRGB = 0,    // $DESCRIPTION: "sRGB"
  DT_IOP_ARGB,        // $DESCRIPTION: "Adobe RGB"
  DT_IOP_REC709,      // $DESCRIPTION: "gamma Rec709 RGB"
  DT_IOP_LIN_REC709,  // $DESCRIPTION: "linear Rec709 RGB"
  DT_IOP_LIN_REC2020, // $DESCRIPTION: "linear Rec2020 RGB"
} dt_iop_lut3d_colorspace_t;

typedef enum dt_iop_lut3d_interpolation_t
{
  DT_IOP_TETRAHEDRAL = 0, // $DESCRIPTION: "tetrahedral"
  DT_IOP_TRILINEAR = 1,   // $DESCRIPTION: "trilinear"
  DT_IOP_PYRAMID = 2,     // $DESCRIPTION: "pyramid"
} dt_iop_lut3d_interpolation_t;

typedef struct dt_iop_lut3d_params_t
{
  char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
  dt_iop_lut3d_colorspace_t colorspace; // $DEFAULT: DT_IOP_SRGB $DESCRIPTION: "application color space"
  dt_iop_lut3d_interpolation_t interpolation; // $DEFAULT: DT_IOP_TETRAHEDRAL
  int nb_keypoints; // $DEFAULT: 0 >0 indicates the presence of compressed lut
  char c_clut[DT_IOP_LUT3D_MAX_KEYPOINTS*2*3];
  char lutname[DT_IOP_LUT3D_MAX_LUTNAME];
} dt_iop_lut3d_params_t;

typedef struct dt_iop_lut3d_gui_data_t
{
  GtkWidget *hbox;
  GtkWidget *filepath;
  GtkWidget *colorspace;
  GtkWidget *interpolation;
#ifdef HAVE_GMIC
  GtkWidget *lutentry;
  GtkWidget *lutname;
  GtkWidget *lutwindow;
  gulong lutname_handler_id;
#endif
} dt_iop_lut3d_gui_data_t;

typedef enum dt_lut3d_cols_t
{
  DT_LUT3D_COL_NAME = 0,
  DT_LUT3D_COL_VISIBLE,
  DT_LUT3D_NUM_COLS
} dt_lut3d_cols_t;

const char invalid_filepath_prefix[] = "INVALID >> ";

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

#ifdef HAVE_GMIC
void lut3d_decompress_clut(const unsigned char *const input_keypoints, const unsigned int nb_input_keypoints,
                    const unsigned int output_resolution, float *const output_clut_data,
                    const char *const filename);

unsigned int lut3d_get_cached_clut(float *const output_clut_data, const unsigned int output_resolution,
                    const char *const filename);

gboolean lut3d_read_gmz(int *const nb_keypoints, unsigned char *const keypoints, const char *const filename,
              int *const nb_lut, void *g, const char *const lutname, const gboolean newlutname);

#endif // HAVE_GMIC

const char *name()
{
  return _("LUT 3D");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("perform color space corrections and apply look"),
                                      _("corrective or creative"),
                                      _("linear, RGB, display-referred"),
                                      _("defined by profile, RGB"),
                                      _("linear or non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 3)
  {
    typedef struct dt_iop_lut3d_params_v1_t
    {
      char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
      int colorspace;
      int interpolation;
    } dt_iop_lut3d_params_v1_t;

    dt_iop_lut3d_params_v1_t *o = (dt_iop_lut3d_params_v1_t *)old_params;
    dt_iop_lut3d_params_t *n = (dt_iop_lut3d_params_t *)new_params;
    g_strlcpy(n->filepath, o->filepath, sizeof(n->filepath));
    n->colorspace = o->colorspace;
    n->interpolation = o->interpolation;
    n->nb_keypoints = 0;
    memset(&n->c_clut, 0, sizeof(n->c_clut));
    memset(&n->lutname, 0, sizeof(n->lutname));
    return 0;
  }
  if(old_version == 2 && new_version == 3)
  {
    typedef struct dt_iop_lut3d_params_v2_t
    {
      char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
      int colorspace;
      int interpolation;
      int nb_keypoints; // >0 indicates the presence of compressed lut
      char c_clut[DT_IOP_LUT3D_MAX_KEYPOINTS*2*3];
      char lutname[DT_IOP_LUT3D_MAX_LUTNAME];
      uint32_t gmic_version;
    } dt_iop_lut3d_params_v2_t;

    dt_iop_lut3d_params_v2_t *o = (dt_iop_lut3d_params_v2_t *)old_params;
    dt_iop_lut3d_params_t *n = (dt_iop_lut3d_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_lut3d_params_t));
    return 0;
  }

  return 1;
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
    float *const input = ((float *const)in) + k;
    float *const output = ((float *const)out) + k;

    int rgbi[3], i, j;
    float tmp[6];
    dt_aligned_pixel_t rgbd;

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
    dt_aligned_pixel_t rgbd;
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

    if(rgbd[0] > rgbd[1])
    {
      if(rgbd[1] > rgbd[2])
      {
        output[0] = (1-rgbd[0])*clut[i000] + (rgbd[0]-rgbd[1])*clut[i100] + (rgbd[1]-rgbd[2])*clut[i110] + rgbd[2]*clut[i111];
        output[1] = (1-rgbd[0])*clut[i000+1] + (rgbd[0]-rgbd[1])*clut[i100+1] + (rgbd[1]-rgbd[2])*clut[i110+1] + rgbd[2]*clut[i111+1];
        output[2] = (1-rgbd[0])*clut[i000+2] + (rgbd[0]-rgbd[1])*clut[i100+2] + (rgbd[1]-rgbd[2])*clut[i110+2] + rgbd[2]*clut[i111+2];
      }
      else if(rgbd[0] > rgbd[2])
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
      if(rgbd[2] > rgbd[1])
      {
        output[0] = (1-rgbd[2])*clut[i000] + (rgbd[2]-rgbd[1])*clut[i001] + (rgbd[1]-rgbd[0])*clut[i011] + rgbd[0]*clut[i111];
        output[1] = (1-rgbd[2])*clut[i000+1] + (rgbd[2]-rgbd[1])*clut[i001+1] + (rgbd[1]-rgbd[0])*clut[i011+1] + rgbd[0]*clut[i111+1];
        output[2] = (1-rgbd[2])*clut[i000+2] + (rgbd[2]-rgbd[1])*clut[i001+2] + (rgbd[1]-rgbd[0])*clut[i011+2] + rgbd[0]*clut[i111+2];
      }
      else if(rgbd[2] > rgbd[0])
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
    dt_aligned_pixel_t rgbd;
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

    if(rgbd[1] > rgbd[0] && rgbd[2] > rgbd[0])
    {
      output[0] = clut[i000] + (clut[i111]-clut[i011])*rgbd[0] + (clut[i010]-clut[i000])*rgbd[1] + (clut[i001]-clut[i000])*rgbd[2]
        + (clut[i011]-clut[i001]-clut[i010]+clut[i000])*rgbd[1]*rgbd[2];
      output[1] = clut[i000+1] + (clut[i111+1]-clut[i011+1])*rgbd[0] + (clut[i010+1]-clut[i000+1])*rgbd[1] + (clut[i001+1]-clut[i000+1])*rgbd[2]
        + (clut[i011+1]-clut[i001+1]-clut[i010+1]+clut[i000+1])*rgbd[1]*rgbd[2];
      output[2] = clut[i000+2] + (clut[i111+2]-clut[i011+2])*rgbd[0] + (clut[i010+2]-clut[i000+2])*rgbd[1] + (clut[i001+2]-clut[i000+2])*rgbd[2]
        + (clut[i011+2]-clut[i001+2]-clut[i010+2]+clut[i000+2])*rgbd[1]*rgbd[2];
    }
    else if(rgbd[0] > rgbd[1] && rgbd[2] > rgbd[1])
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

void get_cache_filename(const char *const lutname, char *const cache_filename)
{
  char *cache_dir = g_build_filename(g_get_user_cache_dir(), "gmic", NULL);
  char *cache_file = g_build_filename(cache_dir, lutname, NULL);
  g_strlcpy(cache_filename, cache_file, DT_IOP_LUT3D_MAX_PATHNAME);
  g_strlcpy(&cache_filename[strlen(cache_filename)], ".cimgz", DT_IOP_LUT3D_MAX_PATHNAME-strlen(cache_file));
  g_free(cache_dir);
  g_free(cache_file);
}

#ifdef HAVE_GMIC
uint8_t calculate_clut_compressed(dt_iop_lut3d_params_t *const p, const char *const filepath, float **clut)
{
  uint8_t level = DT_IOP_LUT3D_CLUT_LEVEL;
  float *lclut;
  lclut = NULL;
  char cache_filename[DT_IOP_LUT3D_MAX_PATHNAME];
  size_t buf_size_lut;

  get_cache_filename(p->lutname, cache_filename);
  buf_size_lut = (size_t)(level * level * level * 3);
  lclut = dt_alloc_align(16, sizeof(float) * buf_size_lut);
  if(!lclut)
  {
    fprintf(stderr, "[lut3d] error allocating buffer for gmz LUT\n");
    dt_control_log(_("error allocating buffer for gmz LUT"));
    level = 0;
  }
  else
  {
    level = lut3d_get_cached_clut(lclut, level, cache_filename);
    if(!level)
    {  //clut not in cache
      char *c_clut = p->c_clut;
      level = DT_IOP_LUT3D_CLUT_LEVEL;
      lut3d_decompress_clut((const unsigned char *const)c_clut, p->nb_keypoints,
        level, lclut, cache_filename);
    }
  }
  *clut = lclut;
  return level;
}
#endif // HAVE_GMIC


uint16_t calculate_clut_haldclut(dt_iop_lut3d_params_t *const p, const char *const filepath, float **clut)
{
  dt_imageio_png_t png;
  if(read_header(filepath, &png))
  {
    fprintf(stderr, "[lut3d] invalid png file %s\n", filepath);
    dt_control_log(_("invalid png file %s"), filepath);
    return 0;
  }
  dt_print(DT_DEBUG_DEV, "[lut3d] png: width=%d, height=%d, color_type=%d, bit_depth=%d\n", png.width,
           png.height, png.color_type, png.bit_depth);
  if(png.bit_depth !=8 && png.bit_depth != 16)
  {
    fprintf(stderr, "[lut3d] png bit-depth %d not supported\n", png.bit_depth);
    dt_control_log(_("png bit-depth %d not supported"), png.bit_depth);
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }

  // check the file sizes
  uint16_t level = 2;
  while(level * level * level < png.width) ++level;

  if(level * level * level != png.width)
  {
#ifdef HAVE_GMIC
    fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
    dt_control_log(_("invalid level in png file %d %d"), level, png.width);
#else
    if(png.height == 2)
    {
      fprintf(stderr, "[lut3d] this darktable build is not compatible with compressed CLUT\n");
      dt_control_log(_("this darktable build is not compatible with compressed CLUT"));
    }
    else
    {
      fprintf(stderr, "[lut3d] invalid level in png file %d %d\n", level, png.width);
      dt_control_log(_("invalid level in png file %d %d"), level, png.width);
    }
#endif // HAVE_GMIC
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }

  level *= level;  // to be equivalent to cube level
  if(level > 256)
  {
    fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
    dt_control_log(_("error - LUT 3D size %d exceeds the maximum supported"), level);
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
    fprintf(stderr, "[lut3d] error allocating buffer for png LUT\n");
    dt_control_log(_("error allocating buffer for png LUT"));
    fclose(png.f);
    png_destroy_read_struct(&png.png_ptr, &png.info_ptr, NULL);
    return 0;
  }
  if(read_image(&png, buf))
  {
    fprintf(stderr, "[lut3d] error - could not read png image `%s'\n", filepath);
    dt_control_log(_("error - could not read png image %s"), filepath);
    dt_free_align(buf);
    return 0;
  }
  const size_t buf_size_lut = (size_t)png.height * png.height * 3;
  dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu floats for png LUT - level %d\n", buf_size_lut, level);
  float *lclut = dt_alloc_align(16, sizeof(float) * buf_size_lut);
  if(!lclut)
  {
    fprintf(stderr, "[lut3d] error - allocating buffer for png LUT\n");
    dt_control_log(_("error - allocating buffer for png LUT"));
    dt_free_align(buf);
    return 0;
  }
  // get clut values
  const float norm = 1.0f / (powf(2.f, png.bit_depth) - 1.0f);
  if(png.bit_depth == 8)
  {
    for(size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (float)buf[i] * norm;
  }
  else
  {
    for(size_t i = 0; i < buf_size_lut; ++i)
      lclut[i] = (256.0f * (float)buf[2*i] + (float)buf[2*i+1]) * norm;
  }
  dt_free_align(buf);
  *clut = lclut;
  return level;
}

// provided by @rabauke, atof replaces strtod & sccanf which are locale dependent
double dt_atof(const char *str)
{
  if(strncmp(str, "nan", 3) == 0 || strncmp(str, "NAN", 3) == 0)
    return NAN;
  double integral_result = 0;
  double fractional_result = 0;
  double sign = 1;
  if(*str == '+')
  {
    str++;
    sign = +1;
  } else if(*str == '-')
  {
    str++;
    sign = -1;
  }
  if(strncmp(str, "inf", 3) == 0 || strncmp(str, "INF", 3) == 0)
    return sign * INFINITY;
  // search for end of integral part and parse from
  // right to left for numerical stability
  const char * istr_back = str;
  while(*str >= '0' && *str <= '9')
    str++;
  const char * istr_2 = str;
  double imultiplier = 1;
  while(istr_2 != istr_back)
  {
    --istr_2;
    integral_result += (*istr_2 - '0') * imultiplier;
    imultiplier *= 10;
  }
  if(*str == '.')
  {
    str++;
  // search for end of fractional part and parse from
  // right to left for numerical stability
    const char * fstr_back = str;
    while(*str >= '0' && *str <= '9')
      str++;
    const char * fstr_2 = str;
    double fmultiplier = 1;
    while(fstr_2 != fstr_back)
    {
      --fstr_2;
      fractional_result += (*fstr_2 - '0') * fmultiplier;
      fmultiplier *= 10;
    }
    fractional_result /= fmultiplier;
  }
  double result = sign * (integral_result + fractional_result);
  if(*str == 'e' || *str == 'E')
  {
    str++;
    double power_sign = 1;
    if(*str == '+')
    {
      str++;
      power_sign = +1;
    }
    else if(*str == '-')
    {
      str++;
      power_sign = -1;
    }
    double power = 0;
    while(*str >= '0' && *str <= '9')
    {
      power *= 10;
      power += *str - '0';
      str++;
    }
    if(power_sign > 0)
      result *= pow(10, power);
    else
      result /= pow(10, power);
  }
  return result;
}

// return max 3 tokens from the line (separator = ' ' and token length = 50)
// if nb tokens > 3, the 3rd one captures the last input
uint8_t parse_cube_line(char *line, char (*token)[50])
{
  const int max_token_len = 50;
  uint8_t i = 0;
  uint8_t c = 0;
  char *t = &token[0][0];
  char *l = line;

  while(*l != 0 && i < max_token_len)
  {
    if(*l == '#' || *l == '\n' || *l == '\r')
    { // end of useful part of the line
      if(i > 0)
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
    if(*l == ' ' || *l == '\t')
    { // separator
      if(i > 0)
      {
        *t = 0;
        c++;
        i = 0;
        t = &token[c > 2 ? 2 : c][0];
      }
    }
    else
    { // capture info
      *t = *l;
      t++;
      i++;
    }
    l++;
    // sometimes the last lf is missing
    if(*l == 0)
    {
      *t = 0;
      c++;
      return c;
    }
  }
  token[0][max_token_len - 1] = 0;
  token[1][max_token_len - 1] = 0;
  token[2][max_token_len - 1] = 0;
  return c;
}

uint16_t calculate_clut_cube(const char *const filepath, float **clut)
{
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint16_t level = 0;
  float *lclut = NULL;
  uint32_t i = 0;
  size_t buf_size = 0;
  uint32_t out_of_range_nb = 0;

  FILE *cube_file = g_fopen(filepath, "r");

  if(!cube_file)
  {
    fprintf(stderr, "[lut3d] invalid cube file: %s\n", filepath);
    dt_control_log(_("error - invalid cube file: %s"), filepath);
    return 0;
  }
  while((read = getline(&line, &len, cube_file)) != -1)
  {
    const uint8_t nb_token = parse_cube_line(line, token);
    if(nb_token)
    {
      if(token[0][0] == 'T') continue;
      else if(strcmp("DOMAIN_MIN", token[0]) == 0)
      {
        if(strtod(token[1], NULL) != 0.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MIN <> 0.0 is not supported\n");
          dt_control_log(_("DOMAIN MIN <> 0.0 is not supported"));
          if(lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
      }
      else if(strcmp("DOMAIN_MAX", token[0]) == 0)
      {
        if(strtod(token[1], NULL) != 1.0f)
        {
          fprintf(stderr, "[lut3d] DOMAIN MAX <> 1.0 is not supported\n");
          dt_control_log(_("DOMAIN MAX <> 1.0 is not supported"));
          if(lclut) dt_free_align(lclut);
          free(line);
          fclose(cube_file);
        }
      }
      else if(strcmp("LUT_1D_SIZE", token[0]) == 0)
      {
        fprintf(stderr, "[lut3d] 1D cube LUT is not supported\n");
        dt_control_log(_("[1D cube LUT is not supported"));
        free(line);
        fclose(cube_file);
        return 0;
      }
      else if(strcmp("LUT_3D_SIZE", token[0]) == 0)
      {
        level = atoll(token[1]);
        if(level > 256)
        {
          fprintf(stderr, "[lut3d] error - LUT 3D size %d > 256\n", level);
          dt_control_log(_("error - LUT 3D size %d exceeds the maximum supported"), level);
          free(line);
          fclose(cube_file);
          return 0;
        }
        buf_size = level * level * level * 3;
        dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for cube LUT - level %d\n", buf_size, level);
        lclut = dt_alloc_align(16, sizeof(float) * buf_size);
        if(!lclut)
        {
          fprintf(stderr, "[lut3d] error - allocating buffer for cube LUT\n");
          dt_control_log(_("error - allocating buffer for cube LUT"));
          free(line);
          fclose(cube_file);
          return 0;
        }
      }
      else if(nb_token == 3)
      {
        if(!level)
        {
          fprintf(stderr, "[lut3d] error - cube LUT size is not defined\n");
          dt_control_log(_("error - cube LUT size is not defined"));
          free(line);
          fclose(cube_file);
          return 0;
        }
        for(int j=0; j < 3; j++)
        {
          lclut[i+j] = dt_atof(token[j]);
          if(isnan(lclut[i+j]))
          {
            fprintf(stderr, "[lut3d] error - invalid number line %d\n", (int)i/3);
            dt_control_log(_("error - cube LUT invalid number line %d"), (int)i/3);
            free(line);
            fclose(cube_file);
            return 0;
          }
          else if(lclut[i+j] < 0.0 || lclut[i+j] > 1.0)
            out_of_range_nb++;
        }
        i += 3;
      }
    }
  }
  if(i != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - cube LUT lines number %d is not correct, should be %d\n",
            (int)i/3, (int)buf_size/3);
    dt_control_log(_("error - cube LUT lines number %d is not correct, should be %d"),
                   (int)i/3, (int)buf_size/3);
    dt_free_align(lclut);
    free(line);
    fclose(cube_file);
    return 0;
  }
  if(out_of_range_nb)
  {
    fprintf(stderr, "[lut3d] warning - %d out of range values [0,1]\n", out_of_range_nb);
    dt_control_log(_("warning - cube LUT %d out of range values [0,1]"), out_of_range_nb);
  }
  *clut = lclut;
  free(line);
  fclose(cube_file);
  return level;
}

uint16_t calculate_clut_3dl(const char *const filepath, float **clut)
{
  char *line = NULL;
  size_t len = 0;
  ssize_t read;
  char token[3][50];
  uint16_t level = 0;
  float *lclut = NULL;
  int max_value = 0;
  uint32_t i = 0;
  size_t buf_size = 0;

  FILE *cube_file = g_fopen(filepath, "r");

  if(!cube_file)
  {
    fprintf(stderr, "[lut3d] invalid 3dl file: %s\n", filepath);
    dt_control_log(_("error - invalid 3dl file: %s"), filepath);
    return 0;
  }
  while((read = getline(&line, &len, cube_file)) != -1)
  {
    const uint8_t nb_token = parse_cube_line(line, token);
    if(nb_token)
    {
      if(!level)
      {
        if(nb_token > 3)
        {
          // we assume the shaper is linear and gives the size of the cube (level)
          const int min_shaper = atoll(token[0]);
          const int max_shaper = atoll(token[2]);
          if(max_shaper > min_shaper)
          {
            level = nb_token; // max nb_token = 50 < 256
            if(max_shaper < 128)
            {
              fprintf(stderr, "[lut3d] error - the maximum shaper LUT value %d is too low\n", max_shaper);
              dt_control_log(_("error - the maximum shaper LUT value %d is too low"), max_shaper);
              free(line);
              fclose(cube_file);
              return 0;
            }
            buf_size = level * level * level * 3;
            dt_print(DT_DEBUG_DEV, "[lut3d] allocating %zu bytes for 3dl LUT - level %d\n", buf_size, level);
            lclut = dt_alloc_align(16, sizeof(float) * buf_size);
            if(!lclut)
            {
              fprintf(stderr, "[lut3d] error - allocating buffer for 3dl LUT\n");
              dt_control_log(_("error - allocating buffer for 3dl LUT"));
              free(line);
              fclose(cube_file);
              return 0;
            }
          }
        }
      }
      else if(nb_token == 3)
      {
        if(!level)
        {
          fprintf(stderr, "[lut3d] error - 3dl LUT size is not defined\n");
          dt_control_log(_("error - 3dl LUT size is not defined"));
          free(line);
          fclose(cube_file);
          return 0;
        }
        // indexing starts with blue instead of red. need to restore the right index
        const uint32_t level2 = level * level;
        const uint32_t red = i / level2;
        const uint32_t rr = i - red * level2;
        const uint32_t green = rr / level;
        const uint32_t blue = rr - green * level;
        const uint32_t k = red + level * green + level2 * blue;
        for(int j=0; j < 3; j++)
        {
          const uint32_t value = atoll(token[j]);
          lclut[k*3+j] = (float)value;
          if(value > max_value)
            max_value = value;
        }
        i++;
        if(i * 3 > buf_size)
          break;
      }
    }
  }
  if(i * 3 != buf_size || i == 0)
  {
    fprintf(stderr, "[lut3d] error - 3dl LUT lines number is not correct\n");
    dt_control_log(_("error - 3dl LUT lines number is not correct"));
    dt_free_align(lclut);
    free(line);
    fclose(cube_file);
    return 0;
  }
  free(line);
  fclose(cube_file);

  // search bit depth: min 2^x > max_value
  int inorm = 1;
  while((inorm < max_value) && (inorm < 65536))  // bit depth 16
    inorm <<= 1;
  if(inorm < 128)  // bit depth 7
  {
    fprintf(stderr, "[lut3d] error - the maximum LUT value does not match any valid bit depth\n");
    dt_control_log(_("error - the maximum LUT value does not match any valid bit depth"));
    dt_free_align(lclut);
    return 0;
  }
  const float norm = 1.0f / (float)(inorm - 1);
  // normalize the lut
  for(i =0; i < buf_size; i++)
    lclut[i] = CLAMP(lclut[i] * norm, 0.0f, 1.0f);
  *clut = lclut;
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
    : (d->params.colorspace == DT_IOP_ARGB) ? DT_COLORSPACE_ADOBERGB
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  gboolean transform = (work_profile != NULL && lut_profile != NULL) ? TRUE : FALSE;
  cl_mem clut_cl = NULL;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  if(clut && level)
  {
    clut_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3 * level * level * level, (void *)clut);
    if(clut_cl == NULL)
    {
      fprintf(stderr, "[lut3d process_cl] error allocating memory\n");
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto cleanup;
    }
    if(transform)
    {
      const int success = dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_in, dev_out, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      if(!success)
       transform = FALSE;
    }
    if(transform)
      dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_out));
    else
      dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in));
    dt_opencl_set_kernel_args(devid, kernel, 1, CLARG(dev_out), CLARG(width), CLARG(height), CLARG(clut_cl), CLARG(level));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(transform)
      dt_ioppr_transform_image_colorspace_rgb_cl(devid, dev_out, dev_out, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
  }
  else
  { // no lut: identity kernel
    dt_opencl_set_kernel_args(devid, gd->kernel_lut3d_none, 0, CLARG(dev_in), CLARG(dev_out), CLARG(width),
      CLARG(height));
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lut3d_none, sizes);
  }
  if(err != CL_SUCCESS)
  {
    fprintf(stderr, "[lut3d process_cl] error %i enqueue kernel\n", err);
    goto cleanup;
  }

cleanup:
  if(clut_cl) dt_opencl_release_mem_object(clut_cl);

  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl_lut3d] couldn't enqueue kernel! %s\n", cl_errstr(err));
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
    : (d->params.colorspace == DT_IOP_ARGB) ? DT_COLORSPACE_ADOBERGB
    : (d->params.colorspace == DT_IOP_LIN_REC709) ? DT_COLORSPACE_LIN_REC709
    : DT_COLORSPACE_LIN_REC2020;
  const dt_iop_order_iccprofile_info_t *const lut_profile
    = dt_ioppr_add_profile_info_to_list(self->dev, colorspace, "", INTENT_PERCEPTUAL);
  const dt_iop_order_iccprofile_info_t *const work_profile
    = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const gboolean transform = (work_profile != NULL && lut_profile != NULL) ? TRUE : FALSE;
  if(clut)
  {
    if(transform)
    {
      dt_ioppr_transform_image_colorspace_rgb(ibuf, obuf, width, height,
        work_profile, lut_profile, "work profile to LUT profile");
      if(interpolation == DT_IOP_TETRAHEDRAL)
        correct_pixel_tetrahedral(obuf, obuf, (size_t)width * height, clut, level);
      else if(interpolation == DT_IOP_TRILINEAR)
        correct_pixel_trilinear(obuf, obuf, (size_t)width * height, clut, level);
      else
        correct_pixel_pyramid(obuf, obuf, (size_t)width * height, clut, level);
      dt_ioppr_transform_image_colorspace_rgb(obuf, obuf, width, height,
        lut_profile, work_profile, "LUT profile to work profile");
    }
    else
    {
      if(interpolation == DT_IOP_TETRAHEDRAL)
        correct_pixel_tetrahedral(ibuf, obuf, (size_t)width * height, clut, level);
      else if(interpolation == DT_IOP_TRILINEAR)
        correct_pixel_trilinear(ibuf, obuf, (size_t)width * height, clut, level);
      else
        correct_pixel_pyramid(ibuf, obuf, (size_t)width * height, clut, level);
    }
  }
  else  // no clut
  {
    dt_iop_image_copy_by_size(obuf, ibuf, width, height, ch);
  }
}

void filepath_set_unix_separator(char *filepath)
{ // use the unix separator as it works also on windows
  const int len = strlen(filepath);
  for(int i=0; i<len; ++i)
    if(filepath[i]=='\\') filepath[i] = '/';
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

#ifdef HAVE_GMIC
  // make sure the cache dir exists
  char *cache_dir = g_build_filename(g_get_user_cache_dir(), "gmic", NULL);
  char *cache_gmic_dir = dt_loc_init_generic(cache_dir, NULL, NULL);
  g_free(cache_dir);
  g_free(cache_gmic_dir);
#endif // HAVE_GMIC
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

static int calculate_clut(dt_iop_lut3d_params_t *const p, float **clut)
{
  uint16_t level = 0;
  const char *filepath = p->filepath;
#ifdef HAVE_GMIC
  if(p->nb_keypoints && filepath[0])
  {
    // compressed in params. no need to read the file
    level = calculate_clut_compressed(p, filepath, clut);
  }
  else
  { // read the file
#endif  // HAVE_GMIC
    gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
    if(filepath[0] && lutfolder[0])
    {
      char *fullpath = g_build_filename(lutfolder, filepath, NULL);
      if(g_str_has_suffix (filepath, ".png") || g_str_has_suffix (filepath, ".PNG"))
      {
        level = calculate_clut_haldclut(p, fullpath, clut);
      }
      else if(g_str_has_suffix (filepath, ".cube") || g_str_has_suffix (filepath, ".CUBE"))
      {
        level = calculate_clut_cube(fullpath, clut);
      }
      else if(g_str_has_suffix (filepath, ".3dl") || g_str_has_suffix (filepath, ".3DL"))
      {
        level = calculate_clut_3dl(fullpath, clut);
      }
      g_free(fullpath);
    }
    g_free(lutfolder);
#ifdef HAVE_GMIC
  }
#endif // HAVE_GMIC
  return level;
}

#ifdef HAVE_GMIC
static gboolean list_match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_iop_lut3d_gui_data_t *g)
{
  gchar *str = NULL;
  gboolean visible;

  gtk_tree_model_get(model, iter, DT_LUT3D_COL_NAME, &str, -1);
  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(g->lutentry)), -1);

  visible = (g_strrstr(haystack, needle) != NULL);

  g_free(haystack);
  g_free(needle);
  g_free(str);
  gtk_list_store_set((GtkListStore *)model, iter, DT_LUT3D_COL_VISIBLE, visible, -1);
  return FALSE;
}

static void apply_filter_lutname_list(dt_iop_lut3d_gui_data_t *g)
{
  GtkTreeModel *modelf = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(modelf));
  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)list_match_string, g);
}

void lut3d_add_lutname_to_list(void *gv, const char *const lutname)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)gv;
  GtkTreeModel *modelf = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(modelf));
  GtkTreeIter iter;
  gtk_list_store_append((GtkListStore *)model, &iter);
  gtk_list_store_set((GtkListStore *)model, &iter, DT_LUT3D_COL_NAME, lutname, DT_LUT3D_COL_VISIBLE, TRUE, -1);
}

void lut3d_clear_lutname_list(void *gv)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)gv;
  GtkTreeModel *modelf = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(modelf));
  // keep lutname_callback quiet while clearing the list
  GtkTreeSelection *selection = gtk_tree_view_get_selection((GtkTreeView *)g->lutname);
  g_signal_handler_block(G_OBJECT(selection), g->lutname_handler_id);
  gtk_list_store_clear((GtkListStore *)model);
  g_signal_handler_unblock(G_OBJECT(selection), g->lutname_handler_id);
}

static gboolean select_lutname_in_list(dt_iop_lut3d_gui_data_t *g, const char *const lutname)
{
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection((GtkTreeView *)g->lutname);
  GtkTreeModel *model = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  if(lutname)
  {
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
    {
     gchar *name;
     gtk_tree_model_get(model, &iter, DT_LUT3D_COL_NAME, &name, -1);
     if(!g_strcmp0(lutname, name))
     {
       gtk_tree_selection_select_iter(selection, &iter);
       GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
       gtk_tree_view_scroll_to_cell((GtkTreeView *)g->lutname, path, NULL, TRUE, 0.2, 0);
       gtk_tree_path_free(path);
       g_free(name);
       return TRUE;
     }
     g_free(name);
     valid = gtk_tree_model_iter_next(model, &iter);
    }
    return FALSE;
  }
  else  // select the first in the list
  {
    if(gtk_tree_model_iter_nth_child(model, &iter, NULL, 0))
    {
      gtk_tree_selection_select_iter(selection, &iter);
      return TRUE;
    }
    else
    {
      return FALSE;
    }
  }
}

static void get_selected_lutname(dt_iop_lut3d_gui_data_t *g, char *const lutname)
{
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection((GtkTreeView *)g->lutname);
  GtkTreeModel *model = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gchar *name;
    gtk_tree_model_get(model, &iter, DT_LUT3D_COL_NAME, &name, -1);
    g_strlcpy(lutname, name, DT_IOP_LUT3D_MAX_LUTNAME);
    g_free(name);
  }
  else lutname[0] = 0;
}

static void get_compressed_clut(dt_iop_module_t *self, gboolean newlutname)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  int nb_lut = 0;
  char *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if(p->filepath[0] && lutfolder[0])
  {
    if(g_str_has_suffix (p->filepath, ".gmz") || g_str_has_suffix (p->filepath, ".GMZ"))
    {
      char *fullpath = g_build_filename(lutfolder, p->filepath, NULL);
      gboolean lut_found = lut3d_read_gmz(&p->nb_keypoints, (unsigned char *const)p->c_clut, fullpath,
              &nb_lut, (void *)g, p->lutname, newlutname);
      // to be able to fix evolution issue, keep the gmic version with the compressed lut
      if(lut_found)
      {
        if(!newlutname)
          select_lutname_in_list(g, p->lutname);
      }
      else if(nb_lut)
      {
        select_lutname_in_list(g, NULL);
        get_selected_lutname(g, p->lutname);
      }
      else if(p->lutname[0])
      { // read has failed - make sure lutname appear in the list (for user info)
        if(!select_lutname_in_list(g, p->lutname))
        {
          lut3d_add_lutname_to_list(g, p->lutname);
          select_lutname_in_list(g, p->lutname);
        }
      }
      g_free(fullpath);
    }
  }
  g_free(lutfolder);
}

static void show_hide_controls(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  GtkTreeModel *model = gtk_tree_view_get_model((GtkTreeView *)g->lutname);
  const int nb_luts = gtk_tree_model_iter_n_children(model, NULL);
  if((nb_luts > 1) || ((nb_luts > 0) &&
       g_str_has_prefix(dt_bauhaus_combobox_get_text(g->filepath), invalid_filepath_prefix)))
  {
    int nb_pixels = (20*(nb_luts+1) > 200) ? 200 : 20*(nb_luts);
    if(nb_luts > 100)
      gtk_widget_set_visible(g->lutentry, TRUE);
    else
      gtk_widget_set_visible(g->lutentry, FALSE);
    gtk_widget_set_visible(g->lutwindow, TRUE);
    gtk_scrolled_window_set_min_content_height((GtkScrolledWindow *)g->lutwindow, DT_PIXEL_APPLY_DPI(nb_pixels));
  }
  else
  {
    gtk_widget_set_visible(g->lutentry, FALSE);
    gtk_widget_set_visible(g->lutwindow, FALSE);
  }
}
#endif  // HAVE_GMIC

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)p1;
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;

  if(strcmp(p->filepath, d->params.filepath) != 0 || strcmp(p->lutname, d->params.lutname) != 0 )
  { // new clut file
    if(d->clut)
    { // reset current clut if any
      dt_free_align(d->clut);
      d->clut = NULL;
      d->level = 0;
    }
    d->level = calculate_clut(p, &d->clut);
  }
  memcpy(&d->params, p, sizeof(dt_iop_lut3d_params_t));
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_lut3d_data_t));
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;
  memcpy(&d->params, self->default_params, sizeof(dt_iop_lut3d_params_t));
  d->clut = NULL;
  d->level = 0;
  d->params.filepath[0] = '\0';
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lut3d_data_t *d = (dt_iop_lut3d_data_t *)piece->data;;
  if(d->clut)
    dt_free_align(d->clut);
  d->clut = NULL;
  d->level = 0;
  free(piece->data);
  piece->data = NULL;
}

static void filepath_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  char filepath[DT_IOP_LUT3D_MAX_PATHNAME];
  g_strlcpy(filepath, dt_bauhaus_combobox_get_text(widget), sizeof(filepath));
  if(!g_str_has_prefix(filepath, invalid_filepath_prefix))
  {
    filepath_set_unix_separator(filepath);
#ifdef HAVE_GMIC
    dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
    if(strcmp(filepath, p->filepath) != 0 && !(g_str_has_suffix(filepath, ".gmz") || g_str_has_suffix(filepath, ".GMZ")))
    {
      // if new file is gmz we try to keep the same lut
      p->nb_keypoints = 0;
      p->lutname[0] = 0;
      lut3d_clear_lutname_list(g);
    }
    g_strlcpy(p->filepath, filepath, sizeof(p->filepath));
    get_compressed_clut(self, FALSE);
    show_hide_controls(self);
    gtk_entry_set_text(GTK_ENTRY(g->lutentry), "");
#else
    g_strlcpy(p->filepath, filepath, sizeof(p->filepath));
#endif // HAVE_GMIC
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

#ifdef HAVE_GMIC
static void entry_callback(GtkEntry *entry, dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  apply_filter_lutname_list(g);
}

static void lutname_callback(GtkTreeSelection *selection, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  GtkTreeIter iter;
  GtkTreeModel *model;
  gchar *lutname;

  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gtk_tree_model_get(model, &iter, DT_LUT3D_COL_NAME, &lutname, -1);
    if(lutname[0] && strcmp(lutname, p->lutname) != 0)
    {
      g_strlcpy(p->lutname, lutname, sizeof(p->lutname));
      get_compressed_clut(self, TRUE);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    g_free(lutname);
  }
}

static gboolean mouse_scroll(GtkWidget *view, GdkEventScroll *event, dt_lib_module_t *self)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model((GtkTreeView *)view);
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gboolean next = FALSE;
    if(event->delta_y > 0)
      next = gtk_tree_model_iter_next(model, &iter);
    else
      next = gtk_tree_model_iter_previous(model, &iter);
    if(next)
    {
      gtk_tree_selection_select_iter(selection, &iter);
      GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
      gtk_tree_view_set_cursor((GtkTreeView *)view, path, NULL, FALSE);
      gtk_tree_path_free(path);
      return TRUE;
    }
  }
  return FALSE;
}
#endif // HAVE_GMIC

// remove root lut folder from path
static void remove_root_from_path(const char *const lutfolder, char *const filepath)
{
  const int j = strlen(lutfolder) + 1;
  int i;
  for(i = 0; filepath[i+j] != '\0'; i++)
    filepath[i] = filepath[i+j];
  filepath[i] = '\0';
}

int check_extension(const struct dirent *namestruct)
{
  const char *filename = namestruct->d_name;
  int res = 0;
  if(!filename || !filename[0]) return res;
  char *p = g_strrstr(filename,".");
  if(!p) return res;
  char *fext = g_ascii_strdown(g_strdup(p), -1);
#ifdef HAVE_GMIC
  if(!g_strcmp0(fext, ".png") || !g_strcmp0(fext, ".cube") || !g_strcmp0(fext, ".3dl")
      || !g_strcmp0(fext, ".gmz")) res = 1;
#else
  if(!g_strcmp0(fext, ".png") || !g_strcmp0(fext, ".cube") || !g_strcmp0(fext, ".3dl") ) res = 1;
#endif // HAVE_GMIC
  g_free(fext);
  return res;
}

// update filepath combobox with all files in the current folder
static void update_filepath_combobox(dt_iop_lut3d_gui_data_t *g, char *filepath, char *lutfolder)
{
  if(!filepath[0])
    dt_bauhaus_combobox_clear(g->filepath);
  else if(!dt_bauhaus_combobox_set_from_text(g->filepath, filepath))
  {
    // new folder -> update the files list
    char *relativepath = g_path_get_dirname(filepath);
    char *folder = g_build_filename(lutfolder, relativepath, NULL);

    struct dirent **entries;
    const int numentries = scandir(folder, &entries, check_extension, alphasort);

    dt_bauhaus_combobox_clear(g->filepath);
    for(int i = 0; i < numentries; i++)
    {
      const char *file = entries[i]->d_name;
      char *ofilepath = (strcmp(relativepath, ".") != 0)
              ? g_build_filename(relativepath, file, NULL)
              : g_strdup(file);
      filepath_set_unix_separator(ofilepath);
      dt_bauhaus_combobox_add_aligned(g->filepath, ofilepath, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
      g_free(ofilepath);
      free(entries[i]);
    }
    if(numentries != -1) free(entries);

    if(!dt_bauhaus_combobox_set_from_text(g->filepath, filepath))
    { // file may have disappeared - show it
      char *invalidfilepath = g_strconcat(invalid_filepath_prefix, filepath, NULL);
      dt_bauhaus_combobox_add_aligned(g->filepath, invalidfilepath, DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
      dt_bauhaus_combobox_set_from_text(g->filepath, invalidfilepath);
      g_free(invalidfilepath);
    }
    g_free(relativepath);
    g_free(folder);
  }
}

static void button_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar* lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if(strlen(lutfolder) == 0)
  {
    fprintf(stderr, "[lut3d] LUT root folder not defined\n");
    dt_control_log(_("LUT root folder not defined"));
    g_free(lutfolder);
    return;
  }
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
        _("select LUT file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN,
        _("_select"), _("_cancel"));
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  char *composed = g_build_filename(lutfolder, p->filepath, NULL);
  if(strlen(p->filepath) == 0 || g_access(composed, F_OK) == -1)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), lutfolder);
  else
    gtk_file_chooser_select_filename(GTK_FILE_CHOOSER(filechooser), composed);
  g_free(composed);

  GtkFileFilter* filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.png");
  gtk_file_filter_add_pattern(filter, "*.PNG");
  gtk_file_filter_add_pattern(filter, "*.cube");
  gtk_file_filter_add_pattern(filter, "*.CUBE");
  gtk_file_filter_add_pattern(filter, "*.3dl");
  gtk_file_filter_add_pattern(filter, "*.3DL");
#ifdef HAVE_GMIC
  gtk_file_filter_add_pattern(filter, "*.gmz");
  gtk_file_filter_add_pattern(filter, "*.GMZ");
  gtk_file_filter_set_name(filter, _("hald CLUT (png), 3D LUT (cube or 3dl) or gmic compressed LUT (gmz)"));
#else
  gtk_file_filter_set_name(filter, _("hald CLUT (png) or 3D LUT (cube or 3dl)"));
#endif // HAVE_GMIC
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(filechooser), filter);

  // let this option to allow the user to see the actual content of the folder
  // but any selected file with ext <> png or cube will be ignored
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *filepath = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    if(strcmp(lutfolder, filepath) < 0)
    {
      remove_root_from_path(lutfolder, filepath);
      filepath_set_unix_separator(filepath);
      update_filepath_combobox(g, filepath, lutfolder);
    }
    else if(!filepath[0])// file chosen outside of root folder
    {
      fprintf(stderr, "[lut3d] select file outside LUT root folder is not allowed\n");
      dt_control_log(_("select file outside LUT root folder is not allowed"));
    }
    g_free(filepath);
    gtk_widget_set_sensitive(g->filepath, p->filepath[0]);
  }
  g_free(lutfolder);
  g_object_unref(filechooser);
}

static void _show_hide_colorspace(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  GList *iop_order_list = self->dev->iop_order_list;
  const int order_lut3d = dt_ioppr_get_iop_order(iop_order_list, self->op, self->multi_priority);
  const int order_colorin = dt_ioppr_get_iop_order(iop_order_list, "colorin", -1);
  const int order_colorout = dt_ioppr_get_iop_order(iop_order_list, "colorout", -1);
  if(order_lut3d < order_colorin || order_lut3d > order_colorout)
  {
    gtk_widget_hide(g->colorspace);
  }
  else
  {
    gtk_widget_show(g->colorspace);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = (dt_iop_lut3d_gui_data_t *)self->gui_data;
  dt_iop_lut3d_params_t *p = (dt_iop_lut3d_params_t *)self->params;
  gchar *lutfolder = dt_conf_get_string("plugins/darkroom/lut3d/def_path");
  if(!lutfolder[0])
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
  g_free(lutfolder);

  _show_hide_colorspace(self);

#ifdef HAVE_GMIC
  if(p->lutname[0])
  {
    get_compressed_clut(self, FALSE);
  }
  show_hide_controls(self);
#endif // HAVE_GMIC
}

void module_moved_callback(gpointer instance, dt_iop_module_t *self)
{
  _show_hide_colorspace(self);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_lut3d_gui_data_t *g = IOP_GUI_ALLOC(lut3d);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_name(button, "non-flat");
#ifdef HAVE_GMIC
  gtk_widget_set_tooltip_text(button, _("select a png (haldclut)"
      ", a cube, a 3dl or a gmz (compressed LUT) file "
      "CAUTION: 3D LUT folder must be set in preferences/processing before choosing the LUT file"));
#else
  gtk_widget_set_tooltip_text(button, _("select a png (haldclut)"
      ", a cube or a 3dl file "
      "CAUTION: 3D LUT folder must be set in preferences/processing before choosing the LUT file"));
#endif // HAVE_GMIC
  gtk_box_pack_start(GTK_BOX(g->hbox), button, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(button_clicked), self);

  g->filepath = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_entries_ellipsis(g->filepath, PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(g->hbox), g->filepath, TRUE, TRUE, 0);
#ifdef HAVE_GMIC
  gtk_widget_set_tooltip_text(g->filepath,
    _("the file path (relative to LUT folder) is saved with image along with the LUT data if it's a compressed LUT (gmz)"));
#else
  gtk_widget_set_tooltip_text(g->filepath,
    _("the file path (relative to LUT folder) is saved with image (and not the LUT data themselves)"));
#endif // HAVE_GMIC
  g_signal_connect(G_OBJECT(g->filepath), "value-changed", G_CALLBACK(filepath_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->hbox), TRUE, TRUE, 0);

#ifdef HAVE_GMIC
  // text entry
  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_tooltip_text(entry, _("enter LUT name"));
  gtk_box_pack_start((GtkBox *)self->widget,entry, TRUE, TRUE, 0);
  gtk_widget_add_events(entry, GDK_KEY_RELEASE_MASK);
  g_signal_connect(G_OBJECT(entry), "changed", G_CALLBACK(entry_callback), self);
  g->lutentry = entry;
  // treeview
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  g->lutwindow = sw;
  gtk_scrolled_window_set_policy((GtkScrolledWindow *)sw, GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  GtkTreeModel *lutmodel = (GtkTreeModel *)gtk_list_store_new(DT_LUT3D_NUM_COLS, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkTreeModel *lutfilter = gtk_tree_model_filter_new(lutmodel, NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(lutfilter), DT_LUT3D_COL_VISIBLE);
  g_object_unref(lutmodel);

  GtkTreeView *view = (GtkTreeView *)gtk_tree_view_new();
  g->lutname = (GtkWidget *)view;
  gtk_widget_set_name((GtkWidget *)view, "lutname");
  gtk_tree_view_set_model(view, lutfilter);
  gtk_tree_view_set_hover_selection(view, FALSE);
  gtk_tree_view_set_headers_visible(view, FALSE);
  gtk_container_add(GTK_CONTAINER(sw), (GtkWidget *)view);
  gtk_widget_set_tooltip_text((GtkWidget *)view, _("select the LUT"));
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes ("lutname", renderer,
                                                   "text", DT_LUT3D_COL_NAME, NULL);
  gtk_tree_view_append_column(view, col);
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
  g->lutname_handler_id = g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(lutname_callback), self);
  g_signal_connect(G_OBJECT(view), "scroll-event", G_CALLBACK(mouse_scroll), (gpointer)self);
  gtk_box_pack_start((GtkBox *)self->widget, sw , TRUE, TRUE, 0);
#endif // HAVE_GMIC

  g->colorspace = dt_bauhaus_combobox_from_params(self, "colorspace");
  gtk_widget_set_tooltip_text(g->colorspace, _("select the color space in which the LUT has to be applied"));

  g->interpolation = dt_bauhaus_combobox_from_params(self, N_("interpolation"));
  gtk_widget_set_tooltip_text(g->interpolation, _("select the interpolation method"));

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED,
                            G_CALLBACK(module_moved_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(module_moved_callback), self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
