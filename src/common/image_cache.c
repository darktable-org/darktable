
#include "common/darktable.h"
#include "common/image_cache.h"

#include <stdlib.h>
#include <stdio.h>

void dt_image_cache_init(dt_image_cache_t *cache, int32_t entries)
{
  pthread_mutex_init(&(cache->mutex), NULL);
  cache->num_lines = entries;
  cache->line = (dt_image_cache_line_t *)malloc(sizeof(dt_image_cache_line_t)*cache->num_lines);
  cache->by_id = (int16_t *)malloc(sizeof(int16_t)*cache->num_lines);
  for(int k=0;k<cache->num_lines;k++)
  {
    cache->by_id[k] = k;
    dt_image_init(&(cache->line[k].image));
    cache->line[k].lock.users = cache->line[k].lock.write = 0;
    cache->line[k].image.cacheline = k;
    cache->line[k].lru = k-1;
    cache->line[k].mru = k+1;
  }
  cache->lru = 0;
  cache->mru = cache->num_lines-1;
}

void dt_image_cache_cleanup(dt_image_cache_t *cache)
{
  // free mipmap cache lines
  for(int k=0;k<cache->num_lines;k++)
  {
    // TODO: update images set !!
    dt_image_cleanup(&(cache->line[k].image));
  }
  free(cache->line);
  free(cache->by_id);
  pthread_mutex_destroy(&(cache->mutex));
}

int32_t dt_image_cache_bsearch(const int32_t id)
{
  dt_image_cache_t *cache = darktable.image_cache;
  unsigned int min = 0, max = cache->num_lines;
  unsigned int t = max/2;
  while (t != min)
  {
    if(cache->line[cache->by_id[t-1]].image.id < id) min = t;
    else max = t;
    t = (min + max)/2;
  }
  if(cache->line[cache->by_id[t]].image.id != id) return -1;
  return cache->by_id[t];
}

int dt_image_cache_compare_id(const int16_t *l1, const int16_t *l2)
{
  return darktable.image_cache->line[*l1].image.id - darktable.image_cache->line[*l2].image.id;
}

dt_image_t *dt_image_cache_get(int32_t id, const char mode)
{
  dt_image_t *img = dt_image_cache_use(id, mode);
  if(img == NULL) return NULL;
  if(img->film_id == -1) if(dt_image_open2(img, id))
  {
    dt_image_cache_release(img, mode);
    return NULL;
  }
  return img;
}

dt_image_t *dt_image_cache_use(int32_t id, const char mode)
{
  dt_image_cache_t *cache = darktable.image_cache;
  pthread_mutex_lock(&(cache->mutex));
  // int16_t *res = bsearch(&id, cache->by_id, cache->num_lines, sizeof(int16_t), (int(*)(const void *, const void *))&dt_image_cache_compare_id);
  int32_t res = dt_image_cache_bsearch(id);
  dt_image_t *ret = NULL;
  int16_t k = cache->lru;
  if(res < 0)
  {
    // get least recently used image without lock and replace it:
    for(int i=0;i<cache->num_lines;i++)
    {
      if(cache->line[k].image.id == -1) break;
      if(cache->line[k].lock.write == 0 && cache->line[k].lock.users == 0) break;
      k = cache->line[k].mru;
    }
    if(k == cache->num_lines)
    {
      fprintf(stderr, "[image_cache_use] all slots are in use!\n");
      pthread_mutex_unlock(&(cache->mutex));
      return NULL;
    }
    // TODO: update images set !!
    dt_image_cleanup(&(cache->line[k].image));
    dt_image_init(&(cache->line[k].image));
    cache->line[k].image.id = id;
    cache->line[k].image.cacheline = k;
    cache->line[k].image.film_id = -1;
    // TODO: insertion sort faster here?
    qsort(cache->by_id, cache->num_lines, sizeof(int16_t), (int(*)(const void *, const void *))&dt_image_cache_compare_id);
    res = k;
  }
  if(cache->line[res].lock.write)
  {
    ret = NULL;
  }
  else
  {
    // update lock
    cache->line[res].lock.users++;
    if(mode == 'w') cache->line[res].lock.write = 1;
    ret = &(cache->line[res].image);
  }
  // update least recently used/most recently used linked list:
  cache->line[cache->mru].mru = res;
  if(cache->line[res].lru >= 0)                cache->line[cache->line[res].lru].mru = cache->line[res].mru;
  if(cache->line[res].mru < cache->num_lines)  cache->line[cache->line[res].mru].lru = cache->line[res].lru;
  if(cache->lru == res) cache->lru = cache->line[res].mru;
  cache->line[res].mru = cache->num_lines+1;
  cache->line[res].lru = cache->mru;
  cache->mru = res;
  pthread_mutex_unlock(&(cache->mutex));
  return ret;
}

void dt_image_cache_release(dt_image_t *img, const char mode)
{
  dt_image_cache_t *cache = darktable.image_cache;
  pthread_mutex_lock(&(cache->mutex));
  cache->line[img->cacheline].lock.users--;
  if(mode == 'w') cache->line[img->cacheline].lock.write = 0;
  pthread_mutex_unlock(&(cache->mutex));
}

