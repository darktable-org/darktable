#ifndef DT_IMAGE_H
#define DT_IMAGE_H

#include <inttypes.h>
#include <pthread.h>

// how large would the average screen be (largest mip map size) ?
#define DT_IMAGE_WINDOW_SIZE 1200

typedef enum
{
  DT_IMAGE_DELETE = 1,
  DT_IMAGE_OKAY = 2,
  DT_IMAGE_NICE = 3,
  DT_IMAGE_EXCELLENT = 4,
  DT_IMAGE_THUMBNAIL = 16
}
dt_image_flags_t;

typedef enum
{
  DT_IMAGE_MIP0 = 0,
  DT_IMAGE_MIP1 = 1,
  DT_IMAGE_MIP2 = 2,
  DT_IMAGE_MIP3 = 3,
  DT_IMAGE_MIP4 = 4,
  DT_IMAGE_MIPF = 5,
  DT_IMAGE_FULL = 6,
  DT_IMAGE_NONE = 7
}
dt_image_buffer_t;

typedef struct dt_image_lock_t
{
  unsigned write : 1;
  unsigned users : 7;
}
dt_image_lock_t;

// __attribute__ ((aligned (128)))
typedef struct dt_image_t
{
  // minimal exif data here (all in multiples of 4-byte to interface nicely with c++):
  int32_t orientation;
  float exif_exposure;
  float exif_aperture;
  float exif_iso;
  float exif_focal_length;
  char exif_maker[32];
  char exif_model[32];
  char exif_lens[52];
  char exif_datetime_taken[20];
  char filename[512];
  // common stuff
  int32_t width, height, output_width, output_height;
  // used by library
  int32_t num, flags, film_id, id;
  // cache
  int32_t cacheline; // for image_cache
  uint8_t *mip[DT_IMAGE_MIPF]; // for mipmap_cache
  float *mipf;
  dt_image_lock_t lock[DT_IMAGE_NONE];
  // raw image
  int32_t shrink, wb_auto, wb_cam;
  float exposure;
  float *pixels;
#ifdef _DEBUG
  int32_t mip_buf_size[DT_IMAGE_NONE];
#endif
}
dt_image_t;

// image buffer operations:
/** inits basic values to sensible defaults. */
void dt_image_init(dt_image_t *img);
/** returns the full path name where the image was imported from. */
void dt_image_full_path(dt_image_t *img, char *pathname, int len);
/** opens an image with minimal storage from the data base and stores it in image cache. */ 
int dt_image_open(const int32_t id);
int dt_image_open2(dt_image_t *img, const int32_t id);
/** imports a new image from raw/etc file and adds it to the data base and image cache. */
int dt_image_import(const int32_t film_id, const char *filename);
/** cleanup. */
void dt_image_cleanup(dt_image_t *img);
/** loads the requested buffer to cache, with read lock set. */
int dt_image_load(dt_image_t *img, dt_image_buffer_t mip);
/** prefetches given image buffer (mip map level/float preview/full raw), without marking it as used. */
void dt_image_prefetch(dt_image_t *img, dt_image_buffer_t mip);
/** returns appropriate mip map size for given area to paint on (width, height). */
dt_image_buffer_t dt_image_get_matching_mip_size(const dt_image_t *img, const int32_t width, const int32_t height, int32_t *w, int32_t *h);
/** returns appropriate mip map size for given mip level. */
void dt_image_get_mip_size(const dt_image_t *img, dt_image_buffer_t mip, int32_t *w, int32_t *h);
/** returns real image extends within mip map buffer size, in floating point. */
void dt_image_get_exact_mip_size(const dt_image_t *img, dt_image_buffer_t mip, float *w, float *h);
/** writes mip4 through to all smaller levels. */
int dt_image_update_mipmaps(dt_image_t *img);

// memory management interface
typedef struct dt_mipmap_cache_t
{
  pthread_mutex_t mutex;
  int32_t num_entries[DT_IMAGE_NONE];
  dt_image_t **mip_lru[DT_IMAGE_NONE];
}
dt_mipmap_cache_t;

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache, int32_t entries);
void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache);
/** print some cache statistics. */
void dt_mipmap_cache_print(dt_mipmap_cache_t *cache);

/** if in debug mode, asserts image buffer size for mip is alloc'ed this large. */
void dt_image_check_buffer(dt_image_t *image, dt_image_buffer_t mip, int32_t size);
/** alloc new buffer for this mip map and image. also lock for writing. */
int dt_image_alloc(dt_image_t *img, dt_image_buffer_t mip);
/** destroy buffer. */
void dt_image_free(dt_image_t *img, dt_image_buffer_t mip);
/** gets the requested image buffer or a smaller preview if it is not available (w lock || =NULL), marking this with the read lock. returns found mip level. */
dt_image_buffer_t dt_image_get(dt_image_t *img, const dt_image_buffer_t mip, const char mode);
/** unflags the used flag of given mip map level. these remove r and w locks, respectively. dropping the w lock will leave the r lock in place. */
void dt_image_release(dt_image_t *img, dt_image_buffer_t mip, const char mode);
/** locks the given mode if the buffer is available. returns non-zero and does nothing else on failure (no async loading is scheduled). */
int dt_image_lock_if_available(dt_image_t *img, const dt_image_buffer_t mip_in, const char mode);

/** converts img->pixels to img->mipf to img->mip[4--0]. needs full image buffer r locked. */
int dt_image_raw_to_preview(dt_image_t *img);
/** up-converts mip4 to mipf using guessed gamma values. needs mip4 r locked. */
int dt_image_preview_to_raw(dt_image_t *img);
#endif
