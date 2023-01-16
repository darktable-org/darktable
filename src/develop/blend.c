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
#include "blend.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "develop/imageop_math.h"
#include <math.h>

typedef enum _develop_mask_post_processing
{
  DEVELOP_MASK_POST_NONE = 0,
  DEVELOP_MASK_POST_BLUR = 1,
  DEVELOP_MASK_POST_FEATHER_IN = 2,
  DEVELOP_MASK_POST_FEATHER_OUT = 3,
  DEVELOP_MASK_POST_TONE_CURVE = 4,
} _develop_mask_post_processing;

static dt_develop_blend_params_t _default_blendop_params
    = { DEVELOP_MASK_DISABLED,
        DEVELOP_BLEND_CS_NONE,
        DEVELOP_BLEND_NORMAL2,
        0.0f,
        100.0f,
        DEVELOP_COMBINE_NORM_EXCL,
        0,
        0,
        0.0f,
        DEVELOP_MASK_GUIDE_IN_AFTER_BLUR,
        0.0f,
        0.0f,
        0.0f,
        0.0f, // detail mask threshold
        { 0, 0, 0 },
        { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f,
          0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
        { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { 0 }, 0, 0, FALSE };

static inline dt_develop_blend_colorspace_t _blend_default_module_blend_colorspace(dt_iop_module_t *module,
                                                                                   gboolean is_scene_referred)
{
  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    switch(module->blend_colorspace(module, NULL, NULL))
    {
      case IOP_CS_RAW:
        return DEVELOP_BLEND_CS_RAW;
      case IOP_CS_LAB:
      case IOP_CS_LCH:
        return DEVELOP_BLEND_CS_LAB;
      case IOP_CS_RGB:
        return is_scene_referred ? DEVELOP_BLEND_CS_RGB_SCENE : DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_HSL:
        return DEVELOP_BLEND_CS_RGB_DISPLAY;
      case IOP_CS_JZCZHZ:
        return DEVELOP_BLEND_CS_RGB_SCENE;
      default:
        return DEVELOP_BLEND_CS_NONE;
    }
  }
  else
    return DEVELOP_BLEND_CS_NONE;
}

dt_develop_blend_colorspace_t dt_develop_blend_default_module_blend_colorspace(dt_iop_module_t *module)
{
  const gboolean is_scene_referred = dt_is_scene_referred();
  return _blend_default_module_blend_colorspace(module, is_scene_referred);
}

static void _blend_init_blendif_boost_parameters(dt_develop_blend_params_t *blend_params,
                                                 dt_develop_blend_colorspace_t cst)
{
  if(cst == DEVELOP_BLEND_CS_RGB_SCENE)
  {
    // update the default boost parameters for Jz and Cz so that the sRGB white is represented by a value
    // "close" to 1.0. sRGB white (R=1.0, G=1.0, B=1.0) after conversion becomes Jz=0.01758 and will be shown
    // as 1.8. In order to allow enough sensitivity in the low values, the boost factor should be set to
    // log2(0.001) = -6.64385619. To keep the minimum boost factor at zero an offset of that value is added in
    // the GUI. To display the initial boost factor at zero, the default value will be set to that value also.
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_in] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Jz_out] = -6.64385619f;
    blend_params->blendif_boost_factors[DEVELOP_BLENDIF_Cz_out] = -6.64385619f;
  }
}

