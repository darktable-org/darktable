/*
    This file is part of darktable,
    Copyright (C) 2016-2024 darktable developers.

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

#include "common/color_picker.h"
#include "common/bspline.h"
#include "common/darktable.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "develop/format.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

typedef DT_ALIGNED_PIXEL uint32_t dt_aligned_weights_t[4];

static inline size_t _box_size(const int *const box)
{
  return (size_t)((box[3] - box[1]) * (box[2] - box[0]));
}

static inline void _update_stats_by_ch(dt_aligned_pixel_t acc,
                                       dt_aligned_pixel_t low,
                                       dt_aligned_pixel_t high,
                                       const int ch,
                                       const float pick)
{
  acc[ch] += pick;
  low[ch] = MIN(low[ch], pick);
  high[ch] = MAX(high[ch], pick);
}

static inline void _update_stats_4ch(dt_aligned_pixel_t acc,
                                     dt_aligned_pixel_t low,
                                     dt_aligned_pixel_t high,
                                     const dt_aligned_pixel_t pick)
{
  // need all channels as blend pickers may use 4th to reverse hues
  for_four_channels(k,aligned(acc,low,high,pick:16))
    _update_stats_by_ch(acc, low, high, k, pick[k]);
}

DT_OMP_DECLARE_SIMD(aligned(rgb, JzCzhz: 16) uniform(profile))
static inline void rgb_to_JzCzhz(const dt_aligned_pixel_t rgb,
                                 dt_aligned_pixel_t JzCzhz,
                                 const dt_iop_order_iccprofile_info_t *const profile)
{
  dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t JzAzBz = { 0.0f, 0.0f, 0.0f };

  if(profile)
  {
    dt_aligned_pixel_t XYZ_D50 = { 0.0f, 0.0f, 0.0f };
    dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D50, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
    dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
  }
  else
  {
    // This should not happen (we don't know what RGB is), but use this when profile is not defined
    dt_XYZ_D50_2_XYZ_D65(rgb, XYZ_D65);
  }

  dt_XYZ_2_JzAzBz(XYZ_D65, JzAzBz);
  dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);
}

typedef void((*picker_worker_4ch)(dt_aligned_pixel_t acc,
                                  dt_aligned_pixel_t low,
                                  dt_aligned_pixel_t high,
                                  const float *const pixels,
                                  const size_t width,
                                  const void *const data));

typedef void((*picker_worker_1ch)(dt_aligned_pixel_t acc,
                                  dt_aligned_pixel_t low,
                                  dt_aligned_pixel_t high,
                                  dt_aligned_weights_t weights,
                                  const float *const pixels,
                                  const size_t j,
                                  const dt_iop_roi_t *const roi,
                                  const int *const box,
                                  const void *const data));

static inline void _color_picker_rgb_or_lab(dt_aligned_pixel_t acc,
                                            dt_aligned_pixel_t low,
                                            dt_aligned_pixel_t high,
                                            const float *const pixels,
                                            const size_t width,
                                            const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
    for_each_channel(k, aligned(acc,low,high,pixels:16))
      _update_stats_by_ch(acc, low, high, k, pixels[i + k]);
}

static inline void _color_picker_lch(dt_aligned_pixel_t acc,
                                     dt_aligned_pixel_t low,
                                     dt_aligned_pixel_t high,
                                     const float *const pixels,
                                     const size_t width,
                                     const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_Lab_2_LCH(pixels + i, pick);
    // allow for determining sensible max/min values
    // FIXME: the mean calculation of hue isn't always right, use circular mean calc instead?
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    _update_stats_4ch(acc, low, high, pick);
  }
}

static inline void _color_picker_hsl(dt_aligned_pixel_t acc,
                                     dt_aligned_pixel_t low,
                                     dt_aligned_pixel_t high,
                                     const float *const pixels,
                                     const size_t width,
                                     const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_RGB_2_HSL(pixels + i, pick);
    // allow for determining sensible max/min values
    // FIXME: the mean calculation of hue isn't always right, use circular mean calc instead?
    pick[3] = pick[0] < 0.5f ? pick[0] + 0.5f : pick[0] - 0.5f;
    _update_stats_4ch(acc, low, high, pick);
  }
}

static inline void _color_picker_jzczhz(dt_aligned_pixel_t acc,
                                        dt_aligned_pixel_t low,
                                        dt_aligned_pixel_t high,
                                        const float *const pixels,
                                        const size_t width,
                                        const void *const data)
{
  const dt_iop_order_iccprofile_info_t *const profile = data;
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    rgb_to_JzCzhz(pixels + i, pick, profile);
    // allow for determining sensible max/min values
    // FIXME: the mean calculation of hue isn't always right, use circular mean calc instead?
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    _update_stats_4ch(acc, low, high, pick);
  }
}

static inline void _color_picker_bayer(dt_aligned_pixel_t acc,
                                       dt_aligned_pixel_t low,
                                       dt_aligned_pixel_t high,
                                       dt_aligned_weights_t weights,
                                       const float *const pixels,
                                       const size_t j,
                                       const dt_iop_roi_t *const roi,
                                       const int *const box,
                                       const void *const data)
{
  const uint32_t filters = GPOINTER_TO_UINT(data);
  for(size_t i = box[0]; i < box[2]; i++)
  {
    const int c = FC(j + roi->y, i + roi->x, filters);
    _update_stats_by_ch(acc, low, high, c, pixels[i]);
    weights[c]++;
  }
}

static inline void _color_picker_xtrans(dt_aligned_pixel_t acc,
                                        dt_aligned_pixel_t low,
                                        dt_aligned_pixel_t high,
                                        dt_aligned_weights_t weights,
                                        const float *const pixels,
                                        const size_t j,
                                        const dt_iop_roi_t *const roi,
                                        const int *const box,
                                        const void *const data)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])data;
  for(size_t i = box[0]; i < box[2]; i++)
  {
    const int c = FCxtrans(j, i, roi, xtrans);
    _update_stats_by_ch(acc, low, high, c, pixels[i]);
    weights[c]++;
  }
}

static void _color_picker_work_4ch(const float *const pixel,
                                   const dt_iop_roi_t *const roi,
                                   const int *const box,
                                   lib_colorpicker_stats pick,
                                   const void *const data,
                                   const picker_worker_4ch worker,
                                   const size_t min_for_threads)
{
  const int width = roi->width;
  const size_t size = _box_size(box);
  const size_t stride = 4 * (size_t)(box[2] - box[0]);
  const size_t off_mul = 4 * width;
  const size_t off_add = 4 * box[0];

  dt_aligned_pixel_t acc = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t low = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
  dt_aligned_pixel_t high = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };

  // min_for_threads depends on # of samples and complexity of the
  // colorspace conversion
  DT_OMP_FOR(if (size > min_for_threads) reduction(+ : acc[:4]) reduction(min : low[:4]) reduction(max : high[:4]))
  for(size_t j = box[1]; j < box[3]; j++)
  {
    const size_t offset = j * off_mul + off_add;
    worker(acc, low, high, pixel + offset, stride, data);
  }

  // copy all four channels, as four some colorspaces there may be
  // meaningful data in the fourth pixel
  for_four_channels(c)
  {
    pick[DT_PICK_MEAN][c] = acc[c] / (float)size;
    pick[DT_PICK_MIN][c] = low[c];
    pick[DT_PICK_MAX][c] = high[c];
  }
}

static void _color_picker_work_1ch(const float *const pixel,
                                   const dt_iop_roi_t *const roi,
                                   const int *const box,
                                   lib_colorpicker_stats pick,
                                   const void *const data,
                                   const picker_worker_1ch worker,
                                   const size_t min_for_threads)
{
  const int width = roi->width;
  dt_aligned_pixel_t acc = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t low = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
  dt_aligned_pixel_t high = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
  dt_aligned_weights_t weights = { 0u, 0u, 0u, 0u };

  // worker logic is slightly different from 4-channel as we need to
  // keep track of position in the mosiac
  DT_OMP_FOR(if (_box_size(box) > min_for_threads) reduction(+ : acc[:4], weights[:4]) reduction(min : low[:4]) reduction(max : high[:4]))
  for(size_t j = box[1]; j < box[3]; j++)
  {
    worker(acc, low, high, weights, pixel + width * j, j, roi, box, data);
  }

  copy_pixel(pick[DT_PICK_MIN], low);
  copy_pixel(pick[DT_PICK_MAX], high);
  // and finally normalize data. For bayer, there is twice as much green.
  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  for_each_channel(c)
    pick[DT_PICK_MEAN][c] = weights[c] ? (acc[c] / (float)weights[c]) : 0.0f;
}

void dt_color_picker_backtransform_box(dt_develop_t *dev,
                                       const int num,
                                       const float *in,
                                       float *out)
{
  const float wd  = MAX(1, dev->preview_pipe->iwidth);
  const float ht  = MAX(1, dev->preview_pipe->iheight);
  const float wdp = MAX(1, dev->preview_pipe->processed_width);
  const float htp = MAX(1, dev->preview_pipe->processed_height);

  const int out_num = num == 2 ? 4 : 1;

  for(int i = 0; i < out_num; i++)
  {
    out[i * 2    ] = wdp * in[(i % 3 > 0) * 2];
    out[i * 2 + 1] = htp * in[(i % 2) * 2 + 1];
  }
  dt_dev_distort_backtransform(dev, out, out_num);
  for(int i = 0; i < out_num; i++)
  {
    out[i * 2    ] = CLIP(out[i * 2    ] / wd);
    out[i * 2 + 1] = CLIP(out[i * 2 + 1] / ht);
  }
}

static void _sort_coordinates(float *fbox)
{
  float tmp;
  #define SWAP(a, b) { tmp = (a); (a) = (b); (b) = tmp; }
  if(fbox[0] > fbox[2]) SWAP(fbox[0], fbox[2]);
  if(fbox[1] > fbox[3]) SWAP(fbox[1], fbox[3]);
  if(fbox[4] > fbox[6]) SWAP(fbox[4], fbox[6]);
  if(fbox[5] > fbox[7]) SWAP(fbox[5], fbox[7]);
  if(fbox[0] > fbox[4]) SWAP(fbox[0], fbox[4]);
  if(fbox[1] > fbox[5]) SWAP(fbox[1], fbox[5]);
  if(fbox[2] > fbox[6]) SWAP(fbox[2], fbox[6]);
  if(fbox[3] > fbox[7]) SWAP(fbox[3], fbox[7]);
  if(fbox[2] > fbox[4]) SWAP(fbox[2], fbox[4]);
  if(fbox[3] > fbox[5]) SWAP(fbox[3], fbox[5]);
  #undef SWAP
}

// Use by darkroom visualizing
void dt_color_picker_transform_box(dt_develop_t *dev,
                                   const int num,
                                   const float *in,
                                   float *out,
                                   gboolean scale)
{
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;
  const float wdp = scale ? dev->preview_pipe->processed_width : 1.0f;
  const float htp = scale ? dev->preview_pipe->processed_height : 1.0f;

  const gboolean box = num == 2;
  if(wd < 1.0f || ht < 1.0f)
  {
    for(int i = 0; i < num; i++)
      out[i] = in[i];
    return;
  }

  dt_pickerbox_t fbox;
  for(int i = 0; i < 8; i += 2)
  {
    fbox[i    ] = wd * in[i    ];
    fbox[i + 1] = ht * in[i + 1];
  }

  dt_dev_distort_transform(dev, fbox, box ? 4 : 1);

  if(box) // sort the 4 point coordinates
  {
    _sort_coordinates(fbox);
    out[0] = 0.5f * (fbox[0] + fbox[2]) / wdp;
    out[1] = 0.5f * (fbox[1] + fbox[3]) / htp;
    out[2] = 0.5f * (fbox[4] + fbox[6]) / wdp;
    out[3] = 0.5f * (fbox[5] + fbox[7]) / htp;
  }
  else
  {
    out[0] = fbox[0] / wdp;
    out[1] = fbox[1] / htp;
  }
}

// calculate box in current module's coordinates for the color picker
gboolean dt_color_picker_box(dt_iop_module_t *module,
                             const dt_iop_roi_t *roi,
                             const dt_colorpicker_sample_t *const sample,
                             dt_pixelpipe_picker_source_t picker_source,
                             int *box)
{
  if(picker_source == PIXELPIPE_PICKER_OUTPUT
     && !sample->pick_output)
    return TRUE;

  dt_develop_t *dev = darktable.develop;
  const float wd = dev->preview_pipe->iwidth;
  const float ht = dev->preview_pipe->iheight;

  const int width = roi->width;
  const int height = roi->height;
  const gboolean isbox = sample->size == DT_LIB_COLORPICKER_SIZE_BOX;

  /* get absolute pixel coordinates in final preview image.
     we transform back all 4 corner locations to current module coordinates,
     sort the coordinates, and use average of 2 highest and 2 lowest for the
     resulting rectangle.
  */
  dt_pickerbox_t fbox;
  for(int i = 0; i < 8; i += 2)
  {
    fbox[i    ] = wd * (isbox ? sample->box[i    ] : sample->point[0]);
    fbox[i + 1] = ht * (isbox ? sample->box[i + 1] : sample->point[1]);
  }

  const gboolean expanded = module->flags() & IOP_FLAGS_EXPAND_ROI_IN;
  const gboolean input = picker_source == PIXELPIPE_PICKER_INPUT;
  dt_dev_distort_transform_plus(dev, dev->preview_pipe,
                                module->iop_order - (expanded && input ? 1 : 0),
                                input ? DT_DEV_TRANSFORM_DIR_BACK_INCL : DT_DEV_TRANSFORM_DIR_BACK_EXCL,
                                fbox, 4);

  _sort_coordinates(fbox);
  box[0] = 0.5f * (fbox[0] + fbox[2]) - roi->x;
  box[1] = 0.5f * (fbox[1] + fbox[3]) - roi->y;
  box[2] = 0.5f * (fbox[4] + fbox[6]) - roi->x;
  box[3] = 0.5f * (fbox[5] + fbox[7]) - roi->y;

  // make sure we sample at least one point
  box[2] = MAX(box[2], box[0] + 1);
  box[3] = MAX(box[3], box[1] + 1);

  // FIXME: on invalid box, caller should set sample to something like
  // NaN to flag it as invalid
  if(box[0] >= width || box[1] >= height || box[2] < 0 || box[3] < 0)
    return TRUE;

  // clamp bounding box to roi
  box[0] = CLAMP(box[0], 0, width - 1);
  box[1] = CLAMP(box[1], 0, height - 1);
  box[2] = CLAMP(box[2], 1, width);
  box[3] = CLAMP(box[3], 1, height);

  // return an error condition if area has not at least 1 pixel width and height
  return (box[2] - box[0] < 1) || (box[3] - box[1] < 1);
}

