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

#include "common/darktable.h"
#include "common/exif.h"
#include "common/grealpath.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/imageio_jpeg.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "libraw/libraw.h"
#ifdef HAVE_SQUISH
#include "squish/csquish.h"
#endif

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <limits.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <xmmintrin.h>

#define DT_MIPMAP_CACHE_FILE_MAGIC 0xD71337
#define DT_MIPMAP_CACHE_FILE_VERSION 23
#define DT_MIPMAP_CACHE_DEFAULT_FILE_NAME "mipmaps"

#define DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE (1<<0)

struct dt_mipmap_buffer_dsc
{
  uint32_t width;
  uint32_t height;
  uint32_t size;
  uint32_t flags;
  /* NB: sizeof must be a multiple of 4*sizeof(float) */
}  __attribute__((packed));

// last resort mem alloc for dead images. sizeof(dt_mipmap_buffer_dsc) + dead image pixels (8x8)
// __m128 type for sse alignment.
static __m128 dt_mipmap_cache_static_dead_image[1 + 64];

static inline void
dead_image_8(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)buf->buf - 1;
  dsc->width = dsc->height = 8;
  assert(dsc->size > 64*sizeof(uint32_t));
  const uint32_t X = 0xffffffffu;
  const uint32_t o = 0u;
  const uint32_t image[] =
  {
    o, o, o, o, o, o, o, o,
    o, o, X, X, X, X, o, o,
    o, X, o, X, X, o, X, o,
    o, X, X, X, X, X, X, o,
    o, o, X, o, o, X, o, o,
    o, o, o, o, o, o, o, o,
    o, o, X, X, X, X, o, o,
    o, o, o, o, o, o, o, o
  };
  memcpy(buf->buf, image, sizeof(uint32_t)*64);
}

static inline void
dead_image_f(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return;
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)buf->buf - 1;
  dsc->width = dsc->height = 8;
  assert(dsc->size > 64*4*sizeof(float));
  const __m128 X = _mm_set1_ps(1.0f);
  const __m128 o = _mm_set1_ps(0.0f);
  const __m128 image[] =
  {
    o, o, o, o, o, o, o, o,
    o, o, X, X, X, X, o, o,
    o, X, o, X, X, o, X, o,
    o, X, X, X, X, X, X, o,
    o, o, X, o, o, X, o, o,
    o, o, o, o, o, o, o, o,
    o, o, X, X, X, X, o, o,
    o, o, o, o, o, o, o, o
  };
  memcpy(buf->buf, image, sizeof(__m128)*64);
}

static inline int32_t
compressed_buffer_size(const int32_t compression_type, const int width, const int height)
{
  if(width <= 8 && height <= 8)
    // skulls are uncompressed
    return 8*8*sizeof(uint32_t);
  else if(compression_type)
    // need 8 byte for each 4x4 block of pixels.
    // round correctly, so a 3x3 image will still consume one block:
    return ((width-1)/4 + 1) * ((height-1)/4 + 1) * 8;
  else // uncompressed:
    return width*height*sizeof(uint32_t);
}

static inline int32_t
buffer_is_broken(dt_mipmap_buffer_t *buf)
{
  if(!buf->buf) return 0;
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)buf->buf - 1;
  if(buf->width  != dsc->width) return 1;
  if(buf->height != dsc->height) return 2;
  // somewhat loose bound:
  if(buf->width*buf->height > dsc->size) return 3;
  return 0;
}

static inline uint32_t
get_key(const uint32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^29 (~500 million images)
  return (((uint32_t)size) << 29) | (imgid-1);
}

static inline uint32_t
get_imgid(const uint32_t key)
{
  return (key & 0x1fffffff) + 1;
}

static inline dt_mipmap_size_t
get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 29);
}

typedef struct _iterate_data_t
{
  FILE *f;
  uint8_t *blob;
  int compression_type;
  dt_mipmap_size_t mip;
}
_iterate_data_t;

static int
_write_buffer(const uint32_t key, const void *data, void *user_data)
{
  if(!data) return 1;
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)data;
  // too small to write. no error, but don't write.
  if(dsc->width <= 8 && dsc->height <= 8) return 0;

  _iterate_data_t *d = (_iterate_data_t *)user_data;
  int written = fwrite(&(d->mip), sizeof(dt_mipmap_size_t), 1, d->f);
  if(written != 1) return 1;
  written = fwrite(&key, sizeof(uint32_t), 1, d->f);
  if(written != 1) return 1;

  if(d->compression_type)
  {
    // write buffer size, wd, ht and the full blob, as it is in memory.
    const int32_t length = compressed_buffer_size(d->compression_type, dsc->width, dsc->height);
    written = fwrite(&length, sizeof(int32_t), 1, d->f);
    if(written != 1) return 1;
    written = fwrite(&dsc->width, sizeof(int32_t), 1, d->f);
    if(written != 1) return 1;
    written = fwrite(&dsc->height, sizeof(int32_t), 1, d->f);
    if(written != 1) return 1;
    written = fwrite(dsc+1, sizeof(uint8_t), length, d->f);
    if(written != length) return 1;
  }
  else
  {
    dt_mipmap_buffer_t buf;
    buf.width  = dsc->width;
    buf.height = dsc->height;
    buf.imgid  = get_imgid(key);
    buf.size   = get_size(key);
    // skip to next 8-byte alignment, for sse buffers.
    buf.buf    = (uint8_t *)(dsc+1);

    const int cache_quality = dt_conf_get_int("database_cache_quality");
    const int32_t length = dt_imageio_jpeg_compress(buf.buf, d->blob, buf.width, buf.height, MIN(100, MAX(10, cache_quality)));
    written = fwrite(&length, sizeof(int32_t), 1, d->f);
    if(written != 1) return 1;
    written = fwrite(d->blob, sizeof(uint8_t), length, d->f);
    if(written != length) return 1;
  }

  return 0;
}

