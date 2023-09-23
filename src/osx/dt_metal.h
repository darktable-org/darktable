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

#include "common/darktable.h"
#include <sys/types.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_metal_device_t
{
  dt_pthread_mutex_t lock;
  u_int64_t devid;
  void *device;
} dt_metal_device_t;


/**
 * main struct, stored in darktable.metal.
 * holds pointers to all
 */
typedef struct dt_metal_t
{
  int num_devs;
  dt_metal_device_t *dev;
} dt_metal_t;


void dt_metal_init(dt_metal_t *metal);


#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