void dt_develop_blend_init_blend_parameters(dt_develop_blend_params_t *blend_params,
                                            dt_develop_blend_colorspace_t cst)
{
  memcpy(blend_params, &_default_blendop_params, sizeof(dt_develop_blend_params_t));
  blend_params->blend_cst = cst;
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

void dt_develop_blend_init_blendif_parameters(dt_develop_blend_params_t *blend_params,
                                              dt_develop_blend_colorspace_t cst)
{
  blend_params->blend_cst = cst;
  blend_params->blend_mode = _default_blendop_params.blend_mode;
  blend_params->blend_parameter = _default_blendop_params.blend_parameter;
  blend_params->blendif = _default_blendop_params.blendif;
  memcpy(blend_params->blendif_parameters, _default_blendop_params.blendif_parameters,
         sizeof(_default_blendop_params.blendif_parameters));
  memcpy(blend_params->blendif_boost_factors, _default_blendop_params.blendif_boost_factors,
         sizeof(_default_blendop_params.blendif_boost_factors));
  _blend_init_blendif_boost_parameters(blend_params, cst);
}

dt_iop_colorspace_type_t dt_develop_blend_colorspace(const dt_dev_pixelpipe_iop_t *const piece,
                                                     dt_iop_colorspace_type_t cst)
{
  const dt_develop_blend_params_t *const bp = (const dt_develop_blend_params_t *)piece->blendop_data;
  if(!bp) return cst;
  switch(bp->blend_cst)
  {
    case DEVELOP_BLEND_CS_RAW:
      return IOP_CS_RAW;
    case DEVELOP_BLEND_CS_LAB:
      return IOP_CS_LAB;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
    case DEVELOP_BLEND_CS_RGB_SCENE:
      return IOP_CS_RGB;
    default:
      return cst;
  }
}

void dt_develop_blendif_process_parameters(float *const restrict parameters,
                                           const dt_develop_blend_params_t *const params)
{
  const int32_t blend_csp = params->blend_cst;
  const uint32_t blendif = params->blendif;
  const float *blendif_parameters = params->blendif_parameters;
  const float *boost_factors = params->blendif_boost_factors;
  for(size_t i = 0, j = 0; i < DEVELOP_BLENDIF_SIZE; i++, j += DEVELOP_BLENDIF_PARAMETER_ITEMS)
  {
    if(blendif & (1 << i))
    {
      float offset = 0.0f;
      if(blend_csp == DEVELOP_BLEND_CS_LAB && (i == DEVELOP_BLENDIF_A_in || i == DEVELOP_BLENDIF_A_out
          || i == DEVELOP_BLENDIF_B_in || i == DEVELOP_BLENDIF_B_out))
      {
        offset = 0.5f;
      }
      parameters[j + 0] = (blendif_parameters[i * 4 + 0] - offset) * exp2f(boost_factors[i]);
      parameters[j + 1] = (blendif_parameters[i * 4 + 1] - offset) * exp2f(boost_factors[i]);
      parameters[j + 2] = (blendif_parameters[i * 4 + 2] - offset) * exp2f(boost_factors[i]);
      parameters[j + 3] = (blendif_parameters[i * 4 + 3] - offset) * exp2f(boost_factors[i]);
      // pre-compute increasing slope and decreasing slope
      parameters[j + 4] = 1.0f / fmaxf(0.001f, parameters[j + 1] - parameters[j + 0]);
      parameters[j + 5] = 1.0f / fmaxf(0.001f, parameters[j + 3] - parameters[j + 2]);
      // handle the case when one end is open to avoid clipping input/output values
      if(blendif_parameters[i * 4 + 0] <= 0.0f && blendif_parameters[i * 4 + 1] <= 0.0f)
      {
        parameters[j + 0] = -INFINITY;
        parameters[j + 1] = -INFINITY;
      }
      if(blendif_parameters[i * 4 + 2] >= 1.0f && blendif_parameters[i * 4 + 3] >= 1.0f)
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

// See function definition in blend.h for important information
int dt_develop_blendif_init_masking_profile(struct dt_dev_pixelpipe_iop_t *piece,
                                            dt_iop_order_iccprofile_info_t *blending_profile,
                                            dt_develop_blend_colorspace_t cst)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  const dt_colormatrix_t M = {
      {  0.9555766f, -0.0230393f,  0.0631636f, 0.0f },
      { -0.0282895f,  1.0099416f,  0.0210077f, 0.0f },
      {  0.0122982f, -0.0204830f,  1.3299098f, 0.0f },
  };

  const dt_iop_order_iccprofile_info_t *const profile = (cst == DEVELOP_BLEND_CS_RGB_SCENE)
      ? dt_ioppr_get_pipe_current_profile_info(piece->module, piece->pipe)
      : dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);
  if(!profile) return 0;

  memcpy(blending_profile, profile, sizeof(dt_iop_order_iccprofile_info_t));
  for(size_t y = 0; y < 3; y++)
  {
    for(size_t x = 0; x < 3; x++)
    {
      float sum = 0.0f;
      for(size_t i = 0; i < 3; i++)
        sum += M[y][i] * profile->matrix_in[i][x];
      blending_profile->matrix_out[y][x] = sum;
      blending_profile->matrix_out_transposed[x][y] = sum;
    }
  }

  return 1;
}

static inline float _detail_mask_threshold(const float level, const gboolean detail)
{
  // this does some range calculation for smoother ui experience
  return 0.005f * (detail ? powf(level, 2.0f) : 1.0f - powf(fabs(level), 0.5f ));
}

static void _refine_with_detail_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *mask, const struct dt_iop_roi_t *const roi_in, const struct dt_iop_roi_t *const roi_out, const float level)
{
  if(level == 0.0f) return;
  const gboolean info = ((darktable.unmuted & DT_DEBUG_MASKS) && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL));

  const gboolean detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);

  float *tmp = NULL;
  float *lum = NULL;
  float *warp_mask = NULL;

  dt_dev_pixelpipe_t *p = piece->pipe;
  if(p->rawdetail_mask_data == NULL) return;

  const int iwidth  = p->rawdetail_mask_roi.width;
  const int iheight = p->rawdetail_mask_roi.height;
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  if(info) fprintf(stderr, "[_refine_with_detail_mask] in module %s %ix%i --> %ix%i\n", self->op, iwidth, iheight, owidth, oheight);

  const size_t bufsize = (size_t)MAX(iwidth * iheight, owidth * oheight);

  tmp = dt_alloc_align_float(bufsize);
  lum = dt_alloc_align_float(bufsize);
  if((tmp == NULL) || (lum == NULL)) goto error;

  dt_masks_calc_detail_mask(p->rawdetail_mask_data, lum, tmp, iwidth, iheight, threshold, detail);
  dt_free_align(tmp);
  tmp = NULL;

  // here we have the slightly blurred full detail mask available
  warp_mask = dt_dev_distort_detail_mask(p, lum, self);
  dt_free_align(lum);
  lum = NULL;

  if(warp_mask == NULL) goto error;

  const size_t msize = (size_t)owidth * oheight;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mask, warp_mask, msize) \
  schedule(simd:static) aligned(mask, warp_mask : 64)
 #endif
  for(size_t idx =0; idx < msize; idx++)
    mask[idx] = mask[idx] * warp_mask[idx];
  dt_free_align(warp_mask);

  return;

  error:
  dt_control_log(_("detail mask blending error"));
  dt_free_align(warp_mask);
  dt_free_align(lum);
  dt_free_align(tmp);
}

static size_t _develop_mask_get_post_operations(const dt_develop_blend_params_t *const params,
                                                const dt_dev_pixelpipe_iop_t *const piece,
                                                _develop_mask_post_processing operations[3])
{
  const gboolean mask_feather = params->feathering_radius > 0.1f && piece->colors >= 3;
  const gboolean mask_blur = params->blur_radius > 0.1f;
  const gboolean mask_tone_curve = fabsf(params->contrast) >= 0.01f || fabsf(params->brightness) >= 0.01f;
  const gboolean mask_feather_before = params->feathering_guide == DEVELOP_MASK_GUIDE_IN_BEFORE_BLUR
                                       || params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR;
  const gboolean mask_feather_out = params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_BEFORE_BLUR
                                    || params->feathering_guide == DEVELOP_MASK_GUIDE_OUT_AFTER_BLUR;
  const float opacity = fminf(fmaxf(params->opacity / 100.0f, 0.0f), 1.0f);

  memset(operations, 0, sizeof(_develop_mask_post_processing) * 3);
  size_t index = 0;

  if(mask_feather)
  {
    if(mask_blur && mask_feather_before)
    {
      operations[index++] = mask_feather_out ? DEVELOP_MASK_POST_FEATHER_OUT : DEVELOP_MASK_POST_FEATHER_IN;
      operations[index++] = DEVELOP_MASK_POST_BLUR;
    }
    else
    {
      if(mask_blur)
        operations[index++] = DEVELOP_MASK_POST_BLUR;
      operations[index++] = mask_feather_out ? DEVELOP_MASK_POST_FEATHER_OUT : DEVELOP_MASK_POST_FEATHER_IN;
    }
  }
  else if(mask_blur)
  {
    operations[index++] = DEVELOP_MASK_POST_BLUR;
  }

  if(mask_tone_curve && opacity > 1e-4f)
  {
    operations[index++] = DEVELOP_MASK_POST_TONE_CURVE;
  }

  return index;
}


