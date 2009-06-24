#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/jobs.h"
#include "control/control.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "gui/gtk.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <assert.h>


uint8_t dt_dev_default_gamma[0x10000];
float dt_dev_de_gamma[0x100];

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
    // int32_t tmp = 0x10000 * powf(k/(float)0x10000, 1./2.2);
    int32_t tmp;
	  if (k<0x10000*linear) tmp = MIN(c*k, 0xFFFF);
	  else tmp = MIN(pow(a*k/0x10000+b, g)*0x10000, 0xFFFF);
    arr[k] = tmp>>8;
  }
}

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  dev->gui_leaving = 0;
  pthread_mutex_init(&dev->history_mutex, NULL);
  dev->history_end = 0;
  dev->history = NULL; // empty list

  dev->gui_attached = gui_attached;
  dev->width = -1;
  dev->height = -1;
  dev->mipf = NULL;

  dev->image = NULL;
  dev->image_dirty = dev->preview_dirty = 
  dev->image_loading = dev->preview_loading = 0;

  dev->pipe = dev->preview_pipe = NULL;
  dev->histogram = dev->histogram_pre = NULL;

  if(dev->gui_attached)
  {
    dev->pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->pipe);
    dt_dev_pixelpipe_init(dev->preview_pipe);

    dev->histogram = (float *)malloc(sizeof(float)*4*256);
    dev->histogram_pre = (float *)malloc(sizeof(float)*4*256);
    bzero(dev->histogram, sizeof(float)*256*4);
    bzero(dev->histogram_pre, sizeof(float)*256*4);
    dev->histogram_max = -1;
    dev->histogram_pre_max = -1;

    float lin, gam;
    DT_CTL_GET_GLOBAL(lin, dev_gamma_linear);
    DT_CTL_GET_GLOBAL(gam, dev_gamma_gamma);
    dt_dev_set_gamma_array(dev, lin, gam, dt_dev_default_gamma);
    int last = 0; // invert
    for(int i=0;i<0x100;i++) for(int k=last;k<0x10000;k++)
      if(dt_dev_default_gamma[k] >= i) { last = k; dt_dev_de_gamma[i] = k/(float)0x10000; break; }
  }
  for(int i=0;i<0x100;i++) dev->gamma[i] = dt_dev_default_gamma[i<<8];

  dev->iop_instance = 0;
  dev->iop = NULL;
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  // image_cache does not have to be unref'd, this is done outside develop module.
  // unref used mipmap buffers:
  if(dev->image)
  {
    dt_image_release(dev->image, DT_IMAGE_FULL, 'w');
    dt_image_release(dev->image, DT_IMAGE_FULL, 'r');
    if(dev->mipf) dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
  }
  if(dev->pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->pipe);
    free(dev->pipe);
  }
  if(dev->preview_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview_pipe);
    free(dev->preview_pipe);
  }
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
  pthread_mutex_destroy(&dev->history_mutex);
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
  if(!dev->image || !dev->image->mipf || !dev->gui_attached) return;
  dt_job_t job;
  dt_dev_process_preview_job_init(&job, dev);
  int err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_3);
  if(err) fprintf(stderr, "[dev_process_preview] job queue exceeded!\n");
}

void dt_dev_invalidate(dt_develop_t *dev)
{
  dev->preview_dirty = dev->image_dirty = 1;
}

void dt_dev_process_preview_job(dt_develop_t *dev)
{
  if(dev->preview_loading)
  { 
    // prefetch and lock
    if(dt_image_get(dev->image, DT_IMAGE_MIPF, 'r') != DT_IMAGE_MIPF) return; // not loaded yet. load will issue a gtk redraw on completion, which in turn will trigger us again later.
    dev->mipf = dev->image->mipf;
    // drop reference again, we were just testing. dev holds one already.
    dt_image_release(dev->image, DT_IMAGE_MIPF, 'r');
    // init pixel pipeline for preview.
    dt_image_get_mip_size(dev->image, DT_IMAGE_MIPF, &dev->mipf_width, &dev->mipf_height);
    dt_image_get_exact_mip_size(dev->image, DT_IMAGE_MIPF, &dev->mipf_exact_width, &dev->mipf_exact_height);
    dt_dev_pixelpipe_set_input(dev->preview_pipe, dev, dev->image->mipf, dev->mipf_width, dev->mipf_height);
    dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
    dev->preview_loading = 0;
  }

  // TODO: always process the whole downsampled mipf buffer, to allow for fast scrolling and mip4 write-through.
  dt_dev_zoom_t zoom;
  float zoom_x, zoom_y;
  int closeup;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);

