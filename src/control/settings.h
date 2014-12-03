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
#ifndef DT_CTL_SETTINGS_H
#define DT_CTL_SETTINGS_H

#include "common/darktable.h"
#include "control/signal.h"
#include "common/dtpthread.h"

#include <inttypes.h>

typedef enum dt_dev_zoom_t
{
  DT_ZOOM_FIT = 0,
  DT_ZOOM_FILL = 1,
  DT_ZOOM_1 = 2,
  DT_ZOOM_FREE = 3
} dt_dev_zoom_t;

typedef char dt_dev_operation_t[20];

#define DEV_NUM_OP_PARAMS 10

typedef union dt_dev_operation_params_t
{
  int32_t i[DEV_NUM_OP_PARAMS];
  float f[DEV_NUM_OP_PARAMS];
} dt_dev_operation_params_t;

typedef enum dt_lib_filter_t
{
  DT_LIB_FILTER_ALL = 0,
  DT_LIB_FILTER_STAR_NO = 1,
  DT_LIB_FILTER_STAR_1 = 2,
  DT_LIB_FILTER_STAR_2 = 3,
  DT_LIB_FILTER_STAR_3 = 4,
  DT_LIB_FILTER_STAR_4 = 5,
  DT_LIB_FILTER_STAR_5 = 6,
  DT_LIB_FILTER_REJECT = 7
} dt_lib_filter_t;

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
