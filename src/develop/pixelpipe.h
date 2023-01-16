/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* The pixelpipe types here are all defined as a bit mask to ensure easy testing via & operator */
typedef enum dt_dev_pixelpipe_type_t
{
  DT_DEV_PIXELPIPE_NONE      = 0,
  DT_DEV_PIXELPIPE_EXPORT    = 1 << 0,
  DT_DEV_PIXELPIPE_FULL      = 1 << 1,
  DT_DEV_PIXELPIPE_PREVIEW   = 1 << 2,
  DT_DEV_PIXELPIPE_THUMBNAIL = 1 << 3,
  DT_DEV_PIXELPIPE_PREVIEW2  = 1 << 4,
  DT_DEV_PIXELPIPE_ANY       = DT_DEV_PIXELPIPE_EXPORT | DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_PREVIEW
                               | DT_DEV_PIXELPIPE_THUMBNAIL | DT_DEV_PIXELPIPE_PREVIEW2,
  DT_DEV_PIXELPIPE_FAST      = 1 << 8
} dt_dev_pixelpipe_type_t;

/** when to collect histogram */
typedef enum dt_dev_request_flags_t
{
  DT_REQUEST_NONE = 0,
  DT_REQUEST_ON = 1 << 0,
  DT_REQUEST_ONLY_IN_GUI = 1 << 1
} dt_dev_request_flags_t;

// params to be used to collect histogram
typedef struct dt_dev_histogram_collection_params_t
{
  /** histogram_collect: if NULL, correct is set; else should be set manually */
  const struct dt_histogram_roi_t *roi;
  /** count of histogram bins. */
  uint32_t bins_count;
} dt_dev_histogram_collection_params_t;

// params used to collect histogram during last histogram capture
typedef struct dt_dev_histogram_stats_t
{
  /** count of histogram bins. */
  uint32_t bins_count;
  /** size of currently allocated buffer, or 0 if none */
  size_t buf_size;
  /** count of pixels sampled during histogram capture. */
  uint32_t pixels;
  /** count of channels: 1 for RAW, 3 for rgb/Lab. */
  uint32_t ch;
} dt_dev_histogram_stats_t;

#ifndef DT_IOP_PARAMS_T
#define DT_IOP_PARAMS_T
typedef void dt_iop_params_t;
#endif

const char *dt_pixelpipe_name(dt_dev_pixelpipe_type_t pipe);

#include "develop/pixelpipe_hb.h"

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