#if 0
  // FIXME: mipf precise width and actual mip width differ!
  float scale = (closeup?2:1)*dev->image->width/(float)dev->mipf_width;//1:1;
  // roi after scale has been applied:
  if     (zoom == DT_ZOOM_FIT)  scale = fminf(dev->width/(float)dev->mipf_width, dev->height/(float)dev->mipf_height);
  else if(zoom == DT_ZOOM_FILL) scale = fmaxf(dev->width/(float)dev->mipf_width, dev->height/(float)dev->mipf_height);
  dev->capwidth_preview  = MIN(dev->width,  dev->mipf_width *scale);
  dev->capheight_preview = MIN(dev->height, dev->mipf_height*scale);
  int x, y;
  x = scale*dev->mipf_width *(.5+zoom_x)-dev->capwidth_preview/2;
  y = scale*dev->mipf_height*(.5+zoom_y)-dev->capheight_preview/2;
  x      = MAX(0, x);
  y      = MAX(0, y);
#endif
 
  // adjust pipeline according to changed flag set by {add,pop}_history_item.
  // this locks dev->history_mutex.
restart:
  if(dev->gui_leaving) return;
  dt_dev_pixelpipe_change(dev->preview_pipe, dev);
  // if(dt_dev_pixelpipe_process(dev->preview_pipe, dev, x, y, dev->capwidth_preview, dev->capheight_preview, scale)) goto restart;
  if(dt_dev_pixelpipe_process(dev->preview_pipe, dev, 0, 0, dev->mipf_width, dev->mipf_height, 1.0)) goto restart;

  dev->preview_dirty = 0;
  dt_control_queue_draw();
}

void dt_dev_process_image_job(dt_develop_t *dev)
{
  if(dt_image_lock_if_available(dev->image, DT_IMAGE_FULL, 'r'))
  {
    dt_dev_raw_load(dev, dev->image);
  }
  else dt_image_release(dev->image, DT_IMAGE_FULL, 'r'); // raw load already keeps one reference, we were just testing.
  dt_dev_zoom_t zoom;
  float zoom_x, zoom_y;
  int closeup;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);

  float scale = (closeup?2:1);//1:1;
  // roi after scale has been applied:
  if     (zoom == DT_ZOOM_FIT)  scale = fminf(dev->width/(float)dev->image->width, dev->height/(float)dev->image->height);
  else if(zoom == DT_ZOOM_FILL) scale = fmaxf(dev->width/(float)dev->image->width, dev->height/(float)dev->image->height);
  dev->capwidth  = MIN(MIN(dev->width,  dev->image->width *scale), DT_IMAGE_WINDOW_SIZE);
  dev->capheight = MIN(MIN(dev->height, dev->image->height*scale), DT_IMAGE_WINDOW_SIZE);
  int x, y;
  x = MAX(0, scale*dev->image->width *(.5+zoom_x)-dev->capwidth/2);
  y = MAX(0, scale*dev->image->height*(.5+zoom_y)-dev->capheight/2);
 
#ifndef HAVE_GEGL
  // only necessary for full pixels pipeline
  assert(dev->capwidth  <= DT_IMAGE_WINDOW_SIZE);
  assert(dev->capheight <= DT_IMAGE_WINDOW_SIZE);
#endif

  // printf("process: %d %d -> %d %d scale %f\n", x, y, dev->capwidth, dev->capheight, scale);
  // adjust pipeline according to changed flag set by {add,pop}_history_item.
restart:
  if(dev->gui_leaving) return;
  // this locks dev->history_mutex.
  dt_dev_pixelpipe_change(dev->pipe, dev);
  if(dt_dev_pixelpipe_process(dev->pipe, dev, x, y, dev->capwidth, dev->capheight, scale)) goto restart;

  // maybe we got zoomed/panned in the meantime?
  if(dev->pipe->changed != DT_DEV_PIPE_UNCHANGED) goto restart;
  dev->image_dirty = 0;

  dt_control_queue_draw();
}

void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img)
{
  // only load if not already there.
  if(dt_image_lock_if_available(dev->image, DT_IMAGE_FULL, 'r') || dev->image->shrink)
  // if(!dev->image->pixels || dev->image->shrink)
  {
    int err;
restart:
    dev->image_loading = 1;
    dev->image->shrink = 0;
    // not loaded from cache because it is obviously not there yet. so load unshrinked version:
    err = dt_image_load(img, DT_IMAGE_FULL); // load and lock
    if(err) fprintf(stderr, "[dev_raw_load] failed to load image %s!\n", img->filename);

    // obsoleted by another job?
    if(dev->image != img)
    {
      printf("[dev_raw_load] recovering from obsoleted read!\n");
      img = dev->image;
      goto restart;
    }
  }
  if(dev->gui_attached)
  {
    // init pixel pipeline for preview.
    dt_dev_pixelpipe_set_input(dev->pipe, dev, dev->image->pixels, dev->image->width, dev->image->height);
    dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
    dev->image_loading = 0;
    dev->image_dirty = 1;
    // during load, a mipf update could have been issued.
    dt_dev_pixelpipe_flush_caches(dev->preview_pipe);
    dt_dev_process_image(dev);
  }
}

