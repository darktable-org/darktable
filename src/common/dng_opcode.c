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

#include <glib.h>
#include <stdio.h>

#include "debug.h"
#include "dng_opcode.h"

#define OPCODE_ID_GAINMAP (9)

static double get_double(uint8_t *ptr)
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

static float get_float(uint8_t *ptr)
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

static uint32_t get_long(uint8_t *ptr)
{
  uint32_t in;
  memcpy(&in, ptr, sizeof(in));
  return GUINT32_FROM_BE(in);
}

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  g_list_free_full(img->dng_gain_maps, g_free);
  img->dng_gain_maps = NULL;

  uint32_t count = get_long(&buf[0]);
  uint32_t offset = 4;
  while(count > 0)
  {
    uint32_t opcode_id = get_long(&buf[offset]);
    uint32_t flags = get_long(&buf[offset + 8]);
    uint32_t param_size = get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + 16];

    if(offset + 16 + param_size > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList2\n");
      return;
    }

    if(opcode_id == OPCODE_ID_GAINMAP)
    {
      uint32_t gain_count = (param_size - 76) / 4;
      dt_dng_gain_map_t *gm = g_malloc(sizeof(dt_dng_gain_map_t) + gain_count * sizeof(float));
      gm->top = get_long(&param[0]);
      gm->left = get_long(&param[4]);
      gm->bottom = get_long(&param[8]);
      gm->right = get_long(&param[12]);
      gm->plane = get_long(&param[16]);
      gm->planes = get_long(&param[20]);
      gm->row_pitch = get_long(&param[24]);
      gm->col_pitch = get_long(&param[28]);
      gm->map_points_v = get_long(&param[32]);
      gm->map_points_h = get_long(&param[36]);
      gm->map_spacing_v = get_double(&param[40]);
      gm->map_spacing_h = get_double(&param[48]);
      gm->map_origin_v = get_double(&param[56]);
      gm->map_origin_h = get_double(&param[64]);
      gm->map_planes = get_long(&param[72]);
      for(int i = 0; i < gain_count; i++)
        gm->map_gain[i] = get_float(&param[76 + 4*i]);

      img->dng_gain_maps = g_list_append(img->dng_gain_maps, gm);
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d\n",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

    offset += 16 + param_size;
    count--;
  }
}