static inline float *_develop_blend_process_copy_region(const float *const restrict input, const size_t iwidth,
                                                        const size_t xoffs, const size_t yoffs,
                                                        const size_t owidth, const size_t oheight)
{
  const size_t ioffset = yoffs * iwidth + xoffs;
  float *const restrict output = dt_alloc_align_float(owidth * oheight);
  if(output == NULL)
  {
    return NULL;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
        dt_omp_firstprivate(input, output, iwidth, ioffset, owidth, oheight)
#endif
  for(size_t y = 0; y < oheight; y++)
  {
    const size_t iindex = y * iwidth + ioffset;
    const size_t oindex = y * owidth;
    memcpy(output + oindex, input + iindex, sizeof(float) * owidth);
  }

  return output;
}

static inline void _develop_blend_process_free_region(float *const restrict input)
{
  dt_free_align(input);
}


static void _develop_blend_process_feather(const float *const guide, float *const mask, const size_t width,
                                           const size_t height, const int ch, const float guide_weight,
                                           const float feathering_radius, const float scale)
{
  const float sqrt_eps = 1.f;
  int w = (int)(2 * feathering_radius * scale + 0.5f);
  if(w < 1) w = 1;

  float *const restrict mask_bak = dt_alloc_align_float(width * height);
  if(mask_bak)
  {
    memcpy(mask_bak, mask, sizeof(float) * width * height);
    guided_filter(guide, mask_bak, mask, width, height, ch, w, sqrt_eps, guide_weight, 0.f, 1.f);
    dt_free_align(mask_bak);
  }
}


static void _develop_blend_process_mask_tone_curve(float *const restrict mask, const size_t buffsize,
                                                   const float contrast, const float brightness,
                                                   const float opacity)
{
  const float mask_epsilon = 16 * FLT_EPSILON;  // empirical mask threshold for fully transparent masks
  const float e = expf(3.f * contrast);
#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) aligned(mask:64) \
    dt_omp_firstprivate(brightness, buffsize, e, mask, mask_epsilon, opacity)
#endif
  for(size_t k = 0; k < buffsize; k++)
  {
    float x = mask[k] / opacity;
    x = 2.f * x - 1.f;
    if(1.f - brightness <= 0.f)
      x = mask[k] <= mask_epsilon ? -1.f : 1.f;
    else if(1.f + brightness <= 0.f)
      x = mask[k] >= 1.f - mask_epsilon ? 1.f : -1.f;
    else if(brightness > 0.f)
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

  const size_t ch = piece->colors;           // the number of channels in the buffer
  const int xoffs = roi_out->x - roi_in->x;
  const int yoffs = roi_out->y - roi_in->y;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const size_t buffsize = (size_t)owidth * oheight;
  const float iscale = roi_in->scale;
  const float oscale = roi_out->scale;
  const gboolean rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

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
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // check if mask should be suppressed temporarily (i.e. just set to global opacity value)
  const gboolean suppress_mask = self->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                                 && (piece->pipe == self->dev->pipe)
                                 && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL);

  // obtaining the list of mask operations to perform
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size = _develop_mask_get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(d->opacity / 100.0f, 0.0f), 1.0f);

  // allocate space for blend mask
  float *const restrict _mask = dt_alloc_align_float(buffsize);
  if(!_mask)
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return;
  }
  float *const restrict mask = _mask;

  if(mask_mode == DEVELOP_MASK_ENABLED || suppress_mask)
  {
    // blend uniformly (no drawn or parametric mask)
    dt_iop_image_fill(mask,opacity,owidth,oheight,1);  //mask[k] = opacity;
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
      {
        dt_iop_image_scaled_copy(mask, raster_mask, opacity, owidth, oheight, 1); //mask[k] = opacity * raster_mask[k];
      }
      if(free_mask) dt_free_align(raster_mask);
    }
    else
    {
      // fallback for when the raster mask couldn't be applied
      const float value = d->raster_mask_invert ? 0.0 : 1.0;
      dt_iop_image_fill(mask, value, owidth, oheight, 1);  //mask[k] = value;
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
        dt_iop_image_invert(mask, 1.0f, owidth, oheight, 1); //mask[k] = 1.0f - mask[k];
      }
    }
    else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    _refine_with_detail_mask(self, piece, mask, roi_in, roi_out, d->details);

    // get parametric mask (if any) and apply global opacity
    switch(blend_csp)
    {
      case DEVELOP_BLEND_CS_LAB:
        dt_develop_blendif_lab_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_DISPLAY:
        dt_develop_blendif_rgb_hsl_make_mask(piece, (const float *const restrict)ivoid,
                                             (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RGB_SCENE:
        dt_develop_blendif_rgb_jzczhz_make_mask(piece, (const float *const restrict)ivoid,
                                                (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      case DEVELOP_BLEND_CS_RAW:
        dt_develop_blendif_raw_make_mask(piece, (const float *const restrict)ivoid,
                                         (const float *const restrict)ovoid, roi_in, roi_out, mask);
        break;
      default:
        break;
    }

    // post processing the mask
    for(size_t index = 0; index < post_operations_size; ++index)
    {
      _develop_mask_post_processing operation = post_operations[index];
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        const float guide_weight = cst == IOP_CS_RGB ? 100.0f : 1.0f;
        float *restrict guide = (float *restrict)ivoid;
        if(!rois_equal)
          guide = _develop_blend_process_copy_region(guide, ch * iwidth, ch * xoffs, ch * yoffs,
                                                     ch * owidth, ch * oheight);
        if(guide)
          _develop_blend_process_feather(guide, mask, owidth, oheight, ch, guide_weight,
                                         d->feathering_radius, roi_out->scale / piece->iscale);
        if(!rois_equal)
          _develop_blend_process_free_region(guide);
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        const float guide_weight = cst == IOP_CS_RGB ? 100.0f : 1.0f;
        _develop_blend_process_feather((const float *const restrict)ovoid, mask, owidth, oheight, ch,
                                       guide_weight, d->feathering_radius, roi_out->scale / piece->iscale);
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
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
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        _develop_blend_process_mask_tone_curve(mask, buffsize, d->contrast, d->brightness, opacity);
      }
    }
  }

  // now apply blending with per-pixel opacity value as defined in mask
  // select the blend operator
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_LAB:
      dt_develop_blendif_lab_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                   roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      dt_develop_blendif_rgb_hsl_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                       roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RGB_SCENE:
      dt_develop_blendif_rgb_jzczhz_blend(piece, (const float *const restrict)ivoid, (float *const restrict)ovoid,
                                          roi_in, roi_out, mask, request_mask_display);
      break;
    case DEVELOP_BLEND_CS_RAW:
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
static void _refine_with_detail_mask_cl(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, float *mask, const struct dt_iop_roi_t *roi_in,
                                const struct dt_iop_roi_t *roi_out, const float level, const int devid)
{
  if(level == 0.0f) return;
  const gboolean info = ((darktable.unmuted & DT_DEBUG_MASKS) && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL));

  const int detail = (level > 0.0f);
  const float threshold = _detail_mask_threshold(level, detail);
  float *lum = NULL;
  cl_mem tmp = NULL;
  cl_mem blur = NULL;
  cl_mem out = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  dt_dev_pixelpipe_t *p = piece->pipe;
  if(p->rawdetail_mask_data == NULL) return;

  const int iwidth  = p->rawdetail_mask_roi.width;
  const int iheight = p->rawdetail_mask_roi.height;
  const int owidth  = roi_out->width;
  const int oheight = roi_out->height;
  if(info) fprintf(stderr, "[_refine_with_detail_mask_cl] in module %s %ix%i --> %ix%i\n", self->op, iwidth, iheight, owidth, oheight);

  lum = dt_alloc_align_float((size_t)iwidth * iheight);
  if(lum == NULL) goto error;
  tmp = dt_opencl_alloc_device(devid, iwidth, iheight, sizeof(float));
  if(tmp == NULL) goto error;
  out = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  if(out == NULL) goto error;
  blur = dt_opencl_alloc_device_buffer(devid, sizeof(float) * iwidth * iheight);
  if(blur == NULL) goto error;

  err = dt_opencl_write_host_to_device(devid, p->rawdetail_mask_data, tmp, iwidth, iheight, sizeof(float));
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[refine_with_detail_mask_cl] write rawdetail_mask_data: %s\n", cl_errstr(err));
    goto error;
  }

  {
    const int kernel = darktable.opencl->blendop->kernel_read_mask;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, iwidth, iheight,
      CLARG(out), CLARG(tmp), CLARG(iwidth), CLARG(iheight));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[refine_with_detail_mask_cl] kernel_read_mask: %s\n", cl_errstr(err));
      goto error;
    }
  }

  {
    const int kernel = darktable.opencl->blendop->kernel_calc_blend;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, iwidth, iheight,
      CLARG(out), CLARG(blur), CLARG(iwidth), CLARG(iheight), CLARG(threshold), CLARG(detail));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[refine_with_detail_mask_cl] kernel_calc_blend: %s\n", cl_errstr(err));
      goto error;
    }
  }

  {
    float blurmat[13];
    dt_masks_blur_9x9_coeff(blurmat, 2.0f);
    cl_mem dev_blurmat = NULL;
    dev_blurmat = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 13, blurmat);
    if(dev_blurmat != NULL)
    {
      const int clkernel = darktable.opencl->blendop->kernel_mask_blur;
      err = dt_opencl_enqueue_kernel_2d_args(devid, clkernel, iwidth, iheight,
        CLARG(blur), CLARG(out), CLARG(iwidth), CLARG(iheight), CLARG(dev_blurmat));
      dt_opencl_release_mem_object(dev_blurmat);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[refine_with_detail_mask_cl] kernel_mask_blur: %s\n", cl_errstr(err));
        goto error;
      }
    }
    else
    {
      dt_opencl_release_mem_object(dev_blurmat);
      goto error;
    }
  }

  {
    const int kernel = darktable.opencl->blendop->kernel_write_mask;
    err = dt_opencl_enqueue_kernel_2d_args(devid, kernel, iwidth, iheight,
      CLARG(out), CLARG(tmp), CLARG(iwidth), CLARG(iheight));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[refine_with_detail_mask_cl] kernel_write_mask: %s\n", cl_errstr(err));
      goto error;
    }
  }

  err = dt_opencl_read_host_from_device(devid, lum, tmp, iwidth, iheight, sizeof(float));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
  tmp = blur = out = NULL;

  // here we have the slightly blurred full detail available
  float *warp_mask = dt_dev_distort_detail_mask(p, lum, self);
  if(warp_mask == NULL) goto error;
  dt_free_align(lum);
  lum = NULL;

  const int msize = owidth * oheight;
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(mask, warp_mask, msize) \
  schedule(simd:static) aligned(mask, warp_mask : 64)
 #endif
  for(int idx = 0; idx < msize; idx++)
  {
    mask[idx] = mask[idx] * warp_mask[idx];
  }
  dt_free_align(warp_mask);
  return;

  error:
  dt_control_log(_("detail mask CL blending problem"));
  dt_free_align(lum);
  dt_opencl_release_mem_object(tmp);
  dt_opencl_release_mem_object(blur);
  dt_opencl_release_mem_object(out);
}

