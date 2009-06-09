
#include "develop/pixelpipe_cache.h"

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
  for(int k=0;k<entries;k++) free(cache->data[k]);
  free(cache->data);
  free(cache->hash);
  free(cache->used);
}

uint64_t dt_dev_pixelpipe_cache_hash(float scale, int x, int y, dt_develop_t *dev, int module);
{
  // bernstein hash (djb2)
  uint64_t hash = 5381;
  // go through all modules up to module and compute a weird hash using the operation and params.
  GList *modules = dev->iop;
  for(int k=0;k<module;k++)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(module->enabled)
    {
      uint64_t c;
      const char *str = module->op;
      while (c = *str++) hash = ((hash << 5) + hash) ^ c; /* hash * 33 + c */
      str = module->params;
      for(int i=0;i<module->params_size;i++) hash = ((hash << 5) + hash) ^ str[i];
    }
    modules = g_list_next(modules);
  }
  // also add scale, x and y:
  const char *str = (const char *)&scale;
  for(int i=0;i<12;i++) hash = ((hash << 5) + hash) ^ str[i];
  return hash;
}

int dt_dev_pixelpipe_cache_available(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash)
{
  // search for hash in cache
  for(int k=0;k<cache->entries;k++) if(cache->hash[k] == hash) return 1;
  return 0;
}

int dt_dev_pixelpipe_cache_get(dt_dev_pixelpipe_cache_t *cache, const uint64_t hash, void **data)
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
      cache->used[k] = 0; // this is the MRU entry
    }
  }

  if(!*data)
  { // kill LRU entry
    *data = cache->data[max];
    cache->used[max] = 0;
    return 1;
  }
  else return 0;
}
