#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/jobs.h"
#include "control/control.h"
#include "common/image_cache.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>

#ifdef DT_USE_GEGL

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  dev->iop_instance = 0;
  dev->iop = NULL;
  dt_iop_module_t *module;
  module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
  dt_iop_load_module(module, dev, "gamma");
  dev->iop = g_list_append(dev->iop, module);
  module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
  dt_iop_load_module(module, dev, "tonecurve");
  dev->iop = g_list_append(dev->iop, module);
  // TODO:
  // dt_iop_load_module(module, dev, "saturation");
  // or better: one module for: blackpoint, whitepoint, exposure, fill darks, saturation.

  dev->history_end = 0;
  dev->history = NULL; // empty list

  dev->gui_attached = gui_attached;
  dev->width = -1;
  dev->height = -1;
  dev->pixmap = NULL;

  dev->image = NULL;
  dev->image_loading = dev->image_processing = dev->preview_loading = dev->preview_processing = 0;

  dev->histogram = dev->histogram_pre = NULL;
  dev->gegl = gegl_node_new();
  if(dev->gui_attached)
  {
    dev->histogram = (uint32_t *)malloc(sizeof(int32_t)*256*4);
    bzero(dev->histogram, sizeof(int32_t)*256*4);
    dev->histogram_max = -1;
    dev->histogram_pre = (uint32_t *)malloc(sizeof(int32_t)*256*4);
    bzero(dev->histogram_pre, sizeof(int32_t)*256*4);
    dev->histogram_pre_max = -1;

    // TODO: init gegl nodes:
    dev->gegl_buffer = gegl_buffer_new(NULL, babl_format("RGB float"));
    dev->gegl_load_buffer = gegl_node_new_child(dev->gegl, "operation", "gegl:load-buffer", "buffer", dev->gegl_buffer, NULL);
    dev->gegl_crop  = gegl_node_new_child(dev->gegl, "operation", "gegl:crop", "x", 0.0, "y", 0.0, "width", 0.0, "height", 0.0, NULL);
    dev->gegl_scale = gegl_node_new_child(dev->gegl, "operation", "gegl:scale", "origin-x", 0.0, "origin-y", 0.0, "x", 1.0, "y", 1.0, NULL);
    dev->gegl_translate = gegl_node_new_child(dev->gegl, "operation", "gegl:translate", "origin-x", 0.0, "origin-y", 0.0, "x", .0, "y", .0, NULL);
    // TODO: init gegl_pixbuf node!
    // TODO: prepend buffer loading and std processing nodes!
    gegl_node_link_many(dev->gegl_load_buffer, dev->gegl_translate, dev->gegl_scale, dev->gegl_crop, dev->gegl_pixbuf, NULL);
  }
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  // image_cache does not have to be unref'd, this is done outside develop module.
  // unref used mipmap buffers:
  if(dev->image)
  {
    dt_image_release(dev->image, DT_IMAGE_FULL, 'w');
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');
    dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
  }
  g_object_unref (dev->gegl);
  while(dev->history)
  {
    free(((dt_dev_history_item_t *)dev->history->data)->params);
    free( (dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_unload_module((dt_iop_module_t *)dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  if(dev->pixmap) g_object_unref(dev->pixmap); 
  free(dev->histogram);
  free(dev->histogram_pre);
}

void dt_dev_process_image(dt_develop_t *dev)
{
  if(!dev->image || dev->image_loading || !dev->gui_attached) return;
  dt_job_t job;
  dt_dev_process_image_job_init(&job, dev);
  int err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_2);
  if(err) fprintf(stderr, "[dev_process_image] job queue exceeded!\n");
}

void dt_dev_process_preview(dt_develop_t *dev)
{
  if(!dev->image || dev->preview_loading || !dev->gui_attached) return;
  dt_job_t job;
  dt_dev_process_preview_job_init(&job, dev);
  int err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_3);
  if(err) fprintf(stderr, "[dev_process_preview] job queue exceeded!\n");
}

void dt_dev_process_preview_job(dt_develop_t *dev)
{
  dev->preview_processing = 1;
  GeglProcessor *processor;
  GeglRectangle  roi;

  roi = (GeglRectangle){0, 0, dev->width, dev->height};

  processor = gegl_node_new_processor (dev->gegl_preview_pixbuf, &roi);
  while (gegl_processor_work (processor, NULL)) ;

  g_object_unref (processor);
  dev->preview_processing = 0;
  dt_control_queue_draw();
}

void dt_dev_process_image_job(dt_develop_t *dev)
{
  dev->image_processing = 1;
  GeglProcessor *processor;
  GeglRectangle  roi;

  roi = (GeglRectangle){0, 0, dev->width, dev->height};

  // TODO: replace by
  // void gegl_node_blit (GeglNode * node, gdouble scale, const GeglRectangle * roi, const Babl * format, gpointer destination_buf, gint rowstride, GeglBlitFlags flags)

  processor = gegl_node_new_processor (dev->gegl_pixbuf, &roi);
  while (gegl_processor_work (processor, NULL)) ;

  g_object_unref (processor);
  dev->image_processing = 0;
  dt_control_queue_draw();
}

void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img)
{
  // only load if not already there.
  if(dev->image->pixels && !dev->image->shrink) return;
restart:
  dev->image_loading = 1;
  dev->image_processing = 1;
  dev->image->shrink = 0;
  pthread_mutex_unlock(&dev->cache_mutex);
  int err = dt_image_load(img, DT_IMAGE_FULL);
  if(err) fprintf(stderr, "[dev_raw_load] failed to load image %s!\n", img->filename);

  // obsoleted by another job?
  if(dev->image != img)
  {
    printf("[dev_raw_load] recovering from obsoleted read!\n");
    img = dev->image;
    goto restart;
  }
  dev->image_loading = 0;
  // trigger processing pipeline:
  dt_dev_process_image(dev);
}

void dt_dev_load_image(dt_develop_t *dev, dt_image_t *image)
{
  GeglRectangle rect;
  rect = (GeglRectangle){0, 0, image->width, image->height};
  if(gegl_buffer_set_extent(dev->gegl_buffer, &rect)) return;
  gegl_node_set(dev->gegl_scale, "x", 1.0, "y", 1.0, NULL);
  // TODO: update scale factors

  dt_dev_read_history(dev);
  if(dev->gui_attached)
  {
    if(!dev->image->pixels || dev->image->shrink)
    {
      dt_job_t job;
      dt_dev_raw_load_job_init(&job, dev, dev->image);
      int32_t err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_1);
      if(err) fprintf(stderr, "[dev_load_image] job queue exceeded!\n");
    }
  }
  else dt_dev_raw_load(dev, dev->image); // in this thread.
}

static gboolean dt_dev_configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  dt_develop_t *dev = darktable.develop;
  // TODO:resize event: update ROI and scale factors!
  if(dev->width != event->width || dev->height != event->height)
  {
    GdkPixmap *tmppixmap = gdk_pixmap_new(da->window, event->width,  event->height, -1);
    if(dev->pixmap)
    {
      int minw = dev->width, minh = dev->height;
      if(event->width  < minw) minw = event->width;
      if(event->height < minh) minh = event->height;
      gdk_draw_drawable(tmppixmap, da->style->fg_gc[GTK_WIDGET_STATE(da)], dev->pixmap, 0, 0, 0, 0, minw, minh);
      g_object_unref(dev->pixmap); 
    }
    dev->pixmap = tmppixmap;
    dev->width = event->width;
    dev->height = event->height;
  }
}