static inline void _blend_process_cl_exchange(cl_mem *a, cl_mem *b)
{
  cl_mem tmp = *a;
  *a = *b;
  *b = tmp;
}

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
  const gboolean rois_equal = iwidth == owidth || iheight == oheight || xoffs == 0 || yoffs == 0;

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
  const dt_develop_blend_colorspace_t blend_csp = d->blend_cst;
  const dt_iop_colorspace_type_t cst = dt_develop_blend_colorspace(piece, IOP_CS_NONE);

  // check if mask should be suppressed temporarily (i.e. just set to global
  // opacity value)
  const gboolean suppress_mask = self->suppress_mask && self->dev->gui_attached && (self == self->dev->gui_module)
                                 && (piece->pipe == self->dev->pipe)
                                 && (mask_mode & DEVELOP_MASK_MASK_CONDITIONAL);

  // obtaining the list of mask operations to perform
  _develop_mask_post_processing post_operations[3];
  const size_t post_operations_size = _develop_mask_get_post_operations(d, piece, post_operations);

  // get the clipped opacity value  0 - 1
  const float opacity = fminf(fmaxf(d->opacity / 100.0f, 0.0f), 1.0f);

  // allocate space for blend mask
  float *_mask = dt_alloc_align_float(buffsize);
  if(!_mask)
  {
    dt_control_log(_("could not allocate buffer for blending"));
    return FALSE;
  }
  float *const mask = _mask;

  // setup some kernels
  int kernel_mask;
  int kernel;
  switch(blend_csp)
  {
    case DEVELOP_BLEND_CS_RAW:
      kernel = darktable.opencl->blendop->kernel_blendop_RAW;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_RAW;
      break;

    case DEVELOP_BLEND_CS_RGB_DISPLAY:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_hsl;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_hsl;
      break;

    case DEVELOP_BLEND_CS_RGB_SCENE:
      kernel = darktable.opencl->blendop->kernel_blendop_rgb_jzczhz;
      kernel_mask = darktable.opencl->blendop->kernel_blendop_mask_rgb_jzczhz;
      break;

    case DEVELOP_BLEND_CS_LAB:
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
  const size_t sizes[] = { ROUNDUPDWD(owidth, devid), ROUNDUPDHT(oheight, devid), 1 };

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_blendif_params = NULL;
  cl_mem dev_boost_factors = NULL;
  cl_mem dev_mask_1 = NULL;
  cl_mem dev_mask_2 = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_guide = NULL;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;

  cl_mem dev_work_profile_info = NULL;
  cl_mem dev_work_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *work_profile_info_cl = NULL;
  cl_float *work_profile_lut_cl = NULL;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { owidth, oheight, 1 };

  // parameters, for every channel the 4 limits + pre-computed increasing slope and decreasing slope
  float parameters[DEVELOP_BLENDIF_PARAMETER_ITEMS * DEVELOP_BLENDIF_SIZE] DT_ALIGNED_ARRAY;
  dt_develop_blendif_process_parameters(parameters, d);

  // copy blend parameters to constant device memory
  dev_blendif_params = dt_opencl_copy_host_to_device_constant(devid, sizeof(parameters), parameters);
  if(dev_blendif_params == NULL) goto error;

  dev_mask_1 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
  if(dev_mask_1 == NULL) goto error;

  dt_iop_order_iccprofile_info_t profile;
  const int use_profile = dt_develop_blendif_init_masking_profile(piece, &profile, blend_csp);

  err = dt_ioppr_build_iccprofile_params_cl(use_profile ? &profile : NULL, devid, &profile_info_cl,
                                            &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] profile_info_cl: %s\n", cl_errstr(err));
    goto error;
  }
  if(mask_mode == DEVELOP_MASK_ENABLED || suppress_mask)
  {
    // blend uniformly (no drawn or parametric mask)

    // set dev_mask with global opacity value
    dt_opencl_set_kernel_args(devid, kernel_set_mask, 0, CLARG(dev_mask_1), CLARG(owidth), CLARG(oheight),
      CLARG(opacity));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_set_mask, sizes);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] kernel_set_mask: %s\n", cl_errstr(err));
      goto error;
    }
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
      {
        dt_iop_image_scaled_copy(mask, raster_mask,opacity, owidth, oheight, 1); //mask[k] = opacity * raster_mask[k];
      }
      if(free_mask) dt_free_align(raster_mask);
    }
    else
    {
      // fallback for when the raster mask couldn't be applied
      const float value = d->raster_mask_invert ? 0.0 : 1.0;
      dt_iop_image_fill(mask, value, owidth, oheight, 1); //mask[k] = value;
    }

    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] write raster mask dev_mask_1: %s\n", cl_errstr(err));
      goto error;
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
        dt_iop_image_invert(mask, 1.0f, owidth, oheight, 1); //mask[k] = 1.0f - mask[k]
      }
    }
    else if((!(self->flags() & IOP_FLAGS_NO_MASKS)) && (d->mask_mode & DEVELOP_MASK_MASK))
    {
      // no form defined but drawn mask active
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_MASKS_POS) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    else
    {
      // we fill the buffer with 1.0f or 0.0f depending on mask_combine
      const float fill = (d->mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;
      dt_iop_image_fill(mask, fill, owidth, oheight, 1); //mask[k] = fill;
    }
    _refine_with_detail_mask_cl(self, piece, mask, roi_in, roi_out, d->details, devid);

    // write mask from host to device
    dev_mask_2 = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float));
    if(dev_mask_2 == NULL) goto error;
    err = dt_opencl_write_host_to_device(devid, mask, dev_mask_1, owidth, oheight, sizeof(float));
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] write drawn mask dev_mask_1: %s\n", cl_errstr(err));
      goto error;
    }
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
    dt_opencl_set_kernel_args(devid, kernel_mask, 0, CLARG(dev_in), CLARG(dev_out), CLARG(dev_mask_1),
      CLARG(dev_mask_2), CLARG(owidth), CLARG(oheight), CLARG(opacity), CLARG(blendif), CLARG(dev_blendif_params),
      CLARG(mask_mode), CLARG(mask_combine), CLARRAY(2, offs), CLARG(dev_profile_info), CLARG(dev_profile_lut),
      CLARG(use_profile));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask, sizes);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] apply global opacity: %s\n", cl_errstr(err));
      goto error;
    }

    // the mask is now located in dev_mask_2, put it in dev_mask_1
    _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);

    // post processing the mask (it will always be stored in dev_mask_1)
    for(size_t index = 0; index < post_operations_size; ++index)
    {
      _develop_mask_post_processing operation = post_operations[index];
      if(operation == DEVELOP_MASK_POST_FEATHER_IN)
      {
        int w = (int)(2 * d->feathering_radius * roi_out->scale / piece->iscale + 0.5f);
        if(w < 1) w = 1;
        const float sqrt_eps = 1.0f;
        const float guide_weight = cst == IOP_CS_RGB ? 100.0f : 1.0f;

        cl_mem guide = dev_in;
        if(!rois_equal)
        {
          dev_guide = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * 4);
          if(dev_guide == NULL) goto error;
          guide = dev_guide;
          size_t origin_1[] = { xoffs, yoffs, 0 };
          size_t origin_2[] = { 0, 0, 0 };
          err = dt_opencl_enqueue_copy_image(devid, dev_in, guide, origin_2, origin_1, region);
          if(err != CL_SUCCESS) goto error;
        }
        guided_filter_cl(devid, guide, dev_mask_1, dev_mask_2, owidth, oheight, ch, w, sqrt_eps, guide_weight,
                         0.0f, 1.0f);
        if(!rois_equal)
        {
          dt_opencl_release_mem_object(dev_guide);
          dev_guide = NULL;
        }
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_FEATHER_OUT)
      {
        int w = (int)(2 * d->feathering_radius * roi_out->scale / piece->iscale + 0.5f);
        if(w < 1) w = 1;
        const float sqrt_eps = 1.0f;
        const float guide_weight = cst == IOP_CS_RGB ? 100.0f : 1.0f;

        guided_filter_cl(devid, dev_out, dev_mask_1, dev_mask_2, owidth, oheight, ch, w, sqrt_eps, guide_weight,
                         0.0f, 1.0f);
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_BLUR)
      {
        const float sigma = d->blur_radius * roi_out->scale / piece->iscale;
        const float mmax[] = { 1.0f };
        const float mmin[] = { 0.0f };

        dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, owidth, oheight, 1, mmax, mmin, sigma, 0);
        if(!g) goto error;
        err = dt_gaussian_blur_cl(g, dev_mask_1, dev_mask_2);
        dt_gaussian_free_cl(g);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] DEVELOP_MASK_POST_BLUR: %s\n", cl_errstr(err));
          goto error;
        }
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
      else if(operation == DEVELOP_MASK_POST_TONE_CURVE)
      {
        const float e = expf(3.f * d->contrast);
        const float brightness = d->brightness;
        dt_opencl_set_kernel_args(devid, kernel_mask_tone_curve, 0, CLARG(dev_mask_1), CLARG(dev_mask_2),
          CLARG(owidth), CLARG(oheight), CLARG(e), CLARG(brightness), CLARG(opacity));
        err = dt_opencl_enqueue_kernel_2d(devid, kernel_mask_tone_curve, sizes);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] DEVELOP_MASK_POST_TONE_CURVE: %s\n", cl_errstr(err));
          goto error;
        }
        _blend_process_cl_exchange(&dev_mask_1, &dev_mask_2);
      }
    }

    // get rid of dev_mask_2
    dt_opencl_release_mem_object(dev_mask_2);
    dev_mask_2 = NULL;
  }

  // get temporary buffer for output image to overcome readonly/writeonly limitation
  dev_tmp = dt_opencl_alloc_device(devid, owidth, oheight, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  err = dt_opencl_enqueue_copy_image(devid, dev_out, dev_tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  if(request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    // load the boost factors in the device memory
    dev_boost_factors = dt_opencl_copy_host_to_device_constant(devid, sizeof(d->blendif_boost_factors),
                                                               d->blendif_boost_factors);
    if(dev_blendif_params == NULL) goto error;

    // the display channel of Lab blending is generated in RGB and should be transformed to Lab
    // the transformation in the pipeline is currently always using the work profile
    dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
    const int use_work_profile = work_profile != NULL;

    err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &work_profile_info_cl, &work_profile_lut_cl,
                                              &dev_work_profile_info, &dev_work_profile_lut);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] work_profile_info_cl: %s\n", cl_errstr(err));
      goto error;
    }
    // let us display a specific channel
    dt_opencl_set_kernel_args(devid, kernel_display_channel, 0, CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_mask_1),
      CLARG(dev_out), CLARG(owidth), CLARG(oheight), CLARRAY(2, offs), CLARG(request_mask_display), CLARG(dev_boost_factors),
      CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(use_profile), CLARG(dev_work_profile_info),
      CLARG(dev_work_profile_lut), CLARG(use_work_profile));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel_display_channel, sizes);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] kernel_display_channel: %s\n", cl_errstr(err));
      goto error;
    }
  }
  else
  {
    // apply blending with per-pixel opacity value as defined in dev_mask_1
    const unsigned int blend_mode = d->blend_mode;
    const float blend_parameter = exp2f(d->blend_parameter);
    dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in), CLARG(dev_tmp), CLARG(dev_mask_1), CLARG(dev_out),
      CLARG(owidth), CLARG(oheight), CLARG(blend_mode), CLARG(blend_parameter), CLARRAY(2, offs), CLARG(mask_display));
    err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] blend_parameter: %s\n", cl_errstr(err));
      goto error;
    }
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

  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_tmp);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl, &dev_work_profile_info,
                                     &dev_work_profile_lut);
  return TRUE;