static int
dt_mipmap_cache_get_filename(
  gchar* mipmapfilename, size_t size)
{
  int r = -1;
  char* abspath = NULL;

  // Directory
  char cachedir[DT_MAX_PATH_LEN];
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Build the mipmap filename
  const gchar *dbfilename = dt_database_get_path(darktable.db);
  if (!strcmp(dbfilename, ":memory:"))
  {
    snprintf(mipmapfilename, size, "%s", dbfilename);
    r = 0;
    goto exit;
  }

  abspath = g_realpath(dbfilename);
  if(!abspath)
    abspath = g_strdup(dbfilename);

  GChecksum* chk = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(chk, (guchar*)abspath, strlen(abspath));
  const gchar *filename = g_checksum_get_string(chk);

  if(!filename || filename[0] == '\0')
    snprintf(mipmapfilename, size, "%s/%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME);
  else
    snprintf(mipmapfilename, size, "%s/%s-%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME, filename);

  g_checksum_free(chk);
  r = 0;

exit:
  g_free(abspath);

  return r;
}

static int
dt_mipmap_cache_serialize(dt_mipmap_cache_t *cache)
{
  gchar dbfilename[DT_MAX_PATH_LEN];
  if (dt_mipmap_cache_get_filename(dbfilename, sizeof(dbfilename)))
  {
    fprintf(stderr, "[mipmap_cache] could not retrieve cache filename; not serializing\n");
    return 1;
  }
  if (!strcmp(dbfilename, ":memory:"))
  {
    // fprintf(stderr, "[mipmap_cache] library is in memory; not serializing\n");
    return 0;
  }

  // only store smallest thumbs.
  const dt_mipmap_size_t mip = DT_MIPMAP_2;

  _iterate_data_t d;
  d.f = NULL;
  d.blob = (uint8_t *)malloc(cache->mip[mip].buffer_size);
  int written = 0;
  FILE *f = fopen(dbfilename, "wb");
  if(!f) goto write_error;
  d.f = f;
  // fprintf(stderr, "[mipmap_cache] serializing to `%s'\n", dbfilename);

  // write version info:
  const int32_t magic = DT_MIPMAP_CACHE_FILE_MAGIC + DT_MIPMAP_CACHE_FILE_VERSION;
  written = fwrite(&magic, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;

  // store compression type
  written = fwrite(&cache->compression_type, sizeof(int32_t), 1, f);
  if(written != 1) goto write_error;

  for(int i=0; i<=mip; i++)
  {
    // print max sizes for this cache
    written = fwrite(&cache->mip[i].max_width, sizeof(int32_t), 1, f);
    if(written != 1) goto write_error;
    written = fwrite(&cache->mip[i].max_height, sizeof(int32_t), 1, f);
    if(written != 1) goto write_error;
  }

  for(int i=0; i<=mip; i++)
  {
    d.mip = (dt_mipmap_size_t)i;
    d.compression_type = cache->compression_type;
    if(dt_cache_for_all(&cache->mip[i].cache, _write_buffer, &d)) goto write_error;
  }

  free(d.blob);
  fclose(f);
  return 0;

write_error:
  fprintf(stderr, "[mipmap_cache] serialization to `%s' failed!\n", dbfilename);
  if(f) fclose(f);
  free(d.blob);
  return 1;
}

static int
dt_mipmap_cache_deserialize(dt_mipmap_cache_t *cache)
{
  int32_t rd = 0;
  const dt_mipmap_size_t mip = DT_MIPMAP_2;
  uint8_t *blob = NULL;
  FILE *f = NULL;
  int file_width[mip+1], file_height[mip+1];

  gchar dbfilename[DT_MAX_PATH_LEN];
  if (dt_mipmap_cache_get_filename(dbfilename, sizeof(dbfilename)))
  {
    fprintf(stderr, "[mipmap_cache] could not retrieve cache filename; not deserializing\n");
    return 1;
  }
  if (!strcmp(dbfilename, ":memory:"))
  {
    // fprintf(stderr, "[mipmap_cache] library is in memory; not deserializing\n");
    return 0;
  }

  // drop any old cache if the database is new. in that case newly imported images will probably mapped to old thumbnails
  if(dt_database_is_new(darktable.db) && g_file_test(dbfilename, G_FILE_TEST_IS_REGULAR))
  {
    fprintf(stderr, "[mipmap_cache] database is new, dropping old cache `%s'\n", dbfilename);
    goto read_finalize;
  }

  f = fopen(dbfilename, "rb");
  if(!f)
  {
    if (errno == ENOENT)
    {
      fprintf(stderr, "[mipmap_cache] cache is empty, file `%s' doesn't exist\n", dbfilename);
    }
    else
    {
      fprintf(stderr, "[mipmap_cache] failed to open the cache from `%s'\n", dbfilename);
    }
    goto read_finalize;
  }

  // read version info:
  const int32_t magic = DT_MIPMAP_CACHE_FILE_MAGIC + DT_MIPMAP_CACHE_FILE_VERSION;
  int32_t magic_file = 0;
  rd = fread(&magic_file, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(magic_file == DT_MIPMAP_CACHE_FILE_MAGIC + 22)
  {
    // same format, but compression was broken in 22 and below
  }
  else if(magic_file != magic)
  {
    if(magic_file > DT_MIPMAP_CACHE_FILE_MAGIC && magic_file < magic)
      fprintf(stderr, "[mipmap_cache] cache version too old, dropping `%s' cache\n", dbfilename);
    else
      fprintf(stderr, "[mipmap_cache] invalid cache file, dropping `%s' cache\n", dbfilename);
    goto read_finalize;
  }

  // also read compression type and yell out on missmatch.
  int32_t compression = -1;
  rd = fread(&compression, sizeof(int32_t), 1, f);
  if(rd != 1) goto read_error;
  if(compression != cache->compression_type)
  {
    fprintf(stderr, "[mipmap_cache] cache is %s, but settings say we should use %s, dropping `%s' cache\n",
            compression == 0 ? "uncompressed" : (compression == 1 ? "low quality compressed" : "high quality compressed"),
            cache->compression_type == 0 ? "no compression" : (cache->compression_type == 1 ? "low quality compression" : "high quality compression"),
            dbfilename);
    goto read_finalize;
  }
  if(compression && (magic_file == DT_MIPMAP_CACHE_FILE_MAGIC + 22))
  {
    // compression is enabled and we have the affected version. can't read that.
    fprintf(stderr, "[mipmap_cache] dropping compressed cache v22 to regenerate mips without artifacts.\n");
    goto read_finalize;
  }

  for (int i=0; i<=mip; i++)
  {
    rd = fread(&file_width[i], sizeof(int32_t), 1, f);
    if(rd != 1) goto read_error;
    rd = fread(&file_height[i], sizeof(int32_t), 1, f);
    if(rd != 1) goto read_error;
    if(file_width[i]  != cache->mip[i].max_width ||
        file_height[i] != cache->mip[i].max_height)
    {
      fprintf(stderr, "[mipmap_cache] cache settings changed, dropping `%s' cache\n", dbfilename);
      goto read_finalize;
    }
  }

  if(cache->compression_type) blob = NULL;
  else blob = malloc(sizeof(uint32_t)*file_width[mip]*file_height[mip]);

  while(!feof(f))
  {
    int level = 0;
    rd = fread(&level, sizeof(int), 1, f);
    if (level > mip) break;

    int32_t key = 0;
    rd = fread(&key, sizeof(int32_t), 1, f);
    if(rd != 1) break; // first value is break only, goes to eof.
    int32_t length = 0;
    rd = fread(&length, sizeof(int32_t), 1, f);
    if(rd != 1) goto read_error;

    uint8_t *data = (uint8_t *)dt_cache_read_get(&cache->mip[level].cache, key);
    struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)data;
    if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE)
    {
      if(cache->compression_type)
      {
        int32_t wd, ht;
        rd = fread(&wd, sizeof(int32_t), 1, f);
        if(rd != 1) goto read_error;
        rd = fread(&ht, sizeof(int32_t), 1, f);
        if(rd != 1) goto read_error;
        dsc->width = wd;
        dsc->height = ht;
        if(length != compressed_buffer_size(cache->compression_type, wd, ht)) goto read_error;
        // directly read from disk into cache:
        rd = fread(data + sizeof(*dsc), 1, length, f);
        if(rd != length) goto read_error;
      }
      else
      {
        // jpg too large?
        if(length > sizeof(uint32_t)*file_width[mip]*file_height[mip]) goto read_error;
        rd = fread(blob, sizeof(uint8_t), length, f);
        if(rd != length) goto read_error;
        // no compression, the image is still compressed on disk, as jpg
        dt_imageio_jpeg_t jpg;
        if(dt_imageio_jpeg_decompress_header(blob, length, &jpg) ||
            (jpg.width > file_width[level] || jpg.height > file_height[level]) ||
            dt_imageio_jpeg_decompress(&jpg, data+sizeof(*dsc)))
        {
          fprintf(stderr, "[mipmap_cache] failed to decompress thumbnail for image %d!\n", get_imgid(key));
        }
        dsc->width = jpg.width;
        dsc->height = jpg.height;
      }
      dsc->flags &= ~DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
      // these come write locked in case idata[3] == 1, so release that!
      dt_cache_write_release(&cache->mip[level].cache, key);
    }
    dt_cache_read_release(&cache->mip[level].cache, key);
  }

  fclose(f);
  free(blob);
  return 0;

read_error:
  fprintf(stderr, "[mipmap_cache] failed to recover the cache from `%s'\n", dbfilename);
read_finalize:
  if(f) fclose(f);
  free(blob);
  g_unlink(dbfilename);
  return 1;
}

static void _init_f(float   *buf, uint32_t *width, uint32_t *height, const uint32_t imgid);
static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, const uint32_t imgid, const dt_mipmap_size_t size);

static int32_t
scratchmem_allocate(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  dt_mipmap_cache_one_t *c = (dt_mipmap_cache_one_t *)data;
  // slot is exactly aligned with encapsulated cache's position and already allocated
  *cost = c->buffer_size;
  return 0;
}

int32_t
dt_mipmap_cache_allocate(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  dt_mipmap_cache_one_t *c = (dt_mipmap_cache_one_t *)data;
  // slot is exactly aligned with encapsulated cache's position and already allocated
  *cost = c->buffer_size;
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)*buf;
  // set width and height:
  dsc->width = c->max_width;
  dsc->height = c->max_height;
  dsc->size = c->buffer_size;
  dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;

  // fprintf(stderr, "[mipmap cache alloc] slot %d/%d for imgid %d size %d buffer size %d (%p)\n", slot, c->cache.bucket_mask+1, get_imgid(key), get_size(key), c->buffer_size, *buf);
  return 1;
}

