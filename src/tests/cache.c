/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.

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

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#ifdef _OPENMP
#  include <omp.h>
#endif

int32_t
alloc_dummy(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  *cost = 1; // also the default
  *buf = (void *)(long int)key;
  return 1;
}

int main(int argc, char *arg[])
{
  dt_cache_t cache;
  dt_cache_init(&cache, 110000, 16, 64, 20);
  dt_cache_set_allocate_callback(&cache, alloc_dummy, NULL);

#ifdef _OPENMP
#  pragma omp parallel for default(none) schedule(guided) shared(cache, stderr) num_threads(16)
#endif
  for(int k=0;k<100000;k++)
  {
    void *data = (void *)(long int)k;
    const int size = 0;//dt_cache_size(&cache);
    const int con1 = dt_cache_contains(&cache, k);
    const int val1 = (int)(long int)dt_cache_read_get(&cache, k);
    const int val2 = (int)(long int)dt_cache_read_get(&cache, k);
    dt_cache_read_release(&cache, k);
    dt_cache_read_release(&cache, k);
    const int con2 = dt_cache_contains(&cache, k);
    fprintf(stderr, "\rinserted number %d, size %d, value %d - %d, contains %d - %d", k, size, val1, val2, con1, con2);
    assert (con1 == 0);
    assert (con2 == 1);
    assert (val2 == k);
  }
  fprintf(stderr, "\n");
  fprintf(stderr, "[passed] inserting 100000 entries concurrently\n");

  const int size = dt_cache_size(&cache);
  const int lru_cnt   = lru_check_consistency(&cache);
  const int lru_cnt_r = lru_check_consistency_reverse(&cache);
  // fprintf(stderr, "lru list contains %d|%d/%d entries\n", lru_cnt, lru_cnt_r, size);
  assert(size == lru_cnt);
  fprintf(stderr, "[passed] cache lru consistency after insertions\n");

  // also hammer removals.
#ifdef _OPENMP
#  pragma omp parallel for default(none) schedule(guided) shared(cache, stderr) num_threads(16)
#endif
  for(int k=0;k<100000;k+=5)
  {
    dt_cache_remove(&cache, k);
  }
  const int size2 = dt_cache_size(&cache);
  const int lru_cnt2 = lru_check_consistency(&cache);
  assert(size2 == lru_cnt2);
  assert(size2 == 100000-100000/5);
  fprintf(stderr, "[passed] cache lru consistency after removals, have %d entries left.\n", size2);

  // TODO: implement and test automatic garbage collection.

  dt_cache_cleanup(&cache);
  exit(0);
}
