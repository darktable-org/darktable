
#include "common/cache.h"

#include <stdlib.h>
#include <stdio.h>

int main(int argc, char *arg[])
{
  dt_cache_t cache;
  dt_cache_init(&cache, 100000, 16, 64, 1);

  // TODO: openmp
#ifdef _OPENMP
#  pragma omp parallel for default(none) schedule(static) shared(cache, stderr)
#endif
  for(int k=0;k<10000;k++)
  {
    void *data = (void *)(long int)k;
    const int size = dt_cache_size(&cache);
    const int con1 = dt_cache_contains(&cache, k);
    const int val1 = (int)(long int)dt_cache_put(&cache, k, data);
    const int val2 = (int)(long int)dt_cache_put(&cache, k, data);
    const int con2 = dt_cache_contains(&cache, k);
    fprintf(stderr, "inserted number %d, size %d, value %d - %d, contains %d - %d\n", k, size, val1, val2, con1, con2);
  }

  dt_cache_cleanup(&cache);
  exit(0);
}