#if 0
void
dt_mipmap_cache_deallocate(void *data, const uint32_t key, void *payload)
{
  // nothing. memory is only allocated once.
  // TODO: overwrite buffer with not-found image?
}
#endif


// callback for the imageio core to allocate memory.
// only needed for _F and _FULL buffers, as they change size
// with the input image. will allocate img->width*img->height*img->bpp bytes.
void*
dt_mipmap_cache_alloc(dt_image_t *img, dt_mipmap_size_t size, dt_mipmap_cache_allocator_t a)
{
  assert(size == DT_MIPMAP_FULL);

  struct dt_mipmap_buffer_dsc** dsc = (struct dt_mipmap_buffer_dsc**)a;

  int32_t wd = img->width;
  int32_t ht = img->height;
  int32_t bpp = img->bpp;
  const uint32_t buffer_size =
    ((wd*ht*bpp) + sizeof(**dsc));

  // buf might have been alloc'ed before,
  // so only check size and re-alloc if necessary:
  if(!(*dsc) || ((*dsc)->size < buffer_size) || ((void *)*dsc == (void *)dt_mipmap_cache_static_dead_image))
  {
    if((void *)*dsc != (void *)dt_mipmap_cache_static_dead_image)
      dt_free_align(*dsc);
    *dsc = dt_alloc_align(64, buffer_size);
    // fprintf(stderr, "[mipmap cache] alloc for key %u %p\n", get_key(img->id, size), *buf);
    if(!(*dsc))
    {
      // return fallback: at least alloc size for a dead image:
      *dsc = (struct dt_mipmap_buffer_dsc *)dt_mipmap_cache_static_dead_image;
      // allocator holds the pointer. but imageio client is tricked to believe allocation failed:
      return NULL;
    }
    // set buffer size only if we're making it larger.
    (*dsc)->size = buffer_size;
  }
  (*dsc)->width = wd;
  (*dsc)->height = ht;
  (*dsc)->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;

  // fprintf(stderr, "full buffer allocating img %u %d x %d = %u bytes (%p)\n", img->id, img->width, img->height, buffer_size, *buf);

  // trick the user into using a pointer without the header:
  return (*dsc)+1;
}

