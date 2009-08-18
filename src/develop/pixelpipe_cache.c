
#include "develop/pixelpipe_cache.h"
#include <stdlib.h>
#include <inttypes.h>

void dt_dev_pixelpipe_cache_init(dt_dev_pixelpipe_cache_t *cache, int entries, int size)
{
  cache->entries = entries;
  cache->data = (void **)malloc(sizeof(void *)*entries);
  cache->hash = (uint64_t *)malloc(sizeof(uint64_t)*entries);
  cache->used = (int32_t *)malloc(sizeof(int32_t)*entries);
  for(int k=0;k<entries;k++)
  {
    cache->data[k] = (void *)dt_alloc_align(16, size);
    cache->hash[k] = -1;
    cache->used[k] = 0;
  }
}

void dt_dev_pixelpipe_cache_cleanup(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k=0;k<cache->entries;k++) free(cache->data[k]);
  free(cache->data);
  free(cache->hash);
  free(cache->used);
}

uint64_t dt_dev_pixelpipe_cache_hash(int imgid, float scale, int32_t x, int32_t y, dt_dev_pixelpipe_t *pipe, int module)
{
  // bernstein hash (djb2)
  uint64_t hash = 5381 + imgid;
  // go through all modules up to module and compute a weird hash using the operation and params.
  GList *pieces = pipe->nodes;
  for(int k=0;k<module&&pieces;k++)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)pieces->data;
    hash = ((hash << 5) + hash) ^ piece->hash;
    pieces = g_list_next(pieces);
  }
  // also add scale, x and y:
  const char *str = (const char *)&scale;
  for(int i=0;i<4;i++) hash = ((hash << 5) + hash) ^ str[i];
  str = (const char *)&x;
  for(int i=0;i<4;i++) hash = ((hash << 5) + hash) ^ str[i];
  str = (const char *)&y;
  for(int i=0;i<4;i++) hash = ((hash << 5) + hash) ^ str[i];
  return hash;
}

int dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  // search for hash in cache
  for(int k=0;k<cache->entries;k++) if(cache->hash[k] == hash) return 1;
  return 0;
}

int dt_dev_pixelpipe_cache_get_important(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data)
{
  return dt_dev_pixelpipe_cache_get_weighted(cache, hash, data, -4);
}

int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data)
{
  return dt_dev_pixelpipe_cache_get_weighted(cache, hash, data, 0);
}

int dt_dev_pixelpipe_cache_get_weighted(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data, int weight)
{
  *data = NULL;
  int max_used = -1, max = 0;
  for(int k=0;k<cache->entries;k++)
  { // search for hash in cache
    if(cache->used[k] > max_used)
    {
      max_used = cache->used[k];
      max = k;
    }
    cache->used[k]++; // age all entries
    if(cache->hash[k] == hash)
    {
      *data = cache->data[k];
      cache->used[k] = weight; // this is the MRU entry
    }
  }

  if(!*data)
  { // kill LRU entry
    *data = cache->data[max];
    cache->hash[max] = hash;
    cache->used[max] = weight;
    return 1;
  }
  else return 0;
}

void dt_dev_pixelpipe_cache_flush(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k=0;k<cache->entries;k++)
  {
    cache->hash[k] = -1;
    cache->used[k] = 0;
  }
}

void dt_dev_pixelpipe_cache_print(dt_dev_pixelpipe_cache_t *cache)
{
  for(int k=0;k<cache->entries;k++)
  {
    printf("pixelpipe cacheline %d ", k);
    printf("used %d by %"PRIu64"", cache->used[k], cache->hash[k]);
    printf("\n");
  }
}
