/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#include "blend.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include <math.h>


void dt_develop_blendif_process_parameters(float *const restrict parameters,
                                           const dt_develop_blend_params_t *const params,
                                           const dt_iop_colorspace_type_t cst)
{
  const uint32_t blendif = params->blendif;
  const float *blendif_parameters = params->blendif_parameters;
  for(size_t i = 0, j = 0; i < DEVELOP_BLENDIF_SIZE; i++, j += DEVELOP_BLENDIF_PARAMETER_ITEMS)
  {
    if(blendif & (1 << i))
    {
      float offset = 0.0f;
      if(cst == iop_cs_Lab && (i == DEVELOP_BLENDIF_A_in || i == DEVELOP_BLENDIF_A_out
          || i == DEVELOP_BLENDIF_B_in || i == DEVELOP_BLENDIF_B_out))
      {
        offset = 0.5f;
      }
      parameters[j + 0] = blendif_parameters[i * 4 + 0] - offset;
      parameters[j + 1] = blendif_parameters[i * 4 + 1] - offset;
      parameters[j + 2] = blendif_parameters[i * 4 + 2] - offset;
      parameters[j + 3] = blendif_parameters[i * 4 + 3] - offset;
      // pre-compute increasing slope and decreasing slope
      parameters[j + 4] = 1.0f / fmaxf(0.001f, parameters[j + 1] - parameters[j + 0]);
      parameters[j + 5] = 1.0f / fmaxf(0.001f, parameters[j + 3] - parameters[j + 2]);
      // handle the case when one end is open to avoid clipping input/output values
      if(blendif_parameters[i * 4 + 0] <= -offset && blendif_parameters[i * 4 + 1] <= -offset)
      {
        parameters[j + 0] = -INFINITY;
        parameters[j + 1] = -INFINITY;
      }
      if(blendif_parameters[i * 4 + 2] >= 1.0f - offset && blendif_parameters[i * 4 + 3] >= 1.0f - offset)
      {
        parameters[j + 2] = INFINITY;
        parameters[j + 3] = INFINITY;
      }
    }
    else
    {
      parameters[j + 0] = -INFINITY;
      parameters[j + 1] = -INFINITY;
      parameters[j + 2] = INFINITY;
      parameters[j + 3] = INFINITY;
      parameters[j + 4] = 0.0f;
      parameters[j + 5] = 0.0f;
    }
  }
}