// callback for the cache backend to initialize payload pointers
int32_t
dt_mipmap_cache_allocate_dynamic(void *data, const uint32_t key, int32_t *cost, void **buf)
{
  dt_mipmap_cache_one_t *cache = (dt_mipmap_cache_one_t *)data;
  // for full image buffers
  struct dt_mipmap_buffer_dsc* dsc = *buf;
  // alloc mere minimum for the header + broken image buffer:
  if(!dsc)
  {
    if(cache->size == DT_MIPMAP_F)
    {
      // these are fixed-size:
      *buf = dt_alloc_align(16, cache->buffer_size);
    }
    else
    {
      *buf = dt_alloc_align(16, sizeof(*dsc)+sizeof(float)*4*64);
    }
    // fprintf(stderr, "[mipmap cache] alloc dynamic for key %u %p\n", key, *buf);
    if(!(*buf))
    {
      fprintf(stderr, "[mipmap cache] memory allocation failed!\n");
      exit(1);
    }
    dsc = *buf;
    if(cache->size == DT_MIPMAP_F)
    {
      dsc->width = cache->max_width;
      dsc->height = cache->max_height;
      dsc->size = cache->buffer_size;
    }
    else
    {
      dsc->width = 0;
      dsc->height = 0;
      dsc->size = sizeof(*dsc)+sizeof(float)*4*64;
    }
  }
  assert(dsc->size >= sizeof(*dsc));
  dsc->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;

  // cost is just flat one for the buffer, as the buffers might have different sizes,
  // to make sure quota is meaningful.
  *cost = 1;
  // fprintf(stderr, "dummy allocing %p\n", *buf);
  return 1; // request write lock
}

void
dt_mipmap_cache_deallocate_dynamic(void *data, const uint32_t key, void *payload)
{
  dt_mipmap_cache_one_t *cache = (dt_mipmap_cache_one_t *)data;
  if(cache->size == DT_MIPMAP_F)
  {
    free(payload);
  }
  // else:
  // don't clean up anything, as we are re-allocating.
}

