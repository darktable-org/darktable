/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.
    copyright (c) 2016 LebedevRI.

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

// unit test for the concurrent LRU cache
#include "common/cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _OPENMP
#include <omp.h>
#endif

static const size_t goal = 100000;
#ifdef _OPENMP
static const size_t threads = 16;
#endif

static void test_cache(dt_cache_t *const cache)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(guided) num_threads(threads)
#endif
  for(size_t k = 0; k < goal; k++)
  {
    const int con1 = dt_cache_contains(cache, k);
    dt_cache_entry_t *val1 = dt_cache_get(cache, k, 'r');
    dt_cache_entry_t *val2 = dt_cache_get(cache, k, 'r');
    const int con2 = dt_cache_contains(cache, k);

#ifdef _DEBUG
    assert(con1 == 0);
    assert(con2 == 1);
    assert(val1 == val2);
    assert(val2->key == k);
#else
    if(!(con1 == 0)) exit(EXIT_FAILURE);
    if(!(con2 == 1)) exit(EXIT_FAILURE);
    if(!(val1 == val2)) exit(EXIT_FAILURE);
    if(!(val2->key == k)) exit(EXIT_FAILURE);
#endif

    dt_cache_release(cache, val1);
    dt_cache_release(cache, val2);
  }

  fprintf(stderr,
          "[passed] inserting %zu entries concurrently; cost = %zu; cost quota = %zu; usage = %05.2f%%\n",
          goal, cache->cost, cache->cost_quota, 100.0f * (double)cache->cost / (double)cache->cost_quota);
}

int main(int argc, char *arg[])
{
  {
    dt_cache_t cache;

    // really hammer it, make quota insanely low:
    const size_t entry_size = 100;
    const size_t cost_quota = (double)entry_size * (double)goal * (double)0.5;

    dt_cache_init(&cache, entry_size, cost_quota);

    test_cache(&cache);

    dt_cache_cleanup(&cache);
  }

  {
    // now a harder case: a cache with only one entry and a lot of threads fighting over it:

    dt_cache_t cache;

    // really hammer it, make quota insanely low:
    dt_cache_init(&cache, 1, 2);

    test_cache(&cache);

    dt_cache_cleanup(&cache);
  }

  exit(EXIT_SUCCESS);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
