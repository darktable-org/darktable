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

#pragma once

// implement an atomic variable for inter-thread signalling purposes
// the manner in which we implement depends on the capabilities of the compiler:
//   1. standard-compliant C++ compiler: use C++11 atomics in <atomic>
//   2. standard-compliant C compiler: use C11 atomics in <stdatomic.h>
//   3. GCC 4.8+: use intrinsics
//   4. otherwise: fall back to using Posix mutex to serialize access

#if defined(__cplusplus) && __cplusplus > 201100

#include <atomic>

typedef std::atomic<int> dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value) { std::atomic_store(var,value); }
inline int dt_atomic_get_int(dt_atomic_int *var) { return std::atomic_load(var); }
inline int dt_atomic_add_int(dt_atomic_int *var, int incr) { return std::atomic_fetch_add(var,incr); }
inline int dt_atomic_sub_int(dt_atomic_int *var, int decr) { return std::atomic_fetch_sub(var,decr); }
inline int dt_atomic_exch_int(dt_atomic_int *var, int value) { return std::atomic_exchange(var,value); }
inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{ return std::atomic_compare_exchange_strong(var,expected,value); }

#elif !defined(__STDC_NO_ATOMICS__)

#include <stdatomic.h>

typedef atomic_int dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value) { atomic_store(var,value); }
inline int dt_atomic_get_int(dt_atomic_int *var) { return atomic_load(var); }
inline int dt_atomic_add_int(dt_atomic_int *var, int incr) { return atomic_fetch_add(var,incr); }
inline int dt_atomic_sub_int(dt_atomic_int *var, int decr) { return atomic_fetch_sub(var,decr); }
inline int dt_atomic_exch_int(dt_atomic_int *var, int value) { return atomic_exchange(var,value); }
inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{ return atomic_compare_exchange_strong(var,expected,value); }

#elif defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNU_MINOR__ >= 8))
// we don't have or aren't supposed to use C11 atomics, but the compiler is a recent-enough version of GCC
// that we can use GNU intrinsics corresponding to the C11 atomics

typedef volatile int dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value) { __atomic_store(var,&value,__ATOMIC_SEQ_CST); }
inline int dt_atomic_get_int(dt_atomic_int *var)
{ int value ; __atomic_load(var,&value,__ATOMIC_SEQ_CST); return value; }

inline int dt_atomic_add_int(dt_atomic_int *var, int incr) { return __atomic_fetch_add(var,incr,__ATOMIC_SEQ_CST); }
inline int dt_atomic_sub_int(dt_atomic_int *var, int decr) { return __atomic_fetch_sub(var,decr,__ATOMIC_SEQ_CST); }
inline int dt_atomic_exch_int(dt_atomic_int *var, int value)
{ int orig;  __atomic_exchange(var,&value,&orig,__ATOMIC_SEQ_CST); return orig; }
inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{ return __atomic_compare_exchange(var,expected,&value,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); }

#else
// we don't have or aren't supposed to use C11 atomics, and don't have GNU intrinsics, so
// fall back to using a mutex for synchronization
#include <pthread.h>

extern pthread_mutex_t dt_atom_mutex;

typedef int dt_atomic_int;
inline void dt_atomic_set_int(dt_atomic_int *var, int value)
{
  pthread_mutex_lock(&dt_atom_mutex);
  *var = value;
  pthread_mutex_unlock(&dt_atom_mutex);
}

inline int dt_atomic_get_int(const dt_atomic_int *const var)
{
  pthread_mutex_lock(&dt_atom_mutex);
  int value = *var;
  pthread_mutex_unlock(&dt_atom_mutex);
  return value;
}

inline int dt_atomic_add_int(const dt_atomic_int *const var, int incr)
{
  pthread_mutex_lock(&dt_atom_mutex);
  int value = *var;
  *var += incr;
  pthread_mutex_unlock(&dt_atom_mutex);
  return value;
}

inline int dt_atomic_sub_int(const dt_atomic_int *const var, int decr)
{
  pthread_mutex_lock(&dt_atom_mutex);
  int value = *var;
  *var -= decr;
  pthread_mutex_unlock(&dt_atom_mutex);
  return value;
}

inline int dt_atomic_exch_int(dt_atomic_int *var, int value)
{
  pthread_mutex_lock(&dt_atom_mutex);
  int origvalue = *var;
  *var = value;
  pthread_mutex_unlock(&dt_atom_mutex);
  return origvalue;
}

inline int dt_atomic_CAS_int(dt_atomic_int *var, int *expected, int value)
{
  pthread_mutex_lock(&dt_atom_mutex);
  int origvalue = *var;
  int success = FALSE;
  if (origvalue == *expected)
  {
    *var = value;
    success = TRUE;
  }
  *expected = origvalue;
  pthread_mutex_unlock(&dt_atom_mutex);
  return success;
}

#endif // __STDC_NO_ATOMICS__

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