static uint32_t
nearest_power_of_two(const uint32_t value)
{
  uint32_t rc = 1;
  while(rc < value) rc <<= 1;
  return rc;
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  // make sure static memory is initialized
  struct dt_mipmap_buffer_dsc *dsc = (struct dt_mipmap_buffer_dsc *)dt_mipmap_cache_static_dead_image;
  dead_image_f((dt_mipmap_buffer_t *)(dsc+1));

  cache->compression_type = 0;
  gchar *compression = dt_conf_get_string("cache_compression");
  if(compression)
  {
    if(!strcmp(compression, "low quality (fast)"))
      cache->compression_type = 1;
    else if(!strcmp(compression, "high quality (slow)"))
      cache->compression_type = 2;
    g_free(compression);
  }

  dt_print(DT_DEBUG_CACHE, "[mipmap_cache_init] using %s\n", cache->compression_type == 0 ? "no compression" :
           (cache->compression_type == 1 ? "low quality compression" : "slow high quality compression"));

  // adjust numbers to be large enough to hold what mem limit suggests.
  // we want at least 100MB, and consider 8G just still reasonable.
  size_t max_mem = CLAMPS(dt_conf_get_int64("cache_memory"), 100u<<20, ((uint64_t)8)<<30);
  const uint32_t parallel = CLAMP(dt_conf_get_int ("worker_threads")*dt_conf_get_int("parallel_export"), 1, 8);
  const int32_t max_size = 2048, min_size = 32;
  int32_t wd = darktable.thumbnail_width;
  int32_t ht = darktable.thumbnail_height;
  wd = CLAMPS(wd, min_size, max_size);
  ht = CLAMPS(ht, min_size, max_size);
  // round up to a multiple of 8, so we can divide by two 3 times
  if(wd & 0xf) wd = (wd & ~0xf) + 0x10;
  if(ht & 0xf) ht = (ht & ~0xf) + 0x10;
  // cache these, can't change at runtime:
  cache->mip[DT_MIPMAP_F].max_width  = wd;
  cache->mip[DT_MIPMAP_F].max_height = ht;
  cache->mip[DT_MIPMAP_F-1].max_width  = wd;
  cache->mip[DT_MIPMAP_F-1].max_height = ht;
  for(int k=DT_MIPMAP_F-2; k>=DT_MIPMAP_0; k--)
  {
    cache->mip[k].max_width  = cache->mip[k+1].max_width  / 2;
    cache->mip[k].max_height = cache->mip[k+1].max_height / 2;
  }

  // initialize some per-thread cached scratchmem for uncompressed buffers during thumb creation:
  if(cache->compression_type)
  {
    cache->scratchmem.max_width = wd;
    cache->scratchmem.max_height = ht;
    cache->scratchmem.buffer_size = wd*ht*sizeof(uint32_t);
    cache->scratchmem.size = DT_MIPMAP_3; // at max.
    // TODO: use thread local storage instead (zero performance penalty on linux)
    dt_cache_init(&cache->scratchmem.cache, parallel, parallel, 64, 0.9f*parallel*wd*ht*sizeof(uint32_t));
    // might have been rounded to power of two:
    const int cnt = dt_cache_capacity(&cache->scratchmem.cache);
    cache->scratchmem.buf = dt_alloc_align(64, cnt * wd*ht*sizeof(uint32_t));
    dt_cache_static_allocation(&cache->scratchmem.cache, (uint8_t *)cache->scratchmem.buf, wd*ht*sizeof(uint32_t));
    dt_cache_set_allocate_callback(&cache->scratchmem.cache,
                                   scratchmem_allocate, &cache->scratchmem);
    dt_print(DT_DEBUG_CACHE,
             "[mipmap_cache_init] cache has % 5d entries for temporary compression buffers (% 4.02f MB).\n",
             cnt, cnt* wd*ht*sizeof(uint32_t)/(1024.0*1024.0));
  }

  for(int k=DT_MIPMAP_3; k>=0; k--)
  {
    // clear stats:
    cache->mip[k].stats_requests = 0;
    cache->mip[k].stats_near_match = 0;
    cache->mip[k].stats_misses = 0;
    cache->mip[k].stats_fetches = 0;
    cache->mip[k].stats_standin = 0;
    // buffer stores width and height + actual data
    const int width  = cache->mip[k].max_width;
    const int height = cache->mip[k].max_height;
    // header + adjusted for dxt compression:
    cache->mip[k].buffer_size = 4*sizeof(uint32_t) + compressed_buffer_size(cache->compression_type, width, height);
    cache->mip[k].size = k;
    // level of parallelism also gives minimum size (which is twice that)
    // is rounded to a power of two by the cache anyways, we might as well.
    // XXX this needs adjustment for video mode (more full-res thumbs for replay)
    // TODO: collect hit/miss stats and auto-adjust to user browsing behaviour
    // TODO: can #prefetches be collected this way, too?
    const size_t max_mem2 = MAX(0, (k == 0) ? (max_mem) : (max_mem/(k+4)));
    uint32_t thumbnails = MAX(2, nearest_power_of_two((uint32_t)((double)max_mem2/cache->mip[k].buffer_size)));
    while(thumbnails > parallel && (size_t)thumbnails * cache->mip[k].buffer_size > max_mem2) thumbnails /= 2;

    // try to utilize that memory well (use 90% quota), the hopscotch paper claims good scalability up to
    // even more than that.
    dt_cache_init(&cache->mip[k].cache, thumbnails,
                  parallel,
                  64, 0.9f*thumbnails*cache->mip[k].buffer_size);

    // might have been rounded to power of two:
    thumbnails = dt_cache_capacity(&cache->mip[k].cache);
    max_mem -= thumbnails * cache->mip[k].buffer_size;
    // dt_print(DT_DEBUG_CACHE, "[mipmap mem] %4.02f left\n", max_mem/(1024.0*1024.0));
    cache->mip[k].buf = dt_alloc_align(64, thumbnails * cache->mip[k].buffer_size);
    dt_cache_static_allocation(&cache->mip[k].cache, (uint8_t *)cache->mip[k].buf, cache->mip[k].buffer_size);
    dt_cache_set_allocate_callback(&cache->mip[k].cache,
                                   dt_mipmap_cache_allocate, &cache->mip[k]);
    // dt_cache_set_cleanup_callback(&cache->mip[k].cache,
    // &dt_mipmap_cache_deallocate, &cache->mip[k]);

    dt_print(DT_DEBUG_CACHE,
             "[mipmap_cache_init] cache has % 5d entries for mip %d (% 4.02f MB).\n",
             thumbnails, k, thumbnails * cache->mip[k].buffer_size/(1024.0*1024.0));
  }

  // full buffer needs dynamic alloc:
  const int full_entries = MAX(2, parallel); // even with one thread you want two buffers. one for dr one for thumbs.
  int32_t max_mem_bufs = nearest_power_of_two(full_entries);

  // for this buffer, because it can be very busy during import, we want the minimum
  // number of entries in the hashtable to be 16, but leave the quota as is. the dynamic
  // alloc/free properties of this cache take care that no more memory is required.
  dt_cache_init(&cache->mip[DT_MIPMAP_FULL].cache, max_mem_bufs, parallel, 64, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_FULL].cache,
                                 dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  // dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_FULL].cache,
  // &dt_mipmap_cache_deallocate_dynamic, &cache->mip[DT_MIPMAP_FULL]);
  cache->mip[DT_MIPMAP_FULL].buffer_size = 0;
  cache->mip[DT_MIPMAP_FULL].size = DT_MIPMAP_FULL;
  cache->mip[DT_MIPMAP_FULL].buf = NULL;

  // same for mipf:
  dt_cache_init(&cache->mip[DT_MIPMAP_F].cache, max_mem_bufs, parallel, 64, max_mem_bufs);
  dt_cache_set_allocate_callback(&cache->mip[DT_MIPMAP_F].cache,
                                 dt_mipmap_cache_allocate_dynamic, &cache->mip[DT_MIPMAP_F]);
  dt_cache_set_cleanup_callback(&cache->mip[DT_MIPMAP_F].cache,
                                dt_mipmap_cache_deallocate_dynamic, &cache->mip[DT_MIPMAP_F]);
  cache->mip[DT_MIPMAP_F].buffer_size = 4*sizeof(uint32_t) +
                                        4*sizeof(float) * cache->mip[DT_MIPMAP_F].max_width * cache->mip[DT_MIPMAP_F].max_height;
  cache->mip[DT_MIPMAP_F].size = DT_MIPMAP_F;
  cache->mip[DT_MIPMAP_F].buf = NULL;

  dt_mipmap_cache_deserialize(cache);
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  dt_mipmap_cache_serialize(cache);
  for(int k=0; k<DT_MIPMAP_F; k++)
  {
    dt_cache_cleanup(&cache->mip[k].cache);
    // now mem is actually freed, not during cache cleanup
    dt_free_align(cache->mip[k].buf);
  }
  dt_cache_cleanup(&cache->mip[DT_MIPMAP_FULL].cache);
  dt_cache_cleanup(&cache->mip[DT_MIPMAP_F].cache);

  // clean up temporary buffers for decompressed images, if any:
  if(cache->compression_type)
  {
    dt_cache_cleanup(&cache->scratchmem.cache);
    dt_free_align(cache->scratchmem.buf);
  }
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  for(int k=0; k<(int)DT_MIPMAP_F; k++)
  {
    printf("[mipmap_cache] level [i%d] (%4dx%4d) fill %.2f/%.2f MB (%.2f%% in %u/%u buffers)\n", k,
        cache->mip[k].max_width, cache->mip[k].max_height, cache->mip[k].cache.cost/(1024.0*1024.0),
           cache->mip[k].cache.cost_quota/(1024.0*1024.0),
           100.0f*(float)cache->mip[k].cache.cost/(float)cache->mip[k].cache.cost_quota,
           dt_cache_size(&cache->mip[k].cache),
           dt_cache_capacity(&cache->mip[k].cache));
  }
  for(int k=(int)DT_MIPMAP_F; k<=(int)DT_MIPMAP_FULL; k++)
  {
    printf("[mipmap_cache] level [f%d] fill %d/%d slots (%.2f%% in %u/%u buffers)\n", k,
           (uint32_t)cache->mip[k].cache.cost,
           (uint32_t)cache->mip[k].cache.cost_quota,
           100.0f*(float)cache->mip[k].cache.cost/(float)cache->mip[k].cache.cost_quota,
           dt_cache_size(&cache->mip[k].cache),
           dt_cache_capacity(&cache->mip[k].cache));
  }
  if(cache->compression_type)
  {
    printf("[mipmap_cache] scratch fill %.2f/%.2f MB (%.2f%% in %u/%u buffers)\n", cache->scratchmem.cache.cost/(1024.0*1024.0),
           cache->scratchmem.cache.cost_quota/(1024.0*1024.0),
           100.0f*(float)cache->scratchmem.cache.cost/(float)cache->scratchmem.cache.cost_quota,
           dt_cache_size(&cache->scratchmem.cache),
           dt_cache_capacity(&cache->scratchmem.cache));
  }
  uint64_t sum = 0;
  uint64_t sum_fetches = 0;
  uint64_t sum_standins = 0;
  for(int k=0; k<=(int)DT_MIPMAP_FULL; k++)
  {
    sum += cache->mip[k].stats_requests;
    sum_fetches += cache->mip[k].stats_fetches;
    sum_standins += cache->mip[k].stats_standin;
  }
  printf("[mipmap_cache] level | near match | miss | stand-in | fetches | total rq\n");
  for(int k=0; k<=(int)DT_MIPMAP_FULL; k++)
    printf("[mipmap_cache] %c%d    | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n", k > 3 ? 'f' : 'i', k,
        100.0*cache->mip[k].stats_near_match/(float)cache->mip[k].stats_requests,
        100.0*cache->mip[k].stats_misses/(float)cache->mip[k].stats_requests,
        100.0*cache->mip[k].stats_standin/(float)sum_standins,
        100.0*cache->mip[k].stats_fetches/(float)sum_fetches,
        100.0*cache->mip[k].stats_requests/(float)sum);
  printf("\n\n");
  // very verbose stats about locks/users
  //dt_cache_print(&cache->mip[DT_MIPMAP_3].cache);
}