void dt_dev_set_histogram_pre(dt_develop_t *dev)
{
  // TODO: replace by gegl histogram node
  if(!dev->image) return;
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  float *buf = dt_dev_get_cached_buf(dev, img, DT_ZOOM_FIT, 'r');
  if(buf)
  {
    const float sx = dev->image->width *fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    const float sy = dev->image->height*fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    dev->histogram_pre_max = dt_iop_create_histogram_f(buf, sx, sy, dev->cache_width, dev->histogram_pre);
    dt_dev_release_cached_buf(dev, img, DT_ZOOM_FIT);
  }
  else if(dev->small_raw_hash == img->num)
  {
    dev->histogram_pre_max = dt_iop_create_histogram_f(dev->small_raw_cached, dev->small_raw_width, dev->small_raw_height, dev->small_raw_width, dev->histogram_pre);
    dt_dev_update_cache(dev, img, DT_ZOOM_FIT);
  }
}

void dt_dev_set_histogram(dt_develop_t *dev)
{
  // TODO: count bins in pixmap
  if(!dev->image) return;
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  float *buf = dt_dev_get_cached_buf(dev, img, DT_ZOOM_FIT, 'r');
  if(buf)
  {
    const float sx = dev->image->width *fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    const float sy = dev->image->height*fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    dev->histogram_max = dt_iop_create_histogram_final_f(buf, sx, sy, dev->cache_width, dev->histogram, dev->gamma, dev->tonecurve);
    dt_dev_release_cached_buf(dev, img, DT_ZOOM_FIT);
  }
  else if(dev->small_raw_hash == img->num)
  {
    dev->histogram_max = dt_iop_create_histogram_final_f(dev->small_raw_cached, dev->small_raw_width, dev->small_raw_height, dev->small_raw_width, dev->histogram, dev->gamma, dev->tonecurve);
    dt_dev_update_cache(dev, img, DT_ZOOM_FIT);
  }
}

// helper used to synch a single history item with db
int dt_dev_write_history_item(dt_develop_t *dev, dt_dev_history_item_t *h, int32_t num)
{
  if(!dev->image) return 1;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1 and num = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  rc = sqlite3_bind_int (stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "insert into history (imgid, num) values (?1, ?2)", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, dev->image->id);
    rc = sqlite3_bind_int (stmt, 2, num);
    rc = sqlite3_step (stmt);
  }
  rc = sqlite3_finalize (stmt);
  rc = sqlite3_prepare_v2(darktable.db, "update history set operation = ?1, op_params = ?2, module = ?3, enabled = ?4 where imgid = ?5 and num = ?6", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, h->op, strlen(h->op), SQLITE_STATIC);
  rc = sqlite3_bind_blob(stmt, 2, &(h->op_params), sizeof(dt_dev_operation_params_t), SQLITE_STATIC);
  rc = sqlite3_bind_int (stmt, 3, h->iop);
  rc = sqlite3_bind_int (stmt, 4, h->enabled);
  rc = sqlite3_bind_int (stmt, 5, dev->image->id);
  rc = sqlite3_bind_int (stmt, 6, num);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  return 0;
}

// TODO: port to gegl and glist
void dt_dev_add_history_item(dt_develop_t *dev, dt_dev_operation_t op)
{
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  if(dev->gui_attached)
  {
    dt_ctl_gui_mode_t gui;
    DT_CTL_GET_GLOBAL(gui, gui);
    if(gui != DT_DEVELOP) return;
    dt_control_clear_history_items(dev->history_top-1);
  }
  // this is called exclusively from the gtk thread, so no lock is necessary.
  int32_t num = dt_dev_update_fixed_pipeline(dev, op, img->num);

  if(strncmp(img->operation, op, 20) != 0)
  {
    dev->history_top++;
    if(dev->gui_attached) dt_control_add_history_item(dev->history_top-1, op);
    for(int k=0;k<3;k++) img[1].cacheline[k] = img[0].cacheline[k];
    img ++;
    if(dev->history_top >= dev->history_max)
    {
      dev->history_max *= 2;
      dt_dev_image_t *tmp = (dt_dev_image_t *)malloc(sizeof(dt_dev_image_t)*dev->history_max);
      memcpy(tmp, dev->history, dev->history_max/2 * sizeof(dt_dev_image_t));
      free(dev->history);
      dev->history = tmp;
    }
  }
  if(dev->gui_attached)
  {
    pthread_mutex_lock(&(darktable.control->image_mutex));
    img->settings = darktable.control->image_settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
  }
  DT_CTL_SET_GLOBAL_STR(dev_op, img->operation, 20);
  DT_CTL_GET_GLOBAL(img->op_params, dev_op_params);
  img->num = num;
  strncpy(img->operation, op, 20);
  dt_print(DT_DEBUG_DEV, "pushing history %d item with hash %d, operation %d and cachelines %d %d %d\n", dev->history_top-1, num, op, img->cacheline[0], img->cacheline[1], img->cacheline[2]);

  dev->small_backbuf_hash = -1;
  dt_dev_update_small_cache(dev);
}

// TODO: only adjust history_end and gegl links!
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt)
{
  // this is called exclusively from the gtk thread, so no lock is necessary.
  if(cnt == dev->history_top-1) return;
  dev->history_top = cnt + 1;
  if(dev->gui_attached)
  {
    pthread_mutex_lock(&(darktable.control->image_mutex));
    darktable.control->image_settings = dev->history[dev->history_top-1].settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
    DT_CTL_SET_GLOBAL_STR(dev_op, dev->history[dev->history_top-1].operation, 20);
    DT_CTL_SET_GLOBAL(dev_op_params, dev->history[dev->history_top-1].op_params);
  }
  (void)dt_dev_update_fixed_pipeline(dev, "original", 0);

  dev->small_backbuf_hash = -1;
  dt_dev_update_small_cache(dev);
}

// TODO: port to glist!
void dt_dev_write_history(dt_develop_t *dev)
{
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  sqlite3_step(stmt);
  rc = sqlite3_finalize (stmt);
  for(int k=0;k<dev->history_top;k++)
    (void)dt_dev_write_history_item(dev, dev->history+k, k);
}

