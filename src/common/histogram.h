/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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

#ifndef DT_COMMON_HISTOGRAM_H
#define DT_COMMON_HISTOGRAM_H

#include <stdint.h>

#include "develop/imageop.h"
#include "develop/pixelpipe.h"

void dt_histogram_helper_cs_RAW_uint16(const dt_dev_histogram_collection_params_t *histogram_params,
                                       const void *pixel, uint32_t *histogram, int j);

typedef void((*dt_worker)(const dt_dev_histogram_collection_params_t *const histogram_params,
                          const void *pixel, uint32_t *histogram, int j));

void dt_histogram_worker(const dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const void *const pixel,
                         uint32_t **histogram, const dt_worker Worker);

void dt_histogram_helper(const dt_dev_histogram_collection_params_t *histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, dt_iop_colorspace_type_t cst,
                         const void *pixel, uint32_t **histogram);

void dt_histogram_max_helper(const dt_dev_histogram_stats_t *const histogram_stats,
                             dt_iop_colorspace_type_t cst, uint32_t **histogram, uint32_t *histogram_max);

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