void
dt_mipmap_cache_read_get(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf,
  const uint32_t imgid,
  const dt_mipmap_size_t mip,
  const dt_mipmap_get_flags_t flags)
{
  const uint32_t key = get_key(imgid, mip);
  if(flags == DT_MIPMAP_TESTLOCK)
  {
    // simple case: only get and lock if it's there.
    struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)dt_cache_read_testget(&cache->mip[mip].cache, key);
    if(dsc)
    {
      buf->width  = dsc->width;
      buf->height = dsc->height;
      buf->imgid  = imgid;
      buf->size   = mip;
      // skip to next 8-byte alignment, for sse buffers.
      buf->buf    = (uint8_t *)(dsc+1);
    }
    else
    {
      // set to NULL if failed.
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->size  = DT_MIPMAP_NONE;
      buf->buf   = NULL;
    }
  }
  else if(flags == DT_MIPMAP_PREFETCH)
  {
    // and opposite: prefetch without locking
    if(mip > DT_MIPMAP_FULL || mip < DT_MIPMAP_0) return;
    dt_job_t j;
    dt_image_load_job_init(&j, imgid, mip);
    // if the job already exists, make it high-priority, if not, add it:
    if(dt_control_revive_job(darktable.control, &j) < 0)
      dt_control_add_job(darktable.control, &j);
  }
  else if(flags == DT_MIPMAP_BLOCKING)
  {
    // simple case: blocking get
    struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)dt_cache_read_get(&cache->mip[mip].cache, key);
    if(!dsc)
    {
      // should never happen for anything but full images which have been moved.
      assert(mip == DT_MIPMAP_FULL || mip == DT_MIPMAP_F);
      // fprintf(stderr, "[mipmap cache get] no data in cache for imgid %u size %d!\n", imgid, mip);
      // sorry guys, no image for you :(
      buf->width = buf->height = 0;
      buf->imgid = 0;
      buf->size  = DT_MIPMAP_NONE;
      buf->buf   = NULL;
    }
    else
    {
      // fprintf(stderr, "[mipmap cache get] found data in cache for imgid %u size %d\n", imgid, mip);
      // uninitialized?
      //assert(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE || dsc->size == 0);
      if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE)
      {
        __sync_fetch_and_add (&(cache->mip[mip].stats_fetches), 1);
        // fprintf(stderr, "[mipmap cache get] now initializing buffer for img %u mip %d!\n", imgid, mip);
        // we're write locked here, as requested by the alloc callback.
        // now fill it with data:
        if(mip == DT_MIPMAP_FULL)
        {
          // load the image:
          // make sure we access the r/w lock as shortly as possible!
          dt_image_t buffered_image;
          const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
          buffered_image = *cimg;
          // dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
          // dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
          dt_image_cache_read_release(darktable.image_cache, cimg);

          char filename[DT_MAX_PATH_LEN];
          gboolean from_cache = TRUE;
          dt_image_full_path(buffered_image.id, filename, DT_MAX_PATH_LEN, &from_cache);

          dt_mipmap_cache_allocator_t a = (dt_mipmap_cache_allocator_t)&dsc;
          struct dt_mipmap_buffer_dsc* prvdsc = dsc;
          dt_imageio_retval_t ret = dt_imageio_open(&buffered_image, filename, a);
          if(dsc != prvdsc)
          {
            // fprintf(stderr, "[mipmap cache] realloc %p\n", data);
            // write back to cache, too.
            // in case something went wrong, still keep the buffer and return it to the hashtable
            // so we don't produce mem leaks or unnecessary mem fragmentation.
            dt_cache_realloc(&cache->mip[mip].cache, key, 1, (void*)dsc);
          }
          if(ret != DT_IMAGEIO_OK)
          {
            // fprintf(stderr, "[mipmap read get] error loading image: %d\n", ret);
            //
            // we can only return a zero dimension buffer if the buffer has been allocated.
            // in case dsc couldn't be allocated and points to the static buffer, it contains
            // a dead image already.
            if((void *)dsc != (void *)dt_mipmap_cache_static_dead_image) dsc->width = dsc->height = 0;
          }
          else
          {
            // swap back new image data:
            cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
            dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
            *img = buffered_image;
            // fprintf(stderr, "[mipmap read get] initializing full buffer img %u with %u %u -> %d %d (%p)\n", imgid, data[0], data[1], img->width, img->height, data);
            // don't write xmp for this (we only changed db stuff):
            dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
            dt_image_cache_read_release(darktable.image_cache, img);
          }
        }
        else if(mip == DT_MIPMAP_F)
        {
          _init_f((float *)(dsc+1), &dsc->width, &dsc->height, imgid);
        }
        else
        {
          // 8-bit thumbs, possibly need to be compressed:
          if(cache->compression_type)
          {
            // get per-thread temporary storage without malloc from a separate cache:
            const int key = dt_control_get_threadid();
            // const void *cbuf =
            dt_cache_read_get(&cache->scratchmem.cache, key);
            uint8_t *scratchmem = (uint8_t *)dt_cache_write_get(&cache->scratchmem.cache, key);
            _init_8(scratchmem, &dsc->width, &dsc->height, imgid, mip);
            buf->width  = dsc->width;
            buf->height = dsc->height;
            buf->imgid  = imgid;
            buf->size   = mip;
            buf->buf = (uint8_t *)(dsc+1);
            dt_mipmap_cache_compress(buf, scratchmem);
            dt_cache_write_release(&cache->scratchmem.cache, key);
            dt_cache_read_release(&cache->scratchmem.cache, key);
          }
          else
          {
            _init_8((uint8_t *)(dsc+1), &dsc->width, &dsc->height, imgid, mip);
          }
        }
        dsc->flags &= ~DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
        // drop the write lock
        dt_cache_write_release(&cache->mip[mip].cache, key);
        /* raise signal that mipmaps has been flushed to cache */
        dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED);
      }
      buf->width  = dsc->width;
      buf->height = dsc->height;
      buf->imgid  = imgid;
      buf->size   = mip;
      buf->buf = (uint8_t *)(dsc+1);
      if(dsc->width == 0 || dsc->height == 0)
      {
        // fprintf(stderr, "[mipmap cache get] got a zero-sized image for img %u mip %d!\n", imgid, mip);
        if(mip < DT_MIPMAP_F)       dead_image_8(buf);
        else if(mip == DT_MIPMAP_F) dead_image_f(buf);
        else buf->buf = NULL; // full images with NULL buffer have to be handled, indicates `missing image'
      }
    }
  }
  else if(flags == DT_MIPMAP_BEST_EFFORT)
  {
    __sync_fetch_and_add (&(cache->mip[mip].stats_requests), 1);
    // best-effort, might also return NULL.
    // never decrease mip level for float buffer or full image:
    dt_mipmap_size_t min_mip = (mip >= DT_MIPMAP_F) ? mip : DT_MIPMAP_0;
    for(int k=mip; k>=min_mip && k>=0; k--)
    {
      // already loaded?
      dt_mipmap_cache_read_get(cache, buf, imgid, k, DT_MIPMAP_TESTLOCK);
      if(buf->buf && buf->width > 0 && buf->height > 0)
      {
        if(mip != k) __sync_fetch_and_add (&(cache->mip[k].stats_standin), 1);
        return;
      }
      // didn't succeed the first time? prefetch for later!
      if(mip == k)
      {
        __sync_fetch_and_add (&(cache->mip[mip].stats_near_match), 1);
        dt_mipmap_cache_read_get(cache, buf, imgid, mip, DT_MIPMAP_PREFETCH);
      }
    }
    __sync_fetch_and_add (&(cache->mip[mip].stats_misses), 1);
    // fprintf(stderr, "[mipmap cache get] image not found in cache: imgid %u mip %d!\n", imgid, mip);
    // nothing found :(
    buf->buf   = NULL;
    buf->imgid = 0;
    buf->size  = DT_MIPMAP_NONE;
    buf->width = buf->height = 0;
  }
}