void dt_dev_read_history(dt_develop_t *dev)
{
  // TODO: port to new history items and database format.
  // TODO: on demand loading of modules!
  if(dev->gui_attached) dt_control_clear_history_items(0);
  if(!dev->image) return;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select * from history where imgid = ?1 order by num", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  dev->history_top = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      dt_dev_image_t *h = dev->history + dev->history_top;
      h->num = sqlite3_column_int(stmt, 2);
      strncpy(h->operation, (char *)sqlite3_column_text(stmt, 3), 20);
      memcpy(&(h->op_params), sqlite3_column_blob(stmt, 4), sizeof(dt_dev_operation_params_t));
      memcpy(&(h->settings), sqlite3_column_blob(stmt, 5), sizeof(dt_ctl_image_settings_t));
      dev->history_top++;
      if(dev->history_top >= dev->history_max)
      {
        dev->history_max *= 2;
        dt_dev_image_t *tmp = (dt_dev_image_t *)malloc(sizeof(dt_dev_image_t)*dev->history_max);
        memcpy(tmp, dev->history, dev->history_max/2 * sizeof(dt_dev_image_t));
        free(dev->history);
        dev->history = tmp;
      }
      if(dev->gui_attached) dt_control_add_history_item(dev->history_top-1, dev->history[dev->history_top-1].operation);
    }
    while(sqlite3_step(stmt) == SQLITE_ROW);
    rc = sqlite3_finalize (stmt);
  }
  else
  {
    rc = sqlite3_finalize (stmt);
    dev->history_top = 1;
    dev->history[0].num = 1;//dev->image->id << 6;
    for(int k=0;k<3;k++) dev->history[0].cacheline[k] = 0;
    dev->history[0].settings = darktable.control->image_defaults;
    strncpy(dev->history[0].operation, "original", 20);
    if(dev->gui_attached) dt_control_add_history_item(0, "original");
    dt_dev_write_history_item(dev, dev->history, 0);
  }

  if(dev->gui_attached)
  {
    DT_CTL_SET_GLOBAL_STR(dev_op, dev->history[dev->history_top-1].operation, 20);
    DT_CTL_SET_GLOBAL(dev_op_params, dev->history[dev->history_top-1].op_params);
    pthread_mutex_lock(&(darktable.control->image_mutex));
    darktable.control->image_settings = dev->history[dev->history_top - 1].settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
  }
  (void)dt_dev_update_fixed_pipeline(dev, "original", 0);
}

void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxww, float *boxhh)
{
  float boxw = 1, boxh = 1;
  if(zoom == DT_ZOOM_1)
  {
    const float imgw = (closeup ? 2 : 1)*dev->image->width;
    const float imgh = (closeup ? 2 : 1)*dev->image->height;
    const float devw = MIN(imgw, dev->cache_width);
    const float devh = MIN(imgh, dev->cache_height);
    boxw = fminf(1.0, devw/imgw); boxh = fminf(1.0, devh/imgh);
  }
  else if(zoom == DT_ZOOM_FILL)
  {
    const float imgw = dev->image->width;
    const float imgh = dev->image->height;
    const float devw = dev->cache_width;
    const float devh = dev->cache_height;
    boxw = devw/(imgw*fmaxf(devw/imgw, devh/imgh)); boxh = devh/(imgh*fmaxf(devw/imgw, devh/imgh));
  }
  
  if(*zoom_x < boxw/2 - .5f) *zoom_x = boxw/2 - .5f;
  if(*zoom_x > .5f - boxw/2) *zoom_x = .5f - boxw/2;
  if(*zoom_y < boxh/2 - .5f) *zoom_y = boxh/2 - .5f;
  if(*zoom_y > .5f - boxh/2) *zoom_y = .5f - boxh/2;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

// TODO: switch to gegl:write-buffer and GeglBuffer to pixels!
void dt_dev_export(dt_job_t *job)
{
  // TODO: use gegl output nodes and gegl_node_process!
  while(1)
  {
    // TODO: progress bar in ctl?
    char filename[1024];
    dt_image_t *img = NULL;
    pthread_mutex_lock(&(darktable.library->film->images_mutex));
    int32_t rc, start = darktable.library->film->last_exported++;
    pthread_mutex_unlock(&(darktable.library->film->images_mutex));
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select * from (select * from selected_images) as dreggn limit ?1, 1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, start);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int imgid = sqlite3_column_int(stmt, 0);
      img = dt_image_cache_get(imgid, 'r');
    }
    sqlite3_finalize(stmt);
    if(!img)
    {
      // TODO: reset progress bar
      return;
    }

    snprintf(filename, 1024, "%s/darktable_exported", darktable.library->film->dirname);
    if(g_mkdir_with_parents(filename, 0755))
    {
      dt_image_cache_release(img, 'r');
      fprintf(stderr, "[dev_export] could not create directory %s!\n", filename);
      return;
    }
    snprintf(filename, 1024-4, "%s/darktable_exported/%s", darktable.library->film->dirname, img->filename);
    char *c = filename + strlen(filename);
    for(;c>filename && *c != '.';c--);
    if(c <= filename) c = filename + strlen(filename);

    // read type from global config.
    dt_dev_export_format_t fmt;
    DT_CTL_GET_GLOBAL(fmt, dev_export_format);
    switch(fmt)
    {
      case DT_DEV_EXPORT_JPG:
        strncpy(c, ".jpg", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PNG:
        strncpy(c, ".png", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PPM16:
        strncpy(c, ".ppm", 4);
        dt_imageio_export_16(img, filename);
        break;
      case DT_DEV_EXPORT_PFM:
        strncpy(c, ".pfm", 4);
        dt_imageio_export_f(img, filename);
        break;
      default:
        break;
    }
    dt_image_cache_release(img, 'r');
    printf("[dev_export] exported to `%s'\n", filename);
  }
}

#else // old non-gegl version:

uint8_t dt_dev_default_gamma[0x10000];
float dt_dev_de_gamma[0x100];

int32_t dt_dev_update_fixed_pipeline(dt_develop_t *dev, dt_dev_operation_t op, int32_t hash)
{
  if(strncmp(op, "original", 20) == 0)
  {
    dt_control_get_tonecurve(dev->tonecurve, &(dev->history[dev->history_top-1].settings));
    dt_dev_set_gamma(dev);
    dt_dev_set_histogram_pre(dev);
  }
  else if(strncmp(op, "tonecurve", 20) == 0)
  {
    dt_control_get_tonecurve(dev->tonecurve, &(dev->history[dev->history_top-1].settings));
  }
  else if(strncmp(op, "gamma", 20) == 0)
  {
    dt_dev_set_gamma(dev);
  }
  else
  { // plugin op on buffer
    dt_dev_set_histogram_pre(dev);
    // hash += (dev->last_hash++) * (1<<11);
    hash = dev->last_hash++;
  }
  dt_dev_set_histogram(dev);
  return hash;
}

void dt_dev_update_cache(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom)
{
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  int test = dt_dev_test_cached_buf(dev, img, zoom) &&
    dev->cache_zoom_x[img->cacheline[zoom]] == zoom_x && dev->cache_zoom_y[img->cacheline[zoom]] == zoom_y;
  pthread_mutex_lock(&dev->cache_mutex);
  if(!dev->image || !dev->gui_attached || dev->image_loading || test)
  {
    pthread_mutex_unlock(&dev->cache_mutex);
    return; // refuse silly cache op.
  }
  pthread_mutex_unlock(&dev->cache_mutex);
  dt_job_t job;
  dt_dev_cache_load_job_init(&job, dev, dev->history_top - 1, zoom);
  int err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_2 + zoom);
  if(err) fprintf(stderr, "[dev_update_cache] job queue exceeded!\n");
}