void dt_develop_blend_process(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                              const void *const ivoid, void *const ovoid, const struct dt_iop_roi_t *const roi_in,
                              const struct dt_iop_roi_t *const roi_out)
{
  if(piece->pipe->bypass_blendif && self->dev->gui_attached && (self == self->dev->gui_module)) return;

  const dt_develop_blend_params_t *const d = (const dt_develop_blend_params_t *const)piece->blendop_data;
  if(!d) return;

  const unsigned int mask_mode = d->mask_mode;
  // check if blend is disabled
  if(!(mask_mode & DEVELOP_MASK_ENABLED)) return;

  const int ch = piece->colors;           // the number of channels in the buffer
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const _Bool rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

  // In most cases of blending-enabled modules input and output of the module have
  // the exact same dimensions. Only in very special cases we allow a module's input
  // to exceed its output. This is namely the case for the spot removal module where
  // the source of a patch might lie outside the roi of the output image. Therefore:
  // We can only handle blending if roi_out and roi_in have the same scale and
  // if roi_out fits into the area given by roi_in. xoffs and yoffs describe the relative
  // offset of the input image to the output image.
  if(oscale != iscale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0) && (owidth + xoffs > iwidth || oheight + yoffs > iheight)))
  {
    dt_control_log(_("skipped blending in module '%s': roi's do not match"), self->op);
    return;
  }

  // does user want us to display a specific channel?
  const dt_dev_pixelpipe_display_mask_t request_mask_display =
    (self->dev->gui_attached && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe)
     && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL))
        ? self->request_mask_display
        : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_iop_colorspace_type_t cst = self->blend_colorspace(self, piece->pipe, piece);

  // check if mask should be suppressed temporarily (i.e. just set to global
  // opacity value)
  const _Bool suppress_mask = self->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                              && (piece->pipe == self->dev->pipe) && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL);
  const _Bool mask_feather = d->feathering_radius > 0.1f;
  const _Bool mask_blur = d->blur_radius > 0.1f;
  const _Bool mask_tone_curve = fabsf(d->contrast) >= 0.01f || fabsf(d->brightness) >= 0.01f;

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  // allocate space for blend mask
  float *const restrict _mask = dt_alloc_align(64, buffsize * sizeof(float));
  if(!_mask)
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return;
  }
  float *const restrict mask = _mask;

  if(mask_mode == DEVELOP_MASK_ENABLED || suppress_mask)
  {
    // blend uniformly (no drawn or parametric mask)

#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(mask:64)\
    dt_omp_firstprivate(buffsize, mask, opacity) schedule(static)
#endif
    for(size_t i = 0; i < buffsize; i++) mask[i] = opacity;
  }
  else if(mask_mode & DEVELOP_MASK_RASTER)
  {
    /* use a raster mask from another module earlier in the pipe */
    gboolean free_mask = FALSE; // if no transformations were applied we get the cached original back
    float *raster_mask = dt_dev_get_raster_mask(piece->pipe, self->raster_mask.sink.source, self->raster_mask.sink.id,
                                                self, &free_mask);

    if(raster_mask)
    {
      // invert if required
      if(d->raster_mask_invert)
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) aligned(mask, raster_mask:64)\
        dt_omp_firstprivate(buffsize, mask, opacity, raster_mask) \
        schedule(static)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = (1.0 - raster_mask[i]) * opacity;
      else
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) aligned(mask, raster_mask:64)\
        dt_omp_firstprivate(buffsize, mask, opacity, raster_mask) \
        schedule(static)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = raster_mask[i] * opacity;
      if(free_mask) dt_free_align(raster_mask);
    }
    else
    {
      // fallback for when the raster mask couldn't be applied
      const float value = d->raster_mask_invert ? 0.0 : 1.0;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) aligned(mask:64)\
      dt_omp_firstprivate(buffsize, mask, value) schedule(static)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = value;
    }
  }
  else
  {
    // we blend with a drawn and/or parametric mask

    // get the drawn mask if there is one
    dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, d->mask_id);

    if(form && (!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      dt_masks_group_render_roi(self, piece, form, roi_out, mask);

      if(d->mask_combine & DEVELOP_COMBINE_MASKS_POS)
      {
        // if we have a mask and this flag is set -> invert the mask
#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(mask:64)\
        dt_omp_firstprivate(buffsize, mask) schedule(static)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = 1.0f - mask[i];
      }
    }
    else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(mask:64)\
      dt_omp_firstprivate(buffsize, mask, fill) schedule(static)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(mask:64)\
      dt_omp_firstprivate(buffsize, mask, fill) schedule(static)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = fill;
    }

    // get parametric mask (if any) and apply global opacity
    switch(cst)
    {
      case iop_cs_Lab:
        dt_develop_blendif_lab_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      case iop_cs_rgb:
        dt_develop_blendif_rgb_hsl_make_mask(piece, (const float *const restrict)ivoid,
                                             (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      case iop_cs_RAW:
        dt_develop_blendif_raw_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      default:
        break;
    }

    if(mask_feather)
    {
      int w = (int)(2 * d->feathering_radius * roi_out->scale / piece->iscale + 0.5f);
      if(w < 1) w = 1;
      float sqrt_eps = 1.f;
      float guide_weight = 1.f;
      switch(cst)
      {
        case iop_cs_rgb:
          guide_weight = 100.f;
          break;
        case iop_cs_Lab:
          guide_weight = 1.f;
          break;
        case iop_cs_RAW:
        default:
          assert(0);
      }
      float *const restrict mask_bak = dt_alloc_align(64, sizeof(*mask_bak) * buffsize);
      if(mask_bak)
      {
        memcpy(mask_bak, mask, sizeof(*mask_bak) * buffsize);
        float *guide = (d->feathering_guide == DEVELOP_MASK_GUIDE_IN) ? (float *const restrict)ivoid
                                                                      : (float *const restrict)ovoid;
        if(!rois_equal && d->feathering_guide == DEVELOP_MASK_GUIDE_IN)
        {
          float *const restrict guide_tmp = dt_alloc_align(64, sizeof(*guide_tmp) * buffsize * ch);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(ch, guide_tmp, ivoid, iwidth, oheight, owidth, xoffs, yoffs)
#endif
          for(size_t y = 0; y < oheight; y++)
          {
            size_t iindex = ((size_t)(y + yoffs) * iwidth + xoffs) * ch;
            size_t oindex = (size_t)y * owidth * ch;
            memcpy(guide_tmp + oindex, (float *)ivoid + iindex, sizeof(*guide_tmp) * owidth * ch);
          }
          guide = guide_tmp;
        }
        guided_filter(guide, mask_bak, mask, owidth, oheight, ch, w, sqrt_eps, guide_weight, 0.f, 1.f);
        if(!rois_equal && d->feathering_guide == DEVELOP_MASK_GUIDE_IN) dt_free_align(guide);
        dt_free_align(mask_bak);
      }
    }
    if(mask_blur)
    {
      const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
      const float mmax[] = { 1.0f };
      const float mmin[] = { 0.0f };

      dt_gaussian_t *g = dt_gaussian_init(owidth, oheight, 1, mmax, mmin, sigma, 0);
      if(g)
      {
        dt_gaussian_blur(g, mask, mask);
        dt_gaussian_free(g);
      }
    }

    if(mask_tone_curve && opacity > 1e-4f)
    {
      const float mask_epsilon = 16 * FLT_EPSILON;  // empirical mask threshold for fully transparent masks
      const float e = expf(3.f * d->contrast);
      const float brightness = d->brightness;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) aligned(mask:64)\
      dt_omp_firstprivate(brightness, buffsize, e, mask, mask_epsilon, opacity) schedule(static)
#endif
      for(size_t k = 0; k < buffsize; k++)
      {
        float x = mask[k] / opacity;
        x = 2.f * x - 1.f;
        if (1.f - brightness <= 0.f)
          x = mask[k] <= mask_epsilon ? -1.f : 1.f;
        else if (1.f + brightness <= 0.f)
          x = mask[k] >= 1.f - mask_epsilon ? 1.f : -1.f;
        else if (brightness > 0.f)
        {
          x = (x + brightness) / (1.f - brightness);
          x = fminf(x, 1.f);
        }
        else
        {
          x = (x + brightness) / (1.f + brightness);
          x = fmaxf(x, -1.f);
        }
        mask[k] = clamp_range_f(
            ((x * e / (1.f + (e - 1.f) * fabsf(x))) / 2.f + 0.5f) * opacity, 0.f, 1.f);
      }
    }
  }

  // now apply blending with per-pixel opacity value as defined in mask
  // select the blend operator
  switch(cst)
  {
    case iop_cs_Lab:
      dt_develop_blendif_lab_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                   roi_in, roi_out, mask, request_mask_display);
      break;
    case iop_cs_rgb:
      dt_develop_blendif_rgb_hsl_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                       roi_in, roi_out, mask, request_mask_display);
      break;
    case iop_cs_RAW:
      dt_develop_blendif_raw_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                   roi_in, roi_out, mask, request_mask_display);
      break;
    default:
      break;
  }

  // register if _this_ module should expose mask or display channel
  if(request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = request_mask_display;
  }

  // check if we should store the mask for export or use in subsequent modules
  // TODO: should we skip raster masks?
  if(piece->pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(self, 0))
  {
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(0), _mask);
  }
  else
  {
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(0));
    dt_free_align(_mask);
  }
}

