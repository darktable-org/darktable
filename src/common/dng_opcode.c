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
#include <string.h>

#include "debug.h"
#include "dng_opcode.h"

#define OPCODE_ID_GAINMAP (9)
#define OPCODE_ID_WARP_RECTILINEAR (1)
#define OPCODE_ID_VIGNETTE_RADIAL (3)

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


static void _put_long(uint8_t *ptr, uint32_t val)
{
  uint32_t out = GUINT32_TO_BE(val);
  memcpy(ptr, &out, sizeof(out));
}

static void _put_double(uint8_t *ptr, double val)
{
  guint64 out;
  union { guint64 in; double v; } u;
  u.v = val;
  out = GUINT64_TO_BE(u.in);
  memcpy(ptr, &out, sizeof(out));
}

static void _put_float(uint8_t *ptr, float val)
{
  guint32 out;
  union { guint32 in; float v; } u;
  u.v = val;
  out = GUINT32_TO_BE(u.in);
  memcpy(ptr, &out, sizeof(out));
}

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  g_list_free_full(img->dng_gain_maps, g_free);
  img->dng_gain_maps = NULL;

  uint32_t count = _get_long(&buf[0]);
  uint32_t offset = 4;
  while(count > 0)
  {
    uint32_t opcode_id = _get_long(&buf[offset]);
    uint32_t flags = _get_long(&buf[offset + 8]);
    uint32_t param_size = _get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + 16];

    if(offset + 16 + param_size > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList2");
      return;
    }

    if(opcode_id == OPCODE_ID_GAINMAP)
    {
      uint32_t gain_count = (param_size - 76) / 4;
      dt_dng_gain_map_t *gm = g_malloc(sizeof(dt_dng_gain_map_t) + gain_count * sizeof(float));
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
      for(int i = 0; i < gain_count; i++)
        gm->map_gain[i] = _get_float(&param[76 + 4*i]);

      img->dng_gain_maps = g_list_append(img->dng_gain_maps, gm);
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

    offset += 16 + param_size;
    count--;
  }
}

void dt_dng_opcode_process_opcode_list_3(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  dt_image_correction_data_t *cd = &img->exif_correction_data;
  cd->dng.has_warp = FALSE;
  cd->dng.has_vignette = FALSE;

  uint32_t count = _get_long(&buf[0]);
  uint32_t offset = 4;
  while(count > 0)
  {
    uint32_t opcode_id = _get_long(&buf[offset]);
    uint32_t flags = _get_long(&buf[offset + 8]);
    uint32_t param_size = _get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + 16];

    if(offset + 16 + param_size > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList3");
      return;
    }

    if(opcode_id == OPCODE_ID_WARP_RECTILINEAR)
    {
      const int planes = _get_long(&param[0]);
      if((planes != 1) && (planes != 3))
      {
        dt_print(DT_DEBUG_IMAGEIO, "[OPCODE_ID_WARP_RECTILINEAR] Invalid number of planes %i", planes);
        return;
      }

      cd->dng.planes = planes;
      for(int p = 0; p < planes; p++)
      {
        for(int i = 0; i < 6; i++)
          cd->dng.cwarp[p][i] = _get_double(&param[4 + 8 * (i + p*6)]);
      }

      for(int i = 0; i < 2; i++)
        cd->dng.centre_warp[i] = _get_double(&param[4 + 8 * (i + planes * 6)]);

      img->exif_correction_type = CORRECTION_TYPE_DNG;
      cd->dng.has_warp = TRUE;
    }

    else if(opcode_id == OPCODE_ID_VIGNETTE_RADIAL)
    {
      for(int i = 0; i < 5; i++)
        cd->dng.cvig[i] = _get_double(&param[8 * i]);
      for(int i = 0; i < 2; i++)
        cd->dng.centre_vig[i] = _get_double(&param[8 * (5 + i)]);

      cd->dng.has_vignette = TRUE;
      img->exif_correction_type = CORRECTION_TYPE_DNG;
    }

    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 has unsupported %s opcode %d",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

    offset += 16 + param_size;
    count--;
  }
}