void dt_dev_update_small_cache(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  // only one process running anyways, jobs will not pile up
  // pthread_mutex_lock(&dev->cache_mutex);
  // if(dev->small_raw_loading || dev->small_raw_hash == dev->history[dev->history_top-1].num)
  // {
  //   pthread_mutex_unlock(&dev->cache_mutex);
  //   return;
  // }
  // dev->small_raw_loading = 2;
  // pthread_mutex_unlock(&dev->cache_mutex);
  dt_job_t job;
  dt_dev_small_cache_load_init(&job, dev);
  int err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_5);
  if(err) fprintf(stderr, "[dev_update_small_cache] job queue exceeded!\n");
}

void dt_dev_set_gamma_array(dt_develop_t *dev, const float linear, const float gamma, uint8_t *arr)
{
  double a, b, c, g;
  if(linear<1.0)
  {
    g = gamma*(1.0-linear)/(1.0-gamma*linear);
    a = 1.0/(1.0+linear*(g-1));
    b = linear*(g-1)*a;
    c = pow(a*linear+b, g)/linear;
  }
  else
  {
    a = b = g = 0.0;
    c = 1.0;
  }

  for(int k=0;k<0x10000;k++)
  {
    int32_t tmp = 0x10000 * powf(k/(float)0x10000, 1./2.2);
	  if (k<0x10000*linear) tmp = MIN(c*k, 0xFFFF);
	  else tmp = MIN(pow(a*k/0x10000+b, g)*0x10000, 0xFFFF);
    arr[k] = tmp>>8;
  }
}

void dt_dev_set_gamma(dt_develop_t *dev)
{
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  dt_dev_set_gamma_array(dev, img->settings.dev_gamma_linear, img->settings.dev_gamma_gamma, dev->gamma);
}

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  dev->gui_attached = gui_attached;
  dev->image = NULL;
  dev->image_loading = 0;
  dev->small_raw_loading = 0;
  dev->small_backbuf = dev->backbuf = NULL;
  dev->backbuf_hash = dev->small_backbuf_hash = -1;
  dev->small_raw_cached = NULL;
  dev->small_raw_hash = -1;
  dev->small_raw_height = dev->small_raw_width = -1;
  pthread_mutex_init(&dev->cache_mutex, NULL);
  dev->num_cachelines = DT_DEV_CACHELINES;
  dev->cache = (float **)malloc(dev->num_cachelines*sizeof(float*));
  dev->cache_zoom_x = (float *)malloc(dev->num_cachelines*sizeof(float));
  dev->cache_zoom_y = (float *)malloc(dev->num_cachelines*sizeof(float));
  dev->cache_sorted = (int32_t *)malloc(dev->num_cachelines*sizeof(int32_t));
  dev->cache_hash = (int32_t *)malloc(dev->num_cachelines*sizeof(uint32_t));
  dev->cache_used = (int32_t *)malloc(dev->num_cachelines*sizeof(uint32_t));
  dev->small_raw_buf = NULL;
  for(int k=0;k<dev->num_cachelines;k++)
  {
    dev->cache_hash[k] = -1;
    dev->cache[k] = NULL;
    dev->cache_used[k] = 0;
    dev->cache_sorted[k] = k;
  }
  dev->cache_width = dev->cache_height = -1;
  
  dev->history_top = 0;
  dev->history_max = 100;
  dev->history = (dt_dev_image_t *)malloc(sizeof(dt_dev_image_t)*dev->history_max);
  for(int k=0;k<3;k++) dev->history[0].cacheline[k] = 0;

  float lin, gam;
  DT_CTL_GET_GLOBAL(lin, dev_gamma_linear);
  DT_CTL_GET_GLOBAL(gam, dev_gamma_gamma);
  dt_dev_set_gamma_array(dev, lin, gam, dt_dev_default_gamma);
  dt_dev_set_gamma_array(dev, lin, gam, dev->gamma);
  int last = 0; // invert 0.1 0.45 fn
  for(int i=0;i<0x100;i++) for(int k=last;k<0x10000;k++)
    if(dt_dev_default_gamma[k] >= i) { last = k; dt_dev_de_gamma[i] = k/(float)0x10000; break; }
  // for(int k=0;k<0x10000;k++) dt_dev_de_gamma[k] = CLAMP(0xffff*powf(k/(double)0xffff, 2.2), 0, 0xffff);

  dev->histogram = (uint32_t *)malloc(sizeof(int32_t)*256*4);
  bzero(dev->histogram, sizeof(int32_t)*256*4);
  dev->histogram_max = -1;
  dev->histogram_pre = (uint32_t *)malloc(sizeof(int32_t)*256*4);
  bzero(dev->histogram_pre, sizeof(int32_t)*256*4);
  dev->histogram_pre_max = -1;
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  // TODO: check if image_cache has to be released as well?
  if(dev->image)
  {
    dt_image_release(dev->image, DT_IMAGE_FULL, 'w');
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');
    dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
  }
#ifdef DT_USE_GEGL 
  g_object_unref (dev->gegl);
#else
  pthread_mutex_destroy(&dev->cache_mutex);
  for(int k=0;k<dev->num_cachelines;k++) free(dev->cache[k]);
  free(dev->cache);
  free(dev->cache_sorted);
  free(dev->cache_used);
  free(dev->cache_hash);
  free(dev->cache_zoom_x);
  free(dev->cache_zoom_y);
  free(dev->backbuf);
  free(dev->small_backbuf);
  free(dev->small_raw_buf);
#endif
  free(dev->history);
  free(dev->histogram);
  free(dev->histogram_pre);
}

