/*
   This file is part of darktable,
   copyright (c) 2009--2010 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <xmmintrin.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include <gtk/gtk.h>
#include <inttypes.h>


DT_MODULE_INTROSPECTION(2, dt_iop_highlights_params_t)

typedef enum dt_iop_highlights_mode_t
{
  DT_IOP_HIGHLIGHTS_CLIP = 0,
  DT_IOP_HIGHLIGHTS_LCH = 1,
  DT_IOP_HIGHLIGHTS_INPAINT = 2,
} dt_iop_highlights_mode_t;

typedef struct dt_iop_highlights_params_t
{
  dt_iop_highlights_mode_t mode;
  float blendL, blendC, blendh; // unused
  float clip;
} dt_iop_highlights_params_t;

typedef struct dt_iop_highlights_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *mode;
} dt_iop_highlights_gui_data_t;

typedef dt_iop_highlights_params_t dt_iop_highlights_data_t;

typedef struct dt_iop_highlights_global_data_t
{
  int kernel_highlights_1f;
  int kernel_highlights_4f;
} dt_iop_highlights_global_data_t;

const char *name()
{
  return _("highlight reconstruction");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    memcpy(new_params, old_params, sizeof(dt_iop_highlights_params_t) - sizeof(float));
    dt_iop_highlights_params_t *n = (dt_iop_highlights_params_t *)new_params;
    n->clip = 1.0f;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  const float clip = d->clip
                     * fminf(piece->pipe->processed_maximum[0],
                             fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  const int filters = dt_image_filter(&piece->pipe->image);
  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) || !filters)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 4, sizeof(int), (void *)&d->mode);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_4f, 5, sizeof(float), (void *)&clip);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_4f, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 4, sizeof(int), (void *)&d->mode);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 5, sizeof(float), (void *)&clip);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 6, sizeof(int), (void *)&roi_out->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 7, sizeof(int), (void *)&roi_out->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highlights_1f, 8, sizeof(int), (void *)&filters);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highlights_1f, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  else
    return 4 * sizeof(float);
}

static uint8_t FCxtrans(const int row, const int col, const dt_iop_roi_t *const roi,
                        const uint8_t (*const xtrans)[6])
{
  return xtrans[(row + roi->y) % 6][(col + roi->x) % 6];
}

static inline void _interpolate_color_xtrans(void *ivoid, void *ovoid, const dt_iop_roi_t *const roi_in,
                                             const dt_iop_roi_t *const roi_out, int dim, int dir, int other,
                                             const float *clip, const uint8_t (*const xtrans)[6],
                                             const int pass)
{
  // similar to Bayer version, but in Bayer each row/column has only
  // green/red or green/blue transitions, in x-trans there can be
  // red/green, red/blue, and green/blue
  float ratios[2][3] = { { 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f } };
  float *in, *out;

  int i = 0, j = 0;
  if(dim == 0)
    j = other;
  else
    i = other;
  ssize_t offs = dim ? roi_out->width : 1;
  if(dir < 0) offs = -offs;
  int beg, end;
  if(dim == 0 && dir == 1)
  {
    beg = 0;
    end = roi_out->width;
  }
  else if(dim == 0 && dir == -1)
  {
    beg = roi_out->width - 1;
    end = -1;
  }
  else if(dim == 1 && dir == 1)
  {
    beg = 0;
    end = roi_out->height;
  }
  else if(dim == 1 && dir == -1)
  {
    beg = roi_out->height - 1;
    end = -1;
  }
  else
    return;

  if(dim == 1)
  {
    out = (float *)ovoid + i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + i + (size_t)beg * roi_out->width;
  }
  else
  {
    out = (float *)ovoid + beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + beg + (size_t)j * roi_out->width;
  }

  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;
    if(i < 2 || i > roi_out->width - 3 || j < 2 || j > roi_out->height - 3)
    {
      if(pass == 3) out[0] = in[0];
    }
    else
    {
      const uint8_t f0 = FCxtrans(j, i, roi_in, xtrans);
      const uint8_t f1 = FCxtrans(dim ? (j + dir) : j, dim ? i : (i + dir), roi_in, xtrans);
      const uint8_t f2 = FCxtrans(dim ? (j + dir * 2) : j, dim ? i : (i + dir * 2), roi_in, xtrans);
      const float clip0 = clip[f0];
      const float clip1 = clip[f1];
      const float clip2 = clip[f2];

      // record ratio to next different-colored pixel if this & next unclamped
      if(in[0] < clip0 && in[0] > 1e-5f)
      {
        if(in[offs] < clip1 && in[offs] > 1e-5f)
        {
          if(f0 != f1)
          { // not first of gg block
            if(f0 < f1)
              ratios[f0][f1] = (3.0f * ratios[f0][f1] + in[0] / in[offs]) / 4.0f;
            else
              ratios[f1][f0] = (3.0f * ratios[f1][f0] + in[offs] / in[0]) / 4.0f;
          }
          else
          {
            if(in[offs * 2] < clip2 && in[offs * 2] > 1e-5f)
            {
              if(f0 < f2)
                ratios[f0][f2] = (3.0f * ratios[f0][f2] + in[0] / in[offs * 2]) / 4.0f;
              else
                ratios[f2][f0] = (3.0f * ratios[f2][f0] + in[offs * 2] / in[0]) / 4.0f;
            }
          }
        }
      }

      if(in[0] >= clip0 - 1e-5f)
      {
        float add = 0.0f;
        if(f0 != f1) // not double green block
        {
          if(in[offs] >= clip1 - 1e-5f)
          {
            add = fmaxf(clip0, clip1);
          }
          else
          {
            if(f0 < f1)
              add = in[offs] * ratios[f0][f1];
            else
              add = in[offs] / ratios[f1][f0];
          }
        }
        else
        {
          if(1 && in[offs] < clip1 - 1e-5f) // adjacent green isn't clipped
          {
            add = in[offs];
          }
          else if(in[offs * 2] >= clip2 - 1e-5f)
          {
            add = fmaxf(clip0, clip2);
          }
          else
          {
            if(f0 < f2)
              add = in[offs * 2] * ratios[f0][f2];
            else
              add = in[offs * 2] / ratios[f2][f0];
          }
        }

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = (out[0] + add) / 4.0f;
        else
          out[0] += add;
      }
      else
      {
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

static int FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static inline void _interpolate_color(void *ivoid, void *ovoid, const dt_iop_roi_t *roi_out, int dim, int dir,
                                      int other, const float *clip, const uint32_t filters, const int pass)
{
  float ratio = 1.0f;
  float *in, *out;

  int i = 0, j = 0;
  if(dim == 0)
    j = other;
  else
    i = other;
  ssize_t offs = dim ? roi_out->width : 1;
  if(dir < 0) offs = -offs;
  int beg, end;
  if(dim == 0 && dir == 1)
  {
    beg = 0;
    end = roi_out->width;
  }
  else if(dim == 0 && dir == -1)
  {
    beg = roi_out->width - 1;
    end = -1;
  }
  else if(dim == 1 && dir == 1)
  {
    beg = 0;
    end = roi_out->height;
  }
  else if(dim == 1 && dir == -1)
  {
    beg = roi_out->height - 1;
    end = -1;
  }
  else
    return;

  if(dim == 1)
  {
    out = (float *)ovoid + i + (size_t)beg * roi_out->width;
    in = (float *)ivoid + i + (size_t)beg * roi_out->width;
  }
  else
  {
    out = (float *)ovoid + beg + (size_t)j * roi_out->width;
    in = (float *)ivoid + beg + (size_t)j * roi_out->width;
  }
  for(int k = beg; k != end; k += dir)
  {
    if(dim == 1)
      j = k;
    else
      i = k;
    const float clip0 = clip[FC(j, i, filters)];
    const float clip1 = clip[FC(dim ? (j + 1) : j, dim ? i : (i + 1), filters)];
    if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
    {
      if(pass == 3) out[0] = in[0];
    }
    else
    {
      if(in[0] < clip0 && in[0] > 1e-5f)
      { // both are not clipped
        if(in[offs] < clip1 && in[offs] > 1e-5f)
        { // update ratio, exponential decay. ratio = in[odd]/in[even]
          if(k & 1)
            ratio = (3.0f * ratio + in[0] / in[offs]) / 4.0f;
          else
            ratio = (3.0f * ratio + in[offs] / in[0]) / 4.0f;
        }
      }

      if(in[0] >= clip0 - 1e-5f)
      { // in[0] is clipped, restore it as in[1] adjusted according to ratio
        float add = 0.0f;
        if(in[offs] >= clip1 - 1e-5f)
          add = fmaxf(clip0, clip1);
        else if(k & 1)
          add = in[offs] * ratio;
        else
          add = in[offs] / ratio;

        if(pass == 0)
          out[0] = add;
        else if(pass == 3)
          out[0] = (out[0] + add) / 4.0f;
        else
          out[0] += add;
      }
      else
      {
        if(pass == 3) out[0] = in[0];
      }
    }
    out += offs;
    in += offs;
  }
}

static void process_lch_xtrans(void *ivoid, void *ovoid, const int width, const int height, const float clip)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(ovoid, ivoid)
#endif
  for(int j = 0; j < height; j++)
  {
    float *out = (float *)ovoid + (size_t)width * j;
    float *in = (float *)ivoid + (size_t)width * j;
    for(int i = 0; i < width; i++)
    {
      if(i < 3 || i > width - 3 || j < 3 || j > height - 3)
      {
        // fast path for border
        out[0] = in[0];
      }
      else
      {
        const float near_clip = 0.96f * clip;
        const float post_clip = 1.10f * clip;
        float blend = 0.0f;
        float mean = 0.0f;
        for(int jj = -3; jj < 3; jj++)
        {
          for(int ii = -3; ii < 3; ii++)
          {
            const float val = in[(size_t)jj * width + ii];
            mean += val;
            blend += (fminf(post_clip, val) - near_clip) / (post_clip - near_clip);
          }
        }
        blend = CLAMP(blend, 0.0f, 1.0f);
        if(blend > 0)
        {
          // recover:
          mean /= 36.0f;
          out[0] = blend * mean + (1.f - blend) * in[0];
        }
        else
          out[0] = in[0];
      }
      out++;
      in++;
    }
  }
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_filter(&piece->pipe->image);
  dt_iop_highlights_data_t *data = (dt_iop_highlights_data_t *)piece->data;

  const float clip = data->clip
                     * fminf(piece->pipe->processed_maximum[0],
                             fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
  // const int ch = piece->colors;
  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) || !filters)
  {
    const __m128 clipm = _mm_set1_ps(clip);
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      float *out = (float *)ovoid + (size_t)4 * roi_out->width * j;
      float *in = (float *)ivoid + (size_t)4 * roi_in->width * j;
      for(int i = 0; i < roi_out->width; i++)
      {
        _mm_stream_ps(out, _mm_min_ps(clipm, _mm_set_ps(in[3], in[2], in[1], in[0])));
        in += 4;
        out += 4;
      }
    }
    _mm_sfence();
    return;
  }

  switch(data->mode)
  {
    case DT_IOP_HIGHLIGHTS_INPAINT: // a1ex's (magiclantern) idea of color inpainting:
    {
      const float clips[4] = { 0.987 * data->clip * piece->pipe->processed_maximum[0],
                               0.987 * data->clip * piece->pipe->processed_maximum[1],
                               0.987 * data->clip * piece->pipe->processed_maximum[2], clip };

      if(filters == 9u)
      {
        const dt_image_t *img = &self->dev->image_storage;
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, img)
#endif
        for(int j = 0; j < roi_out->height; j++)
        {
          _interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, img->xtrans, 0);
          _interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, img->xtrans, 1);
        }
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, img)
#endif
        for(int i = 0; i < roi_out->width; i++)
        {
          _interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, img->xtrans, 2);
          _interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, img->xtrans, 3);
        }
        break;
      }

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        _interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
        _interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
      }

// up/down directions
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
      for(int i = 0; i < roi_out->width; i++)
      {
        _interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
        _interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
      }
      break;
    }
    case DT_IOP_HIGHLIGHTS_LCH:
      if(filters == 9u)
      {
        process_lch_xtrans(ivoid, ovoid, roi_out->width, roi_out->height, clip);
        break;
      }
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none) shared(ovoid, ivoid, roi_in, roi_out, data, piece)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        float *out = (float *)ovoid + (size_t)roi_out->width * j;
        float *in = (float *)ivoid + (size_t)roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++)
        {
          if(i == 0 || i == roi_out->width - 1 || j == 0 || j == roi_out->height - 1)
          {
            // fast path for border
            out[0] = in[0];
          }
          else
          {
            // analyse one bayer block to get same number of rggb pixels each time
            const float near_clip = 0.96f * clip;
            const float post_clip = 1.10f * clip;
            float blend = 0.0f;
            float mean = 0.0f;
            for(int jj = 0; jj <= 1; jj++)
            {
              for(int ii = 0; ii <= 1; ii++)
              {
                const float val = in[(size_t)jj * roi_out->width + ii];
                mean += val * 0.25f;
                blend += (fminf(post_clip, val) - near_clip) / (post_clip - near_clip);
              }
            }
            blend = CLAMP(blend, 0.0f, 1.0f);
            if(blend > 0)
            {
              // recover:
              out[0] = blend * mean + (1.f - blend) * in[0];
            }
            else
              out[0] = in[0];
          }
          out++;
          in++;
        }
      }
      break;
    default:
    case DT_IOP_HIGHLIGHTS_CLIP:
    {
      const __m128 clipm = _mm_set1_ps(clip);
      const size_t n = (size_t)roi_out->height * roi_out->width;
      float *const out = (float *)ovoid;
      float *const in = (float *)ivoid;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
      for(int j = 0; j < n; j += 4) _mm_stream_ps(out + j, _mm_min_ps(clipm, _mm_load_ps(in + j)));
      _mm_sfence();
      // lets see if there's a non-multiple of four rest to process:
      if(n & 3)
        for(size_t j = n & ~3u; j < n; j++) out[j] = MIN(clip, in[j]);
      break;
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void clip_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  p->clip = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mode_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;
  p->mode = dt_bauhaus_combobox_get(combo);
  if(p->mode > DT_IOP_HIGHLIGHTS_INPAINT) p->mode = DT_IOP_HIGHLIGHTS_INPAINT;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)p1;
  dt_iop_highlights_data_t *d = (dt_iop_highlights_data_t *)piece->data;
  memcpy(d, p, sizeof(*p));

  piece->process_cl_ready = 1;

  // x-trans images not implemented in OpenCL yet
  if(pipe->image.filters == 9u) piece->process_cl_ready = 0;

  // no OpenCL for DT_IOP_HIGHLIGHTS_INPAINT yet.
  if(d->mode == DT_IOP_HIGHLIGHTS_INPAINT) piece->process_cl_ready = 0;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_highlights_global_data_t *gd
      = (dt_iop_highlights_global_data_t *)malloc(sizeof(dt_iop_highlights_global_data_t));
  module->data = gd;
  gd->kernel_highlights_1f = dt_opencl_create_kernel(program, "highlights_1f");
  gd->kernel_highlights_4f = dt_opencl_create_kernel(program, "highlights_4f");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highlights_global_data_t *gd = (dt_iop_highlights_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highlights_4f);
  dt_opencl_free_kernel(gd->kernel_highlights_1f);
  free(module->data);
  module->data = NULL;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlights_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)module->params;
  dt_bauhaus_slider_set(g->clip, p->clip);
  dt_bauhaus_combobox_set(g->mode, p->mode);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_highlights_params_t tmp = (dt_iop_highlights_params_t){
    .mode = DT_IOP_HIGHLIGHTS_CLIP, .blendL = 1.0, .blendC = 0.0, .blendh = 0.0, .clip = 1.0
  };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  // only on for raw images:
  if(dt_image_is_raw(&module->dev->image_storage))
    module->default_enabled = 1;
  else
    module->default_enabled = 0;

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_highlights_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highlights_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_highlights_data_t));
  module->params = malloc(sizeof(dt_iop_highlights_params_t));
  module->default_params = malloc(sizeof(dt_iop_highlights_params_t));
  module->priority = 66; // module order created by iop_dependencies.py, do not edit!
  module->default_enabled = 1;
  module->params_size = sizeof(dt_iop_highlights_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlights_gui_data_t));
  dt_iop_highlights_gui_data_t *g = (dt_iop_highlights_gui_data_t *)self->gui_data;
  dt_iop_highlights_params_t *p = (dt_iop_highlights_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("method"));
  dt_bauhaus_combobox_add(g->mode, _("clip highlights"));
  dt_bauhaus_combobox_add(g->mode, _("reconstruct in LCh"));
  dt_bauhaus_combobox_add(g->mode, _("reconstruct color"));
  g_object_set(G_OBJECT(g->mode), "tooltip-text", _("highlight reconstruction method"), (char *)NULL);

  g->clip = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->clip, 3);
  g_object_set(G_OBJECT(g->clip), "tooltip-text",
               _("manually adjust the clipping threshold against"
                 " magenta highlights (you shouldn't ever need to touch this)"),
               (char *)NULL);
  dt_bauhaus_widget_set_label(g->clip, NULL, _("clipping threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->clip, TRUE, TRUE, 0);


  g_signal_connect(G_OBJECT(g->clip), "value-changed", G_CALLBACK(clip_callback), self);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
