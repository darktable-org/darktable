/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/image_cache.h"
#include "control/conf.h"
#include "common/imageio_jpeg.h"
#include "common/image_compression.h"
#include "common/debug.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <string.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <errno.h>

#define DT_IMAGE_CACHE_FILE_MAGIC 0xD71337
#define DT_IMAGE_CACHE_FILE_VERSION 5
#define DT_IMAGE_CACHE_FILE_NAME "mipmaps"

int dt_image_cache_check_consistency(dt_image_cache_t *cache)
{
#if 1//def _DEBUG
  int i = cache->lru;
  if(cache->line[i].lru != -1) return 1;
  int num = 1;
  for(int k=0; k<cache->num_lines; k++)
  {
    i = cache->line[i].mru;
    if(i >= cache->num_lines || i < 0) printf("line %d got next %d/%d\n", k, i, cache->num_lines);
    if(i >= cache->num_lines) return 2;
    if(i < 0) return 3;
    num ++;
    if(cache->line[i].image.cacheline != i) return 4;
    // printf("next lru: `%s'\n", cache->line[i].image.filename);
    if(i == cache->mru) break;
  }
  if(num != cache->num_lines) return 5;
  i = cache->mru;
  if(cache->line[i].mru != cache->num_lines) return 6;
  num = 1;
  for(int k=0; k<cache->num_lines; k++)
  {
    i = cache->line[i].lru;
    if(i >= cache->num_lines || i < 0) printf("line %d got next %d/%d\n", k, i, cache->num_lines);
    if(i >= cache->num_lines) return 7;
    if(i < 0) return 8;
    num ++;
    if(cache->line[i].image.cacheline != i) return 9;
    // printf("next mru: `%s'\n", cache->line[i].image.filename);
    if(i == cache->lru) break;
  }
  if(num != cache->num_lines) return 10;
  return 0;
#else
  return 0;
#endif
}

