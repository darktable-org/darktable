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


// unit test for the concurrent hopscotch hashmap and the LRU cache built on top of it.
#include "common/cache.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

int main(int argc, char *arg[])
{
  dt_cache_t cache;
  dt_cache_init(&cache, 110000, 16, 64, 1);

#ifdef _OPENMP
#  pragma omp parallel for default(none) schedule(guided) shared(cache, stderr)
#endif
  for(int k=0;k<100000;k++)
  {
    void *data = (void *)(long int)k;
    const int size = dt_cache_size(&cache);
    const int con1 = dt_cache_contains(&cache, k);
    const int val1 = (int)(long int)dt_cache_put(&cache, k, data);
    const int val2 = (int)(long int)dt_cache_put(&cache, k, data);
    const int con2 = dt_cache_contains(&cache, k);
    fprintf(stderr, "\rinserted number %d, size %d, value %d - %d, contains %d - %d", k, size, val1, val2, con1, con2);
    assert (con1 == 0);
    assert (con2 == 1);
    assert (val2 == k);
  }
  fprintf(stderr, "\n");

  dt_cache_cleanup(&cache);
  exit(0);
}