error:
  dt_free_align(_mask);
  dt_opencl_release_mem_object(dev_blendif_params);
  dt_opencl_release_mem_object(dev_boost_factors);
  dt_opencl_release_mem_object(dev_mask_1);
  dt_opencl_release_mem_object(dev_mask_2);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_guide);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_ioppr_free_iccprofile_params_cl(&work_profile_info_cl, &work_profile_lut_cl, &dev_work_profile_info,
                                     &dev_work_profile_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] error: %s\n", cl_errstr(err));
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
  b->kernel_blendop_mask_rgb_hsl = dt_opencl_create_kernel(program, "blendop_mask_rgb_hsl");
  b->kernel_blendop_mask_rgb_jzczhz = dt_opencl_create_kernel(program, "blendop_mask_rgb_jzczhz");
  b->kernel_blendop_Lab = dt_opencl_create_kernel(program, "blendop_Lab");
  b->kernel_blendop_RAW = dt_opencl_create_kernel(program, "blendop_RAW");
  b->kernel_blendop_rgb_hsl = dt_opencl_create_kernel(program, "blendop_rgb_hsl");
  b->kernel_blendop_rgb_jzczhz = dt_opencl_create_kernel(program, "blendop_rgb_jzczhz");
  b->kernel_blendop_mask_tone_curve = dt_opencl_create_kernel(program, "blendop_mask_tone_curve");
  b->kernel_blendop_set_mask = dt_opencl_create_kernel(program, "blendop_set_mask");
  b->kernel_blendop_display_channel = dt_opencl_create_kernel(program, "blendop_display_channel");

  const int program_rcd = 31;
  b->kernel_calc_Y0_mask = dt_opencl_create_kernel(program_rcd, "calc_Y0_mask");
  b->kernel_calc_scharr_mask = dt_opencl_create_kernel(program_rcd, "calc_scharr_mask");
  b->kernel_write_scharr_mask = dt_opencl_create_kernel(program_rcd, "write_scharr_mask");
  b->kernel_write_mask = dt_opencl_create_kernel(program_rcd, "writeout_mask");
  b->kernel_read_mask  = dt_opencl_create_kernel(program_rcd, "readin_mask");
  b->kernel_calc_blend = dt_opencl_create_kernel(program_rcd, "calc_detail_blend");
  b->kernel_mask_blur  = dt_opencl_create_kernel(program_rcd, "fastblur_mask_9x9");

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
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_mask_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_Lab);
  dt_opencl_free_kernel(b->kernel_blendop_RAW);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_hsl);
  dt_opencl_free_kernel(b->kernel_blendop_rgb_jzczhz);
  dt_opencl_free_kernel(b->kernel_blendop_mask_tone_curve);
  dt_opencl_free_kernel(b->kernel_blendop_set_mask);
  dt_opencl_free_kernel(b->kernel_blendop_display_channel);
  dt_opencl_free_kernel(b->kernel_calc_Y0_mask);
  dt_opencl_free_kernel(b->kernel_calc_scharr_mask);
  dt_opencl_free_kernel(b->kernel_write_scharr_mask);
  dt_opencl_free_kernel(b->kernel_write_mask);
  dt_opencl_free_kernel(b->kernel_read_mask);
  dt_opencl_free_kernel(b->kernel_calc_blend);
  dt_opencl_free_kernel(b->kernel_mask_blur);
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

  dt_develop_blend_params_t *const bldata = (dt_develop_blend_params_t *const)piece->blendop_data;
  if(bldata)
  {
    if(bldata->details != 0.0f)
      tiling->factor += 0.75f; // details mask requires 3 additional quarter buffers
  }
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