#ifdef HAVE_OPENCL
int dt_develop_blend_process_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                                cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out)
{
  if(piece->pipe->bypass_blendif && self->dev->gui_attached && (self == self->dev->gui_module)) return TRUE;

  dt_develop_blend_params_t *const d = (dt_develop_blend_params_t *const)piece->blendop_data;
  if(!d) return TRUE;

  const unsigned int mask_mode = d->mask_mode;
  // check if blend is disabled: just return, output is already in dev_out
  if(!(mask_mode & DEVELOP_MASK_ENABLED)) return TRUE;

  const int ch = piece->colors; // the number of channels in the buffer
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const _Bool rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

  // In most cases of blending-enabled modules input and output of the module have
  // the exact same dimensions. Only in very special cases we allow a module's input
  // to exceed its output. This is namely the case for the spot removal module where
  // the source of a patch might lie outside the roi of the output image. Therefore:
  // We can only handle blending if roi_out and roi_in have the same scale and
  // if roi_out fits into the area given by roi_in. xoffs and yoffs describe the relative
  // offset of the input image to the output image. */
  if(oscale != iscale || xoffs < 0 || yoffs < 0
     || ((xoffs > 0 || yoffs > 0) && (owidth + xoffs > iwidth || oheight + yoffs > iheight)))
  {
    dt_control_log(_("skipped blending in module '%s': roi's do not match"), self->op);
    return TRUE;
  }

  // only non-zero if mask_display was set by an _earlier_ module
  const dt_dev_pixelpipe_display_mask_t mask_display = piece->pipe->mask_display;

  // does user want us to display a specific channel?
  const dt_dev_pixelpipe_display_mask_t request_mask_display
      = (self->dev->gui_attached && (self == self->dev->gui_module) && (piece->pipe == self->dev->pipe)
         && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL))
            ? self->request_mask_display
            : DT_DEV_PIXELPIPE_DISPLAY_NONE;

  // get channel max values depending on colorspace
  const dt_iop_colorspace_type_t cst = self->blend_colorspace(self, piece->pipe, piece);

  // check if mask should be suppressed temporarily (i.e. just set to global
  // opacity value)
  const _Bool suppress_mask = self->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                              && (piece->pipe == self->dev->pipe) && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL);
  const _Bool mask_feather = d->feathering_radius > 0.1f;
  const _Bool mask_blur = d->blur_radius > 0.1f;
  const _Bool mask_tone_curve = fabsf(d->contrast) >= 0.01f || fabsf(d->brightness) >= 0.01f;

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(0.0f, (d->opacity / 100.0f)), 1.0f);

  // allocate space for blend mask
  float *_mask = dt_alloc_align(64, buffsize * sizeof(float));
  if(!_mask)
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return FALSE;
  }
  float *const mask = _mask;

  // setup some kernels
  int kernel_mask;
  int kernel;
  switch(cst)
  {
    case iop_cs_RAW:
      kernel = darktable.opencl->blendop->kernel_blendop_RAW;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_RAW;
      break;

    case iop_cs_rgb:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb;
      break;

    case iop_cs_Lab:
    default:
      kernel = darktable.opencl->blendop->kernel_blendop_Lab;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_Lab;
      break;
  }
  int kernel_mask_tone_curve = darktable.opencl->blendop->kernel_blendop_mask_tone_curve;
  int kernel_set_mask = darktable.opencl->blendop->kernel_blendop_set_mask;
  int kernel_display_channel = darktable.opencl->blendop->kernel_blendop_display_channel;

  const int devid = piece->pipe->devid;
  const int offs[2] = { xoffs, yoffs };
  const size_t sizes[] = { ROUNDUPWD(owidth), ROUNDUPHT(oheight), 1 };

  cl_int err = -999;
  cl_mem dev_m = NULL;
  cl_mem dev_mask_1 = NULL;
  cl_mem dev_mask_2 = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_guide = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { owidth, oheight, 1 };

  // parameters, for every channel the 4 limits + pre-computed increasing slope and decreasing slope
  float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
  dt_develop_blendif_process_parameters(parameters, d, cst);

  // copy blend parameters to constant device memory
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE, parameters);
  if(dev_m == NULL) goto error;

  dev_mask_1 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
  if(dev_mask_1 == NULL) goto error;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  if(mask_mode == DEVELOP_MASK_ENABLED || suppress_mask)
  {
    // blend uniformly (no drawn or parametric mask)

    // set dev_mask with global opacity value
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 0, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 1, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 2, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_set_mask, 3, sizeof(float), (void *)&opacity);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_set_mask, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(mask_mode & DEVELOP_MASK_RASTER)
  {
    /* use a raster mask from another module earlier in the pipe */
    gboolean free_mask = FALSE; // if no transformations were applied we get the cached original back
    float *raster_mask = dt_dev_get_raster_mask(piece->pipe, self->raster_mask.sink.source, self->raster_mask.sink.id,
                                                self, &free_mask);

    if(raster_mask)
    {
      // invert if required
      if(d->raster_mask_invert)
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
        dt_omp_firstprivate(buffsize, mask, opacity) \
        shared(raster_mask)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = (1.0 - raster_mask[i]) * opacity;
      else
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
        dt_omp_firstprivate(buffsize, mask, opacity) \
        shared(raster_mask)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = raster_mask[i] * opacity;
      if(free_mask) dt_free_align(raster_mask);
    }
    else
    {
      // fallback for when the raster mask couldn't be applied
      const float value = d->raster_mask_invert ? 0.0 : 1.0;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, mask, value)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = value;
    }

    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    // we blend with a drawn and/or parametric mask

    // get the drawn mask if there is one
    dt_masks_form_t *form = dt_masks_get_from_id_ext(piece->pipe->forms, d->mask_id);

    if(form && (!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      dt_masks_group_render_roi(self, piece, form, roi_out, mask);

      if(d->mask_combine & DEVELOP_COMBINE_MASKS_POS)
      {
        // if we have a mask and this flag is set -> invert the mask
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, mask)
#endif
        for(size_t i = 0; i < buffsize; i++) mask[i] = 1.0f - mask[i];
      }
    }
    else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, fill, mask)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