void dt_dev_load_image(dt_develop_t *dev, dt_image_t *image)
{
  dev->image = image;
  dev->image_loading = dev->preview_loading = 1;
  if(dt_image_get(dev->image, DT_IMAGE_MIPF, 'r') == DT_IMAGE_MIPF) dev->mipf = dev->image->mipf; // prefetch and lock
  else dev->mipf = NULL;
  dev->image_dirty = dev->preview_dirty = 1;

  // TODO: reset view to fit.

  // TODO: load modules for this image in read history!
  dt_iop_module_t *module;
  dev->iop_instance = 0;

  module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, dev, "tonecurve")) exit(1);
  dev->iop = g_list_append(dev->iop, module);

  module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, dev, "gamma")) exit(1);
  dev->iop = g_list_append(dev->iop, module);

  // TODO: this should read modules on demand!
  dt_dev_read_history(dev);
  if(dev->gui_attached)
  {
    dt_job_t job;
    dt_dev_raw_load_job_init(&job, dev, dev->image);
    int32_t err = dt_control_add_job_res(darktable.control, &job, DT_CTL_WORKER_1);
    if(err) fprintf(stderr, "[dev_load_image] job queue exceeded!\n");
  }
  else dt_dev_raw_load(dev, dev->image); // in this thread.
}

gboolean dt_dev_configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  dt_develop_t *dev = darktable.develop;
  int tb = darktable.control->tabborder;
  if(dev->width - 2*tb != event->width || dev->height - 2*tb != event->height)
  {
    dev->width = event->width - 2*tb;
    dev->height = event->height - 2*tb;
    dev->preview_pipe->changed = dev->pipe->changed = DT_DEV_PIPE_ZOOMED;
    dt_dev_invalidate(dev);
  }
  return TRUE;
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
  // printf("[dev write history item] writing %d - %s params %f %f\n", h->module->instance, h->module->op, *(float *)h->params, *(((float *)h->params)+1));
  rc = sqlite3_finalize (stmt);
  rc = sqlite3_prepare_v2(darktable.db, "update history set operation = ?1, op_params = ?2, module = ?3, enabled = ?4 where imgid = ?5 and num = ?6", -1, &stmt, NULL);
  rc = sqlite3_bind_text(stmt, 1, h->module->op, strlen(h->module->op), SQLITE_TRANSIENT);
  rc = sqlite3_bind_blob(stmt, 2, h->params, h->module->params_size, SQLITE_TRANSIENT);
  rc = sqlite3_bind_int (stmt, 3, h->module->instance);
  rc = sqlite3_bind_int (stmt, 4, h->enabled);
  rc = sqlite3_bind_int (stmt, 5, dev->image->id);
  rc = sqlite3_bind_int (stmt, 6, num);
  rc = sqlite3_step (stmt);
  rc = sqlite3_finalize (stmt);
  return 0;
}

void dt_dev_add_history_item(dt_develop_t *dev, dt_iop_module_t *module)
{
  pthread_mutex_lock(&dev->history_mutex);
  if(dev->gui_attached)
  {
    // if gui_attached pop all operations down to dev->history_end
    dt_control_clear_history_items(dev->history_end-1);
    // remove unused history items:
    GList *history = g_list_nth(dev->history, dev->history_end);
    while(history)
    {
      GList *next = g_list_next(history);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      // printf("removing obsoleted history item: %s\n", hist->module->op);
      free(hist->params);
      dev->history = g_list_delete_link(dev->history, history);
      history = next;
    }
    // TODO: modules should be add/removable!
#if 0 // disabled while there is only tonecurve
    // remove all modules which are not in history anymore:
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      history = dev->history;
      while(history)
      {
        // for all modules in list: go through remaining history. if found, keep it.
        dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
        if(hist->iop == module->instance)
        {
          modules = g_list_next(modules);
          break;
        }
        history = g_list_next(history);
      }
      GList *next = g_list_next(modules);
      module->gui_cleanup(module);
      module->cleanup(module);
      dev->iop = g_list_delete_link(dev->iop, modules);
      modules = *next;
    }
#endif
    /*
    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
      printf("module instance = %d, op = %s\n", module->instance, module->op);
      modules = g_list_next(modules);
    }*/
    history = g_list_nth(dev->history, dev->history_end-1);
    if(!history || module->instance != ((dt_dev_history_item_t *)history->data)->module->instance)
    { // new operation, push new item
      // printf("adding new history item %d - %s\n", dev->history_end, module->op);
      // if(history) printf("because item %d - %s is different operation.\n", dev->history_end-1, ((dt_dev_history_item_t *)history->data)->module->op);
      dev->history_end++;
      if(dev->gui_attached) dt_control_add_history_item(dev->history_end-1, module->op);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));
      hist->enabled = 1;
      hist->module = module;
      hist->params = malloc(module->params_size);
      memcpy(hist->params, module->params, module->params_size);
      dev->history = g_list_append(dev->history, hist);
      dev->pipe->changed = dev->preview_pipe->changed = DT_DEV_PIPE_SYNCH; // topology remains, as modules are fixed for now.
    }
    else
    { // same operation, change params
      // printf("changing same history item %d - %s\n", dev->history_end-1, module->op);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      memcpy(hist->params, module->params, module->params_size);
      dev->pipe->changed = dev->preview_pipe->changed = DT_DEV_PIPE_TOP_CHANGED;
    }
  }
