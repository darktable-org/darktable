#ifndef DARKTABLE_DEVELOP_H
#define DARKTABLE_DEVELOP_H

#include "common/imageio.h"
#include "library/library.h"
#include "control/settings.h"
#include "develop/imageop.h"

#include <cairo.h>
#include <pthread.h>
#ifdef DT_USE_GEGL
#include <gegl.h>
#include <glib.h>

extern uint8_t dt_dev_default_gamma[0x10000];
extern float dt_dev_de_gamma[0x100];

struct dt_iop_module_t;
struct dt_iop_params_t;
typedef struct dt_dev_history_item_t
{
  // dt_dev_operation_t op;          // which operation
  struct dt_iop_module_t *module; // pointer to image operation module
  int32_t enabled;                // switched on/off
  struct dt_iop_params_t *params; // parameters for this operation
}
dt_dev_history_item_t;

struct dt_dev_pixelpipe_t;
typedef struct dt_develop_t
{
  int32_t gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be gui_init'ed.
  int32_t image_loading, image_processing, image_dirty;
  int32_t preview_loading, preview_processing, preview_dirty;

  pthread_mutex_t backbuf_mutex;
  // width, height: dimensions of window
  // capwidth, capheight: actual dimensions of scaled image inside window.
  int32_t width, height, backbuf_size, backbuf_preview_size, capwidth, capheight, capwidth_preview, capheight_preview;
  uint8_t *backbuf, *backbuf_preview;

  // graph for gegl
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe;

  // image under consideration.
  dt_image_t *image;
  int32_t mipf_width, mipf_height;
  float   mipf_exact_width, mipf_exact_height;

  // history stack
  pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;
  // operations pipeline
  int32_t iop_instance;
  GList *iop;

  // histogram for display.
  uint32_t *histogram, *histogram_pre;
  uint32_t histogram_max, histogram_pre_max;
  uint8_t gamma[0x100];
}
dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img);
// TODO: replace these by setting `loading' flag and trigering gegl_process 
void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev, struct dt_image_t *img);

void dt_dev_add_history_item(dt_develop_t *dev, struct dt_iop_module_t *module);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history(dt_develop_t *dev);

void dt_dev_invalidate(dt_develop_t *dev);
void dt_dev_set_gamma(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);

gboolean dt_dev_configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data);

void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxw, float *boxh);

struct dt_job_t;
void dt_dev_export(struct dt_job_t *job);


// ====================
// gui methods

// void dt_dev_init(dt_develop_t *dev);
// void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_leave();
void dt_dev_enter();

void dt_dev_expose(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height);

#else

#define DT_DEV_CACHELINES 5

extern uint8_t dt_dev_default_gamma[0x10000];
extern float dt_dev_de_gamma[0x100];

/**
 * image for devel module.
 * stores cache line pointers,
 * along with an img-op last performed on this img.
 * can be stacked for history.
 */
typedef struct dt_dev_image_t
{
  dt_dev_operation_t operation;
  dt_dev_operation_params_t op_params;
  int32_t cacheline[3];
  int32_t num;
  dt_ctl_image_settings_t settings;
}
dt_dev_image_t;


typedef struct dt_develop_t
{
  int32_t gui_attached; // != 0 if the gui should be notified of changes in hist stack etc.
  int32_t image_loading;

  // small cached version while loading
  float *small_raw_buf;
  float *small_raw_cached;
  int32_t small_raw_hash;
  int32_t small_raw_loading;
  int32_t small_raw_width, small_raw_height;

  // converted img last viewed
  uint8_t *backbuf;
  uint8_t *small_backbuf;
  int32_t backbuf_hash, small_backbuf_hash;

  // image under consideration.
  dt_image_t *image;

  // cached versions of raw images, various zoom rates
  pthread_mutex_t cache_mutex;
  int32_t num_cachelines;
  int32_t cache_width, cache_height;
  float *cache_zoom_x, *cache_zoom_y;
  float **cache;
  int32_t *cache_sorted;
  int32_t *cache_used;
  int32_t *cache_hash;

  // history stack
  uint32_t last_hash;
  int32_t history_top, history_max;
  dt_dev_image_t *history;

  // image specific stuff
  uint32_t *histogram, *histogram_pre;
  uint32_t histogram_max, histogram_pre_max;

  // gamma table
  uint8_t gamma[0x10000];
  uint16_t tonecurve[0x10000];
}
dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_cache_load(dt_develop_t *dev, int32_t stackpos, dt_dev_zoom_t zoom);
void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img);
int  dt_dev_small_cache_load(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev, struct dt_image_t *img);

void dt_dev_add_history_item(dt_develop_t *dev, dt_dev_operation_t op);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history(dt_develop_t *dev);

void dt_dev_set_gamma(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);

void dt_dev_configure(dt_develop_t *dev, int32_t width, int32_t height);
void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxw, float *boxh);

float *dt_dev_get_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom, char mode);
int32_t dt_dev_reserve_new_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom);
int32_t dt_dev_test_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom);
void dt_dev_release_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom);

void dt_dev_update_cache(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom);
void dt_dev_update_small_cache(dt_develop_t *dev);

struct dt_job_t;
void dt_dev_export(struct dt_job_t *job);


// ====================
// gui methods

// void dt_dev_init(dt_develop_t *dev);
// void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_leave();
void dt_dev_enter();

void dt_dev_image_expose(struct dt_develop_t *dev, dt_dev_image_t *image, cairo_t *cr, int32_t width, int32_t height);
void dt_dev_expose(dt_develop_t *dev, cairo_t *cr, int32_t width, int32_t height);

#endif
#endif