#ifdef _OPENMP
  #pragma omp parallel for default(none) \
      dt_omp_firstprivate(buffsize, fill, mask)
#endif
      for(size_t i = 0; i < buffsize; i++) mask[i] = fill;
    }

    // write mask from host to device
    dev_mask_2 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
    if(dev_mask_2 == NULL) goto error;
    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS) goto error;

    // The following call to clFinish() works around a bug in some OpenCL
    // drivers (namely AMD).
    // Without this synchronization point, reads to dev_in would often not
    // return the correct value.
    // This depends on the module after which blending is called. One of the
    // affected ones is sharpen.
    dt_opencl_finish(devid);

    // get parametric mask (if any) and apply global opacity
    const unsigned blendif = d->blendif;
    const unsigned int mask_combine = d->mask_combine;
    const int use_work_profile = (work_profile == NULL) ? 0 : 1;
    dt_opencl_set_kernel_arg(devid, kernel_mask, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 3, sizeof(cl_mem), (void *)&dev_mask_2);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 6, sizeof(float), (void *)&opacity);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 7, sizeof(unsigned), (void *)&blendif);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 8, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 9, sizeof(unsigned), (void *)&mask_mode);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 10, sizeof(unsigned), (void *)&mask_combine);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 11, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 12, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 13, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, kernel_mask, 14, sizeof(int), (void *)&use_work_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_develop_blend_process_cl] error %i enqueue kernel\n", err);
      goto error;
    }

    if(mask_feather)
    {
      int w = (int)(2 * d->feathering_radius * roi_out->scale / piece->iscale + 0.5f);
      if (w < 1)
        w = 1;
      float sqrt_eps = 1.f;
      float guide_weight = 1.f;
      switch(cst)
      {
      case iop_cs_rgb:
        guide_weight = 100.f;
        break;
      case iop_cs_Lab:
        guide_weight = 1.f;
        break;
      case iop_cs_RAW:
      default:
        assert(0);
      }
      cl_mem guide = d->feathering_guide == DEVELOP_MASK_GUIDE_IN ? dev_in : dev_out;
      if(!rois_equal && d->feathering_guide == DEVELOP_MASK_GUIDE_IN)
      {
        dev_guide = dt_opencl_alloc_device(devid, owidth, oheight, 4 * sizeof(float));
        if(dev_guide == NULL) goto error;
        guide = dev_guide;
        size_t origin_1[] = { xoffs, yoffs, 0 };
        size_t origin_2[] = { 0, 0, 0 };
        err = dt_opencl_enqueue_copy_image(devid, dev_in, guide, origin_2, origin_1, region);
        if(err != CL_SUCCESS) goto error;
      }
      guided_filter_cl(devid, guide, dev_mask_2, dev_mask_1, owidth, oheight, ch, w, sqrt_eps, guide_weight, 0.f,
                       1.f);
      if(dev_guide)
      {
        dt_opencl_release_mem_object(dev_guide);
        dev_guide = NULL;
      }
    }
    else
    {
      cl_mem tmp = dev_mask_1;
      dev_mask_1 = dev_mask_2;
      dev_mask_2 = tmp;
    }

    if(mask_blur)
    {
      const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
      const float mmax[] = { 1.0f };
      const float mmin[] = { 0.0f };

      dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, owidth, oheight, 1, mmax, mmin, sigma, 0);
      if(g)
      {
        dt_gaussian_blur_cl(g, dev_mask_1, dev_mask_2);
        dt_gaussian_free_cl(g);
      }
    }
    else
    {
      cl_mem tmp = dev_mask_1;
      dev_mask_1 = dev_mask_2;
      dev_mask_2 = tmp;
    }

    if(mask_tone_curve && opacity > 1e-4f)
    {
      const float e = expf(3.f * d->contrast);
      const float brightness = d->brightness;
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 0, sizeof(cl_mem), (void *)&dev_mask_2);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 1, sizeof(cl_mem), (void *)&dev_mask_1);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 4, sizeof(float), (void *)&e);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 5, sizeof(float), (void *)&brightness);
      dt_opencl_set_kernel_arg(devid, kernel_mask_tone_curve, 6, sizeof(float), (void *)&opacity);
      err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask_tone_curve, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      cl_mem tmp = dev_mask_1;
      dev_mask_1 = dev_mask_2;
      dev_mask_2 = tmp;
    }

    // get rid of dev_mask_2
    dt_opencl_release_mem_object(dev_mask_2);
    dev_mask_2 = NULL;
  }

  // get temporary buffer for output image to overcome readonly/writeonly limitation
  dev_tmp = dt_opencl_alloc_device(devid, owidth, oheight, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    // let us display a specific channel
    const int use_work_profile = (work_profile == NULL) ? 0 : 1;
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 3, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 6, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 7, sizeof(int), (void *)&request_mask_display);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 8, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 9, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, kernel_display_channel, 10, sizeof(int), (void *)&use_work_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_display_channel, sizes);
    if(err != CL_SUCCESS)
    {
      fprintf(stderr, "[dt_develop_blend_process_cl] error %i enqueue kernel\n", err);
      goto error;
    }
  }
  else
  {
    // apply blending with per-pixel opacity value as defined in dev_mask_1
    const unsigned int blend_mode = d->blend_mode;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_mask_1);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(unsigned), (void *)&blend_mode);
    dt_opencl_set_kernel_arg(devid, kernel, 7, 2 * sizeof(int), (void *)&offs);
    dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), (void *)&mask_display);
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  // register if _this_ module should expose mask or display channel
  if(request_mask_display & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL))
  {
    piece->pipe->mask_display = request_mask_display;
  }


  // check if we should store the mask for export or use in subsequent modules
  // TODO: should we skip raster masks?
  if(piece->pipe->store_all_raster_masks || dt_iop_is_raster_mask_used(self, 0))
  {
    //  get back final mask from the device to store it for later use
    if(!(mask_mode & DEVELOP_MASK_RASTER))
    {
      err = dt_opencl_copy_device_to_host(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
      if(err != CL_SUCCESS) goto error;
    }
    g_hash_table_replace(piece->raster_masks, GINT_TO_POINTER(0), _mask);
    }
  else
  {
    g_hash_table_remove(piece->raster_masks, GINT_TO_POINTER(0));
    dt_free_align(_mask);
  }

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_tmp);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  return TRUE;