int dt_dev_small_cache_load(dt_develop_t *dev)
{
  dt_print(DT_DEBUG_DEV, "[small_cache_load] hash %d -> %d\n", dev->small_raw_hash, dev->history[dev->history_top-1].num);
  if(!dev->image) return 1;
  // pthread_mutex_lock(&dev->cache_mutex);
  // if(dev->small_raw_loading == 1)
  // {
  //   pthread_mutex_unlock(&dev->cache_mutex);
  //   return;
  // }
  // dev->small_raw_loading = 1;
  // pthread_mutex_unlock(&dev->cache_mutex);
  dt_image_buffer_t mip = dt_image_get(dev->image, DT_IMAGE_MIPF, 'r');
  if(mip != DT_IMAGE_MIPF)
  { // fail. :(
    pthread_mutex_lock(&dev->cache_mutex);
    dev->small_raw_loading = 0;
    pthread_mutex_unlock(&dev->cache_mutex);
    return 1;
  }
  int wd, ht;
  dt_image_get_mip_size(dev->image, DT_IMAGE_MIPF, &wd, &ht);
  dev->small_raw_width  = wd;
  dev->small_raw_height = ht;
restart_small_cache:
  dt_image_check_buffer(dev->image, DT_IMAGE_MIPF, 3*wd*ht*sizeof(float));
  // need 16-bit buf update?
  pthread_mutex_lock(&dev->cache_mutex);
  int hash_in = dev->history[dev->history_top-1].num;
  int test = dev->small_raw_hash != dev->history[dev->history_top-1].num;
  pthread_mutex_unlock(&dev->cache_mutex);
  if(test)
  {
    float *tmp = dev->image->mipf;
    // memcpy(dev->small_raw_cached, dev->small_raw, wd*ht*3*sizeof(uint16_t));
    // perform all image operations on stack
    const int num_in = dev->history[0].num;
    for(int k=1;k<dev->history_top;k++)
    {
      dt_dev_image_t *img = dev->history + k;
      dt_iop_execute(dev->small_raw_buf, tmp, wd, ht, wd, ht, img->operation, &(img->op_params));
      if(img->num != num_in) tmp = dev->small_raw_buf;
    }
    dev->small_raw_cached = tmp; // set to orig or buf.
    if(dev->history[dev->history_top-1].num != hash_in) goto restart_small_cache; // obsoleted
  }
  if(dev->small_backbuf_hash != dev->history[dev->history_top-1].num)
  {
// #pragma omp parallel for schedule(static) shared(dev)
    for(int i=0;i<wd*ht;i++) for(int k=0;k<3;k++)
      dev->small_backbuf[4*i+2-k] = dev->gamma[dev->tonecurve[(int)CLAMP(0xffff*dev->small_raw_cached[3*i+k], 0, 0xffff)]];
  }

  dt_image_release(dev->image, mip, 'r');
  pthread_mutex_lock(&dev->cache_mutex);
  dev->small_backbuf_hash = 
    dev->small_raw_hash = dev->history[dev->history_top-1].num;
  dev->small_raw_loading = 0;
  pthread_mutex_unlock(&dev->cache_mutex);
  (void)dt_dev_update_fixed_pipeline(dev, dev->history[dev->history_top-1].operation, 0);
  dt_control_queue_draw();
  return 0;
}

void dt_dev_cache_load(dt_develop_t *dev, int32_t stackpos, dt_dev_zoom_t zoom)
{
  pthread_mutex_lock(&dev->cache_mutex);
  if(!dev->image || dev->image_loading || !dev->image->pixels || dev->cache_width < 0 || dev->image->shrink)
  {
    pthread_mutex_unlock(&dev->cache_mutex);
    return; // refuse silly cache op.
  }
  pthread_mutex_unlock(&dev->cache_mutex);

  dt_dev_image_t *img = dev->history + stackpos;
  float zoom_x, zoom_y;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  const int cwd = dev->cache_width, cht = dev->cache_height;
  const int iwd = dev->image->width, iht = dev->image->height;
  // already there, just recently written by this worker thread?
  if(dt_dev_test_cached_buf(dev, img, zoom) && dev->cache_zoom_x[img->cacheline[zoom]] == zoom_x && dev->cache_zoom_y[img->cacheline[zoom]] == zoom_y)
    return;
    // goto finalize;
  // printf("[dev_cache_load] img %d zoom %d to cacheline %d for stackpos %d\n", img->num, zoom, img->cacheline[zoom], stackpos);

  if(stackpos == 0)
  { // clip and zoom from original raw image
    dt_dev_reserve_new_cached_buf(dev, img, zoom);
    dev->cache_zoom_x[img->cacheline[zoom]] = zoom_x;
    dev->cache_zoom_y[img->cacheline[zoom]] = zoom_y;
    dt_image_check_buffer(dev->image, DT_IMAGE_FULL, 3*iwd*iht*sizeof(float));
    float *pixels = dev->image->pixels;
    // resample original image
    if(zoom == DT_ZOOM_1)
    {
      int x = MAX(0, (int)((zoom_x + .5f)*iwd - cwd/2)), y = MAX(0, (int)((zoom_y + .5f)*iht - cht/2));
      const int cht2 = MIN(cht, iht - y);
      const int cwd2 = MIN(cwd, iwd - x);
      int idx = 0;
      for(int j=0;j<cht2;j++)
      {
        for(int i=0;i<cwd2;i++)
        {
          for(int k=0;k<3;k++) dev->cache[img->cacheline[zoom]][3*idx + k] = pixels[3*(iwd*y + x) + k];
          x++; idx++;
        }
        y++; x = MAX(0, (int)((zoom_x + .5f)*iwd - cwd/2));
        idx = cwd*j;
      }
    }
    else if(zoom == DT_ZOOM_FILL)
    {
      float boxw = cwd/(iwd*fmaxf(cwd/(float)iwd, cht/(float)iht)), boxh = cht/(iht*fmaxf(cwd/(float)iwd, cht/(float)iht));
      dt_iop_clip_and_zoom(pixels, (zoom_x + .5f)*iwd - boxw*iwd/2, (zoom_y + .5f)*iht - boxh*iht/2, boxw*iwd, boxh*iht, iwd, iht,
                           dev->cache[img->cacheline[zoom]], 0, 0, cwd, cht, cwd, cht);
    }
    else
    {
      float scale = fminf(cwd/(float)iwd, cht/(float)iht);
      dt_iop_clip_and_zoom(pixels, 0, 0, iwd, iht, iwd, iht,
                           dev->cache[img->cacheline[zoom]], 0, 0, scale*iwd, scale*iht, cwd, cht);
    }
    dt_dev_release_cached_buf(dev, img, zoom);
  }
  else
  {
    // get image from last operation (stack - 1):
    float *buf = NULL, *dst = NULL;
    // recursively acquire buffers of previous operations, let cache_load decide whether loading is necessary.
    dt_dev_cache_load(dev, stackpos-1, zoom);
    buf = dt_dev_get_cached_buf(dev, img-1, zoom, 'r');
    if(buf)
    {
      dst = dt_dev_get_cached_buf(dev, img, zoom, 'w'); // maybe parent buffer is ours?
      if(!dst) dt_dev_reserve_new_cached_buf(dev, img, zoom);
      dev->cache_zoom_x[img->cacheline[zoom]] = zoom_x;
      dev->cache_zoom_y[img->cacheline[zoom]] = zoom_y;
      if(buf != dev->cache[img->cacheline[zoom]])
      { // only perform op if buffer is there and op affected it
        // get operation and params, apply to buffer.
        const float sx = iwd*fminf(cwd/(float)iwd, cht/(float)iht);
        const float sy = iht*fminf(cwd/(float)iwd, cht/(float)iht);
        const int cwd2 = zoom == DT_ZOOM_FIT ? sx : cwd;
        const int cht2 = zoom == DT_ZOOM_FIT ? sy : cht; 
        dt_iop_execute(dev->cache[img->cacheline[zoom]], buf, cwd2, cht2, cwd, cht, img->operation, &(img->op_params));
        dt_dev_release_cached_buf(dev, img-1, zoom);
      }
      dt_dev_release_cached_buf(dev, img, zoom);
    }
  }
  (void)dt_dev_update_fixed_pipeline(dev, img->operation, 0);
// finalize:
  if(stackpos == dev->history_top - 1)
  {
    dt_control_queue_draw();
  }
}

