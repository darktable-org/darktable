/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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


#define DT_UNIT_TEST
// define dt alloc, so we don't need to include the rest of dt:
#define dt_alloc_align(A, B) malloc(B)
#define MAX(a, b) ((a) > (b) ? (a) : (b))

// unit test for the concurrent hopscotch hashmap and the LRU cache built on top of it.
#include "common/cache.h"
#include "common/cache.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _OPENMP
#include <omp.h>
#endif

int32_t alloc_dummy(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  *cost = 1; // also the default
  *buf = (void *)(long int)key;
  // request write lock for our buffer?
  return 0;
}

int main(int argc, char *arg[])
{
  dt_cache_t cache;
  // dt_cache_init(&cache, 110000, 16, 64, 100000);
  // really hammer it, make quota insanely low:
  dt_cache_init(&cache, 110000, 16, 64, 100);
  dt_cache_set_allocate_callback(&cache, alloc_dummy, NULL);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(guided) shared(cache, stderr) num_threads(16)
#endif
  for(int k = 0; k < 100000; k++)
  {
    void *data = (void *)(long int)k;
    const int size = 0; // dt_cache_size(&cache);
    const int con1 = dt_cache_contains(&cache, k);
    const int val1 = (int)(long int)dt_cache_read_get(&cache, k);
    const int val2 = (int)(long int)dt_cache_read_get(&cache, k);
    // fprintf(stderr, "\rinserted number %d, size %d, value %d - %d, contains %d - %d", k, size, val1, val2,
    // con1, con2);
    const int con2 = dt_cache_contains(&cache, k);
    assert(con1 == 0);
    assert(con2 == 1);
    assert(val2 == k);
    dt_cache_read_release(&cache, k);
    dt_cache_read_release(&cache, k);
  }
  dt_cache_print_locked(&cache);
  // fprintf(stderr, "\n");
  fprintf(stderr, "[passed] inserting 100000 entries concurrently\n");

  const int size = dt_cache_size(&cache);
  const int lru_cnt = lru_check_consistency(&cache);
  const int lru_cnt_r = lru_check_consistency_reverse(&cache);
  // fprintf(stderr, "lru list contains %d|%d/%d entries\n", lru_cnt, lru_cnt_r, size);
  assert(size == lru_cnt);
  assert(lru_cnt_r == lru_cnt);
  fprintf(stderr, "[passed] cache lru consistency after removals, have %d entries left.\n", size);

  dt_cache_cleanup(&cache);



  {
    // now a harder case: a cache with only one entry and a lot of threads fighting over it:
    dt_cache_t cache2;
    // really hammer it, make quota insanely low:
    // capacity 1 num threads 1 cache line size 64 ignored, quota 2 (80% => 1)
    dt_cache_init(&cache2, 1, 1, 64, 2);
    dt_cache_set_allocate_callback(&cache2, alloc_dummy, NULL);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(guided) shared(cache2, stderr) num_threads(16)
#endif
    for(int k = 0; k < 100000; k++)
    {
      void *data = (void *)(long int)k;
      const int size = 0; // dt_cache_size(&cache);
      const int con1 = dt_cache_contains(&cache2, k);
      const int val1 = (int)(long int)dt_cache_read_get(&cache2, k);
      const int val2 = (int)(long int)dt_cache_read_get(&cache2, k);
      // fprintf(stderr, "\rinserted number %d, size %d, value %d - %d, contains %d - %d", k, size, val1,
      // val2, con1, con2);
      const int con2 = dt_cache_contains(&cache2, k);
      assert(con1 == 0);
      assert(con2 == 1);
      assert(val2 == k);
      dt_cache_read_release(&cache2, k);
      dt_cache_read_release(&cache2, k);
    }
    dt_cache_print_locked(&cache2);
    // fprintf(stderr, "\n");
    fprintf(stderr, "[passed] inserting 100000 entries concurrently\n");

    const int size = dt_cache_size(&cache2);
    const int lru_cnt = lru_check_consistency(&cache2);
    const int lru_cnt_r = lru_check_consistency_reverse(&cache2);
    // fprintf(stderr, "lru list contains %d|%d/%d entries\n", lru_cnt, lru_cnt_r, size);
    assert(size == lru_cnt);
    assert(lru_cnt_r == lru_cnt);
    fprintf(stderr, "[passed] cache lru consistency after removals, have %d entries left.\n", size);
    dt_cache_cleanup(&cache2);
  }

  exit(0);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