void dt_image_cache_write(dt_image_cache_t *cache)
{
  dt_pthread_mutex_lock(&(cache->mutex));
  if(dt_image_cache_check_consistency(cache))
  {
    // consistency check. if failed, don't write!
    fprintf(stderr, "[image_cache_write] refusing to write corrupted cache.\n");
    dt_pthread_mutex_unlock(&(cache->mutex));
    return;
  }

  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir(cachedir,1024);
  gchar *filename = dt_conf_get_string("cachefile");

  if(!filename || filename[0] == '\0') snprintf(dbfilename, 512, "%s/%s", cachedir,DT_IMAGE_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf(dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf(dbfilename, 512, "%s", filename);
  g_free(filename);

  int written = 0;
  FILE *f = fopen(dbfilename, "wb");
  if(!f) goto write_error;

  // write version info:
  const int32_t magic = DT_IMAGE_CACHE_FILE_MAGIC + DT_IMAGE_CACHE_FILE_VERSION;
  written = fwrite(&magic, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;
  written = fwrite(&darktable.thumbnail_size, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;

  // dump all cache metadata:
  written = fwrite(&(cache->num_lines), sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;
  written = fwrite(&(cache->lru), sizeof(int16_t), 1, f);
  if(written != 1) goto write_error;
  written = fwrite(&(cache->mru), sizeof(int16_t), 1, f);
  if(written != 1) goto write_error;
  written = fwrite(cache->by_id, sizeof(int16_t), cache->num_lines, f);
  if(written != cache->num_lines) goto write_error;

  for(int k=0; k<cache->num_lines; k++)
  {
    // for all images
    int wd, ht;
    dt_image_cache_line_t line;
    dt_image_t *img;
    line = cache->line[k];
    line.lock.users = line.lock.write = 0;
    img = &(cache->line[k].image);
    line.image.pixels = NULL;
    // line.image.exif_inited = 0;
    for(int i=0; i<DT_IMAGE_NONE; i++)
    {
      line.image.lock[i].users = line.image.lock[i].write = 0;
      line.image.mip_buf_size[i] = 0;
    }
    for(int mip=0; mip<DT_IMAGE_MIPF; mip++)
    {
      line.image.mip[mip] = line.image.mip[mip]?(uint8_t*)1:NULL;
      dt_image_get_mip_size(img, mip, &wd, &ht);
      if(wd <= 32 || ht <= 32) line.image.mip[mip] = NULL;
    }
#ifdef DT_IMAGE_CACHE_WRITE_MIPF
    line.image.mipf = line.image.mipf?(float *)1:NULL;
#else
    line.image.mipf = NULL;
#endif
    line.image.import_lock = line.image.force_reimport = 0;
    written = fwrite(&line, sizeof(dt_image_cache_line_t), 1, f);
    if(written != 1) goto write_error;

    for(int mip=0; mip<DT_IMAGE_MIPF; mip++)
    {
      if(!line.image.mip[mip]) continue;
      // printf("writing mip %d for image %d\n", mip, img->id);
      // dump all existing mip[..] in jpeg
      dt_image_get_mip_size(img, mip, &wd, &ht);
      dt_image_check_buffer(img, mip, 4*wd*ht*sizeof(uint8_t));
      uint8_t *blob = (uint8_t *)malloc(4*sizeof(uint8_t)*wd*ht);
      int32_t length = dt_imageio_jpeg_compress(img->mip[mip], blob, wd, ht, MIN(100, MAX(10, dt_conf_get_int("database_cache_quality"))));
      written = fwrite(&length, sizeof(int32_t), 1, f);
      if(written != 1)
      {
        free(blob);
        goto write_error;
      }
      written = fwrite(blob, sizeof(uint8_t), length, f);
      if(written != length)
      {
        free(blob);
        goto write_error;
      }
      free(blob);
    }
    // dump mipf in dct
    if(line.image.mipf)
    {
      dt_image_get_mip_size(img, DT_IMAGE_MIPF, &wd, &ht);
      dt_image_check_buffer(img, DT_IMAGE_MIPF, 3*wd*ht*sizeof(float));
      uint8_t *buf = (uint8_t *)malloc(sizeof(uint8_t)*wd*ht);
      dt_image_compress(img->mipf, buf, wd, ht);
      int32_t length = wd*ht;
      written = fwrite(&length, sizeof(int32_t), 1, f);
      if(written != 1)
      {
        free(buf);
        goto write_error;
      }
      written = fwrite(buf, sizeof(uint8_t), length, f);
      if(written != length)
      {
        free(buf);
        goto write_error;
      }
      free(buf);
    }
  }
  // write marker at the end
  int32_t endmarker = DT_IMAGE_CACHE_FILE_MAGIC;
  written = fwrite(&endmarker, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;
  fclose(f);
  dt_pthread_mutex_unlock(&(cache->mutex));
  return;

write_error:
  if(f) fclose(f);
  fprintf(stderr, "[image_cache_write] failed to dump the cache to `%s'\n", dbfilename);
  g_unlink(dbfilename);
  dt_pthread_mutex_unlock(&(cache->mutex));
}

int dt_image_cache_read(dt_image_cache_t *cache)
{
  dt_pthread_mutex_lock(&(cache->mutex));
  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir (cachedir,1024);
  gchar *filename = dt_conf_get_string ("cachefile");
  if(!filename || filename[0] == '\0') snprintf (dbfilename, 512, "%s/%s", cachedir, DT_IMAGE_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf (dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf (dbfilename, 512, "%s", filename);
  g_free(filename);

  FILE *f = fopen(dbfilename, "rb");
  if(!f) 
  {
    if (errno == ENOENT)
    {
      fprintf(stderr, "[image_cache_read] cache is empty, file `%s' doesn't exist\n", dbfilename);
    }
    else
    {
      fprintf(stderr, "[image_cache_read] failed to open the cache from `%s'\n", dbfilename);
    }
    goto read_finalize;
  }

  int32_t num = 0, rd = 0;

  // read version info:
  const int32_t magic = DT_IMAGE_CACHE_FILE_MAGIC + DT_IMAGE_CACHE_FILE_VERSION;
  int32_t magic_file = 0;
  rd = fread(&magic_file, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(magic_file != magic)
  {
    if(magic_file > DT_IMAGE_CACHE_FILE_MAGIC && magic_file < magic)
        fprintf(stderr, "[image_cache_read] cache version too old, dropping `%s' cache\n", dbfilename);
    else
        fprintf(stderr, "[image_cache_read] invalid cache file, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }
  rd = fread(&magic_file, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(magic_file != darktable.thumbnail_size) 
  {
    fprintf(stderr, "[image_cache_read] cache settings changed, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }

  // read metadata:
  rd = fread(&num, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(cache->num_lines != num) 
  {
    fprintf(stderr, "[image_cache_read] cache settings changed, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }
  rd = fread(&num, sizeof(int16_t), 1, f);
  if(rd != 1) goto read_error;
  cache->lru = num;
  rd = fread(&num, sizeof(int16_t), 1, f);
  if(rd != 1) goto read_error;
  cache->mru = num;
  rd = fread(cache->by_id, sizeof(int16_t), cache->num_lines, f);
  if(rd != cache->num_lines) goto read_error;

  // printf("read cache with %d lines, mru %d lru %d\n", cache->num_lines, cache->mru, cache->lru);

  // read cache lines (images)
  for(int k=0; k<cache->num_lines; k++)
  {
    dt_image_t *image = &(cache->line[k].image);
    rd = fread(cache->line+k, sizeof(dt_image_cache_line_t), 1, f);
    if(rd != 1) goto read_error;
    cache->line[k].image.import_lock = cache->line[k].image.force_reimport = 0;

    // printf("read image `%s' from disk cache\n", image->filename);

    int wd, ht;
    for(int mip=0; mip<DT_IMAGE_MIPF; mip++)
    {
      // read all available mips
      if(!image->mip[mip]) continue;
      image->mip[mip] = NULL;
      // printf("reading mip %d for image %d\n", mip, image->id);
      dt_image_get_mip_size(image, mip, &wd, &ht);
      uint8_t *blob = (uint8_t *)malloc(4*sizeof(uint8_t)*wd*ht);
      int32_t length = 0;
      rd = fread(&length, sizeof(int32_t), 1, f);
      if(rd != 1 || length > 4*sizeof(uint8_t)*wd*ht)
      {
        free(blob);
        goto read_error;
      }
      rd = fread(blob, sizeof(uint8_t), length, f);
      if(rd != length)
      {
        free(blob);
        goto read_error;
      }
      if(!dt_image_alloc(image, mip))
      {
        dt_image_check_buffer(image, mip, 4*wd*ht*sizeof(uint8_t));
        dt_imageio_jpeg_t jpg;
        if(dt_imageio_jpeg_decompress_header(blob, length, &jpg) ||
            (jpg.width != wd || jpg.height != ht) ||
            dt_imageio_jpeg_decompress(&jpg, image->mip[mip]))
        {
          fprintf(stderr, "[image_cache_read] failed to decompress thumbnail!\n");
        }
        dt_image_release(image, mip, 'w');
        dt_image_release(image, mip, 'r');
      }
      free(blob);
    }
    if(image->mipf)
    {
      // read float preview
      image->mipf = NULL;
      dt_image_get_mip_size(image, DT_IMAGE_MIPF, &wd, &ht);
      uint8_t *buf = (uint8_t *)malloc(sizeof(uint8_t)*wd*ht);
      int32_t length = wd*ht;
      rd = fread(&length, sizeof(int32_t), 1, f);
      if(rd != 1 || length != wd*ht)
      {
        free(buf);
        goto read_error;
      }
      rd = fread(buf, sizeof(uint8_t), length, f);
      if(rd != length)
      {
        free(buf);
        goto read_error;
      }
      if(!dt_image_alloc(image, DT_IMAGE_MIPF))
      {
        dt_image_check_buffer(image, DT_IMAGE_MIPF, 3*wd*ht*sizeof(float));
        dt_image_uncompress((uint8_t *)buf, image->mipf, wd, ht);
        dt_image_release(image, DT_IMAGE_MIPF, 'w');
        dt_image_release(image, DT_IMAGE_MIPF, 'r');
      }
      free(buf);
    }
    image->dirty = 0; // This is reset when flushing the cache on exit, but writing happens before flushing ...
  }
  int32_t endmarker = 0xD71337, readmarker = 0;
  rd = fread(&readmarker, sizeof(uint32_t), 1, f);
  if(rd != 1 || readmarker != endmarker) goto read_error;
  fclose(f);
  dt_pthread_mutex_unlock(&(cache->mutex));
  return 0;

read_error:
  fprintf(stderr, "[image_cache_read] failed to recover the cache from `%s'\n", dbfilename);
read_finalize:
  if(f) fclose(f);
  g_unlink(dbfilename);
  dt_pthread_mutex_unlock(&(cache->mutex));
  return 1;
}

/* copy file from src to dest, overwrites destination */
static void _image_cache_copy_file (gchar *src,gchar *dest)
{
  int bs = 4*1024*1024;
  gchar *block = g_malloc (bs);
  int sh,dh,b;
  if ((sh = open (src,O_RDONLY)) != -1)
  {
    if ((dh = open (dest,O_CREAT|O_TRUNC|O_WRONLY,S_IRUSR|S_IWUSR)) != -1)
    {
      while ((b=read(sh,block,bs))>0)
        b=write (dh,block,b);
      close (dh);
    }
    close (sh);
  }
  g_free (block);
}

static void _image_cache_backup()
{
  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir (cachedir,1024);
  gchar *filename = dt_conf_get_string ("cachefile");

  if(!filename || filename[0] == '\0') snprintf (dbfilename, 1024, "%s/%s", cachedir, DT_IMAGE_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf (dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf (dbfilename, 512, "%s", filename);
  g_free(filename);

  char *src = g_strdup (dbfilename);
  g_strlcat(dbfilename,".fallback", 1024);
  _image_cache_copy_file (src,dbfilename);
  g_free (src);
}

static void _image_cache_restore()
{
  char cachedir[1024];
  char dbfilename[1024];
  dt_util_get_user_cache_dir(cachedir,1024);
  gchar *filename = dt_conf_get_string ("cachefile");

  if(!filename || filename[0] == '\0') snprintf (dbfilename, 512, "%s/%s", cachedir,DT_IMAGE_CACHE_FILE_NAME);
  else if(filename[0] != '/')          snprintf (dbfilename, 512, "%s/%s", cachedir, filename);
  else                                 snprintf (dbfilename, 512, "%s", filename);
  g_free(filename);

  char *dest = g_strdup (dbfilename);
  g_strlcat(dbfilename,".fallback", 1024);
  _image_cache_copy_file (dbfilename,dest);
  g_free (dest);
}


void dt_image_cache_init(dt_image_cache_t *cache, int32_t entries, const int32_t load_cached)
{
  dt_pthread_mutex_init(&(cache->mutex), NULL);
  cache->num_lines = entries;
  cache->line = (dt_image_cache_line_t *)malloc(sizeof(dt_image_cache_line_t)*cache->num_lines);
  memset(cache->line,0,sizeof(dt_image_cache_line_t)*cache->num_lines);

  cache->by_id = (int16_t *)malloc(sizeof(int16_t)*cache->num_lines);
  for(int k=0; k<cache->num_lines; k++)
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
  if(load_cached!=0)
  {
    gboolean cb = dt_conf_get_bool ("cachefile_backup");

    if (dt_image_cache_read(cache))
    {
      // the cache failed to load, the file has been
      // deleted.
      // reset to useful values:

      dt_image_cache_cleanup(cache);

      /* restore cachefile backup if first failure */
      if (cb && load_cached==1)
        _image_cache_restore();

      /* lets try to reinit */
      dt_image_cache_init(cache, entries, (cb && load_cached==1)?2:0 );
    }
    else
    {
      /* backup cachefile */
      if (cb && load_cached==1)
        _image_cache_backup();

    }
  }
}

void dt_image_cache_cleanup(dt_image_cache_t *cache)
{
  dt_image_cache_write(cache);
  // free mipmap cache lines
  for(int k=0; k<cache->num_lines; k++)
  {
    dt_image_cache_flush_no_sidecars(&(cache->line[k].image));
    dt_image_cleanup(&(cache->line[k].image));
  }
  free(cache->line);
  cache->line = NULL;
  free(cache->by_id);
  cache->by_id = NULL;
  dt_pthread_mutex_destroy(&(cache->mutex));
}

// return position of cache index in cache->by_id
int16_t dt_image_cache_bsearch(const int32_t id)
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
  return t;
}

dt_image_t *dt_image_cache_get(int32_t id, const char mode)
{
  dt_image_t *img = dt_image_cache_get_uninited(id, mode);
  if(img == NULL) return NULL;
  if(img->film_id == -1) if(dt_image_open2(img, id))
    {
      dt_image_cache_release(img, mode);
      return NULL;
    }
  // printf("[image_cache_get] %ld id %d\n", (long int)img, img->id);
  return img;
}

void dt_image_cache_clear(int32_t id)
{
  dt_image_cache_t *cache = darktable.image_cache;
  dt_pthread_mutex_lock(&(cache->mutex));

  int16_t pos = dt_image_cache_bsearch(id);

  if (pos < 0)
  {
    dt_pthread_mutex_unlock(&(cache->mutex));
    return;
  }

  // we going to assign -1 to id for this cache entry, so fix by_id sorting
  int32_t cache_line = cache->by_id[pos];
  memmove(cache->by_id+1,cache->by_id,pos*sizeof(int16_t));
  cache->by_id[0] = cache_line;

  if(!cache->line[cache_line].lock.write && !cache->line[cache_line].lock.users)
  {
    // clean out mipmaps
    dt_image_cleanup(&(cache->line[cache_line].image));
    // also clean up metadata etc.
    dt_image_init(&(cache->line[cache_line].image));
  }
  // if still locked, at least invalidate the data.
  {
    cache->line[cache_line].image.film_id = -1;
    cache->line[cache_line].image.id = -1;

  }
  dt_pthread_mutex_unlock(&(cache->mutex));
}

dt_image_t *dt_image_cache_get_uninited(int32_t id, const char mode)
{
  // printf("[image_cache_get_uninited] locking image %d %s\n", id, mode == 'w' ? "for writing" : "");
  dt_image_cache_t *cache = darktable.image_cache;
  dt_pthread_mutex_lock(&(cache->mutex));
#ifdef _DEBUG
  if(dt_image_cache_check_consistency(cache))
    fprintf(stderr, "[image_cache_get_uninited] cache is corrupted!\n");
#endif
  
  dt_image_t *ret = NULL;
  int16_t k = cache->lru;

  int16_t pos = dt_image_cache_bsearch(id);
  int32_t cache_line;

  if (pos >= 0)
  {
    cache_line = cache->by_id[pos];
  }
  else
  {
    // get least recently used image without lock and replace it:
    for(int i=0; i<cache->num_lines; i++)
    {
      if(cache->line[k].image.id == -1) break;
      if(cache->line[k].lock.write == 0 && cache->line[k].lock.users == 0)
      {
        // in case image buffers have not been released correctly, do it now:
        for(int i=0; i<DT_IMAGE_NONE; i++) cache->line[k].image.lock[i].users = cache->line[k].image.lock[i].write = 0;
        break;
      }
      k = cache->line[k].mru;
    }
    if(k == cache->num_lines)
    {
      fprintf(stderr, "[image_cache_get_uninited] all %d slots are in use!\n", cache->num_lines);
      dt_pthread_mutex_unlock(&(cache->mutex));
      return NULL;
    }
    cache_line = k;

    int16_t oldpos = 0;
    // find the old position of cacheline in the by_id index
    if (cache->line[cache_line].image.id < 0)
    {
      // can't use bsearch as we may have multiple -1 entries
      while (oldpos < cache->num_lines && cache->by_id[oldpos] != cache_line) oldpos++;
    }
    else
    {
      oldpos = dt_image_cache_bsearch(cache->line[cache_line].image.id);
    }

    if (oldpos < 0 || cache->by_id[oldpos] != cache_line)
    {
      // should never happen, unless cache is broken
      fprintf(stderr, "[image_cache_get_uninited] cache inconsistency found\n");
      dt_pthread_mutex_unlock(&(cache->mutex));
      return NULL;
    }

    // fix by_id sorting, this is faster then sorting everything again
    if (cache->line[cache->by_id[0]].image.id > id)
    {
      // if new id should be in the beginning
      memmove(cache->by_id+1,cache->by_id,oldpos*sizeof(int16_t));
      cache->by_id[0] = cache_line;
    }
    else if (cache->line[cache->by_id[cache->num_lines-1]].image.id < id)
    {
      // if new id should be in the end
      memmove(cache->by_id+oldpos,cache->by_id+oldpos+1,(cache->num_lines-oldpos-1)*sizeof(int16_t));
      cache->by_id[cache->num_lines-1] = cache_line;
    }
    else if (oldpos > 0 && cache->line[cache->by_id[oldpos-1]].image.id > id)
    {
      // if new id should be in the middle and before old position
      unsigned int min = 0, max = oldpos-1;
      unsigned int pos = max/2;
      while (pos != min)
      {
        if(cache->line[cache->by_id[pos]].image.id < id) min = pos;
        else max = pos;
        pos = (min + max)/2;
      }
      pos++;
      memmove(cache->by_id+pos+1,cache->by_id+pos,(oldpos-pos)*sizeof(int16_t));
      cache->by_id[pos] = cache_line;
    }
    else if (oldpos < cache->num_lines - 1 && cache->line[cache->by_id[oldpos+1]].image.id < id)
    {
      // if new id should be in the middle and after old position
      unsigned int min = oldpos+1, max = cache->num_lines-1;
      unsigned int pos = (min + max)/2;
      while (pos != min)
      {
        if(cache->line[cache->by_id[pos]].image.id < id) min = pos;
        else max = pos;
        pos = (min + max)/2;
      }
      memmove(cache->by_id+oldpos,cache->by_id+oldpos+1,(pos-oldpos)*sizeof(int16_t));
      cache->by_id[pos] = cache_line;
    }
    // otherwise do nothing, sorting is ok

    // data/sidecar is flushed at each change for data safety, so no need to write xmp here:
    dt_image_cache_flush_no_sidecars(&(cache->line[cache_line].image));
    dt_image_cleanup(&(cache->line[cache_line].image));
    dt_image_init(&(cache->line[cache_line].image));
    cache->line[cache_line].image.id = id;
    cache->line[cache_line].image.cacheline = cache_line;
    cache->line[cache_line].image.film_id = -1;
  }

  if(cache->line[cache_line].lock.write)
  {
    ret = NULL;
  }
  else
  {
    // update lock
    cache->line[cache_line].lock.users++;
    if(mode == 'w') cache->line[cache_line].lock.write = 1;
    ret = &(cache->line[cache_line].image);
  }
  // update least recently used/most recently used linked list:
  // new top:
  if(cache->mru != cache_line)
  {
    // mru next pointer is end marker, but we are not already stored as cache->mru ???
    g_assert(cache->line[cache_line].mru != cache->num_lines);
    // fill gap:
    if(cache->line[cache_line].lru >= 0)
      cache->line[cache->line[cache_line].lru].mru = cache->line[cache_line].mru;
    cache->line[cache->line[cache_line].mru].lru = cache->line[cache_line].lru;

    if(cache->lru == cache_line) cache->lru = cache->line[cache_line].mru;
    cache->line[cache->mru].mru = cache_line;
    cache->line[cache_line].mru = cache->num_lines;
    cache->line[cache_line].lru = cache->mru;
    cache->mru = cache_line;
  }
#ifdef _DEBUG
  if(dt_image_cache_check_consistency(cache))
    fprintf(stderr, "[image_cache_get_uninited] cache is corrupted!\n");
#endif
  dt_pthread_mutex_unlock(&(cache->mutex));
  return ret;
}

void dt_image_cache_release(dt_image_t *img, const char mode)
{
  if(!img) return;
  dt_image_cache_t *cache = darktable.image_cache;
  dt_pthread_mutex_lock(&(cache->mutex));
  cache->line[img->cacheline].lock.users--;
  if(mode == 'w') cache->line[img->cacheline].lock.write = 0;
  dt_pthread_mutex_unlock(&(cache->mutex));
}

void dt_image_cache_print(dt_image_cache_t *cache)
{
  int users = 0, write = 0, entries = 0;
  for(int k=0; k<cache->num_lines; k++)
  {
    if(cache->line[k].image.id == -1) continue;
    entries++;
    users += cache->line[k].lock.users;
    write += cache->line[k].lock.write;
  }
  printf("image cache: fill: %d/%d, users: %d, writers: %d\n", entries, cache->num_lines, users, write);
}

void dt_image_cache_flush_no_sidecars(dt_image_t *img)
{
  if(img->id <= 0 || img->dirty == 0) return;
  img->dirty = 0;
  int rc;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "update images set width = ?1, height = ?2, maker = ?3, model = ?4, lens = ?5, exposure = ?6, aperture = ?7, iso = ?8, focal_length = ?9, focus_distance = ?10, film_id = ?11, datetime_taken = ?12, flags = ?13, output_width = ?14, output_height = ?15, crop = ?16, raw_parameters = ?17, raw_denoise_threshold = ?18, raw_auto_bright_threshold = ?19, raw_black = ?20, raw_maximum = ?21, orientation = ?22 where id = ?23", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, img->width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, img->height);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, img->exif_maker, strlen(img->exif_maker), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, img->exif_model, strlen(img->exif_model), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, img->exif_lens,  strlen(img->exif_lens),  SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, img->exif_exposure);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, img->exif_aperture);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, img->exif_iso);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, img->exif_focal_length);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, img->exif_focus_distance);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, img->film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 12, img->exif_datetime_taken, strlen(img->exif_datetime_taken), SQLITE_STATIC);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 13, img->flags);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 14, img->output_width);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 15, img->output_height);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 16, img->exif_crop);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 17, *(int32_t *)&(img->raw_params));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 18, img->raw_denoise_threshold);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 19, img->raw_auto_bright_threshold);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 20, img->black);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 21, img->maximum);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 22, img->orientation);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 23, img->id);
  rc = sqlite3_step(stmt);
  if (rc != SQLITE_DONE) fprintf(stderr, "[image_cache_flush] sqlite3 error %d\n", rc);
  sqlite3_finalize(stmt);
}

void dt_image_cache_flush(dt_image_t *img)
{
  // assert(0);
  if(img->id <= 0) return;
  dt_image_cache_flush_no_sidecars(img);
  // also synch dttags file:
  dt_image_write_sidecar_file(img->id);
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