void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img)
{
  int err;
  pthread_mutex_lock(&dev->cache_mutex);
  // only load if not already there.
  if(!dev->image->pixels || dev->image->shrink)
  {
    // TODO:
    if(dev->image_loading)
    {
      // kill still running job
      fprintf(stderr, "[dev_raw_load] still old image loading! TODO: kill job!\n");
      pthread_mutex_unlock(&dev->cache_mutex);
      return;
    }
    
restart:
    dev->image_loading = 1;
    dev->image->shrink = 0;
    pthread_mutex_unlock(&dev->cache_mutex);
    err = dt_image_load(img, DT_IMAGE_FULL);
    if(err)
    {
      fprintf(stderr, "[dev_raw_load] failed to load image %s!\n", img->filename);
    }

    // obsoleted by another job?
    if(dev->image != img)
    {
      printf("[dev_raw_load] recovering from obsoleted read!\n");
      img = dev->image;
      pthread_mutex_lock(&dev->cache_mutex);
      goto restart;
    }

    pthread_mutex_lock(&dev->cache_mutex);
    dev->image_loading = 0;
    pthread_mutex_unlock(&dev->cache_mutex);

    // flush cashe
    dev->backbuf_hash = -1;
    dev->histogram_max = -1;
    dev->histogram_pre_max = -1;
    for(int k=0;k<dev->num_cachelines;k++) dev->cache_hash[k] = -1;

    // printf("[dev_raw_load] starting cache jobs %s\n", img->filename);
    // create cached arrays in threads.
    for(int k=0;k<3;k++)
      dt_dev_update_cache(dev, dev->history + dev->history_top - 1, k);
  }
  else pthread_mutex_unlock(&dev->cache_mutex);
}

void dt_dev_load_image(dt_develop_t *dev, dt_image_t *image)
{
#ifndef DT_USE_GEGL
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select hash from history where imgid = ?1 order by num desc limit 1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, image->id);
  if(sqlite3_step(stmt) != SQLITE_ROW) dev->last_hash = 1;
  else dev->last_hash = sqlite3_column_int(stmt, 0);
  rc = sqlite3_finalize(stmt);
  if(dev->image != image)
  {
    dev->image = image;
    // invalidate all caches.
    dev->small_backbuf_hash = -1;
    dev->backbuf_hash = -1;
    dev->small_raw_hash = -1;
    for(int k=0;k<dev->num_cachelines;k++) dev->cache_hash[k] = -1;
  }
  dt_dev_read_history(dev);

  pthread_mutex_lock(&dev->cache_mutex);
  dev->small_raw_loading = 0;
  pthread_mutex_unlock(&dev->cache_mutex);

  dt_control_get_tonecurve(dev->tonecurve, &(dev->history[dev->history_top-1].settings));
  dt_dev_set_gamma(dev);
#else
  // TODO: load modules needed for this history stack
  // temp fix for now: load fixed pipeline of modules:
  dev->iop[0]->dt = &darktable;
  dev->iop[0]->dev = dev;
  dev->iop[0]->enabled = 1;
  dt_iop_tonecurve_init(dev->iop[0]);
  dt_dev_read_history(dev);
#endif

  if(dev->gui_attached)
  {
    if(!dev->image->pixels || dev->image->shrink)
    {
      dt_job_t job;
      dt_dev_raw_load_job_init(&job, dev, dev->image);
      int32_t err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_1);
      if(err) fprintf(stderr, "[dev_load_image] job queue exceeded!\n");
    }
  }
  else dt_dev_raw_load(dev, dev->image);
}

void dt_dev_configure(dt_develop_t *dev, int32_t width, int32_t height)
{
  // re-alloc cache on resize, flushes all entries :(
  if(dev->cache_width != width || dev->cache_height != height)
  {
    pthread_mutex_lock(&dev->cache_mutex);
    // jobs still working will schedule a redraw when finished.
    for(int k=0;k<dev->num_cachelines;k++)
    {
      if(dev->cache_used[k])
      {
        pthread_mutex_unlock(&dev->cache_mutex);
        return;
      }
    }
    dev->cache_width = width;
    dev->cache_height = height;
    for(int k=0;k<dev->num_cachelines;k++)
    {
      dev->cache_hash[k] = -1;
      dev->cache_used[k] = 0;
      free(dev->cache[k]);
      dev->cache[k] = (float *)malloc(3*sizeof(float)*dev->cache_width*dev->cache_height);
      bzero(dev->cache[k], 3*sizeof(float)*dev->cache_width*dev->cache_height);
      dev->cache_sorted[k] = k;
    }
    free(dev->backbuf);
    free(dev->small_backbuf);
    free(dev->small_raw_buf);
    int wd, ht;
    dt_image_get_mip_size(dev->image, DT_IMAGE_MIPF, &wd, &ht);
    // printf("cache configure! size %d\n", size);
    dev->backbuf = (uint8_t *)malloc(4*sizeof(uint8_t)*dev->cache_width*dev->cache_height);
    dev->small_raw_buf = (float *)malloc(3*sizeof(float)*wd*ht);
    bzero(dev->small_raw_buf, 3*sizeof(float)*wd*ht);
    dev->small_backbuf = (uint8_t *)malloc(4*sizeof(uint8_t)*wd*ht);
    pthread_mutex_unlock(&dev->cache_mutex);
    // update caches
    dt_job_t job;
    dt_dev_raw_load_job_init(&job, dev, dev->image);
    int32_t err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_1);
    if(err) fprintf(stderr, "[dev_expose] job queue exceeded!\n");
  }
}

void dt_dev_set_histogram_pre(dt_develop_t *dev)
{
  if(!dev->image) return;
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  float *buf = dt_dev_get_cached_buf(dev, img, DT_ZOOM_FIT, 'r');
  if(buf)
  {
    const float sx = dev->image->width *fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    const float sy = dev->image->height*fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    dev->histogram_pre_max = dt_iop_create_histogram_f(buf, sx, sy, dev->cache_width, dev->histogram_pre);
    dt_dev_release_cached_buf(dev, img, DT_ZOOM_FIT);
  }
  else if(dev->small_raw_hash == img->num)
  {
    dev->histogram_pre_max = dt_iop_create_histogram_f(dev->small_raw_cached, dev->small_raw_width, dev->small_raw_height, dev->small_raw_width, dev->histogram_pre);
    dt_dev_update_cache(dev, img, DT_ZOOM_FIT);
  }
}

void dt_dev_set_histogram(dt_develop_t *dev)
{
  if(!dev->image) return;
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  float *buf = dt_dev_get_cached_buf(dev, img, DT_ZOOM_FIT, 'r');
  if(buf)
  {
    const float sx = dev->image->width *fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    const float sy = dev->image->height*fminf(dev->cache_width/(float)dev->image->width, dev->cache_height/(float)dev->image->height);
    dev->histogram_max = dt_iop_create_histogram_final_f(buf, sx, sy, dev->cache_width, dev->histogram, dev->gamma, dev->tonecurve);
    dt_dev_release_cached_buf(dev, img, DT_ZOOM_FIT);
  }
  else if(dev->small_raw_hash == img->num)
  {
    dev->histogram_max = dt_iop_create_histogram_final_f(dev->small_raw_cached, dev->small_raw_width, dev->small_raw_height, dev->small_raw_width, dev->histogram, dev->gamma, dev->tonecurve);
    dt_dev_update_cache(dev, img, DT_ZOOM_FIT);
  }
}