void dt_color_picker_helper(const dt_iop_buffer_dsc_t *dsc,
                            const float *const pixel,
                            const dt_iop_roi_t *roi,
                            const int *const box,
                            const gboolean denoise,
                            lib_colorpicker_stats pick,
                            const dt_iop_colorspace_type_t image_cst,
                            const dt_iop_colorspace_type_t picker_cst,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  dt_times_t start_time = { 0 };
  dt_get_perf_times(&start_time);

  for_four_channels(k)
  {
    pick[DT_PICK_MEAN][k] = 0.0f;
    pick[DT_PICK_MIN][k] = FLT_MAX;
    pick[DT_PICK_MAX][k] = -FLT_MAX;
  }

  if(dsc->channels == 4u)
  {
    float *restrict denoised = NULL;
    const float *source = pixel;
    if(denoise)
    {
      // Denoise the image
      size_t padded_size;
      denoised = dt_alloc_align_float(4 * roi->width * roi->height);
      if(denoised)
      {
        float *const tempbuf =
          dt_alloc_perthread_float(4 * roi->width, &padded_size); //TODO: alloc in caller

        // blur without clipping negatives because Lab a and b channels can be
        // legitimately negative
        // FIXME: this blurs whole image even when just a bit is sampled in the
        // case of CPU path
        blur_2D_Bspline(pixel, denoised, tempbuf, padded_size,
                        roi->width, roi->height, 1, FALSE);
        dt_free_align(tempbuf);
        source = denoised;
      }
      else
        dt_print(DT_DEBUG_ALWAYS,
                 "[color picker] unable to alloc working memory, denoising skipped");
    }

    // 4-channel raw images are monochrome, can be read as RGB
    const dt_iop_colorspace_type_t effective_cst =
      image_cst == IOP_CS_RAW ? IOP_CS_RGB : image_cst;

    if(effective_cst == IOP_CS_LAB && picker_cst == IOP_CS_LCH)
    {
      // blending for Lab modules (e.g. color zones and tone curve)
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_lch, 10);
    }
    else if(effective_cst == IOP_CS_RGB && picker_cst == IOP_CS_HSL)
    {
      // display-referred blending for RGB modules
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_hsl, 10);
    }
    else if(effective_cst == IOP_CS_RGB && picker_cst == IOP_CS_JZCZHZ)
    {
      // scene-referred blending for RGB modules
      _color_picker_work_4ch(source, roi, box, pick, profile, _color_picker_jzczhz, 10);
    }
    else if(effective_cst == picker_cst)
    {
      // most iop pickers and the global picker
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 100);
    }
    else if(picker_cst == IOP_CS_NONE)
    {
      // temperature IOP when correcting non-RAW
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 100);
    }
    else
    {
      // fallback, but this shouldn't happen
      dt_print(DT_DEBUG_ALWAYS,
               "[colorpicker] unknown colorspace conversion from %s to %s",
               dt_iop_colorspace_to_name(image_cst), dt_iop_colorspace_to_name(picker_cst));
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 100);
    }

    dt_free_align(denoised);
  }
  else if(dsc->channels == 1u && dsc->filters != 0u && dsc->filters != 9u)
  {
    _color_picker_work_1ch(pixel, roi, box, pick, GUINT_TO_POINTER(dsc->filters),
                           _color_picker_bayer, 100);
  }
  else if(dsc->channels == 1u && dsc->filters == 9u)
  {
    _color_picker_work_1ch(pixel, roi, box, pick, dsc->xtrans,
                           _color_picker_xtrans, 100);
  }
  else
    dt_unreachable_codepath();

  dt_print(DT_DEBUG_PERF,
           "dt_color_picker_helper stats reading %u channels (filters %u) cst %d -> %d "
           "size %zu denoised %d took %.3f secs (%.3f CPU)",
           dsc->channels, dsc->filters, image_cst, picker_cst, _box_size(box), denoise,
           dt_get_lap_time(&start_time.clock), dt_get_lap_utime(&start_time.user));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
