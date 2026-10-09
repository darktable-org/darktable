/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#include <glib.h>
#include <stdio.h>

#include "debug.h"
#include "dng_opcode.h"

#define OPCODE_ID_GAINMAP (9)
#define OPCODE_ID_WARP_RECTILINEAR (1)
#define OPCODE_ID_VIGNETTE_RADIAL (3)

/* Sizes of the fixed-width values serialized in DNG opcode data. */
#define DNG_UINT32_SIZE (4)
#define DNG_FLOAT_SIZE (4)
#define DNG_DOUBLE_SIZE (8)

/* DNG opcode lists start with an opcode count. Each opcode then has a header
 * containing four uint32 values: ID, version, flags, and parameter byte count. */
#define OPCODE_LIST_HEADER_SIZE (DNG_UINT32_SIZE)
#define OPCODE_HEADER_SIZE (4 * DNG_UINT32_SIZE)

/* A GainMap payload has eleven 32-bit fields and four 64-bit floating-point
 * fields before its variable-length array of gain values. */
#define GAIN_MAP_HEADER_SIZE (11 * DNG_UINT32_SIZE + 4 * DNG_DOUBLE_SIZE)

/* WarpRectilinear has six coefficients per plane and a two-value center.
 * VignetteRadial has five coefficients and the same two-value center. */
#define WARP_COEFFICIENTS_PER_PLANE (6)
#define VIGNETTE_COEFFICIENTS (5)
#define OPCODE_CENTER_VALUES (2)
#define WARP_RECTILINEAR_SIZE(planes) \
  (DNG_UINT32_SIZE + DNG_DOUBLE_SIZE * ((planes) * WARP_COEFFICIENTS_PER_PLANE \
                                        + OPCODE_CENTER_VALUES))
#define VIGNETTE_RADIAL_SIZE \
  (DNG_DOUBLE_SIZE * (VIGNETTE_COEFFICIENTS + OPCODE_CENTER_VALUES))

static double _get_double(uint8_t *ptr)
{
  guint64 in;
  union {
    guint64 out;
    double v;
  } u;
  memcpy(&in, ptr, sizeof(in));
  u.out = GUINT64_FROM_BE(in);
  return u.v;
}

static float _get_float(uint8_t *ptr)
{
  guint32 in;
  union {
    guint32 out;
    float v;
  } u;
  memcpy(&in, ptr, sizeof(in));
  u.out = GUINT32_FROM_BE(in);
  return u.v;
}

static uint32_t _get_long(uint8_t *ptr)
{
  uint32_t in;
  memcpy(&in, ptr, sizeof(in));
  return GUINT32_FROM_BE(in);
}

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  g_list_free_full(img->dng_gain_maps, g_free);
  img->dng_gain_maps = NULL;

  if(buf_size < OPCODE_LIST_HEADER_SIZE)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid OpcodeList2 header");
    return;
  }

  uint32_t count = _get_long(&buf[0]);
  uint32_t offset = OPCODE_LIST_HEADER_SIZE;
  while(count > 0)
  {
    if(offset > buf_size || buf_size - offset < OPCODE_HEADER_SIZE)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode header in OpcodeList2");
      return;
    }

    uint32_t opcode_id = _get_long(&buf[offset]);
    uint32_t flags = _get_long(&buf[offset + 8]);
    uint32_t param_size = _get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + OPCODE_HEADER_SIZE];

    if(param_size > buf_size - offset - OPCODE_HEADER_SIZE)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList2");
      return;
    }

    if(opcode_id == OPCODE_ID_GAINMAP)
    {
      if(param_size < GAIN_MAP_HEADER_SIZE
         || (param_size - GAIN_MAP_HEADER_SIZE) % DNG_FLOAT_SIZE)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid GainMap size in OpcodeList2");
        return;
      }

      uint32_t gain_count = (param_size - GAIN_MAP_HEADER_SIZE) / DNG_FLOAT_SIZE;
#if SIZE_MAX < UINT64_MAX
      if(gain_count > (SIZE_MAX - sizeof(dt_dng_gain_map_t)) / sizeof(float))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] GainMap is too large in OpcodeList2");
        return;
      }