error:
  dt_free_align(_mask);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_mask_2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_guide);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/** global init of blendops */
dt_blendop_cl_global_t *dt_develop_blend_init_cl_global(void)
{
#ifdef HAVE_OPENCL
  dt_blendop_cl_global_t *b = (dt_blendop_cl_global_t *)calloc(1, sizeof(dt_blendop_cl_global_t));

  const int program = 3; // blendop.cl, from programs.conf
  b->kernel_blendop_mask_Lab = dt_opencl_create_kernel(program, "blendop_mask_Lab");
  b->kernel_blendop_mask_RAW = dt_opencl_create_kernel(program, "blendop_mask_RAW");
  b->kernel_blendop_mask_rgb = dt_opencl_create_kernel(program, "blendop_mask_rgb");
  b->kernel_blendop_Lab = dt_opencl_create_kernel(program, "blendop_Lab");
  b->kernel_blendop_RAW = dt_opencl_create_kernel(program, "blendop_RAW");
  b->kernel_blendop_rgb = dt_opencl_create_kernel(program, "blendop_rgb");
  b->kernel_blendop_mask_tone_curve = dt_opencl_create_kernel(program, "blendop_mask_tone_curve");
  b->kernel_blendop_set_mask = dt_opencl_create_kernel(program, "blendop_set_mask");
  b->kernel_blendop_display_channel = dt_opencl_create_kernel(program, "blendop_display_channel");
  return b;
#else
  return NULL;
#endif
}