#if 0
  {
  // debug:
  printf("remaining %d history items:\n", dev->history_end);
  GList *history = dev->history;
  int i = 0;
  while(history)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    printf("%d %s\n", i, hist->module->op);
    history = g_list_next(history);
    i++;
  }
  }
#endif

  pthread_mutex_unlock(&dev->history_mutex);

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate(dev);
  dt_control_queue_draw();
}

void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt)
{
  // printf("dev popping all history items >= %d\n", cnt);
  pthread_mutex_lock(&dev->history_mutex);
  darktable.gui->reset = 1;
  dev->history_end = cnt;
  dev->pipe->changed = dev->preview_pipe->changed = DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
  // reset gui params for all modules
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    memcpy(module->params, module->default_params, module->params_size);
    modules = g_list_next(modules);
  }
  // go through history and set gui params
  GList *history = dev->history;
  for(int i=0;i<cnt && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    memcpy(hist->module->params, hist->params, hist->module->params_size);
    history = g_list_next(history);
  }
  // update all gui modules
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    module->gui_update(module);
    modules = g_list_next(modules);
  }
  darktable.gui->reset = 0;
  pthread_mutex_unlock(&dev->history_mutex);
  dt_dev_invalidate(dev);
  dt_control_queue_draw();
}

void dt_dev_write_history(dt_develop_t *dev)
{
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "delete from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  sqlite3_step(stmt);
  rc = sqlite3_finalize (stmt);
  GList *history = dev->history;
  for(int i=0;i<dev->history_end && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    (void)dt_dev_write_history_item(dev, hist, i);
    history = g_list_next(history);
  }
}

void dt_dev_read_history(dt_develop_t *dev)
{
  // TODO: on demand loading of modules!
  if(dev->gui_attached) dt_control_clear_history_items(0);
  if(!dev->image) return;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select * from history where imgid = ?1 order by num", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, dev->image->id);
  dev->history_end = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-num, 2-module_instance, 3-operation char, 4-params blob, 5-enabled
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));
    hist->enabled = sqlite3_column_int(stmt, 5);
    int instance = sqlite3_column_int(stmt, 2);
    // FIXME: this is static pipeline: TODO: load module "operation" and insert in glist, get instance and set here!
    GList *modules = g_list_nth(dev->iop, instance);
    assert(modules);
    hist->module = (dt_iop_module_t *)modules->data;
    assert(strcmp((char *)sqlite3_column_text(stmt, 3), hist->module->op) == 0);
    hist->params = malloc(hist->module->params_size);
    memcpy(hist->params, sqlite3_column_blob(stmt, 4), hist->module->params_size);
    assert(hist->module->params_size == sqlite3_column_bytes(stmt, 4));
    // printf("[dev read history] img %d number %d for operation %d - %s params %f %f\n", sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1), instance, hist->module->op, *(float *)hist->params, *(((float*)hist->params)+1));
    dev->history = g_list_append(dev->history, hist);
    dev->history_end ++;

    if(dev->gui_attached) dt_control_add_history_item(dev->history_end-1, hist->module->op);
  }
  rc = sqlite3_finalize (stmt);
}

void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxww, float *boxhh)
{
  float boxw = 1, boxh = 1;
  if(zoom == DT_ZOOM_1)
  {
    const float imgw = (closeup ? 2 : 1)*dev->image->width;
    const float imgh = (closeup ? 2 : 1)*dev->image->height;
    const float devw = MIN(imgw, dev->width);
    const float devh = MIN(imgh, dev->height);
    boxw = fminf(1.0, devw/imgw); boxh = fminf(1.0, devh/imgh);
  }
  else if(zoom == DT_ZOOM_FILL)
  {
    const float imgw = dev->image->width;
    const float imgh = dev->image->height;
    const float devw = dev->width;
    const float devh = dev->height;
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

