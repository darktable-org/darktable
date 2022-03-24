/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include "common/atomic.h"

extern inline void dt_atomic_set_int(dt_atomic_int *var, int value);
extern inline int dt_atomic_get_int(dt_atomic_int *var);
extern inline int dt_atomic_add_int(dt_atomic_int *var, int incr);
extern inline int dt_atomic_sub_int(dt_atomic_int *var, int decr);
extern inline int dt_atomic_exch_int(dt_atomic_int *var, int value);
extern inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value);

#if !defined(__STDC_NO_ATOMICS__)
// using C11 atomics, everything is handled in the header file, so we don't need to define anything in this file

#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNU_MINOR__ >= 8))
// using GNU intrinsics. everything is handled in the header file

#else
// we fell back to using a global mutex for synchronization
// this is that mutex's definition
pthread_mutex_t dt_atom_mutex = PTHREAD_MUTEX_INITIALIZER;

#endif // __STDC_NO_ATOMICS__

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