/** global cleanup of blendops */
void dt_develop_blend_free_cl_global(dt_blendop_cl_global_t *b)
{
#ifdef HAVE_OPENCL
  if(!b) return;

  dt_opencl_free_kernel(b->kernel_blendop_mask_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_mask_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb);
  dt_opencl_free_kernel(b->kernel_blendop_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_rgb);
  dt_opencl_free_kernel(b->kernel_blendop_mask_tone_curve);
  dt_opencl_free_kernel(b->kernel_blendop_set_mask);
  dt_opencl_free_kernel(b->kernel_blendop_display_channel);

  free(b);
#endif
}

/** blend version */
int dt_develop_blend_version(void)
{
  return DEVELOP_BLEND_VERSION;
}

/** report back specific memory requirements for blend step (only relevant for OpenCL path) */
void tiling_callback_blendop(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                             struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 3.5f; // in + out + (guide, tmp) + two quarter buffers for the mask
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

/** check if content of params is all zero, indicating a non-initialized set of
   blend parameters
    which needs special care. */
gboolean dt_develop_blend_params_is_all_zero(const void *params, size_t length)
{
  const char *data = (const char *)params;

  for(size_t k = 0; k < length; k++)
    if(data[k]) return FALSE;

  return TRUE;
}

/** update blendop params from older versions */
int dt_develop_blend_legacy_params(dt_iop_module_t *module, const void *const old_params,
                                   const int old_version, void *new_params, const int new_version,
                                   const int length)
{
  // first deal with all-zero parmameter sets, regardless of version number.
  // these occurred in previous
  // darktable versions when modules
  // without blend support stored zero-initialized data in history stack. that's
  // no problem unless the module
  // gets blend
  // support later (e.g. module exposure). remedy: we simply initialize with the
  // current default blend params
  // in this case.
  if(dt_develop_blend_params_is_all_zero(old_params, length))
  {
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d;
    return 0;
  }

  if(old_version == 1 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params1_t)) return 1;

    dt_develop_blend_params1_t *o = (dt_develop_blend_params1_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->blend_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_BLEND_NORMAL2 : o->mode;
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    return 0;
  }

  if(old_version == 2 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params2_t)) return 1;

    dt_develop_blend_params2_t *o = (dt_develop_blend_params2_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_BLEND_NORMAL2 : o->mode;
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & 0xff; // only just in case: knock out all bits
                                    // which were undefined in version
                                    // 2; also switch off old "active" bit
    for(int i = 0; i < (4 * 8); i++) n->blendif_parameters[i] = o->blendif_parameters[i];

    return 0;
  }

  if(old_version == 3 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params3_t)) return 1;

    dt_develop_blend_params3_t *o = (dt_develop_blend_params3_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_BLEND_NORMAL2 : o->mode;
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

    return 0;
  }

  if(old_version == 4 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params4_t)) return 1;

    dt_develop_blend_params4_t *o = (dt_develop_blend_params4_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = (o->mode == DEVELOP_BLEND_DISABLED) ? DEVELOP_BLEND_NORMAL2 : o->mode;
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

    return 0;
  }

  if(old_version == 5 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params5_t)) return 1;

    dt_develop_blend_params5_t *o = (dt_develop_blend_params5_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = o->blend_mode;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    // this is needed as version 5 contained a bug which screwed up history
    // stacks of even older
    // versions. potentially bad history stacks can be identified by an active
    // bit no. 32 in blendif.
    n->blendif = (o->blendif & (1u << DEVELOP_BLENDIF_active) ? o->blendif | 31 : o->blendif)
                 & ~(1u << DEVELOP_BLENDIF_active);
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));

    return 0;
  }

  if(old_version == 6 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params6_t)) return 1;

    dt_develop_blend_params6_t *o = (dt_develop_blend_params6_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = o->blend_mode;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
    return 0;
  }

  if(old_version == 7 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params7_t)) return 1;

    dt_develop_blend_params7_t *o = (dt_develop_blend_params7_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = o->blend_mode;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
    return 0;
  }

  if(old_version == 8 && new_version == 9)
  {
    if(length != sizeof(dt_develop_blend_params8_t)) return 1;

    dt_develop_blend_params8_t *o = (dt_develop_blend_params8_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;
    dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)module->default_blendop_params;

    *n = *d; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = o->blend_mode;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters, 4 * DEVELOP_BLENDIF_SIZE * sizeof(float));
    return 0;
  }

  return 1;
}

int dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so, const void *const old_params,
                                           const int old_version, void *new_params, const int new_version,
                                           const int length)
{
  // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
  dt_iop_module_t *module;
  module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, NULL))
  {
    free(module);
    return 1;
  }

  if(module->params_size == 0)
  {
    dt_iop_cleanup_module(module);
    free(module);
    return 1;
  }

  // convert the old blend params to new
  int res = dt_develop_blend_legacy_params(module, old_params, old_version,
                                           new_params, dt_develop_blend_version(),
                                           length);
  dt_iop_cleanup_module(module);
  free(module);
  return res;
}

// tools/update_modelines.sh
// remove-trailing-space on;
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
