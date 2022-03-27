/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#pragma once

#include <stdint.h>

#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "common/iop_profile.h"

/*
 * histogram region of interest
 *
 * image is located in (0,     0)      .. (width,           height)
 * but only            (crop_x,crop_y) .. (width-crop_width,height-crop_height)
 * will be sampled
 */
typedef struct dt_histogram_roi_t
{
  int width, height, crop_x, crop_y, crop_width, crop_height;
} dt_histogram_roi_t;

void dt_histogram_helper_cs_RAW_uint16(const dt_dev_histogram_collection_params_t *histogram_params,
                                       const void *pixel, uint32_t *histogram, int j,
                                       const dt_iop_order_iccprofile_info_t *const profile_info);

typedef void((*dt_worker)(const dt_dev_histogram_collection_params_t *const histogram_params,
                          const void *pixel, uint32_t *histogram, int j,
                          const dt_iop_order_iccprofile_info_t *const profile_info));

void dt_histogram_worker(dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const void *const pixel,
                         uint32_t **histogram, const dt_worker Worker,
                         const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const dt_iop_colorspace_type_t cst,
                         const dt_iop_colorspace_type_t cst_to, const void *pixel, uint32_t **histogram,
                         const int compensate_middle_grey, const dt_iop_order_iccprofile_info_t *const profile_info);

void dt_histogram_max_helper(const dt_dev_histogram_stats_t *const histogram_stats,
                             const dt_iop_colorspace_type_t cst, const dt_iop_colorspace_type_t cst_to,
                             uint32_t **histogram, uint32_t *histogram_max);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