int32_t dt_dev_test_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom)
{
  pthread_mutex_lock(&dev->cache_mutex);
  int32_t cacheline = img->cacheline[zoom];
  int32_t hash = (img->num << 4) | zoom;
  int32_t res = 0;
  if(dev->cache_hash[cacheline] == hash) res = 1; // found it!
  pthread_mutex_unlock(&dev->cache_mutex);
  return res;
}

float *dt_dev_get_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom, char mode)
{
  // dt_print(DT_DEBUG_DEV, "[dev_get_cached_buf] img %d %d %d\n", img->cacheline[0], img->cacheline[1], img->cacheline[2]);
  pthread_mutex_lock(&dev->cache_mutex);
  int32_t cacheline = img->cacheline[zoom];
  int32_t hash = (img->num << 4) | zoom;
  if(dev->cache_hash[cacheline] != hash || (dev->cache_used[cacheline] & 2))
  {
    dt_print(DT_DEBUG_DEV, "[dev_get_cached_buf] miss line %d for img %d|%d, write lock: %d\n", cacheline, img->num, zoom, dev->cache_used[cacheline] & 2);
    pthread_mutex_unlock(&dev->cache_mutex);
    return NULL; // cache miss: not there or currently writing.
  }
  // sort lru list
  for(int k=0;k<dev->num_cachelines;k++)
  {
    if(dev->cache_sorted[k] == cacheline)
    {
      for(int i=k;i<dev->num_cachelines-1;i++) dev->cache_sorted[i] = dev->cache_sorted[i+1];
      dev->cache_sorted[dev->num_cachelines-1] = cacheline;
      break;
    }
  }
  dev->cache_used[cacheline] = 1;
  if(mode == 'w') dev->cache_used[cacheline] |= 2;
  dt_print(DT_DEBUG_DEV, "[dev_get_cached_buf] img %d zoom %d in cacheline %d mode '%c'\n", img->num, zoom, cacheline, mode);
  pthread_mutex_unlock(&dev->cache_mutex);
  return dev->cache[cacheline];
}

int32_t dt_dev_reserve_new_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom)
{
  pthread_mutex_lock(&dev->cache_mutex);
  int32_t hash = (img->num << 4) | zoom;
  // kick lru object and reserve it as mru for this hash.
  int32_t line = img->cacheline[zoom];
  if(dev->cache_hash[line] != hash) line = dev->cache_sorted[0];
  int32_t kicked = dev->cache_hash[line];
  dev->cache_hash[line] = hash;
  img->cacheline[zoom] = line;
  int found = 0;
  for(int i=0;i<dev->num_cachelines-1;i++)
  {
    if(dev->cache_sorted[i] == line) found = 1;
    if(found) dev->cache_sorted[i] = dev->cache_sorted[i+1];
  }
  dt_print(DT_DEBUG_DEV, "[dev_reserve_new_cached_buf] line %d for img %d|%d (was hash %d), write lock: %d\n", line, img->num, zoom, kicked, dev->cache_used[line] == 2);

  dev->cache_sorted[dev->num_cachelines-1] = line;
  dev->cache_used[line] = 2;
  // propagate up the stack
  for(dt_dev_image_t *i2 = img+1;i2 < dev->history+dev->history_top && i2->num == img->num;i2++) for(int k=0;k<3;k++) i2->cacheline[k] = img->cacheline[k];
  pthread_mutex_unlock(&dev->cache_mutex);
  return line;
}

void dt_dev_release_cached_buf(dt_develop_t *dev, dt_dev_image_t *img, dt_dev_zoom_t zoom)
{
  pthread_mutex_lock(&dev->cache_mutex);
  int32_t line = img->cacheline[zoom];
  dev->cache_used[line] = 0;
  pthread_mutex_unlock(&dev->cache_mutex);
  dt_print(DT_DEBUG_DEV, "[dev_release_buf] line %d for img %d|%d\n", line, img->num, zoom);
}

// helper used to synch a single history item with db
int dt_dev_write_history_item(dt_develop_t *dev, dt_dev_image_t *h, int32_t num)
{
  if(!dev->image) return 1;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1 and num = ?2", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  rc = sqlite3_bind_int (stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    rc = sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "insert into history (imgid, num) values (?1, ?2)", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, dev->image->id);
    rc = sqlite3_bind_int (stmt, 2, num);
    rc = sqlite3_step (stmt);
  }
  rc = sqlite3_finalize (stmt);
  rc = sqlite3_prepare_v2(darktable.db, "update history set operation = ?1, op_params = ?2, settings = ?3, hash = ?4 where imgid = ?5 and num = ?6", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, h->operation, strlen(h->operation), SQLITE_STATIC);
  rc = sqlite3_bind_blob(stmt, 2, &(h->op_params), sizeof(dt_dev_operation_params_t), SQLITE_STATIC);
  rc = sqlite3_bind_blob(stmt, 3, &(h->settings), sizeof(dt_ctl_image_settings_t), SQLITE_STATIC);
  rc = sqlite3_bind_int (stmt, 4, h->num);
  rc = sqlite3_bind_int (stmt, 5, dev->image->id);
  rc = sqlite3_bind_int (stmt, 6, num);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  return 0;
}

void dt_dev_add_history_item(dt_develop_t *dev, dt_dev_operation_t op)
{
  dt_dev_image_t *img = dev->history + dev->history_top - 1;
  if(dev->gui_attached)
  {
    dt_ctl_gui_mode_t gui;
    DT_CTL_GET_GLOBAL(gui, gui);
    if(gui != DT_DEVELOP) return;
    dt_control_clear_history_items(dev->history_top-1);
  }
  // this is called exclusively from the gtk thread, so no lock is necessary.
  int32_t num = dt_dev_update_fixed_pipeline(dev, op, img->num);

  if(strncmp(img->operation, op, 20) != 0)
  {
    dev->history_top++;
    if(dev->gui_attached) dt_control_add_history_item(dev->history_top-1, op);
    for(int k=0;k<3;k++) img[1].cacheline[k] = img[0].cacheline[k];
    img ++;
    if(dev->history_top >= dev->history_max)
    {
      dev->history_max *= 2;
      dt_dev_image_t *tmp = (dt_dev_image_t *)malloc(sizeof(dt_dev_image_t)*dev->history_max);
      memcpy(tmp, dev->history, dev->history_max/2 * sizeof(dt_dev_image_t));
      free(dev->history);
      dev->history = tmp;
    }
  }
  if(dev->gui_attached)
  {
    pthread_mutex_lock(&(darktable.control->image_mutex));
    img->settings = darktable.control->image_settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
  }
  DT_CTL_SET_GLOBAL_STR(dev_op, img->operation, 20);
  DT_CTL_GET_GLOBAL(img->op_params, dev_op_params);
  img->num = num;
  strncpy(img->operation, op, 20);
  dt_print(DT_DEBUG_DEV, "pushing history %d item with hash %d, operation %d and cachelines %d %d %d\n", dev->history_top-1, num, op, img->cacheline[0], img->cacheline[1], img->cacheline[2]);

  dev->small_backbuf_hash = -1;
  dt_dev_update_small_cache(dev);
}

