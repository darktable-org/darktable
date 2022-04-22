/*
    This file is part of darktable,
    Copyright (C) 2017-2021 darktable developers.

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

#ifndef DT_DEVELOP_HEAL_H
#define DT_DEVELOP_HEAL_H

/* heals dest_buffer using src_buffer as a reference and mask_buffer to define the area to be healed
 * the 3 buffers must have the same size, but mask_buffer is 1 channel and is tested for != 0.f
 */
void dt_heal(const float *const src_buffer, float *dest_buffer, const float *const mask_buffer, const int width,
             const int height, const int ch, const int max_iter);

#ifdef HAVE_OPENCL

typedef struct dt_heal_cl_global_t
{
  int kernel_dummy;
} dt_heal_cl_global_t;

typedef struct heal_params_cl_t
{
  dt_heal_cl_global_t *global;
  int devid;
} heal_params_cl_t;

dt_heal_cl_global_t *dt_heal_init_cl_global(void);
void dt_heal_free_cl_global(dt_heal_cl_global_t *g);

heal_params_cl_t *dt_heal_init_cl(const int devid);
void dt_heal_free_cl(heal_params_cl_t *p);

cl_int dt_heal_cl(heal_params_cl_t *p, cl_mem dev_src, cl_mem dev_dest, const float *const mask_buffer,
                  const int width, const int height, const int max_iter);

#endif
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