#endif

      dt_dng_gain_map_t *gm =
        g_try_malloc(sizeof(dt_dng_gain_map_t) + gain_count * sizeof(float));
      if(!gm)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Failed to allocate GainMap in OpcodeList2");
        return;
      }
      gm->top = _get_long(&param[0]);
      gm->left = _get_long(&param[4]);
      gm->bottom = _get_long(&param[8]);
      gm->right = _get_long(&param[12]);
      gm->plane = _get_long(&param[16]);
      gm->planes = _get_long(&param[20]);
      gm->row_pitch = _get_long(&param[24]);
      gm->col_pitch = _get_long(&param[28]);
      gm->map_points_v = _get_long(&param[32]);
      gm->map_points_h = _get_long(&param[36]);
      gm->map_spacing_v = _get_double(&param[40]);
      gm->map_spacing_h = _get_double(&param[48]);
      gm->map_origin_v = _get_double(&param[56]);
      gm->map_origin_h = _get_double(&param[64]);
      gm->map_planes = _get_long(&param[72]);

      const uint64_t map_points =
        (uint64_t)gm->map_points_v * gm->map_points_h;
      if(gm->map_planes && map_points > UINT64_MAX / gm->map_planes)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid GainMap dimensions in OpcodeList2");
        g_free(gm);
        return;
      }

      const uint64_t expected_gain_count = map_points * gm->map_planes;
      if(expected_gain_count != gain_count)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid GainMap data length in OpcodeList2");
        g_free(gm);
        return;
      }

      for(uint32_t i = 0; i < gain_count; i++)
        gm->map_gain[i] = _get_float(&param[GAIN_MAP_HEADER_SIZE + DNG_FLOAT_SIZE * i]);

      img->dng_gain_maps = g_list_append(img->dng_gain_maps, gm);
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

    offset += OPCODE_HEADER_SIZE + param_size;
    count--;
  }
}

void dt_dng_opcode_process_opcode_list_3(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  dt_image_correction_data_t *cd = &img->exif_correction_data;
  cd->dng.has_warp = FALSE;
  cd->dng.has_vignette = FALSE;

  if(buf_size < OPCODE_LIST_HEADER_SIZE)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid OpcodeList3 header");
    return;
  }

  uint32_t count = _get_long(&buf[0]);
  uint32_t offset = OPCODE_LIST_HEADER_SIZE;
  while(count > 0)
  {
    if(offset > buf_size || buf_size - offset < OPCODE_HEADER_SIZE)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode header in OpcodeList3");
      return;
    }

    uint32_t opcode_id = _get_long(&buf[offset]);
    uint32_t flags = _get_long(&buf[offset + 8]);
    uint32_t param_size = _get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + OPCODE_HEADER_SIZE];

    if(param_size > buf_size - offset - OPCODE_HEADER_SIZE)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList3");
      return;
    }

    if(opcode_id == OPCODE_ID_WARP_RECTILINEAR)
    {
      if(param_size < DNG_UINT32_SIZE)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[OPCODE_ID_WARP_RECTILINEAR] Invalid opcode size");
        return;
      }

      const int planes = _get_long(&param[0]);
      if((planes != 1) && (planes != 3))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[OPCODE_ID_WARP_RECTILINEAR] Invalid number of planes %i", planes);
        return;
      }

      const uint32_t required_size = WARP_RECTILINEAR_SIZE(planes);
      if(param_size < required_size)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[OPCODE_ID_WARP_RECTILINEAR] Invalid opcode size");
        return;
      }

      cd->dng.planes = planes;
      for(int p = 0; p < planes; p++)
      {
        for(int i = 0; i < WARP_COEFFICIENTS_PER_PLANE; i++)
          cd->dng.cwarp[p][i] =
            _get_double(&param[DNG_UINT32_SIZE
                               + DNG_DOUBLE_SIZE * (i + p * WARP_COEFFICIENTS_PER_PLANE)]);
      }

      for(int i = 0; i < OPCODE_CENTER_VALUES; i++)
        cd->dng.centre_warp[i] =
          _get_double(&param[DNG_UINT32_SIZE
                             + DNG_DOUBLE_SIZE * (i + planes * WARP_COEFFICIENTS_PER_PLANE)]);

      img->exif_correction_type = CORRECTION_TYPE_DNG;
      cd->dng.has_warp = TRUE;
    }

    else if(opcode_id == OPCODE_ID_VIGNETTE_RADIAL)
    {
      if(param_size < VIGNETTE_RADIAL_SIZE)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[OPCODE_ID_VIGNETTE_RADIAL] Invalid opcode size");
        return;
      }

      for(int i = 0; i < VIGNETTE_COEFFICIENTS; i++)
        cd->dng.cvig[i] = _get_double(&param[DNG_DOUBLE_SIZE * i]);
      for(int i = 0; i < OPCODE_CENTER_VALUES; i++)
        cd->dng.centre_vig[i] =
          _get_double(&param[DNG_DOUBLE_SIZE * (VIGNETTE_COEFFICIENTS + i)]);

      cd->dng.has_vignette = TRUE;
      img->exif_correction_type = CORRECTION_TYPE_DNG;
    }

    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 has unsupported %s opcode %d",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

    offset += OPCODE_HEADER_SIZE + param_size;
    count--;
  }
}