void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt)
{
  // this is called exclusively from the gtk thread, so no lock is necessary.
  if(cnt == dev->history_top-1) return;
  dev->history_top = cnt + 1;
  if(dev->gui_attached)
  {
    pthread_mutex_lock(&(darktable.control->image_mutex));
    darktable.control->image_settings = dev->history[dev->history_top-1].settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
    DT_CTL_SET_GLOBAL_STR(dev_op, dev->history[dev->history_top-1].operation, 20);
    DT_CTL_SET_GLOBAL(dev_op_params, dev->history[dev->history_top-1].op_params);
  }
  (void)dt_dev_update_fixed_pipeline(dev, "original", 0);

  dev->small_backbuf_hash = -1;
  dt_dev_update_small_cache(dev);
}

void dt_dev_write_history(dt_develop_t *dev)
{
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  sqlite3_step(stmt);
  rc = sqlite3_finalize (stmt);
  for(int k=0;k<dev->history_top;k++)
    (void)dt_dev_write_history_item(dev, dev->history+k, k);
}

void dt_dev_read_history(dt_develop_t *dev)
{
  if(dev->gui_attached) dt_control_clear_history_items(0);
  if(!dev->image) return;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select * from history where imgid = ?1 order by num", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  dev->history_top = 0;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      dt_dev_image_t *h = dev->history + dev->history_top;
      h->num = sqlite3_column_int(stmt, 2);
      strncpy(h->operation, (char *)sqlite3_column_text(stmt, 3), 20);
      memcpy(&(h->op_params), sqlite3_column_blob(stmt, 4), sizeof(dt_dev_operation_params_t));
      memcpy(&(h->settings), sqlite3_column_blob(stmt, 5), sizeof(dt_ctl_image_settings_t));
      dev->history_top++;
      if(dev->history_top >= dev->history_max)
      {
        dev->history_max *= 2;
        dt_dev_image_t *tmp = (dt_dev_image_t *)malloc(sizeof(dt_dev_image_t)*dev->history_max);
        memcpy(tmp, dev->history, dev->history_max/2 * sizeof(dt_dev_image_t));
        free(dev->history);
        dev->history = tmp;
      }
      if(dev->gui_attached) dt_control_add_history_item(dev->history_top-1, dev->history[dev->history_top-1].operation);
    }
    while(sqlite3_step(stmt) == SQLITE_ROW);
    rc = sqlite3_finalize (stmt);
  }
  else
  {
    rc = sqlite3_finalize (stmt);
    dev->history_top = 1;
    dev->history[0].num = 1;//dev->image->id << 6;
    for(int k=0;k<3;k++) dev->history[0].cacheline[k] = 0;
    dev->history[0].settings = darktable.control->image_defaults;
    strncpy(dev->history[0].operation, "original", 20);
    if(dev->gui_attached) dt_control_add_history_item(0, "original");
    dt_dev_write_history_item(dev, dev->history, 0);
  }

  if(dev->gui_attached)
  {
    DT_CTL_SET_GLOBAL_STR(dev_op, dev->history[dev->history_top-1].operation, 20);
    DT_CTL_SET_GLOBAL(dev_op_params, dev->history[dev->history_top-1].op_params);
    pthread_mutex_lock(&(darktable.control->image_mutex));
    darktable.control->image_settings = dev->history[dev->history_top - 1].settings;
    pthread_mutex_unlock(&(darktable.control->image_mutex));
  }
  (void)dt_dev_update_fixed_pipeline(dev, "original", 0);
}

void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxww, float *boxhh)
{
  float boxw = 1, boxh = 1;
  if(zoom == DT_ZOOM_1)
  {
    const float imgw = (closeup ? 2 : 1)*dev->image->width;
    const float imgh = (closeup ? 2 : 1)*dev->image->height;
    const float devw = MIN(imgw, dev->cache_width);
    const float devh = MIN(imgh, dev->cache_height);
    boxw = fminf(1.0, devw/imgw); boxh = fminf(1.0, devh/imgh);
  }
  else if(zoom == DT_ZOOM_FILL)
  {
    const float imgw = dev->image->width;
    const float imgh = dev->image->height;
    const float devw = dev->cache_width;
    const float devh = dev->cache_height;
    boxw = devw/(imgw*fmaxf(devw/imgw, devh/imgh)); boxh = devh/(imgh*fmaxf(devw/imgw, devh/imgh));
  }
  
  if(*zoom_x < boxw/2 - .5f) *zoom_x = boxw/2 - .5f;
  if(*zoom_x > .5f - boxw/2) *zoom_x = .5f - boxw/2;
  if(*zoom_y < boxh/2 - .5f) *zoom_y = boxh/2 - .5f;
  if(*zoom_y > .5f - boxh/2) *zoom_y = .5f - boxh/2;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

void dt_dev_export(dt_job_t *job)
{
  while(1)
  {
    // TODO: progress bar in ctl?
    char filename[1024];
    dt_image_t *img = NULL;
    pthread_mutex_lock(&(darktable.library->film->images_mutex));
    int32_t rc, start = darktable.library->film->last_exported++;
    pthread_mutex_unlock(&(darktable.library->film->images_mutex));
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(darktable.db, "select * from (select * from selected_images) as dreggn limit ?1, 1", -1, &stmt, NULL);
    rc = sqlite3_bind_int(stmt, 1, start);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int imgid = sqlite3_column_int(stmt, 0);
      img = dt_image_cache_get(imgid, 'r');
    }
    sqlite3_finalize(stmt);
    if(!img)
    {
      // TODO: reset progress bar
      return;
    }

    snprintf(filename, 1024, "%s/darktable_exported", darktable.library->film->dirname);
    if(g_mkdir_with_parents(filename, 0755))
    {
      dt_image_cache_release(img, 'r');
      fprintf(stderr, "[dev_export] could not create directory %s!\n", filename);
      return;
    }
    snprintf(filename, 1024-4, "%s/darktable_exported/%s", darktable.library->film->dirname, img->filename);
    char *c = filename + strlen(filename);
    for(;c>filename && *c != '.';c--);
    if(c <= filename) c = filename + strlen(filename);

    // read type from global config.
    dt_dev_export_format_t fmt;
    DT_CTL_GET_GLOBAL(fmt, dev_export_format);
    switch(fmt)
    {
      case DT_DEV_EXPORT_JPG:
        strncpy(c, ".jpg", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PNG:
        strncpy(c, ".png", 4);
        dt_imageio_export_8(img, filename);
        break;
      case DT_DEV_EXPORT_PPM16:
        strncpy(c, ".ppm", 4);
        dt_imageio_export_16(img, filename);
        break;
      case DT_DEV_EXPORT_PFM:
        strncpy(c, ".pfm", 4);
        dt_imageio_export_f(img, filename);
        break;
      default:
        break;
    }
    dt_image_cache_release(img, 'r');
    printf("[dev_export] exported to `%s'\n", filename);
  }
}
#endif