// Serialize OpcodeList2 (gain maps) from dt_image_t to a buffer
// Returns a newly allocated buffer with the opcode list, or NULL on error.
// The caller is responsible for freeing the returned buffer.
uint8_t *dt_dng_opcode_serialize_opcode_list_2(const dt_image_t *img, uint32_t *size)
{
  // Only serialize if we have gain maps
  if(!img->dng_gain_maps || g_list_length(img->dng_gain_maps) == 0)
  {
    *size = 0;
    return NULL;
  }
  
  // Count total parameter size for all gain maps
  uint32_t total_param_size = 0;
  GList *list = img->dng_gain_maps;
  while(list)
  {
    dt_dng_gain_map_t *gm = (dt_dng_gain_map_t *)list->data;
    // Each gain map has 76 bytes fixed + gain_count * 4 bytes
    const uint32_t gain_count = gm->map_points_v * gm->map_points_h * gm->map_planes;
    total_param_size += 76 + gain_count * sizeof(float);
    list = g_list_next(list);
  }
  
  const int opcode_count = g_list_length(img->dng_gain_maps);
  uint32_t total_size = 4 + opcode_count * 16 + total_param_size;
  
  uint8_t *buf = g_malloc(total_size);
  if(!buf)
  {
    *size = 0;
    return NULL;
  }
  
  uint8_t *ptr = buf;
  
  // Write opcode count
  _put_long(ptr, opcode_count);
  ptr += 4;
  
  // Write each gain map opcode
  list = img->dng_gain_maps;
  while(list)
  {
    dt_dng_gain_map_t *gm = (dt_dng_gain_map_t *)list->data;
    
    // Opcode header
    _put_long(ptr, OPCODE_ID_GAINMAP); // opcode_id
    _put_long(ptr + 4, 0); // flags (0 = mandatory)
    _put_long(ptr + 8, 1); // version
    
    const uint32_t gain_count = gm->map_points_v * gm->map_points_h * gm->map_planes;
    const uint32_t param_size = 76 + gain_count * sizeof(float);
    _put_long(ptr + 12, param_size); // param_size
    
    ptr += 16;
    
    // Write parameters
    _put_long(ptr, gm->top);
    _put_long(ptr + 4, gm->left);
    _put_long(ptr + 8, gm->bottom);
    _put_long(ptr + 12, gm->right);
    _put_long(ptr + 16, gm->plane);
    _put_long(ptr + 20, gm->planes);
    _put_long(ptr + 24, gm->row_pitch);
    _put_long(ptr + 28, gm->col_pitch);
    _put_long(ptr + 32, gm->map_points_v);
    _put_long(ptr + 36, gm->map_points_h);
    _put_double(ptr + 40, gm->map_spacing_v);
    _put_double(ptr + 48, gm->map_spacing_h);
    _put_double(ptr + 56, gm->map_origin_v);
    _put_double(ptr + 64, gm->map_origin_h);
    _put_long(ptr + 72, gm->map_planes);
    ptr += 76;
    
    // Write gain values
    for(uint32_t i = 0; i < gain_count; i++)
    {
      _put_float(ptr, gm->map_gain[i]);
      ptr += 4;
    }
    
    list = g_list_next(list);
  }
  
  *size = total_size;
  return buf;
}


// Serialize OpcodeList3 (lens corrections) from dt_image_t to a buffer
// Returns a newly allocated buffer with the opcode list, or NULL on error.
// The caller is responsible for freeing the returned buffer.
uint8_t *dt_dng_opcode_serialize_opcode_list_3(const dt_image_t *img, uint32_t *size)
{
  const dt_image_correction_data_t *cd = &img->exif_correction_data;
  
  // Only serialize if we have DNG corrections and at least one type is present
  if(img->exif_correction_type != CORRECTION_TYPE_DNG) return NULL;
  if(!cd->dng.has_warp && !cd->dng.has_vignette) return NULL;
  
  // Count how many opcodes we need to serialize
  int opcode_count = 0;
  if(cd->dng.has_warp) opcode_count++;
  if(cd->dng.has_vignette) opcode_count++;
  
  // Calculate total buffer size:
  // - 4 bytes for opcode count
  // - For each opcode: 16 bytes header + parameter size
  //   - Warp rectilinear: 4 (planes) + 8*6 (coefficients per plane) + 8*2 (center) bytes
  //   - Vignette radial: 8*5 (coefficients) + 8*2 (center) = 56 bytes param
  uint32_t total_size = 4; // count at start
  if(cd->dng.has_warp)
    total_size += 16 + 4 + 8 * 6 * cd->dng.planes + 8 * 2; // header + param
  if(cd->dng.has_vignette)
    total_size += 16 + 8 * 5 + 8 * 2; // header + param
  
  uint8_t *buf = g_malloc(total_size);
  if(!buf) return NULL;
  
  uint8_t *ptr = buf;
  
  // Write opcode count
  _put_long(ptr, opcode_count);
  ptr += 4;
  
  // Write warp opcode if present
  if(cd->dng.has_warp)
  {
    // Opcode header: opcode_id (4), flags (4), version (4), param_size (4)
    _put_long(ptr, OPCODE_ID_WARP_RECTILINEAR); // opcode_id
    _put_long(ptr + 4, 0); // flags (0 = mandatory)
    _put_long(ptr + 8, 1); // version
    
    const uint32_t param_size = 4 + 8 * 6 * cd->dng.planes + 8 * 2;
    _put_long(ptr + 12, param_size); // param_size
    
    ptr += 16;
    
    // Write parameters: planes (4), then coefficients
    _put_long(ptr, cd->dng.planes);
    ptr += 4;
    
    // Write warp coefficients for each plane
    for(int p = 0; p < cd->dng.planes; p++)
    {
      for(int i = 0; i < 6; i++)
      {
        _put_double(ptr, cd->dng.cwarp[p][i]);
        ptr += 8;
      }
    }
    
    // Write center coordinates
    for(int i = 0; i < 2; i++)
    {
      _put_double(ptr, cd->dng.centre_warp[i]);
      ptr += 8;
    }
  }
  
  // Write vignette opcode if present
  if(cd->dng.has_vignette)
  {
    // Opcode header
    _put_long(ptr, OPCODE_ID_VIGNETTE_RADIAL); // opcode_id
    _put_long(ptr + 4, 0); // flags (0 = mandatory)
    _put_long(ptr + 8, 1); // version
    
    const uint32_t param_size = 8 * (5 + 2); // 5 coefficients + 2 center coordinates
    _put_long(ptr + 12, param_size); // param_size
    
    ptr += 16;
    
    // Write vignette coefficients
    for(int i = 0; i < 5; i++)
    {
      _put_double(ptr, cd->dng.cvig[i]);
      ptr += 8;
    }
    
    // Write center coordinates
    for(int i = 0; i < 2; i++)
    {
      _put_double(ptr, cd->dng.centre_vig[i]);
      ptr += 8;
    }
  }
  
  *size = total_size;
  return buf;
}