static uint32_t _blend_legacy_blend_mode(uint32_t legacy_blend_mode)
{
  uint32_t blend_mode = legacy_blend_mode & DEVELOP_BLEND_MODE_MASK;
  gboolean blend_reverse = FALSE;
  switch(blend_mode) {
    case DEVELOP_BLEND_NORMAL_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      break;
    case DEVELOP_BLEND_INVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_BOUNDED;
      blend_reverse = TRUE;
      break;
    case DEVELOP_BLEND_DISABLED_OBSOLETE:
    case DEVELOP_BLEND_UNBOUNDED_OBSOLETE:
      blend_mode = DEVELOP_BLEND_NORMAL2;
      break;
    case DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE:
      blend_mode = DEVELOP_BLEND_MULTIPLY;
      blend_reverse = TRUE;
      break;
    default:
      break;
  }
  return (blend_reverse ? DEVELOP_BLEND_REVERSE : 0) | blend_mode;
}

/** update blendop params from older versions */
int dt_develop_blend_legacy_params(dt_iop_module_t *module, const void *const old_params,
                                   const int old_version, void *new_params, const int new_version,
                                   const int length)
{
  // edits before version 10 default to a display referred workflow
  dt_develop_blend_colorspace_t cst = _blend_default_module_blend_colorspace(module, 0);

  dt_develop_blend_params_t default_display_blend_params;
  dt_develop_blend_init_blend_parameters(&default_display_blend_params, cst);

  // first deal with all-zero parameter sets, regardless of version number.
  // these occurred in previous darktable versions when modules without blend support stored zero-initialized data
  // in history stack. that's no problem unless the module gets blend support later (e.g. module exposure).
  // remedy: we simply initialize with the current default blend params in this case.
  if(dt_develop_blend_params_is_all_zero(old_params, length))
  {
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params;
    return 0;
  }

  if(old_version == 1 && new_version == 11)
  {
    /** blend legacy parameters version 1 */
    typedef struct dt_develop_blend_params1_t
    {
      uint32_t mode;
      float opacity;
      uint32_t mask_id;
    } dt_develop_blend_params1_t;

    if(length != sizeof(dt_develop_blend_params1_t)) return 1;

    dt_develop_blend_params1_t *o = (dt_develop_blend_params1_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    return 0;
  }

  if(old_version == 2 && new_version == 11)
  {
    /** blend legacy parameters version 2 */
    typedef struct dt_develop_blend_params2_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * 8];
    } dt_develop_blend_params2_t;

    if(length != sizeof(dt_develop_blend_params2_t)) return 1;

    dt_develop_blend_params2_t *o = (dt_develop_blend_params2_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & 0xff; // only just in case: knock out all bits
                                    // which were undefined in version
                                    // 2; also switch off old "active" bit
    for(int i = 0; i < (4 * 8); i++) n->blendif_parameters[i] = o->blendif_parameters[i];

    return 0;
  }

  if(old_version == 3 && new_version == 11)
  {
    /** blend legacy parameters version 3 */
    typedef struct dt_develop_blend_params3_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params3_t;

    if(length != sizeof(dt_develop_blend_params3_t)) return 1;

    dt_develop_blend_params3_t *o = (dt_develop_blend_params3_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 4 && new_version == 11)
  {
    /** blend legacy parameters version 4 */
    typedef struct dt_develop_blend_params4_t
    {
      /** blending mode */
      uint32_t mode;
      /** mixing opacity */
      float opacity;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params4_t;

    if(length != sizeof(dt_develop_blend_params4_t)) return 1;

    dt_develop_blend_params4_t *o = (dt_develop_blend_params4_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = (o->mode == DEVELOP_BLEND_DISABLED_OBSOLETE) ? DEVELOP_MASK_DISABLED : DEVELOP_MASK_ENABLED;
    n->mask_mode |= ((o->blendif & (1u << DEVELOP_BLENDIF_active)) && (n->mask_mode == DEVELOP_MASK_ENABLED))
                        ? DEVELOP_MASK_CONDITIONAL
                        : 0;
    n->blend_mode = _blend_legacy_blend_mode(o->mode);
    n->opacity = o->opacity;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif & ~(1u << DEVELOP_BLENDIF_active); // knock out old unused "active" flag
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 5 && new_version == 11)
  {
    /** blend legacy parameters version 5 (identical to version 6)*/
    typedef struct dt_develop_blend_params5_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params5_t;

    if(length != sizeof(dt_develop_blend_params5_t)) return 1;

    dt_develop_blend_params5_t *o = (dt_develop_blend_params5_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
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
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);

    return 0;
  }

  if(old_version == 6 && new_version == 11)
  {
    /** blend legacy parameters version 6 (identical to version 7) */
    typedef struct dt_develop_blend_params6_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params6_t;

    if(length != sizeof(dt_develop_blend_params6_t)) return 1;

    dt_develop_blend_params6_t *o = (dt_develop_blend_params6_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 7 && new_version == 11)
  {
    /** blend legacy parameters version 7 */
    typedef struct dt_develop_blend_params7_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** blur radius */
      float radius;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params7_t;

    if(length != sizeof(dt_develop_blend_params7_t)) return 1;

    dt_develop_blend_params7_t *o = (dt_develop_blend_params7_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blur_radius = o->radius;
    n->blendif = o->blendif;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 8 && new_version == 11)
  {
    /** blend legacy parameters version 8 */
    typedef struct dt_develop_blend_params8_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
    } dt_develop_blend_params8_t;

    if(length != sizeof(dt_develop_blend_params8_t)) return 1;

    dt_develop_blend_params8_t *o = (dt_develop_blend_params8_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    return 0;
  }

  if(old_version == 9 && new_version == 11)
  {
    /** blend legacy parameters version 9 */
    typedef struct dt_develop_blend_params9_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask
       *  or raster mask */
      uint32_t mask_mode;
      /** blending mode */
      uint32_t blend_mode;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      int raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params9_t;

    if(length != sizeof(dt_develop_blend_params9_t)) return 1;

    dt_develop_blend_params9_t *o = (dt_develop_blend_params9_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source, sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_id;
    n->raster_mask_invert = o->raster_mask_invert;
    return 0;
  }

  if(old_version == 10 && new_version == 11)
  {
    /** blend legacy parameters version 10 */
    typedef struct dt_develop_blend_params10_t
    {
      /** what kind of masking to use: off, non-mask (uniformly), hand-drawn mask and/or conditional mask
       *  or raster mask */
      uint32_t mask_mode;
      /** blending color space type */
      int32_t blend_cst;
      /** blending mode */
      uint32_t blend_mode;
      /** parameter for the blending */
      float blend_parameter;
      /** mixing opacity */
      float opacity;
      /** how masks are combined */
      uint32_t mask_combine;
      /** id of mask in current pipeline */
      uint32_t mask_id;
      /** blendif mask */
      uint32_t blendif;
      /** feathering radius */
      float feathering_radius;
      /** feathering guide */
      uint32_t feathering_guide;
      /** blur radius */
      float blur_radius;
      /** mask contrast enhancement */
      float contrast;
      /** mask brightness adjustment */
      float brightness;
      /** some reserved fields for future use */
      uint32_t reserved[4];
      /** blendif parameters */
      float blendif_parameters[4 * DEVELOP_BLENDIF_SIZE];
      float blendif_boost_factors[DEVELOP_BLENDIF_SIZE];
      dt_dev_operation_t raster_mask_source;
      int raster_mask_instance;
      int raster_mask_id;
      gboolean raster_mask_invert;
    } dt_develop_blend_params10_t;

    if(length != sizeof(dt_develop_blend_params10_t)) return 1;

    dt_develop_blend_params10_t *o = (dt_develop_blend_params10_t *)old_params;
    dt_develop_blend_params_t *n = (dt_develop_blend_params_t *)new_params;

    *n = default_display_blend_params; // start with a fresh copy of default parameters
    n->mask_mode = o->mask_mode;
    n->blend_cst = o->blend_cst;
    n->blend_mode = _blend_legacy_blend_mode(o->blend_mode);
    n->blend_parameter = o->blend_parameter;
    n->opacity = o->opacity;
    n->mask_combine = o->mask_combine;
    n->mask_id = o->mask_id;
    n->blendif = o->blendif;
    n->feathering_radius = o->feathering_radius;
    n->feathering_guide = o->feathering_guide;
    n->blur_radius = o->blur_radius;
    n->contrast = o->contrast;
    n->brightness = o->brightness;
    // fix intermediate devel versions for details mask and initialize n->details to proper values if something was wrong
    memcpy(&n->details, &o->reserved, sizeof(float));
    if(isnan(n->details)) n->details = 0.0f;
    n->details = fminf(1.0f, fmaxf(-1.0f, n->details));

    memcpy(n->blendif_parameters, o->blendif_parameters, sizeof(float) * 4 * DEVELOP_BLENDIF_SIZE);
    memcpy(n->blendif_boost_factors, o->blendif_boost_factors, sizeof(float) * DEVELOP_BLENDIF_SIZE);
    memcpy(n->raster_mask_source, o->raster_mask_source, sizeof(n->raster_mask_source));
    n->raster_mask_instance = o->raster_mask_instance;
    n->raster_mask_id = o->raster_mask_id;
    n->raster_mask_invert = o->raster_mask_invert;
    return 0;
  }

  return 1;
}

int dt_develop_blend_legacy_params_from_so(dt_iop_module_so_t *module_so, const void *const old_params,
                                           const int old_version, void *new_params, const int new_version,
                                           const int length)
{
  // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
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
  const int res = dt_develop_blend_legacy_params(module, old_params, old_version,
                                                 new_params, dt_develop_blend_version(),
                                                 length);
  dt_iop_cleanup_module(module);
  free(module);
  return res;
}

// tools/update_modelines.sh
// remove-trailing-space on;
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