void
dt_mipmap_cache_write_get(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf)
{
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  // simple case: blocking write get
  struct dt_mipmap_buffer_dsc* dsc = (struct dt_mipmap_buffer_dsc*)dt_cache_write_get(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
  buf->width  = dsc->width;
  buf->height = dsc->height;
  buf->buf    = (uint8_t *)(dsc+1);
  // these have already been set in read_get
  // buf->imgid  = imgid;
  // buf->size   = mip;
}

void
dt_mipmap_cache_read_release(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf)
{
  if(buf->size == DT_MIPMAP_NONE) return;
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  dt_cache_read_release(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
  buf->size = DT_MIPMAP_NONE;
  buf->buf  = NULL;
}

// drop a write lock, read will still remain.
void
dt_mipmap_cache_write_release(
  dt_mipmap_cache_t *cache,
  dt_mipmap_buffer_t *buf)
{
  if(buf->size == DT_MIPMAP_NONE || buf->buf == NULL) return;
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size <  DT_MIPMAP_NONE);
  dt_cache_write_release(&cache->mip[buf->size].cache, get_key(buf->imgid, buf->size));
  buf->size = DT_MIPMAP_NONE;
  buf->buf  = NULL;
}



// return the closest mipmap size
dt_mipmap_size_t
dt_mipmap_cache_get_matching_size(
  const dt_mipmap_cache_t *cache,
  const int32_t width,
  const int32_t height)
{
  // find `best' match to width and height.
  int32_t error = 0x7fffffff;
  dt_mipmap_size_t best = DT_MIPMAP_NONE;
  for(int k=DT_MIPMAP_0; k<DT_MIPMAP_F; k++)
  {
    // find closest l1 norm:
    int32_t new_error = cache->mip[k].max_width + cache->mip[k].max_height
                        - width - height;
    // and allow the first one to be larger in pixel size to override the smaller mip
    if(abs(new_error) < abs(error) || (error < 0 && new_error > 0))
    {
      best = k;
      error = new_error;
    }
  }
  return best;
}

void
dt_mipmap_cache_remove(
  dt_mipmap_cache_t *cache,
  const uint32_t imgid)
{
  // get rid of all ldr thumbnails:
  for(int k=DT_MIPMAP_0; k<DT_MIPMAP_F; k++)
  {
    const uint32_t key = get_key(imgid, k);
    dt_cache_remove(&cache->mip[k].cache, key);
  }
}

static void
_init_f(
  float          *out,
  uint32_t       *width,
  uint32_t       *height,
  const uint32_t  imgid)
{
  const uint32_t wd = *width, ht = *height;

  /* do not even try to process file if it isn't available */
  char filename[2048] = {0};
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid, filename, 2048, &from_cache);
  if (strlen(filename) == 0 || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    return;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING);

  // lock image after we have the buffer, we might need to lock the image struct for
  // writing during raw loading, to write to width/height.
  const dt_image_t *image = dt_image_cache_read_get(darktable.image_cache, imgid);

  dt_iop_roi_t roi_in, roi_out;
  roi_in.x = roi_in.y = 0;
  roi_in.width = image->width;
  roi_in.height = image->height;
  roi_in.scale = 1.0f;

  roi_out.x = roi_out.y = 0;
  roi_out.scale = fminf(wd/(float)image->width, ht/(float)image->height);
  roi_out.width  = roi_out.scale * roi_in.width;
  roi_out.height = roi_out.scale * roi_in.height;

  if(!buf.buf)
  {
    dt_control_log(_("image `%s' is not available!"), image->filename);
    dt_image_cache_read_release(darktable.image_cache, image);
    *width = *height = 0;
    return;
  }

  assert(!buffer_is_broken(&buf));

  if(image->filters)
  {
    // demosaic during downsample
    if(image->bpp == sizeof(float))
      dt_iop_clip_and_zoom_demosaic_half_size_f(
        out, (const float *)buf.buf,
        &roi_out, &roi_in, roi_out.width, roi_in.width,
        dt_image_flipped_filter(image), 1.0f);
    else
      dt_iop_clip_and_zoom_demosaic_half_size(
        out, (const uint16_t *)buf.buf,
        &roi_out, &roi_in, roi_out.width, roi_in.width,
        dt_image_flipped_filter(image));
  }
  else
  {
    // downsample
    dt_iop_clip_and_zoom(out, (const float *)buf.buf,
                         &roi_out, &roi_in, roi_out.width, roi_in.width);
  }
  dt_image_cache_read_release(darktable.image_cache, image);
  dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);

  *width  = roi_out.width;
  *height = roi_out.height;
}


