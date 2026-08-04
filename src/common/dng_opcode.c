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
#include "common/math.h"
#include "develop/imageop.h"

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

static void _parse_and_append_gain_map(uint8_t *param, uint32_t param_size, GList **dng_gain_maps,
                                        const char *source)
{
  uint32_t gain_count = (param_size - 76) / 4;
  dt_dng_gain_map_t *gm = g_malloc(sizeof(dt_dng_gain_map_t) + gain_count * sizeof(float));
  gm->top           = _get_long(&param[0]);
  gm->left          = _get_long(&param[4]);
  gm->bottom        = _get_long(&param[8]);
  gm->right         = _get_long(&param[12]);
  gm->plane         = _get_long(&param[16]);
  gm->planes        = _get_long(&param[20]);
  gm->row_pitch     = _get_long(&param[24]);
  gm->col_pitch     = _get_long(&param[28]);
  gm->map_points_v  = _get_long(&param[32]);
  gm->map_points_h  = _get_long(&param[36]);
  gm->map_spacing_v = _get_double(&param[40]);
  gm->map_spacing_h = _get_double(&param[48]);
  gm->map_origin_v  = _get_double(&param[56]);
  gm->map_origin_h  = _get_double(&param[64]);
  gm->map_planes    = _get_long(&param[72]);
  for(int i = 0; i < gain_count; i++)
    gm->map_gain[i] = _get_float(&param[76 + 4 * i]);
  *dng_gain_maps = g_list_append(*dng_gain_maps, gm);
  dt_print(DT_DEBUG_IMAGEIO,
    "[dng_opcode] %s GainMap parsed: top=%u left=%u bottom=%u right=%u "
    "plane=%u planes=%u row_pitch=%u col_pitch=%u "
    "map_points_v=%u map_points_h=%u map_planes=%u gain_count=%u",
    source, gm->top, gm->left, gm->bottom, gm->right,
    gm->plane, gm->planes, gm->row_pitch, gm->col_pitch,
    gm->map_points_v, gm->map_points_h, gm->map_planes, gain_count);
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
      _parse_and_append_gain_map(param, param_size, &(img->dng_gain_maps), "OpcodeList2");
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
  g_list_free_full(img->dng_gain_maps_opcode3, g_free);
  img->dng_gain_maps_opcode3 = NULL;

  dt_image_correction_data_t *cd = &img->exif_correction_data;
  cd->dng.has_warp = FALSE;
  cd->dng.has_vignette = FALSE;
  cd->dng.has_gainmap = FALSE;

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

    else if(opcode_id == OPCODE_ID_GAINMAP)
    {
      _parse_and_append_gain_map(param, param_size, &(img->dng_gain_maps_opcode3), "OpcodeList3");
      // unlike WarpRectilinear and VignetteRadial this does not set
      // exif_correction_type: the GainMap is applied by demosaic or rawprepare, not
      // by lens, so a file whose only OpcodeList3 entry is a GainMap must not be
      // pushed into the lens embedded metadata method
      cd->dng.has_gainmap = TRUE;
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

/* GainMap application, shared by demosaic (CFA raws) and rawprepare (linear DNGs).
   Both see the data as three colour planes, which is the stage OpcodeList3 is
   defined for, so neither needs any CFA logic. */

const dt_dng_gain_map_t *dt_dng_gain_map_opcode3(const dt_image_t *img)
{
  if(g_list_length(img->dng_gain_maps_opcode3) != 1)
  {
    // an empty list is the normal case and not worth reporting
    if(img->dng_gain_maps_opcode3)
      dt_print(DT_DEBUG_IMAGEIO,
        "[dng_opcode] GainMap rejected: found %u maps, need exactly 1",
        g_list_length(img->dng_gain_maps_opcode3));
    return NULL;
  }

  const dt_dng_gain_map_t *g =
    (const dt_dng_gain_map_t *)g_list_nth_data(img->dng_gain_maps_opcode3, 0);

  /* DNG 1.7.1 p.115 leaves MapPlanes free and says "If Planes > MapPlanes, the
     last gain map plane is used for any remaining planes being modified", so a
     one plane map covering all three channels is legal and is what a pure
     falloff correction looks like. Only 1 and 3 are meaningful for three plane
     data, so anything else is rejected rather than guessed at.

     The map itself need not cover the whole frame -- outside its bounds the
     spec replicates the edge values, which is what the clamp in the sampler
     does -- but Top/Left/Bottom/Right bound the *area modified*, and the spec
     does not say whether MapOrigin and MapSpacing are then relative to that
     rectangle or to the whole image. Requiring the full frame makes the two
     readings identical, so that ambiguity cannot bite us. */
  if(g == NULL
     || g->plane != 0
     || g->planes != 3
     || (g->map_planes != 1 && g->map_planes != 3)
     || g->row_pitch != 1
     || g->col_pitch != 1
     || g->map_points_v < 2
     || g->map_points_h < 2
     /* The spacings are reciprocated below, so reject anything that will not
        survive it. Written as !(x > 0.0) rather than x <= 0.0 so that a NaN
        read from a malformed file is rejected too, and the reciprocal is
        checked because a positive but denormal spacing overflows to infinity
        once it is narrowed to float. */
     || !(g->map_spacing_v > 0.0)
     || !(g->map_spacing_h > 0.0)
     || !dt_isfinite((float)g->map_spacing_v)
     || !dt_isfinite((float)g->map_spacing_h)
     || !dt_isfinite((float)(1.0 / g->map_spacing_v))
     || !dt_isfinite((float)(1.0 / g->map_spacing_h))
     || !dt_isfinite((float)g->map_origin_v)
     || !dt_isfinite((float)g->map_origin_h)
     || g->top != 0
     || g->left != 0
     || g->bottom != (uint32_t)img->height
     || g->right != (uint32_t)img->width)
  {
    dt_print(DT_DEBUG_IMAGEIO,
      "[dng_opcode] GainMap rejected: plane=%u planes=%u map_planes=%u "
      "row_pitch=%u col_pitch=%u map_points_v=%u map_points_h=%u "
      "top=%u left=%u bottom=%u right=%u (image %dx%d)",
      g ? g->plane : 0, g ? g->planes : 0, g ? g->map_planes : 0,
      g ? g->row_pitch : 0, g ? g->col_pitch : 0,
      g ? g->map_points_v : 0, g ? g->map_points_h : 0,
      g ? g->top : 0, g ? g->left : 0, g ? g->bottom : 0, g ? g->right : 0,
      img->width, img->height);
    return NULL;
  }

  return g;
}

void dt_dng_gain_map_apply(const dt_dng_gain_map_t *gm,
                           const dt_image_t *img,
                           const float *const in,
                           float *const out,
                           const int width,
                           const int height,
                           const float x0,
                           const float y0,
                           const float step)
{
  const int map_w = gm->map_points_h;
  const int map_h = gm->map_points_v;
  const int nplanes = gm->map_planes;

  /* The GainMap is defined over the full raw frame, so normalise the raw
     coordinates the caller gave us against the raw dimensions. The half pixel
     offset is the pixel-centred convention the spec spells out for these
     fields (DNG 1.7.1 p.72). */
  const float raw_w = (float)img->width;
  const float raw_h = (float)img->height;

  const float rel_to_map_x = 1.0f / gm->map_spacing_h;
  const float rel_to_map_y = 1.0f / gm->map_spacing_v;
  const float map_origin_h = gm->map_origin_h;
  const float map_origin_v = gm->map_origin_v;

  DT_OMP_FOR()
  for(int row = 0; row < height; row++)
  {
    const float rel_y = (y0 + ((float)row + 0.5f) * step) / raw_h;
    const float y_map = CLAMPF((rel_y - map_origin_v) * rel_to_map_y, 0.0f, (float)(map_h - 1));
    const int y_i0 = MIN((int)y_map, map_h - 1);
    const int y_i1 = MIN(y_i0 + 1, map_h - 1);
    const float y_frac = y_map - y_i0;

    for(int col = 0; col < width; col++)
    {
      const size_t idx = 4 * (size_t)(row * width + col);
      const float rel_x = (x0 + ((float)col + 0.5f) * step) / raw_w;
      const float x_map = CLAMPF((rel_x - map_origin_h) * rel_to_map_x, 0.0f, (float)(map_w - 1));
      const int x_i0 = MIN((int)x_map, map_w - 1);
      const int x_i1 = MIN(x_i0 + 1, map_w - 1);
      const float x_frac = x_map - x_i0;

      for_three_channels(c)
      {
        // "the last gain map plane is used for any remaining planes being modified"
        const int p = MIN(c, nplanes - 1);
        const float g00 = gm->map_gain[(y_i0 * map_w + x_i0) * nplanes + p];
        const float g01 = gm->map_gain[(y_i0 * map_w + x_i1) * nplanes + p];
        const float g10 = gm->map_gain[(y_i1 * map_w + x_i0) * nplanes + p];
        const float g11 = gm->map_gain[(y_i1 * map_w + x_i1) * nplanes + p];

        const float gain_top    = (1.0f - x_frac) * g00 + x_frac * g01;
        const float gain_bottom = (1.0f - x_frac) * g10 + x_frac * g11;

        out[idx + c] = in[idx + c] * ((1.0f - y_frac) * gain_top + y_frac * gain_bottom);
      }

      out[idx + 3] = in[idx + 3];
    }
  }
}

#ifdef HAVE_OPENCL
dt_dng_gain_map_cl_global_t *dt_dng_gain_map_init_cl_global(void)
{
  dt_dng_gain_map_cl_global_t *g = malloc(sizeof(dt_dng_gain_map_cl_global_t));

  const int program = 44; // dng_opcode.cl, from programs.conf
  g->kernel_dng_gain_map = dt_opencl_create_kernel(program, "dng_gain_map");
  return g;
}

void dt_dng_gain_map_free_cl_global(dt_dng_gain_map_cl_global_t *g)
{
  if(!g) return;
  dt_opencl_free_kernel(g->kernel_dng_gain_map);
  free(g);
}

int dt_dng_gain_map_apply_cl(const int devid,
                             const dt_dng_gain_map_t *gm,
                             const dt_image_t *img,
                             cl_mem dev_in,
                             cl_mem dev_out,
                             const int width,
                             const int height,
                             const float x0,
                             const float y0,
                             const float step)
{
  const int map_w = gm->map_points_h;
  const int map_h = gm->map_points_v;
  const int nplanes = gm->map_planes;
  const size_t ngains = (size_t)map_w * map_h * nplanes;

  cl_mem dev_map = dt_opencl_copy_host_to_device_constant
    (devid, ngains * sizeof(float), (void *)gm->map_gain);
  if(dev_map == NULL) return DT_OPENCL_SYSMEM_ALLOCATION;

  // keep these in step with dt_dng_gain_map_apply()
  const float raw_w = (float)img->width;
  const float raw_h = (float)img->height;
  const float rel_to_map_x = 1.0f / gm->map_spacing_h;
  const float rel_to_map_y = 1.0f / gm->map_spacing_v;
  const float map_origin_h = gm->map_origin_h;
  const float map_origin_v = gm->map_origin_v;

  const int err = dt_opencl_enqueue_kernel_2d_args
    (devid, darktable.opencl->dng_gain_map->kernel_dng_gain_map, width, height,
     CLARG(dev_in), CLARG(dev_out), CLARG(dev_map),
     CLARG(width), CLARG(height),
     CLARG(map_w), CLARG(map_h), CLARG(nplanes),
     CLARG(x0), CLARG(y0), CLARG(step),
     CLARG(raw_w), CLARG(raw_h),
     CLARG(rel_to_map_x), CLARG(rel_to_map_y),
     CLARG(map_origin_h), CLARG(map_origin_v));

  dt_opencl_release_mem_object(dev_map);
  return err;
}
#endif