// dummy functions for `export' to mipmap buffers:
typedef struct _dummy_data_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
}
_dummy_data_t;

static int
_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static int
_bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int
_write_image(
  dt_imageio_module_data_t *data,
  const char               *filename,
  const void               *in,
  void                     *exif,
  int                       exif_len,
  int                       imgid)
{
  _dummy_data_t *d = (_dummy_data_t *)data;
  memcpy(d->buf, in, data->width*data->height*sizeof(uint32_t));
  return 0;
}

static void
_init_8(
  uint8_t                *buf,
  uint32_t               *width,
  uint32_t               *height,
  const uint32_t          imgid,
  const dt_mipmap_size_t  size)
{
  const uint32_t wd = *width, ht = *height;
  char filename[DT_MAX_PATH_LEN] = {0};
  gboolean from_cache = TRUE;

  /* do not even try to process file if it isnt available */
  dt_image_full_path(imgid, filename, DT_MAX_PATH_LEN, &from_cache);
  if (strlen(filename) == 0 || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    return;
  }

  const int altered = dt_image_altered(imgid);
  int res = 1;

  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
  const int orientation = dt_image_orientation(cimg);
  // the orientation for this camera is not read correctly from exiv2, so we need
  // to go the full libraw path (as the thumbnail will be flipped the wrong way round)
  const int incompatible = !strncmp(cimg->exif_maker, "Phase One", 9);
  dt_image_cache_read_release(darktable.image_cache, cimg);


  // first try exif thumbnail, that's smaller and thus faster to load:
  if(!altered && !dt_conf_get_bool("never_use_embedded_thumb") &&
      !dt_exif_thumbnail(filename, buf, wd, ht, orientation, width, height))
  {
    res = 0;
  }
  else if(!altered && !dt_conf_get_bool("never_use_embedded_thumb") && !incompatible)
  {
    // try to load the embedded thumbnail in raw
    gboolean from_cache = TRUE;
    memset(filename, 0, DT_MAX_PATH_LEN);
    dt_image_full_path(imgid, filename, DT_MAX_PATH_LEN, &from_cache);

    const char *c = filename + strlen(filename);
    while(*c != '.' && c > filename) c--;
    if(!strcasecmp(c, ".jpg"))
    {
      // try to load jpg
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        uint8_t *tmp = (uint8_t *)malloc(sizeof(uint8_t)*jpg.width*jpg.height*4);
        if(!dt_imageio_jpeg_read(&jpg, tmp))
        {
          // scale to fit
          dt_iop_flip_and_zoom_8(tmp, jpg.width, jpg.height, buf, wd, ht, orientation, width, height);
          res = 0;
        }
        free(tmp);
      }
    }
    else
    {
      uint8_t *tmp = 0;
      int32_t thumb_width, thumb_height, orientation;
      res = dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, &orientation);
      if(!res)
      {
        // scale to fit
        dt_iop_flip_and_zoom_8(tmp, thumb_width, thumb_height, buf, wd, ht, orientation, width, height);
        free(tmp);
      }
    }
  }

  if(res)
  {
    // try the real thing: rawspeed + pixelpipe
    dt_imageio_module_format_t format;
    _dummy_data_t dat;
    format.bpp = _bpp;
    format.write_image = _write_image;
    format.levels = _levels;
    dat.head.max_width  = wd;
    dat.head.max_height = ht;
    dat.buf = buf;
    // export with flags: ignore exif (don't load from disk), don't swap byte order, don't do hq processing, and signal we want thumbnail export
    res = dt_imageio_export_with_flags(imgid, "unused", &format, (dt_imageio_module_data_t *)&dat, 1, 1, 0, 1, NULL,FALSE,NULL,NULL);
    if(!res)
    {
      // might be smaller, or have a different aspect than what we got as input.
      *width  = dat.head.width;
      *height = dat.head.height;
    }
  }

  // fprintf(stderr, "[mipmap init 8] export image %u finished (sizes %d %d => %d %d)!\n", imgid, wd, ht, dat.head.width, dat.head.height);

  // any errors?
  if(res)
  {
    // fprintf(stderr, "[mipmap_cache] could not process thumbnail!\n");
    *width = *height = 0;
    return;
  }

  // TODO: various speed optimizations:
  // TODO: also init all smaller mips!
  // TODO: use mipf, but:
  // TODO: if output is cropped, don't use mipf!
}

// compression stuff: alloc a buffer if needed
uint8_t*
dt_mipmap_cache_alloc_scratchmem(
  const dt_mipmap_cache_t *cache)
{
  const size_t size = cache->mip[DT_MIPMAP_3].max_width *
                      cache->mip[DT_MIPMAP_3].max_height;

  if(cache->compression_type)
  {
    return dt_alloc_align(64, size * 4 * sizeof(uint8_t));
  }
  else // no compression, no buffer:
    return NULL;
}

// decompress the raw mipmapm buffer into the scratchmemory.
// returns a pointer to the decompressed memory block. that's because
// for uncompressed settings, it will point directly to the mipmap
// buffer and scratchmem can be NULL.
uint8_t*
dt_mipmap_cache_decompress(
  const dt_mipmap_buffer_t *buf,
  uint8_t *scratchmem)
{
#ifdef HAVE_SQUISH
  if(darktable.mipmap_cache->compression_type && buf->width > 8 && buf->height > 8)
  {
    squish_decompress_image(scratchmem, buf->width, buf->height, buf->buf, squish_dxt1);
    return scratchmem;
  }
  else
#endif
  {
    return buf->buf;
  }
}

// writes the scratchmem buffer to compressed
// format into the mipmap cache. does nothing
// if compression is disabled.
void
dt_mipmap_cache_compress(
  dt_mipmap_buffer_t *buf,
  uint8_t *const scratchmem)
{
#ifdef HAVE_SQUISH
  // only do something if compression is on, don't compress skulls:
  if(darktable.mipmap_cache->compression_type && buf->width > 8 && buf->height > 8)
  {
    int flags = squish_dxt1;
    // low quality:
    if(darktable.mipmap_cache->compression_type == 1) flags |= squish_colour_range_fit;
    squish_compress_image(scratchmem, buf->width, buf->height, buf->buf, squish_dxt1);
  }
#endif
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
