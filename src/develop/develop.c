/*
    This file is part of darktable,
    Copyright (C) 2009-2025 darktable developers.

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

#include <assert.h>
#include <glib/gprintf.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "common/atomic.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "common/tags.h"
#include "common/presets.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightroom.h"
#include "develop/masks.h"
#include "libs/modulegroups.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "imageio/imageio_common.h"

#ifdef USE_LUA
#include "lua/call.h"
#endif

#define DT_DEV_AVERAGE_DELAY_COUNT 5

void dt_dev_init(dt_develop_t *dev,
                 const gboolean gui_attached)
{
  memset(dev, 0, sizeof(dt_develop_t));
  dev->full_preview = FALSE;
  dev->gui_module = NULL;
  dev->timestamp = 0;
  dev->gui_leaving = FALSE;
  dev->gui_synch = FALSE;

  pthread_mutexattr_t recursive_locking;
  pthread_mutexattr_init(&recursive_locking);
  pthread_mutexattr_settype(&recursive_locking, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&dev->history_mutex, &recursive_locking);

  dev->snapshot_id = -1;
  dev->history_end = 0;
  dev->history = NULL; // empty list
  dev->history_postpone_invalidate = FALSE;
  dev->module_filter_out = NULL;

  dev->gui_attached = gui_attached;
  dev->full.width = -1;
  dev->full.height = -1;

  dt_image_init(&dev->image_storage);
  dev->history_updating = dev->image_force_reload = FALSE;
  dev->autosaving = FALSE;
  dev->autosave_time = 0.0;
  dev->image_invalid_cnt = 0;
  dev->full.pipe = dev->preview_pipe = dev->preview2.pipe = NULL;
  dev->histogram_pre_tonecurve = NULL;
  dev->histogram_pre_levels = NULL;
  dev->forms = NULL;
  dev->form_visible = NULL;
  dev->form_gui = NULL;
  dev->allforms = NULL;

  if(dev->gui_attached)
  {
    dev->full.pipe = malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview2.pipe = malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->full.pipe);
    dt_dev_pixelpipe_init_preview(dev->preview_pipe);
    dt_dev_pixelpipe_init_preview2(dev->preview2.pipe);
    dev->histogram_pre_tonecurve = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_levels = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));

    // FIXME: these are uint32_t, setting to -1 is confusing
    dev->histogram_pre_tonecurve_max = -1;
    dev->histogram_pre_levels_max = -1;
    dev->darkroom_mouse_in_center_area = FALSE;
    dev->darkroom_skip_mouse_events = FALSE;

    if(darktable.gui)
    {
      dev->full.ppd = darktable.gui->ppd;
      dev->full.dpi = darktable.gui->dpi;
      dev->full.dpi_factor = darktable.gui->dpi_factor;
      dev->full.widget = dt_ui_center(darktable.gui->ui);
    }
  }

  dev->iop_instance = 0;
  dev->iop = NULL;
  dev->alliop = NULL;

  dev->allprofile_info = NULL;

  dev->iop_order_version = 0;
  dev->iop_order_list = NULL;

  dev->proxy.exposure.module = NULL;

  dt_dev_init_chroma(dev);

  dev->rawoverexposed.enabled = FALSE;
  dev->rawoverexposed.mode =
    dt_conf_get_int("darkroom/ui/rawoverexposed/mode");
  dev->rawoverexposed.colorscheme =
    dt_conf_get_int("darkroom/ui/rawoverexposed/colorscheme");
  dev->rawoverexposed.threshold =
    dt_conf_get_float("darkroom/ui/rawoverexposed/threshold");

  dev->overexposed.enabled = FALSE;
  dev->overexposed.mode = dt_conf_get_int("darkroom/ui/overexposed/mode");
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");

  dev->full.iso_12646 = dt_conf_get_bool("full_window/iso_12646");
  dev->preview2.iso_12646 = dt_conf_get_bool("second_window/iso_12646");

  dev->full.zoom = dev->preview2.zoom = DT_ZOOM_FIT;
  dev->full.closeup = dev->preview2.closeup = 0;
  dev->full.zoom_x = dev->full.zoom_y = dev->preview2.zoom_x = dev->preview2.zoom_y = 0.0f;
  dev->full.zoom_scale = dev->preview2.zoom_scale = 1.0f;
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  if(!dev) return;
  // image_cache does not have to be unref'd, this is done outside develop module.
  dt_dev_init_chroma(dev);

  if(dev->full.pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->full.pipe);
    free(dev->full.pipe);
  }
  if(dev->preview_pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview_pipe);
    free(dev->preview_pipe);
  }
  if(dev->preview2.pipe)
  {
    dt_dev_pixelpipe_cleanup(dev->preview2.pipe);
    free(dev->preview2.pipe);
  }
  while(dev->history)
  {
    dt_dev_free_history_item(((dt_dev_history_item_t *)dev->history->data));
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->iop->data);
    free(dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  while(dev->alliop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->alliop->data);
    free(dev->alliop->data);
    dev->alliop = g_list_delete_link(dev->alliop, dev->alliop);
  }
  g_list_free_full(dev->iop_order_list, free);
  while(dev->allprofile_info)
  {
    dt_ioppr_cleanup_profile_info
      ((dt_iop_order_iccprofile_info_t *)dev->allprofile_info->data);
    dt_free_align(dev->allprofile_info->data);
    dev->allprofile_info = g_list_delete_link(dev->allprofile_info, dev->allprofile_info);
  }
  dt_pthread_mutex_destroy(&dev->history_mutex);
  free(dev->histogram_pre_tonecurve);
  free(dev->histogram_pre_levels);

  g_list_free_full(dev->forms, (void (*)(void *))dt_masks_free_form);
  g_list_free_full(dev->allforms, (void (*)(void *))dt_masks_free_form);

  dt_conf_set_int("darkroom/ui/rawoverexposed/mode",
                  dev->rawoverexposed.mode);
  dt_conf_set_int("darkroom/ui/rawoverexposed/colorscheme",
                  dev->rawoverexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/rawoverexposed/threshold",
                    dev->rawoverexposed.threshold);

  dt_conf_set_int("darkroom/ui/overexposed/mode", dev->overexposed.mode);
  dt_conf_set_int("darkroom/ui/overexposed/colorscheme", dev->overexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/overexposed/lower", dev->overexposed.lower);
  dt_conf_set_float("darkroom/ui/overexposed/upper", dev->overexposed.upper);

  g_list_free(dev->module_filter_out);
}

void dt_dev_process_image(dt_develop_t *dev)
{
  if(!dev->gui_attached || dev->full.pipe->processing) return;
  const gboolean err
      = dt_control_add_job_res(darktable.control,
                               dt_dev_process_image_job_create(dev), DT_CTL_WORKER_ZOOM_1);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_image] job queue exceeded!");
}

void dt_dev_process_preview(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  const gboolean err = dt_control_add_job_res(darktable.control,
                                         dt_dev_process_preview_job_create(dev),
                                         DT_CTL_WORKER_ZOOM_FILL);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_preview] job queue exceeded!");
}

void dt_dev_process_preview2(dt_develop_t *dev)
{
  const gboolean err = dt_control_add_job_res(darktable.control,
                                         dt_dev_process_preview2_job_create(dev),
                                         DT_CTL_WORKER_ZOOM_2);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[dev_process_preview2] job queue exceeded!");
}

void dt_dev_invalidate(dt_develop_t *dev)
{
  dev->full.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
  if(dev->preview_pipe)
    dev->preview_pipe->input_timestamp = dev->timestamp;
  if(dev->preview2.pipe)
    dev->preview2.pipe->input_timestamp = dev->timestamp;
}

void dt_dev_invalidate_all(dt_develop_t *dev)
{
  if(dev->full.pipe)
    dev->full.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  if(dev->preview_pipe)
    dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  if(dev->preview2.pipe)
    dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
}

void dt_dev_invalidate_preview(dt_develop_t *dev)
{
  dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
  if(dev->full.pipe)
    dev->full.pipe->input_timestamp = dev->timestamp;
  if(dev->preview2.pipe)
    dev->preview2.pipe->input_timestamp = dev->timestamp;
}

static void _dev_average_delay_update(const dt_times_t *start,
                                      uint32_t *average_delay)
{
  dt_times_t end;
  dt_get_times(&end);

  *average_delay += ((end.clock - start->clock) * 1000 / DT_DEV_AVERAGE_DELAY_COUNT
                     - *average_delay / DT_DEV_AVERAGE_DELAY_COUNT);
}

void dt_dev_process_image_job(dt_develop_t *dev,
                              dt_dev_viewport_t *port,
                              dt_dev_pixelpipe_t *pipe,
                              dt_signal_t signal,
                              const int devid)
{
  if(dev->full.pipe->loading && pipe != dev->full.pipe)
  {
    // raw is already loading, no use starting another file access, we wait.
    return;
  }

  if(port == &dev->preview2 && !(port->widget && GTK_IS_WIDGET(port->widget)))
  {
    return;
  }

  dt_pthread_mutex_lock(&pipe->mutex);

  if(dev->gui_leaving)
  {
    dt_pthread_mutex_unlock(&pipe->mutex);
    return;
  }

  dt_control_log_busy_enter();
  dt_control_toast_busy_enter();
  pipe->input_timestamp = dev->timestamp;
  // let gui know to draw preview instead of us, if it's there:
  pipe->status = DT_DEV_PIXELPIPE_RUNNING;

  dt_times_t start;
  dt_get_perf_times(&start);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache,
                      &buf, dev->image_storage.id,
                      port ? DT_MIPMAP_FULL     : DT_MIPMAP_F,
                      port ? DT_MIPMAP_BLOCKING : DT_MIPMAP_BEST_EFFORT,
                      'r');
  dev->image_storage.load_status = buf.loader_status;

  dt_show_times(&start, "[dt_dev_process_image_job] loading image.");

  // failed to load raw?
  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    pipe->status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&pipe->mutex);
    dev->image_invalid_cnt++;
    return; // not loaded yet. load will issue a gtk redraw on
            // completion, which in turn will trigger us again later.
  }

  dt_dev_pixelpipe_set_input(pipe, dev, (float *)buf.buf, buf.width, buf.height,
                             port ? 1.0 : buf.iscale);

  // We require calculation of pixelpipe dimensions via dt_dev_pixelpipe_change() in these cases
  const gboolean initial = pipe->loading || dev->image_force_reload || pipe->input_changed;

  if(pipe->loading)
  {
    // init pixel pipeline
    dt_pthread_mutex_lock(&dev->history_mutex);
    dt_dev_pixelpipe_cleanup_nodes(pipe);
    dt_dev_pixelpipe_create_nodes(pipe, dev);
    dt_pthread_mutex_unlock(&dev->history_mutex);
    if(pipe == dev->full.pipe)
    {
      if(dev->image_force_reload) dt_dev_pixelpipe_cache_flush(pipe);
      dev->image_force_reload = FALSE;
      if(dev->gui_attached)
      {
        // during load, a mipf update could have been issued.
        dev->preview_pipe->input_changed = TRUE;
        dev->preview_pipe->status = DT_DEV_PIXELPIPE_DIRTY;
        dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
        dev->preview2.pipe->input_changed = TRUE;
        dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
        dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH;
        dev->gui_synch = TRUE; // notify gui thread we want to synch
                               // (call gui_update on the modules)
      }
      pipe->changed |= DT_DEV_PIPE_SYNCH;
    }
    else
    {
      dt_dev_pixelpipe_cache_flush(pipe);
      pipe->loading = FALSE;
    }
  }
  if(port != &dev->full && pipe->input_changed)
  {
    dt_dev_pixelpipe_cache_flush(pipe);
    pipe->input_changed = FALSE;
  }

// adjust pipeline according to changed flag set by {add,pop}_history_item.
restart:
  if(dev->gui_leaving)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
    pipe->status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&pipe->mutex);
    return;
  }

  if(port == &dev->full)
    pipe->input_timestamp = dev->timestamp;

  const gboolean pipe_changed = pipe->changed != DT_DEV_PIPE_UNCHANGED;
  // dt_dev_pixelpipe_change() locks history mutex while syncing nodes and finally calculates dimensions
  if(pipe_changed || initial || (port && port->pipe->loading))
    dt_dev_pixelpipe_change(pipe, dev);

  float scale = 1.0f;
  int window_width = G_MAXINT;
  int window_height = G_MAXINT;
  float zoom_x  = 0;
  float zoom_y = 0;

  if(port)
  {
    // if just changed to an image with a different aspect ratio or
    // altered image orientation, the prior zoom xy could now be beyond
    // the image boundary
    if(port->pipe->loading || pipe_changed)
      dt_dev_zoom_move(port, DT_ZOOM_MOVE, 0.0f, 0, 0.0f, 0.0f, TRUE);

    // determine scale according to new dimensions
    dt_dev_zoom_t zoom;
    int closeup;
    dt_dev_get_viewport_params(port, &zoom, &closeup, &zoom_x, &zoom_y);
    scale = dt_dev_get_zoom_scale(port, zoom, 1.0f, 0) * port->ppd;
    window_width = port->width * port->ppd / (1<<closeup);
    window_height = port->height * port->ppd / (1<<closeup);
  }
  // else
  // {
  //   // FIXME full pipe may be busy, so update processed sizes here
  //   // make sure preview pipe is newer than full/preview2 pipes
  //   dev->full.pipe->processed_width = pipe->processed_width * pipe->iscale;
  //   dev->full.pipe->processed_height = pipe->processed_height * pipe->iscale;
  // }

  const int wd = MIN(window_width, scale * pipe->processed_width);
  const int ht = MIN(window_height, scale * pipe->processed_height);
  const int x = port ? MAX(0, scale * pipe->processed_width  * (.5 + zoom_x) - wd / 2) : 0;
  const int y = port ? MAX(0, scale * pipe->processed_height * (.5 + zoom_y) - ht / 2) : 0;

  dt_get_times(&start);

  if(dt_dev_pixelpipe_process(pipe, dev, x, y, wd, ht, scale, devid))
  {
    // interrupted because image changed?
    if(dev->image_force_reload || pipe->loading || pipe->input_changed)
    {
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      dt_control_log_busy_leave();
      dt_control_toast_busy_leave();
      pipe->status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&pipe->mutex);
      return;
    }
    // or because the pipeline changed?
    else
    {
      if(port && port->widget) dt_control_queue_redraw_widget(port->widget);
      goto restart;
    }
  }
  dt_show_times_f(&start,
                  "[dev_process_image] pixel pipeline", "processing `%s'",
                  dev->image_storage.filename);
  _dev_average_delay_update(&start, &pipe->average_delay);

  // maybe we got zoomed/panned in the meantime?
  if(port && pipe->changed != DT_DEV_PIPE_UNCHANGED) goto restart;

  pipe->status = DT_DEV_PIXELPIPE_VALID;
  pipe->loading = FALSE;
  dev->image_invalid_cnt = 0;
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  // if a widget needs to be redraw there's the DT_SIGNAL_*_PIPE_FINISHED signals
  dt_control_log_busy_leave();
  dt_control_toast_busy_leave();
  dt_pthread_mutex_unlock(&pipe->mutex);

  if(dev->gui_attached && !dev->gui_leaving && signal != -1)
    DT_CONTROL_SIGNAL_RAISE(signal);

  if(port) return;

  // preview pipe only

  if(!dev->history_postpone_invalidate)
    dt_image_update_final_size(dev->preview_pipe->output_imgid);

  dev->gui_previous_pipe_time = dt_get_wtime();

#ifdef USE_LUA
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "pixelpipe-processing-complete",
      LUA_ASYNC_TYPENAME, "dt_lua_image_t", GINT_TO_POINTER(dev->image_storage.id),
      LUA_ASYNC_DONE);
#endif
}


static inline void _dt_dev_load_pipeline_defaults(dt_develop_t *dev)
{
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = modules->data;
    dt_iop_reload_defaults(module);
  }
}

// load the raw and get the new image struct, blocking in gui thread
static inline void _dt_dev_load_raw(dt_develop_t *dev,
                                    const dt_imgid_t imgid)
{
  // first load the raw, to make sure dt_image_t will contain all and correct data.
  dt_mipmap_buffer_t buf;
  dt_times_t start;
  dt_get_perf_times(&start);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  dt_show_times(&start, "[dt_dev_load_raw] loading the image.");

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dev->image_storage = *image;
  dt_image_cache_read_release(darktable.image_cache, image);

//  dev->requested_id = (dev->image_storage.load_status == DT_IMAGEIO_OK) ? dev->image_storage.id : 0;
  dev->requested_id = dev->image_storage.id;
}

void dt_dev_reload_image(dt_develop_t *dev,
                         const dt_imgid_t imgid)
{
  _dt_dev_load_raw(dev, imgid);
  dev->image_force_reload = TRUE;
  dev->full.pipe->loading = dev->preview_pipe->loading = dev->preview2.pipe->loading = TRUE;
  dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate(dev); // only invalidate image, preview will follow once it's loaded.
}

float dt_dev_get_zoom_scale(dt_dev_viewport_t *port,
                            dt_dev_zoom_t zoom,
                            const int closeup_factor,
                            const int preview)
{
  float zoom_scale;

  int procw, proch;
  dt_dev_get_processed_size(port, &procw, &proch);

  const float w = (float)port->width / procw;
  const float h = (float)port->height / proch;

  switch(zoom)
  {
    case DT_ZOOM_FIT:
      zoom_scale = fminf(w, h);
      break;
    case DT_ZOOM_FILL:
      zoom_scale = fmaxf(w, h);
      break;
    case DT_ZOOM_1:
      zoom_scale = closeup_factor;
      break;
    default: // DT_ZOOM_FREE
      zoom_scale = port->zoom_scale;
      break;
  }

  if(!zoom_scale) zoom_scale = 1.0f;

  if(preview && darktable.develop->preview_pipe->processed_width)
    zoom_scale *= (float)darktable.develop->full.pipe->processed_width
                  / darktable.develop->preview_pipe->processed_width;

  return zoom_scale;
}

float dt_dev_get_zoom_scale_full(void)
{
  dt_dev_zoom_t zoom;
  int closeup;
  dt_dev_get_viewport_params(&darktable.develop->full, &zoom, &closeup, NULL, NULL);
  const float zoom_scale = dt_dev_get_zoom_scale(&darktable.develop->full, zoom, 1 << closeup, 1);

  return zoom_scale;
}

float dt_dev_get_zoomed_in(void)
{
  dt_dev_zoom_t zoom;
  int closeup;
  dt_dev_get_viewport_params(&darktable.develop->full, &zoom, &closeup, NULL, NULL);
  const float min_scale = dt_dev_get_zoom_scale(&darktable.develop->full, DT_ZOOM_FIT, 1<<closeup, 0);
  const float cur_scale = dt_dev_get_zoom_scale(&darktable.develop->full, zoom, 1<<closeup, 0);

  return cur_scale / min_scale;
}

void dt_dev_load_image(dt_develop_t *dev,
                       const dt_imgid_t imgid)
{
  dt_lock_image(imgid);

  _dt_dev_load_raw(dev, imgid);

  if(dev->full.pipe)
  {
    dev->full.pipe->processed_width = 0;
    dev->full.pipe->processed_height = 0;
    dev->full.pipe->loading = dev->preview_pipe->loading = dev->preview2.pipe->loading = TRUE;
    dev->full.pipe->status = dev->preview_pipe->status = dev->preview2.pipe->status = DT_DEV_PIXELPIPE_DIRTY;
  }
  dev->first_load = TRUE;

  // we need a global lock as the dev->iop set must not be changed
  // until read history is terminated
  dt_pthread_mutex_lock(&darktable.dev_threadsafe);
  dev->iop = dt_iop_load_modules(dev);

  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);
  dt_pthread_mutex_unlock(&darktable.dev_threadsafe);

  dev->first_load = FALSE;

  dt_unlock_image(imgid);
}

void dt_dev_configure(dt_dev_viewport_t *port)
{
  int32_t tb = 0;
  if(port->iso_12646)
  {
    // the border size is taken from conf as an absolute in cm
    // and uses dpi and ppd for an absolute size
    const int bsize = port->dpi * port->ppd * dt_conf_get_float("darkroom/ui/iso12464_border") / 2.54f;
    // for safety, at least 2 pixels and at least 40% for content
    tb = MIN(MAX(2, bsize), 0.3f * MIN(port->orig_width, port->orig_height));
  }
  else if(port == &darktable.develop->full)
  {
    // Reset border size from config
    tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  port->border_size = tb;
  // fixed border on every side
  int32_t wd = port->orig_width - 2*tb;
  int32_t ht = port->orig_height - 2*tb;
  if(port->width != wd || port->height != ht)
  {
    port->width = wd;
    port->height = ht;
    port->pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dt_dev_zoom_move(port, DT_ZOOM_MOVE, 0.0f, TRUE, 0.0f, 0.0f, TRUE);
  }
}

// helper used to synch a single history item with db
static void _dev_write_history_item(const dt_imgid_t imgid,
                                    dt_dev_history_item_t *h,
                                    const int32_t num)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT num FROM main.history WHERE imgid = ?1 AND num = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT INTO main.history (imgid, num) VALUES (?1, ?2)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    sqlite3_step(stmt);
  }

  sqlite3_finalize(stmt);
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "UPDATE main.history"
     " SET operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
     "     blendop_params = ?7, blendop_version = ?8, multi_priority = ?9,"
     "     multi_name = ?10, multi_name_hand_edited = ?11"
     " WHERE imgid = ?5 AND num = ?6",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, h->module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, h->params, h->module->params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, h->module->version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, h->enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, h->blend_params,
                             sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, h->multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, h->multi_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, h->multi_name_hand_edited);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // write masks (if any)
  for(GList *forms = h->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = forms->data;
    if(form)
      dt_masks_write_masks_history_item(imgid, num, form);
  }
}

static void _dev_auto_save(dt_develop_t *dev)
{
  const double user_delay = (double)dt_conf_get_int("autosave_interval");
  const dt_imgid_t imgid = dev->image_storage.id;

  /* We can only autosave database & xmp while we have a valid image id
     and we are not currently loading or changing it in main darkroom
  */
  const double start = dt_get_wtime();
  const gboolean saving = (user_delay >= 1.0)
                        && ((start - dev->autosave_time) > user_delay)
                        && !dev->full.pipe->loading
                        && dev->requested_id == imgid
                        && dt_is_valid_imgid(imgid);

  if(saving)
  {
    dt_dev_write_history(dev);
    dt_image_synch_xmp(imgid);
    const double after = dt_get_wtime();
    dev->autosave_time = after;
    // if writing to database and the xmp took too long we disable
    // autosaving mode for this image
    if((after - start) > 0.5)
    {
      dev->autosaving = FALSE;
      dt_print(DT_DEBUG_DEV, "autosave history disabled, took %.3fs", after - start);

      dt_control_log(_("autosaving history has been disabled for this image"
                       " because of a very large history or a slow drive being used"));
    }
  }
}

static void _dev_auto_module_label(dt_develop_t *dev,
                                   dt_iop_module_t *module)
{
  // adjust the label to match presets if possible or otherwise the default
  // multi_name for this module.
  if(!dt_iop_is_hidden(module)
    && !module->multi_name_hand_edited
    && dt_conf_get_bool("darkroom/ui/auto_module_name_update"))
  {
    const gboolean is_default_params =
      memcmp(module->params, module->default_params, module->params_size) == 0;

    char *preset_name = dt_presets_get_module_label
      (module->op,
       module->params, module->params_size, is_default_params,
       module->blend_params, sizeof(dt_develop_blend_params_t));

    // if we have a preset-name, use it. otherwise set the label to the multi-priority
    // except for 0 where the multi-name is cleared.
    if(preset_name)
      snprintf(module->multi_name,
               sizeof(module->multi_name), "%s", preset_name);
    else if(module->multi_priority != 0)
      snprintf(module->multi_name,
               sizeof(module->multi_name), "%d", module->multi_priority);
    else
      g_strlcpy(module->multi_name, "", sizeof(module->multi_name));

    g_free(preset_name);

    if(dev->gui_attached)
      dt_iop_gui_update_header(module);
  }
}

static void _dev_add_history_item_ext(dt_develop_t *dev,
                                      dt_iop_module_t *module,
                                      const gboolean enable,
                                      const gboolean new_item,
                                      const gboolean no_image,
                                      const gboolean include_masks,
                                      const gboolean auto_name_module)
{
  // try to auto-name the module based on the presets if possible
  if(auto_name_module)
  {
    _dev_auto_module_label(dev, module);
  }

  int kept_module = 0;
  GList *history = g_list_nth(dev->history, dev->history_end);
  // look for leaks on top of history in two steps
  // first remove obsolete items above history_end
  // but keep the always-on modules
  while(history)
  {
    GList *next = g_list_next(history);
    dt_dev_history_item_t *hist = history->data;
    // printf("removing obsoleted history item: %s\n", hist->module->op);

    //check if an earlier instance of the module exists
    gboolean earlier_entry = FALSE;
    GList *prior_history = g_list_nth(dev->history, dev->history_end - 1);
    while(prior_history)
    {
      dt_dev_history_item_t *prior_hist = prior_history->data;
      if(prior_hist->module->so == hist->module->so)
      {
        earlier_entry = TRUE;
        break;
      }
      prior_history = g_list_previous(prior_history);
    }

    if((!hist->module->hide_enable_button && !hist->module->default_enabled)
        || earlier_entry)
    {
      dt_dev_free_history_item(hist);
      dev->history = g_list_delete_link(dev->history, history);
    }
    else
      kept_module++;
    history = next;
  }
  // then remove NIL items there
  while((dev->history_end>0) && (! g_list_nth(dev->history, dev->history_end - 1)))
    dev->history_end--;

  dev->history_end += kept_module;

  history = g_list_nth(dev->history, dev->history_end - 1);
  dt_dev_history_item_t *hist = history ? (dt_dev_history_item_t *)(history->data) : 0;

  // if module should be enabled, do it now
  if(enable)
  {
    module->enabled = TRUE;
    if(!no_image)
    {
      if(module->off)
      {
        ++darktable.gui->reset;
        dt_iop_gui_set_enable_button(module);
        --darktable.gui->reset;
      }
    }
  }

  if(!history                                              // no history yet, push new item
     || new_item                                           // a new item is requested
     || module != hist->module
     || module->instance != hist->module->instance         // add new item for different op
     || module->multi_priority != hist->module->multi_priority // or instance
     || ((dev->focus_hash != hist->focus_hash)                 // or if focused out and in
         // but only add item if there is a difference at all for the same module
         && ((module->params_size != hist->module->params_size)
             || include_masks
             || (module->params_size == hist->module->params_size
                 && memcmp(hist->params, module->params, module->params_size)))))
  {
    // new operation, push new item
    dev->history_end++;

    hist = calloc(1, sizeof(dt_dev_history_item_t));

    g_strlcpy(hist->op_name, module->op, sizeof(hist->op_name));
    hist->focus_hash = dev->focus_hash;
    hist->enabled = module->enabled;
    hist->module = module;
    hist->params = malloc(module->params_size);
    hist->iop_order = module->iop_order;
    hist->multi_priority = module->multi_priority;
    hist->multi_name_hand_edited = module->multi_name_hand_edited;
    g_strlcpy(hist->multi_name, module->multi_name, sizeof(hist->multi_name));
    /* allocate and set hist blend_params */
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
    memcpy(hist->params, module->params, module->params_size);
    memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));
    if(include_masks)
      hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
    else
      hist->forms = NULL;

    dev->history = g_list_append(dev->history, hist);
    if(!no_image)
    {
      dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
      // topology remains, as modules are fixed for now:
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
      dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH;
    }
  }
  else
  {
    // same operation, change params
    hist = (dt_dev_history_item_t *)history->data;
    memcpy(hist->params, module->params, module->params_size);

    if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
      memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));

    hist->iop_order = module->iop_order;
    hist->multi_priority = module->multi_priority;
    hist->multi_name_hand_edited = module->multi_name_hand_edited;
    memcpy(hist->multi_name, module->multi_name, sizeof(module->multi_name));
    hist->enabled = module->enabled;

    if(include_masks)
    {
      g_list_free_full(hist->forms, (void (*)(void *))dt_masks_free_form);
      hist->forms = dt_masks_dup_forms_deep(dev->forms, NULL);
    }
    if(!no_image)
    {
      dev->full.pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
      dev->preview_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
      dev->preview2.pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
    }
  }
  if(module->enabled && !no_image)
    dev->history_last_module = module;

  // possibly save database and sidecar file
  if(dev->autosaving)
    _dev_auto_save(dev);
}

const dt_dev_history_item_t *dt_dev_get_history_item(dt_develop_t *dev, const char *op)
{
  for(GList *l = g_list_last(dev->history); l; l = g_list_previous(l))
  {
    dt_dev_history_item_t *item = l->data;
    if(!g_strcmp0(item->op_name, op))
    {
      return item;
      break;
    }
  }

  return NULL;
}

void dt_dev_add_history_item_ext(dt_develop_t *dev,
                                 dt_iop_module_t *module,
                                 const gboolean enable,
                                 const int no_image)
{
  _dev_add_history_item_ext(dev, module, enable, FALSE, no_image, FALSE, TRUE);
}

static gboolean _dev_undo_start_record_target(dt_develop_t *dev, gpointer target)
{
  const double this_time = dt_get_wtime();
  const double merge_time =
    dev->gui_previous_time + dt_conf_get_float("darkroom/undo/merge_same_secs");
  const double review_time =
    dev->gui_previous_pipe_time + dt_conf_get_float("darkroom/undo/review_secs");
  dev->gui_previous_pipe_time = merge_time;
  if(target && target == dev->gui_previous_target
     && this_time < MIN(merge_time, review_time))
  {
    return FALSE;
  }

  dt_dev_undo_start_record(dev);

  dev->gui_previous_target = target;
  dev->gui_previous_time = this_time;

  return TRUE;
}

static void _dev_add_history_item(dt_develop_t *dev,
                                  dt_iop_module_t *module,
                                  const gboolean enable,
                                  const gboolean new_item,
                                  const gpointer target)
{
  if(!darktable.gui || darktable.gui->reset) return;

  // record current name, needed to ensure we do an undo record
  // if the module name is changed.

  gchar *saved_name = g_strdup(module->multi_name);

  _dev_auto_module_label(dev, module);

  const gboolean multi_name_changed = strcmp(saved_name, module->multi_name) != 0;

  dt_pthread_mutex_lock(&dev->history_mutex);
  const gboolean need_end_record =
    _dev_undo_start_record_target(dev, multi_name_changed ? NULL : target);

  g_free(saved_name);

  if(dev->gui_attached)
  {
    _dev_add_history_item_ext(dev, module, enable, new_item, FALSE, FALSE, FALSE);
  }

  /* attach changed tag reflecting actual change */
  const dt_imgid_t imgid = dev->image_storage.id;
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  const gboolean tag_change = dt_tag_attach(tagid, imgid, FALSE, FALSE);

  /* register change timestamp in cache */
  dt_image_cache_set_change_timestamp(darktable.image_cache, imgid);

  // invalidate buffers and force redraw of darkroom
  if(!dev->history_postpone_invalidate
     || module != dev->gui_module)
    dt_dev_invalidate_all(dev);

  if(need_end_record)
    dt_dev_undo_end_record(dev);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(dev->gui_attached)
  {
    /* signal that history has changed */
    if(tag_change)
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_add_history_item(dt_develop_t *dev,
                             dt_iop_module_t *module,
                             const gboolean enable)
{
  _dev_add_history_item(dev, module, enable, FALSE, NULL);
}

void dt_dev_add_history_item_target(dt_develop_t *dev,
                                    dt_iop_module_t *module,
                                    const gboolean enable,
                                    gpointer target)
{
  _dev_add_history_item(dev, module, enable, FALSE, target);
}

void dt_dev_add_new_history_item(dt_develop_t *dev,
                                 dt_iop_module_t *module,
                                 const gboolean enable)
{
  _dev_add_history_item(dev, module, enable, TRUE, NULL);
}

void dt_dev_add_masks_history_item_ext(dt_develop_t *dev,
                                       dt_iop_module_t *_module,
                                       const gboolean _enable,
                                       const gboolean no_image)
{
  dt_iop_module_t *module = _module;
  gboolean enable = _enable;

  // no module means that is called from the mask manager, so find the iop
  if(module == NULL)
  {
    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = modules->data;
      if(dt_iop_module_is(mod->so, "mask_manager"))
      {
        module = mod;
        break;
      }
    }
    enable = FALSE;
  }
  if(module)
  {
    _dev_add_history_item_ext(dev, module, enable, FALSE, no_image, TRUE, TRUE);
  }
  else
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_dev_add_masks_history_item_ext] can't find mask manager module");
}

void dt_dev_add_masks_history_item(
        dt_develop_t *dev,
        dt_iop_module_t *module,
        const gboolean enable)
{
  gpointer target = NULL;

  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(form && gui)
  {
    dt_masks_point_group_t *fpt = g_list_nth_data(form->points, gui->group_edited);
    if(fpt) target = GINT_TO_POINTER(fpt->formid);
  }

  dt_pthread_mutex_lock(&dev->history_mutex);

  const gboolean need_end_record =
    _dev_undo_start_record_target(dev, target);

  if(dev->gui_attached)
  {
    dt_dev_add_masks_history_item_ext(dev, module, enable, FALSE);
  }

  // invalidate buffers and force redraw of darkroom
  dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate_all(dev);

  if(need_end_record)
    dt_dev_undo_end_record(dev);

  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(dev->gui_attached)
  {
    /* recreate mask list */
    dt_dev_masks_list_change(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_free_history_item(gpointer data)
{
  dt_dev_history_item_t *item = (dt_dev_history_item_t *)data;
  free(item->params);
  free(item->blend_params);
  g_list_free_full(item->forms, (void (*)(void *))dt_masks_free_form);
  free(item);
}

void dt_dev_reload_history_items(dt_develop_t *dev)
{
  dev->focus_hash = 0;

  dt_lock_image(dev->image_storage.id);

  dt_ioppr_set_default_iop_order(dev, dev->image_storage.id);
  dt_dev_pop_history_items(dev, 0);

  // remove unused history items:
  GList *history = g_list_nth(dev->history, dev->history_end);
  while(history)
  {
    GList *next = g_list_next(history);
    dt_dev_history_item_t *hist = history->data;
    hist->module->multi_name_hand_edited = FALSE;
    g_strlcpy(hist->module->multi_name, "", sizeof(hist->module->multi_name));
    dt_dev_free_history_item(hist);
    dev->history = g_list_delete_link(dev->history, history);
    history = next;
  }
  dt_dev_read_history(dev);

  // we have to add new module instances first
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module) && !module->expander)
      {
        dt_iop_gui_init(module);

        /* add module to right panel */
        dt_iop_gui_set_expander(module);
        dt_iop_gui_set_expanded(module, TRUE, FALSE);

        dt_iop_reload_defaults(module);
        dt_iop_gui_update_blending(module);

        // the pipe need to be reconstruct
        dev->full.pipe->changed |= DT_DEV_PIPE_REMOVE;
        dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
        dev->preview2.pipe->changed |= DT_DEV_PIPE_REMOVE;
      }
    }
    else if(!dt_iop_is_hidden(module) && module->expander)
    {
      // we have to ensure that the name of the widget is correct
      dt_iop_gui_update_header(module);
    }
  }

  dt_dev_pop_history_items(dev, dev->history_end);

  dt_ioppr_resync_iop_list(dev);

  // set the module list order
  dt_dev_reorder_gui_module_list(dev);

  dt_unlock_image(dev->image_storage.id);
}

void dt_dev_pop_history_items_ext(dt_develop_t *dev, const int32_t cnt)
{
  dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext begin");

  const int end_prev = dev->history_end;
  dev->history_end = cnt;

  // reset gui params for all modules
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;
    memcpy(module->params, module->default_params, module->params_size);
    dt_iop_commit_blend_params(module, module->default_blendop_params);
    module->enabled = module->default_enabled;

    if(module->multi_priority == 0)
      module->iop_order =
        dt_ioppr_get_iop_order(dev->iop_order_list, module->op, module->multi_priority);
    else
    {
      module->iop_order = INT_MAX;
    }
  }

  // go through history and set gui params
  GList *forms = NULL;
  GList *history = dev->history;
  for(int i = 0; i < cnt && history; i++)
  {
    dt_dev_history_item_t *hist = history->data;
    if(hist->module->params_size == 0)
      memcpy(hist->module->params, hist->module->default_params, hist->module->params_size);
    else
      memcpy(hist->module->params, hist->params, hist->module->params_size);
    dt_iop_commit_blend_params(hist->module, hist->blend_params);

    hist->module->iop_order = hist->iop_order;
    hist->module->enabled = hist->enabled;
    g_strlcpy(hist->module->multi_name, hist->multi_name, sizeof(hist->module->multi_name));
    if(hist->forms) forms = hist->forms;
    hist->module->multi_name_hand_edited = hist->multi_name_hand_edited;

    history = g_list_next(history);
  }

  dt_ioppr_resync_modules_order(dev);

  dt_ioppr_check_duplicate_iop_order(&dev->iop, dev->history);

  dt_ioppr_check_iop_order(dev, 0, "dt_dev_pop_history_items_ext end");

  // check if masks have changed
  gboolean masks_changed = FALSE;
  if(cnt < end_prev)
    history = g_list_nth(dev->history, cnt);
  else if(cnt > end_prev)
    history = g_list_nth(dev->history, end_prev);
  else
    history = NULL;

  for(int i = MIN(cnt, end_prev);
      i < MAX(cnt, end_prev) && history && !masks_changed;
      i++)
  {
    dt_dev_history_item_t *hist = history->data;

    if(hist->forms != NULL)
      masks_changed = TRUE;

    history = g_list_next(history);
  }
  if(masks_changed)
    dt_masks_replace_current_forms(dev, forms);
}

void dt_dev_pop_history_items(dt_develop_t *dev, const int32_t cnt)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  ++darktable.gui->reset;
  GList *dev_iop = g_list_copy(dev->iop);

  dt_dev_pop_history_items_ext(dev, cnt);

  darktable.develop->history_updating = TRUE;

  // update all gui modules
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = modules->data;
    dt_iop_gui_update(module);
    modules = g_list_next(modules);
  }

  darktable.develop->history_updating = FALSE;

  // check if the order of modules has changed
  gboolean dev_iop_changed = (g_list_length(dev_iop) != g_list_length(dev->iop));
  if(!dev_iop_changed)
  {
    modules = dev->iop;
    GList *modules_old = dev_iop;
    while(modules && modules_old)
    {
      dt_iop_module_t *module = modules->data;
      dt_iop_module_t *module_old = modules_old->data;

      if(module->iop_order != module_old->iop_order)
      {
        dev_iop_changed = TRUE;
        break;
      }

      modules = g_list_next(modules);
      modules_old = g_list_next(modules_old);
    }
  }
  g_list_free(dev_iop);

  if(!dev_iop_changed)
  {
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
  }
  else
  {
    dt_dev_pixelpipe_rebuild(dev);
  }

  --darktable.gui->reset;
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  dt_dev_masks_list_change(dev);

  dt_control_queue_redraw_center();
}

static void _cleanup_history(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.masks_history WHERE imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_dev_write_history_ext(dt_develop_t *dev,
                              const dt_imgid_t imgid)
{
  dt_lock_image(imgid);

  _cleanup_history(imgid);

  // write history entries

  GList *history = dev->history;
  dt_print(DT_DEBUG_IOPORDER,
           "[dt_dev_write_history_ext] Writing history image id=%d `%s', iop version: %i",
           imgid, dev->image_storage.filename, dev->iop_order_version);
  for(int i = 0; history; i++)
  {
    dt_dev_history_item_t *hist = history->data;
    _dev_write_history_item(imgid, hist, i);

    dt_print(DT_DEBUG_IOPORDER, "%20s, num %2i, order %2d, v(%i), multiprio %i%s",
      hist->module->op, i, hist->iop_order, hist->module->version(), hist->multi_priority,
      (hist->enabled) ? ", enabled" : "");

    history = g_list_next(history);
  }

  // update history end
  dt_image_set_history_end(imgid, dev->history_end);

  // write the current iop-order-list for this image

  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);

  dt_unlock_image(imgid);
}

void dt_dev_write_history(dt_develop_t *dev)
{
  dt_database_start_transaction(darktable.db);
  dt_dev_write_history_ext(dev, dev->image_storage.id);
  dt_database_release_transaction(darktable.db);
}

static int _dev_get_module_nb_records(void)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count (*) FROM  memory.history",
                              -1, &stmt, NULL);
  sqlite3_step(stmt);
  const int cnt = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return cnt;
}

static void _dev_insert_module(dt_develop_t *dev,
                               dt_iop_module_t *module,
                               const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  // we make sure that the multi-name is updated if possible with the
  // actual preset name if any is defined for the default parameters.

  char *preset_name = dt_presets_get_module_label
    (module->op,
     module->default_params, module->params_size, TRUE,
     module->blend_params, sizeof(dt_develop_blend_params_t));

  DT_DEBUG_SQLITE3_PREPARE_V2(
    dt_database_get(darktable.db),
    "INSERT INTO memory.history VALUES (?1, 0, ?2, ?3, ?4, 1, NULL, 0, 0, ?5, 0)",
    -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, module->version());
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, module->default_params, module->params_size,
                             SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, preset_name ? preset_name : "", -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  g_free(preset_name);

  dt_print(DT_DEBUG_PARAMS, "[dev_insert_module] `%s' inserted to history", module->op);
}

static gboolean _dev_auto_apply_presets(dt_develop_t *dev)
{
  // NOTE: the presets/default iops will be *prepended* into the history.

  const dt_imgid_t imgid = dev->image_storage.id;

  if(!dt_is_valid_imgid(imgid)) return FALSE;

  gboolean run = FALSE;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!(image->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)) run = TRUE;

  const gboolean is_raw = dt_image_is_raw(image);
  const gboolean is_modern_chroma = dt_is_scene_referred();

  // flag was already set? only apply presets once in the lifetime of
  // a history stack.  (the flag will be cleared when removing it).
  if(!run || !dt_is_valid_imgid(image->id))
  {
    // Next section is to recover old edits where all modules with
    // default parameters were not recorded in the db nor in the .XMP.
    //
    // One crucial point is the white-balance which has automatic
    // default based on the camera and depends on the
    // chroma-adaptation. In modern mode the default won't be the same
    // used in legacy mode and if the white-balance is not found on
    // the history one will be added by default using current
    // defaults. But if we are in modern chromatic adaptation the
    // default will not be equivalent to the one used to develop this
    // old edit.

    // So if the current mode is the modern chromatic-adaptation, do check the history.

    if(is_modern_chroma && is_raw)
    {
      // loop over all modules and display a message for default-enabled modules that
      // are not found on the history.

      for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
      {
        dt_iop_module_t *module = modules->data;

        if(module->default_enabled
           && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
           && !dt_history_check_module_exists(imgid, module->op, FALSE))
        {
          dt_print(DT_DEBUG_PARAMS,
                   "[_dev_auto_apply_presets] missing mandatory module %s"
                   " for image id=%d `%s'",
                   module->op, imgid, dev->image_storage.filename);

          // If the module is white-balance and we are dealing with a
          // raw file we need to add one now with the default legacy
          // parameters. And we want to do this only for old edits.
          //
          // For new edits the temperature will be added back
          // depending on the chromatic adaptation the standard way.

          if(dt_iop_module_is(module->so, "temperature")
             && (image->change_timestamp == -1))
          {
            // it is important to recover temperature in this case
            // (modern chroma and not module present as we need to
            // have the pre 3.0 default parameters used.
            const gchar *current_workflow =
              dt_conf_get_string_const("plugins/darkroom/workflow");

            dt_conf_set_string("plugins/darkroom/workflow", "display-referred (legacy)");
            dt_iop_reload_defaults(module);
            _dev_insert_module(dev, module, imgid);
            dt_conf_set_string("plugins/darkroom/workflow", current_workflow);
            dt_iop_reload_defaults(module);
          }
        }
      }
    }

    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    return FALSE;
  }

  //  get current workflow and image characteristics

  const gboolean is_scene_referred = dt_is_scene_referred();
  const gboolean is_display_referred = dt_is_display_referred();
  const gboolean is_workflow_none = !is_scene_referred && !is_display_referred;
  const gboolean has_matrix = dt_image_is_matrix_correction_supported(image);

  //  set filters

  int iformat = 0;
  if(dt_image_is_raw(image))
    iformat |= FOR_RAW;
  else
    iformat |= FOR_LDR;

  if(has_matrix)
    iformat |= FOR_MATRIX;

  if(dt_image_is_hdr(image))
    iformat |= FOR_HDR;

  int excluded = 0;
  if(dt_image_monochrome_flags(image))
    excluded |= FOR_NOT_MONO;
  else
    excluded |= FOR_NOT_COLOR;

  // select all presets from one of the following table and add them
  // into memory.history. Note that this is appended to possibly
  // already present default modules.
  //
  // Also it may be possible that multiple presets for a module not
  // supporting multiple instances (e.g. demosaic) may be added. Those
  // instances are properly merged in dt_dev_read_history_ext.

  char query[2048];
  // clang-format off

  const gboolean auto_module = dt_conf_get_bool("darkroom/ui/auto_module_name_update");

  snprintf(query, sizeof(query),
           "INSERT OR REPLACE INTO memory.history"
           " SELECT ?1, 0, op_version, operation AS op, op_params,"
           "       enabled, blendop_params, blendop_version,"
           "       ROW_NUMBER() OVER (PARTITION BY operation ORDER BY operation) - 1,"
           "       %s, multi_name_hand_edited"
           " FROM data.presets"
           // only auto-applied presets matching the camera/lens/focal/format/exposure
           " WHERE ( (autoapply=1"
           "          AND ((?2 LIKE model AND ?3 LIKE maker)"
           "               OR (?4 LIKE model AND ?5 LIKE maker))"
           "          AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "          AND ?8 BETWEEN exposure_min AND exposure_max"
           "          AND ?9 BETWEEN aperture_min AND aperture_max"
           "          AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "          AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))))"
           // skip non iop modules:
           "   AND operation NOT IN"
           "       ('ioporder', 'metadata', 'modulegroups', 'export',"
           "        'tagging', 'collect', '%s')"
           // select all user's auto presets or the hard-coded presets (for the workflow)
           // if non auto-presets for the same operation and matching
           // camera/lens/focal/format/exposure found.
           "   AND (writeprotect = 0"
           "        OR (SELECT NOT EXISTS"
           "             (SELECT op"
           "              FROM presets"
           "              WHERE autoapply = 1 AND operation = op AND writeprotect = 0"
           "                    AND ((?2 LIKE model AND ?3 LIKE maker)"
           "                         OR (?4 LIKE model AND ?5 LIKE maker))"
           "                    AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
           "                    AND ?8 BETWEEN exposure_min AND exposure_max"
           "                    AND ?9 BETWEEN aperture_min AND aperture_max"
           "                    AND ?10 BETWEEN focal_length_min AND focal_length_max"
           "                    AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0)))))"
           " ORDER BY writeprotect DESC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
           // auto module:
           //  ON  : we take as the preset label either the multi-name
           //        if defined or the preset name.
           //  OFF : we take the multi-name only if hand-edited otherwise a
           //        simple incremental instance number (equivalent to the multi_priority
           //        field is used).
           auto_module
             ? "COALESCE(NULLIF(multi_name,''), NULLIF(name,''))"
             : "CASE WHEN multi_name_hand_edited"
               "  THEN multi_name"
               "  ELSE (ROW_NUMBER() OVER (PARTITION BY operation ORDER BY operation) - 1)"
               " END",
           is_display_referred ? "" : "basecurve");
  // clang-format on

  // query for all modules at once:
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // now we want to auto-apply the iop-order list if one corresponds and none are
  // still applied. Note that we can already have an iop-order list set when
  // copying an history or applying a style to a not yet developed image.

  if(!dt_ioppr_has_iop_order_list(imgid))
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "SELECT op_params"
       " FROM data.presets"
       " WHERE autoapply=1"
       "       AND ((?2 LIKE model AND ?3 LIKE maker) OR (?4 LIKE model AND ?5 LIKE maker))"
       "       AND ?6 LIKE lens AND ?7 BETWEEN iso_min AND iso_max"
       "       AND ?8 BETWEEN exposure_min AND exposure_max"
       "       AND ?9 BETWEEN aperture_min AND aperture_max"
       "       AND ?10 BETWEEN focal_length_min AND focal_length_max"
       "       AND (format = 0 OR (format&?11 != 0 AND ~format&?12 != 0))"
       "       AND operation = 'ioporder'"
       " ORDER BY writeprotect ASC, LENGTH(model), LENGTH(maker), LENGTH(lens)",
       -1, &stmt, NULL);
    // NOTE: the order "writeprotect ASC" is very important as it ensure that
    //       user's defined presets are listed first and will be used instead of
    //       the darktable internal ones.
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->camera_alias, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 5, image->camera_maker, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 6, image->exif_lens, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(FLT_MAX, image->exif_iso)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f,
                                                fminf(1000000, image->exif_exposure)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 9, fmaxf(0.0f,
                                                fminf(1000000, image->exif_aperture)));
    DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 10, fmaxf(0.0f,
                                                 fminf(1000000, image->exif_focal_length)));
    // 0: dontcare, 1: ldr, 2: raw plus monochrome & color
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 11, iformat);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 12, excluded);

    GList *iop_list = NULL;

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_print(DT_DEBUG_PARAMS,
               "[dev_auto_apply_presets] found iop-order preset, apply it on %d", imgid);
      const char *params = (char *)sqlite3_column_blob(stmt, 0);
      const int32_t params_len = sqlite3_column_bytes(stmt, 0);
      iop_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
    }
    else
    {
      // we have no auto-apply order, so apply iop order, depending of the workflow
      if(is_scene_referred || is_workflow_none)
      {
        dt_print(DT_DEBUG_PARAMS,
                 "[dev_auto_apply_presets] no iop-order preset, use DT_IOP_ORDER_{JPG/RAW} on %d", imgid);
        iop_list = dt_ioppr_get_iop_order_list_version((iformat & FOR_LDR)
                                                       ? DT_DEFAULT_IOP_ORDER_JPG
                                                       : DT_DEFAULT_IOP_ORDER_RAW);
      }
      else
      {
        dt_print(DT_DEBUG_PARAMS,
                 "[dev_auto_apply_presets] no iop-order preset, use DT_IOP_ORDER_LEGACY on %d", imgid);
        iop_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
      }
    }

    // add multi-instance entries that could have been added if more
    // than one auto-applied preset was found for a single iop.

    GList *mi_list = dt_ioppr_get_multiple_instances_iop_order_list(imgid, TRUE);
    GList *final_list = dt_ioppr_merge_multi_instance_iop_order_list(iop_list, mi_list);

    dt_ioppr_write_iop_order_list(final_list, imgid);
    g_list_free_full(mi_list, free);
    g_list_free_full(final_list, free);
    dt_ioppr_set_default_iop_order(dev, imgid);

    sqlite3_finalize(stmt);
  }

  image->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;

  // make sure these end up in the image_cache; as the history is not correct right now
  // we don't write the sidecar here but later in dt_dev_read_history_ext
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);

  return TRUE;
}

static void _dev_add_default_modules(dt_develop_t *dev,
                                     const dt_imgid_t imgid)
{
  //start with those modules that cannot be disabled
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;

    if(!dt_history_check_module_exists(imgid, module->op, FALSE)
       && module->default_enabled
       && module->hide_enable_button
       && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
    {
      _dev_insert_module(dev, module, imgid);
    }
  }
  //now modules that can be disabled but are auto-on
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = modules->data;

    if(!dt_history_check_module_exists(imgid, module->op, FALSE)
       && module->default_enabled
       && !module->hide_enable_button
       && !(module->flags() & IOP_FLAGS_NO_HISTORY_STACK))
    {
      _dev_insert_module(dev, module, imgid);
    }
  }
}

static void _dev_merge_history(dt_develop_t *dev,
                               const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  // count what we found:
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM memory.history", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // if there is anything..
    const int cnt = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // workaround a sqlite3 "feature". The above statement to insert
    // items into memory.history is complex and in this case sqlite
    // does not give rowid a linear increment. But the following code
    // really expect that the rowid in this table starts from 0 and
    // increment one by one. So in the following code we rewrite the
    // "num" values from 0 to cnt-1.

    if(cnt > 0)
    {
      // get all rowids
      GList *rowids = NULL;

      // get the rowids in descending order since building the list will reverse the order
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT rowid FROM memory.history ORDER BY rowid DESC",
                                  -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
        rowids = g_list_prepend(rowids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
      sqlite3_finalize(stmt);

      // update num accordingly
      int v = 0;

      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE memory.history SET num=?1 WHERE rowid=?2",
                                  -1, &stmt, NULL);

      // let's wrap this into a transaction, it might make it a little faster.
      dt_database_start_transaction(darktable.db);

      for(GList *r = rowids; r; r = g_list_next(r))
      {
        DT_DEBUG_SQLITE3_CLEAR_BINDINGS(stmt);
        DT_DEBUG_SQLITE3_RESET(stmt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, v);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, GPOINTER_TO_INT(r->data));

        if(sqlite3_step(stmt) != SQLITE_DONE) break;

        v++;
      }

      dt_database_release_transaction(darktable.db);

      g_list_free(rowids);

      // advance the current history by cnt amount, that is, make space
      // for the preset/default iops that will be *prepended* into the
      // history.
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "UPDATE main.history SET num=num+?1 WHERE imgid=?2",
                                  -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

      if(sqlite3_step(stmt) == SQLITE_DONE)
      {
        sqlite3_finalize(stmt);
        // clang-format off
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "UPDATE main.images"
                                    " SET history_end=history_end+?1"
                                    " WHERE id=?2",
                                    -1, &stmt, NULL);
        // clang-format on
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
        DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

        if(sqlite3_step(stmt) == SQLITE_DONE)
        {
          // and finally prepend the rest with increasing numbers (starting at 0)
          sqlite3_finalize(stmt);
          // clang-format off
          DT_DEBUG_SQLITE3_PREPARE_V2(
            dt_database_get(darktable.db),
            "INSERT INTO main.history"
            " SELECT imgid, num, module, operation, op_params, enabled, "
            "        blendop_params, blendop_version, multi_priority,"
            "        multi_name, multi_name_hand_edited"
            " FROM memory.history",
            -1, &stmt, NULL);
          // clang-format on
          sqlite3_step(stmt);
          sqlite3_finalize(stmt);
        }
      }
    }
  }
}

static void _dev_write_history(dt_develop_t *dev,
                               const dt_imgid_t imgid)
{
  _cleanup_history(imgid);
  // write history entries
  GList *history = dev->history;
  for(int i = 0; history; i++)
  {
    dt_dev_history_item_t *hist = history->data;
    _dev_write_history_item(imgid, hist, i);
    history = g_list_next(history);
  }
}

// helper function for debug strings
char *_print_validity(const gboolean state)
{
  if(state)
    return "ok";
  else
    return "WRONG";
}

void dt_dev_read_history_ext(dt_develop_t *dev,
                             const dt_imgid_t imgid,
                             const gboolean no_image)
{
  if(!dt_is_valid_imgid(imgid)) return;
  if(!dev->iop) return;

  dt_lock_image(imgid);

  dt_dev_undo_start_record(dev);

  int auto_apply_modules_count = 0;
  gboolean first_run = FALSE;
  gboolean legacy_params = FALSE;

  dt_ioppr_set_default_iop_order(dev, imgid);

  if(!no_image)
  {
    // cleanup
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                          "DELETE FROM memory.history", NULL, NULL, NULL);

    dt_print(DT_DEBUG_PARAMS, "[dt_dev_read_history_ext] temporary history deleted");

    // Make sure all modules default params are loaded to init
    // history. This is important as some modules have specific
    // defaults based on metadata.

    _dt_dev_load_pipeline_defaults(dev);

    // Prepend all default modules to memory.history

    _dev_add_default_modules(dev, imgid);
    const int default_modules_count = _dev_get_module_nb_records();

    // Maybe add auto-presets to memory.history
    first_run = _dev_auto_apply_presets(dev);
    auto_apply_modules_count = _dev_get_module_nb_records() - default_modules_count;

    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] temporary history initialised with"
             " default params and presets");

    // Now merge memory.history into main.history

    _dev_merge_history(dev, imgid);

    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] temporary history merged with image history");

    //  first time we are loading the image, try to import lightroom .xmp if any
    if(dev->full.pipe && dev->full.pipe->loading && first_run)
      dt_lightroom_import(dev->image_storage.id, dev, TRUE);

    // if a snapshot move all auto-presets into the history_snapshot table

    if(dev->snapshot_id != -1)
    {
      sqlite3_stmt *stmt;

      DT_DEBUG_SQLITE3_PREPARE_V2
        (dt_database_get(darktable.db),
         "INSERT INTO memory.snapshot_history"
         " SELECT ?1, * FROM memory.history",
         -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->snapshot_id);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);

      DT_DEBUG_SQLITE3_EXEC
        (dt_database_get(darktable.db),
         "DELETE FROM memory.history", NULL, NULL, NULL);
    }
  }

  sqlite3_stmt *stmt;

  // Get the end of the history - What's that ???

  int history_end_current = 0;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
    if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
      history_end_current = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // Load current image history from DB
  // clang-format off
  if(dev->snapshot_id == -1)
  {
    // not a snapshot, read from main history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT imgid, num, module, operation,"
                                "       op_params, enabled, blendop_params,"
                                "       blendop_version, multi_priority, multi_name,"
                                "       multi_name_hand_edited"
                                " FROM main.history"
                                " WHERE imgid = ?1"
                                " ORDER BY num",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  }
  else
  {
    // a snapshot, read from history_snapshot
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT id, num, module, operation,"
                                "       op_params, enabled, blendop_params,"
                                "       blendop_version, multi_priority, multi_name,"
                                "       multi_name_hand_edited"
                                " FROM memory.snapshot_history"
                                " WHERE id = ?1"
                                " ORDER BY num",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->snapshot_id);
  }
  // clang-format on

  dev->history_end = 0;

  // Specific handling for None workflow (interdependency)

  const gboolean is_workflow_none = dt_conf_is_equal("plugins/darkroom/workflow", "none");

  dt_iop_module_t *channelmixerrgb = NULL;
  dt_iop_module_t *temperature = NULL;

  // Strip rows from DB lookup. One row == One module in history
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // Unpack the DB blobs
    const int id = sqlite3_column_int(stmt, 0);
    const int num = sqlite3_column_int(stmt, 1);
    const int modversion = sqlite3_column_int(stmt, 2);
    const char *module_name = (const char *)sqlite3_column_text(stmt, 3);
    const void *module_params = sqlite3_column_blob(stmt, 4);
    const int enabled = sqlite3_column_int(stmt, 5);
    const void *blendop_params = sqlite3_column_blob(stmt, 6);
    const int blendop_version = sqlite3_column_int(stmt, 7);
    const int multi_priority = sqlite3_column_int(stmt, 8);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
    const int multi_name_hand_edited = sqlite3_column_int(stmt, 10);

    const int param_length = sqlite3_column_bytes(stmt, 4);
    const int bl_length = sqlite3_column_bytes(stmt, 6);

    // Sanity checks
    const gboolean is_valid_id = (id == imgid || (dev->snapshot_id != -1));
    const gboolean has_module_name = (module_name != NULL);

    if(!(has_module_name && is_valid_id))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_read_history_ext] database history for image"
               " id=%d `%s' seems to be corrupted!",
               imgid, dev->image_storage.filename);
      continue;
    }

    const int iop_order =
      dt_ioppr_get_iop_order(dev->iop_order_list, module_name, multi_priority);
    if(iop_order == INT_MAX)
    {
      dt_print(DT_DEBUG_PIPE | DT_DEBUG_UNDO,
               "[dev_read_history_ext] illegal iop_order for module `%s.%i' in history for image"
               " id=%d `%s'!",
               module_name, multi_priority, imgid, dev->image_storage.filename);
      continue;
    }

    dt_dev_history_item_t *hist = calloc(1, sizeof(dt_dev_history_item_t));
    hist->module = NULL;

    // Find a .so file that matches our history entry, aka a module to
    // run the params stored in DB
    dt_iop_module_t *find_op = NULL;

    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *module = modules->data;
      if(dt_iop_module_is(module->so, module_name))
      {
        // make sure that module not supporting multiple instances are
        // selected here.
        if(module->multi_priority == multi_priority
           || (module->flags() & IOP_FLAGS_ONE_INSTANCE))
        {
          hist->module = module;
          if(multi_name)
            g_strlcpy(module->multi_name, multi_name, sizeof(module->multi_name));
          else
            memset(module->multi_name, 0, sizeof(module->multi_name));
          module->multi_name_hand_edited = multi_name_hand_edited;
          break;
        }
        else if(multi_priority > 0)
        {
          // we just say that we find the name, so we just have to add
          // new instance of this module
          find_op = module;
        }
      }
    }
    if(!hist->module && find_op)
    {
      // we have to add a new instance of this module and set index to modindex
      dt_iop_module_t *new_module = calloc(1, sizeof(dt_iop_module_t));
      if(!dt_iop_load_module(new_module, find_op->so, dev))
      {
        dt_iop_update_multi_priority(new_module, multi_priority);
        new_module->iop_order = iop_order;

        g_strlcpy(new_module->multi_name, multi_name, sizeof(new_module->multi_name));
        new_module->multi_name_hand_edited = multi_name_hand_edited;

        dev->iop = g_list_append(dev->iop, new_module);

        new_module->instance = find_op->instance;
        hist->module = new_module;
      }
    }

    if(!hist->module)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[dev_read_history] the module `%s' requested by image"
               " id=%d `%s' is not installed on this computer!",
               module_name, imgid, dev->image_storage.filename);
      free(hist);
      continue;
    }

    if(is_workflow_none && hist->module->enabled)
    {
      if(dt_iop_module_is(hist->module->so, "temperature"))
        temperature = hist->module;
      if(dt_iop_module_is(hist->module->so, "channelmixerrgb"))
        channelmixerrgb = hist->module;
    }

    // module has no user params and won't bother us in GUI - exit early, we are done
    if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
    {
      free(hist);
      continue;
    }

    // Run a battery of tests
    const gboolean is_valid_module_name =
      (strcmp(module_name, hist->module->op) == 0);
    const gboolean is_valid_blendop_version =
      (blendop_version == dt_develop_blend_version());
    const gboolean is_valid_blendop_size =
      (bl_length == sizeof(dt_develop_blend_params_t));
    const gboolean is_valid_module_version =
      (modversion == hist->module->version());
    const gboolean is_valid_params_size =
      (param_length == hist->module->params_size);

    dt_print(DT_DEBUG_PARAMS,
             "[history] successfully loaded module %s from history\n"
             "\t\t\tblendop v. %i:\tversion %s\tparams %s\n"
             "\t\t\tparams v. %i:\tversion %s\tparams %s",
             module_name,
             blendop_version, _print_validity(is_valid_blendop_version),
             _print_validity(is_valid_blendop_size),
             modversion, _print_validity(is_valid_module_version),
             _print_validity(is_valid_params_size));

    // Init buffers and values
    hist->enabled = enabled;
    hist->num = num;
    hist->iop_order = iop_order;
    hist->multi_priority = (hist->module->flags() & IOP_FLAGS_ONE_INSTANCE)
      ? 0
      : multi_priority;

    hist->multi_name_hand_edited = multi_name_hand_edited;
    g_strlcpy(hist->op_name, hist->module->op, sizeof(hist->op_name));
    if(multi_name) // multi_name can be NULL on DB
      g_strlcpy(hist->multi_name, multi_name, sizeof(hist->multi_name));
    hist->params = malloc(hist->module->params_size);
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));

    // update module iop_order only on active history entries
    if(history_end_current > dev->history_end)
      hist->module->iop_order = hist->iop_order;

    // Copy blending params if valid, else try to convert legacy params
    if(blendop_params
       && is_valid_blendop_version
       && is_valid_blendop_size)
    {
      memcpy(hist->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params
            (hist->module, blendop_params, blendop_version,
             hist->blend_params, dt_develop_blend_version(), bl_length) == 0)
    {
      legacy_params = TRUE;
    }
    else
    {
      memcpy(hist->blend_params, hist->module->default_blendop_params,
             sizeof(dt_develop_blend_params_t));
    }

    // Copy module params if valid, else try to convert legacy params
    if(param_length == 0)
    {
      // special case of auto-init presets being loaded in history
      memcpy(hist->params, hist->module->default_params, hist->module->params_size);
    }
    else if(is_valid_module_version && is_valid_params_size && is_valid_module_name)
    {
      memcpy(hist->params, module_params, hist->module->params_size);
    }
    else
    {
      if(dt_iop_legacy_params
         (hist->module,
          module_params, param_length, modversion,
          &hist->params, hist->module->version()) == 1)
      {
        dt_print(DT_DEBUG_ALWAYS,
                 "[dev_read_history] module `%s' version mismatch: history"
                 " is %d, darktable is %d, image id=%d `%s'",
                 hist->module->op, modversion, hist->module->version(),
                 imgid, dev->image_storage.filename);

        const char *fname =
          dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;

        if(fname > dev->image_storage.filename) fname++;
        dt_control_log(_("%s: module `%s' version mismatch: %d != %d"),
                       fname, hist->module->op,
                       hist->module->version(), modversion);
        dt_dev_free_history_item(hist);
        continue;
      }
      else
      {
        if(dt_iop_module_is(hist->module->so, "spots")
           && modversion == 1)
        {
          // quick and dirty hack to handle spot removal legacy_params
          memcpy(hist->blend_params, hist->module->blend_params,
                 sizeof(dt_develop_blend_params_t));
        }
        legacy_params = TRUE;
      }

      /*
       * Fix for flip iop: previously it was not always needed, but it might be
       * in history stack as "orientation (off)", but now we always want it
       * by default, so if it is disabled, enable it, and replace params with
       * default_params. if user want to, he can disable it.
       */
      if(dt_iop_module_is(hist->module->so, "flip")
         && !hist->enabled
         && labs(modversion) == 1)
      {
        memcpy(hist->params, hist->module->default_params, hist->module->params_size);
        hist->enabled = TRUE;
      }
    }

    // make sure that always-on modules are always on. duh.
    if(hist->module->default_enabled && hist->module->hide_enable_button)
      hist->enabled = TRUE;

    dev->history = g_list_append(dev->history, hist);
    dev->history_end++;
  }
  sqlite3_finalize(stmt);

  // Both modules are actives and found on the history stack, let's
  // again reload the defaults to ensure the whiteblance is properly
  // set depending on the CAT handling.
  if(temperature && channelmixerrgb)
  {
    dt_print(DT_DEBUG_PARAMS,
             "[dt_dev_read_history_ext] reset defaults for workflow none");
    temperature->reload_defaults(temperature);
  }

  dt_ioppr_resync_modules_order(dev);

  if(dev->snapshot_id == -1)
  {
    // find the new history end
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT history_end FROM main.images WHERE id = ?1",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW) // seriously, this should never fail
      if(sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        dev->history_end = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  dt_ioppr_check_iop_order(dev, imgid, "dt_dev_read_history_no_image end");

  dt_masks_read_masks_history(dev, imgid);

  // FIXME : this probably needs to capture dev thread lock
  if(dev->gui_attached && !no_image)
  {
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dt_dev_invalidate_all(dev);

    /* signal history changed */
    dt_dev_undo_end_record(dev);
  }
  dt_dev_masks_list_change(dev);

  // make sure module_dev is in sync with history

  if(dev->snapshot_id != -1)
    goto end_rh;

  _dev_write_history(dev, imgid);
  dt_ioppr_write_iop_order_list(dev->iop_order_list, imgid);

  dt_history_hash_t flags = DT_HISTORY_HASH_CURRENT;
  if(first_run)
  {
    const dt_history_hash_t hash_status = dt_history_hash_get_status(imgid);
    // if altered doesn't mask it
    if(!(hash_status & DT_HISTORY_HASH_CURRENT))
    {
      flags = flags
        | (auto_apply_modules_count
           ? DT_HISTORY_HASH_AUTO
           : DT_HISTORY_HASH_BASIC);
    }
    dt_history_hash_write_from_history(imgid, flags);
    // As we have a proper history right now and this is first_run we
    // possibly write the xmp now
    dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    // depending on the xmp_writing mode we either us safe or relaxed
    const gboolean always = (dt_image_get_xmp_mode() == DT_WRITE_XMP_ALWAYS);
    dt_image_cache_write_release
      (darktable.image_cache,
       image,
       always ? DT_IMAGE_CACHE_SAFE : DT_IMAGE_CACHE_RELAXED);
  }
  else if(legacy_params)
  {
    const dt_history_hash_t hash_status = dt_history_hash_get_status(imgid);
    if(hash_status & (DT_HISTORY_HASH_BASIC | DT_HISTORY_HASH_AUTO))
    {
      // if image not altered keep the current status
      flags = flags | hash_status;
    }
    dt_history_hash_write_from_history(imgid, flags);
  }
  else
  {
    dt_history_hash_write_from_history(imgid, flags);
  }

  end_rh:
  dt_unlock_image(imgid);
}

void dt_dev_read_history(dt_develop_t *dev)
{
  dt_dev_read_history_ext(dev, dev->image_storage.id, FALSE);
}

void dt_dev_reprocess_all(dt_develop_t *dev)
{
  if(darktable.gui->reset) return;
  if(dev && dev->gui_attached)
  {
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview2.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->full.pipe->cache_obsolete = TRUE;
    dev->preview_pipe->cache_obsolete = TRUE;
    dev->preview2.pipe->cache_obsolete = TRUE;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_reprocess_center(dt_develop_t *dev)
{
  if(darktable.gui->reset) return;
  if(dev && dev->gui_attached)
  {
    dev->full.pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->full.pipe->cache_obsolete = TRUE;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_reprocess_preview(dt_develop_t *dev)
{
  if(darktable.gui->reset || !dev || !dev->gui_attached)
    return;

  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->cache_obsolete = TRUE;

  dt_dev_invalidate_preview(dev);
  dt_control_queue_redraw_center();
}

gboolean dt_dev_get_zoom_bounds(dt_dev_viewport_t *port,
                                float *zoom_x,
                                float *zoom_y,
                                float *boxw,
                                float *boxh)
{
  if(port->zoom == DT_ZOOM_FIT)
    return FALSE;

  dt_dev_zoom_t zoom;
  int closeup, procw = 0, proch = 0;
  dt_dev_get_viewport_params(port, &zoom, &closeup, zoom_x, zoom_y);
  dt_dev_get_processed_size(port, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(port, port->zoom, 1<<port->closeup, 0);

  *boxw = procw ? port->width / (procw * scale) : 1.0f;
  *boxh = proch ? port->height / (proch * scale) : 1.0f;

  return *boxw < 1.0f || *boxh < 1.0f;
}

gboolean dt_dev_get_preview_size(const dt_develop_t *dev,
                                 float *wd,
                                 float *ht)
{
  *wd = dev->full.pipe->processed_width / dev->preview_pipe->iscale;
  *ht = dev->full.pipe->processed_height / dev->preview_pipe->iscale;
  return *wd >= 1.f && *ht >= 1.f;
}

void dt_dev_get_processed_size(dt_dev_viewport_t *port,
                               int *procw,
                               int *proch)
{
  // no processed pipes, lets return 0 size
  *procw = *proch = 0;

  if(!port) return;

  // if pipe is processed, lets return its size
  if(port->pipe && port->pipe->processed_width)
  {
    *procw = port->pipe->processed_width;
    *proch = port->pipe->processed_height;
    return;
  }

  dt_develop_t *dev = darktable.develop;

  // fallback on preview pipe
  if(dev->preview_pipe && dev->preview_pipe->processed_width)
  {
    const float scale = dev->preview_pipe->iscale;
    *procw = scale * dev->preview_pipe->processed_width;
    *proch = scale * dev->preview_pipe->processed_height;
  }
}

static float _calculate_new_scroll_zoom_tscale(const int up,
                                               const gboolean constrained,
                                               const float tscaleold,
                                               const float tscalefit)
{
  enum {
    SIZE_SMALL,
    SIZE_MEDIUM,
    SIZE_LARGE
  } image_size;

  if (tscalefit <= 1.0f)
    image_size = SIZE_LARGE;
  else if (tscalefit <= 2.0f)
    image_size = SIZE_MEDIUM;
  else
    image_size = SIZE_SMALL;

  // at 200% zoom level or more, we use a step of 2x, while at lower level we use 1.1x
  const float step =
    up
    ? (tscaleold >= 2.0f ? 2.0f : 1.1f)
    : (tscaleold > 2.0f ? 2.0f : 1.1f);

  // we calculate the new scale
  float tscalenew = up ? tscaleold * step : tscaleold / step;

  // when zooming, secure we include 2:1, 1:1 and FIT levels anyway in the zoom stops
  if ((tscalenew - tscalefit) * (tscaleold - tscalefit) < 0 && image_size != SIZE_SMALL)
    tscalenew = tscalefit;
  else if ((tscalenew - 1.0f) * (tscaleold - 1.0f) < 0)
    tscalenew = 1.0f;
  else if ((tscalenew - 2.0f) * (tscaleold - 2.0f) < 0)
    tscalenew = 2.0f;

  float tscalemax, tscalemin;            // the zoom soft limits
  const float tscaletop = 16.0f; // the zoom hard limits
  const float tscalefloor = MIN(0.5f * tscalefit, 1.0f);

  switch (image_size) // here we set the logic of zoom limits
    {
    case SIZE_LARGE:
      tscalemax = constrained
        ? (tscaleold > 2.0f
           ? tscaletop
           : (tscaleold > 1.0f ? 2.0f : 1.0f))
        : tscaletop;
      tscalemin = constrained
        ? (tscaleold < tscalefit
           ? tscalefloor
           : tscalefit)
        : tscalefloor;
      break;
    case SIZE_MEDIUM:
      tscalemax = constrained
        ? (tscaleold > 2.0f
           ? tscaletop
           : 2.0f)
        : tscaletop;
      tscalemin = constrained
        ? (tscaleold < tscalefit
           ? tscalefloor
           : tscalefit)
        : tscalefloor;
      break;
    case SIZE_SMALL:
      tscalemax = constrained
        ? (tscaleold > 2.0f
           ? tscaletop
           : tscalefit)
        : tscaletop;
      tscalemin = tscalefloor;
      break;
    }

  // we enforce the zoom limits
  tscalenew = up
    ? MIN(tscalenew, tscalemax)
    : MAX(tscalenew, tscalemin);

  return tscalenew;
}

// running with the history locked
static gboolean _dev_distort_backtransform_locked(dt_develop_t *dev,
                                                  dt_dev_pixelpipe_t *pipe,
                                                  const double iop_order,
                                                  const dt_dev_transform_direction_t transf_direction,
                                                  float *points,
                                                  const size_t points_count)
{
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      return FALSE;
    }
    dt_iop_module_t *module = modules->data;
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    if(piece->enabled
       && piece->data
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order))
       && !(dt_iop_module_is_skipped(dev, module)
            && (pipe->type & DT_DEV_PIXELPIPE_BASIC)))
    {
      module->distort_backtransform(module, piece, points, points_count);
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  return TRUE;
}

// running with the history locked
static gboolean _dev_distort_transform_locked(dt_develop_t *dev,
                                              dt_dev_pixelpipe_t *pipe,
                                              const double iop_order,
                                              const dt_dev_transform_direction_t transf_direction,
                                              float *points,
                                              const size_t points_count)
{
  GList *modules = pipe->iop;
  GList *pieces = pipe->nodes;
  while(modules)
  {
    if(!pieces)
    {
      return FALSE;
    }
    dt_iop_module_t *module = modules->data;
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    if(piece->enabled
       && piece->data
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order))
       && !(dt_iop_module_is_skipped(dev, module)
            && (pipe->type & DT_DEV_PIXELPIPE_BASIC)))
    {
      module->distort_transform(module, piece, points, points_count);
    }
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  return TRUE;
}

void dt_dev_zoom_move(dt_dev_viewport_t *port,
                      dt_dev_zoom_t zoom,
                      float scale,
                      int closeup,
                      float x,
                      float y,
                      gboolean constrain)
{
  dt_develop_t *dev = darktable.develop;

  dt_pthread_mutex_lock(&darktable.control->global_mutex);
  dt_pthread_mutex_lock(&dev->history_mutex);

  float pts[2] = { port->zoom_x, port->zoom_y };
  _dev_distort_transform_locked(darktable.develop, port->pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, pts, 1);

  const float old_pts0 = pts[0];
  const float old_pts1 = pts[1];
  const float old_zoom_scale = port->zoom_scale;
  const dt_dev_zoom_t old_zoom = port->zoom;
  int old_closeup = port->closeup;

  int procw, proch;
  dt_dev_get_processed_size(port, &procw, &proch);

  float zoom_x = pts[0] / procw - 0.5f;
  float zoom_y = pts[1] / proch - 0.5f;

  float cur_scale = dt_dev_get_zoom_scale(port, port->zoom, 1<<port->closeup, 0);

  if(zoom == DT_ZOOM_POSITION)
  {
    zoom_x = x;
    zoom_y = y;
  }
  else if(zoom == DT_ZOOM_MOVE)
  {
    zoom_x += scale * x / (procw * cur_scale);
    zoom_y += scale * y / (proch * cur_scale);
    if(closeup) ++old_closeup; // force refresh
  }
  else
  {
    const float ppd = port->ppd;
    const gboolean low_ppd = (ppd == 1);

    if(zoom == DT_ZOOM_1 && closeup == -1)
      closeup = port->zoom != DT_ZOOM_1 || port->closeup ? 0 : 1;
    else if(zoom == DT_ZOOM_1 && closeup == -2)
    {
      // zoom to 1:1 2:1 and back
      const float tscale = cur_scale * ppd;
      closeup = 0;

      const float scalefit = dt_dev_get_zoom_scale(port, DT_ZOOM_FIT, 1, 0) * ppd;

      // Get config so we can check if the user want to cycle through 100%->200%->FIT or
      // only switch between FIT<->100% unless ctrl key is pressed.
      const gboolean cycle_zoom_200 =
            dt_conf_get_bool("darkroom/mouse/middle_button_cycle_zoom_to_200_percent");

      // We are at 200% or above.
      if(tscale > 1.9999f)
      {
        zoom = (cycle_zoom_200 || !constrain) ? DT_ZOOM_FIT : DT_ZOOM_1;
      }
      else if(tscale > 0.9999f) // >= 100%
      {
        zoom = (cycle_zoom_200 || !constrain) ? DT_ZOOM_1 : DT_ZOOM_FIT;
        closeup = (low_ppd && (cycle_zoom_200 || !constrain)) ? 1 : 0;
      }
      else if(((tscale > scalefit) || (tscale < scalefit)) && !cycle_zoom_200)
      {
        zoom = !constrain ? DT_ZOOM_1 : DT_ZOOM_FIT;
      }
      else
      {
        zoom = low_ppd ? DT_ZOOM_1 : DT_ZOOM_FREE;
        if(!cycle_zoom_200)
          closeup = low_ppd && !constrain ? 1 : 0;
      }

      scale = low_ppd ? dt_dev_get_zoom_scale(port, zoom, 1, 0) : (1.0f / ppd);
    }
    else if(zoom == DT_ZOOM_SCROLL)
    {
      zoom = DT_ZOOM_FREE;
      const float fitscale = dt_dev_get_zoom_scale(port, DT_ZOOM_FIT, 1.0, 0);
      const float tscaleold = cur_scale * ppd;
      const float tscale = _calculate_new_scroll_zoom_tscale (closeup, constrain, tscaleold, fitscale * ppd);
      scale = tscale / ppd;

      closeup = 0;
      if(tscale < 1.9999)
        scale = tscale / ppd;
      else
      {
        // pixel doubling instead of interpolation at >= 200% lodpi, >= 400% hidpi
        zoom = DT_ZOOM_1;
        scale = 1.0f;
        if(low_ppd) closeup++;
        if(tscale > 3.9999f) closeup++;
        if(tscale > 7.9999f) closeup++;
        if(tscale > 15.9999) closeup++;
      }

      if(fabsf(scale - 1.0f) < 0.001f) zoom = DT_ZOOM_1;
      if(fabsf(scale - fitscale) < 0.001f) zoom = DT_ZOOM_FIT;
    }
    else if(zoom == DT_ZOOM_FULL_PREVIEW)
    {
      dev->full_preview_last_zoom = port->zoom;
      dev->full_preview_last_closeup = port->closeup;
      dev->full_preview_last_zoom_x = zoom_x;
      dev->full_preview_last_zoom_y = zoom_y;
      zoom = DT_ZOOM_FIT;
      scale = port->zoom_scale;
    }
    else if(zoom == DT_ZOOM_RESTORE)
    {
      zoom = dev->full_preview_last_zoom;
      closeup = dev->full_preview_last_closeup;
      zoom_x = dev->full_preview_last_zoom_x;
      zoom_y = dev->full_preview_last_zoom_y;
      scale = port->zoom_scale;
    }

    port->closeup = closeup;
    port->zoom_scale = scale;
    port->zoom = zoom;
  }

  if(zoom == DT_ZOOM_FIT || !procw || !proch)
    zoom_x = zoom_y = 0.0f;
  else
  {
    float new_scale = dt_dev_get_zoom_scale(port, port->zoom, 1<<port->closeup, 0);

    float boxw = port->width / (procw * new_scale);
    float boxh = port->height / (proch * new_scale);

    if(x >= 0.0f && y >= 0.0f)
    {
      // adjust offset from center so same point under cursor
      float mouse_off_x = (x - port->border_size - 0.5f * port->width) / procw;
      float mouse_off_y = (y - port->border_size - 0.5f * port->height) / proch;
      zoom_x += mouse_off_x / cur_scale - mouse_off_x / new_scale;
      zoom_y += mouse_off_y / cur_scale - mouse_off_y / new_scale;
    }

    zoom_x = boxw > 1.0f ? 0.0f : CLAMP(zoom_x, boxw / 2 - .5, .5 - boxw / 2);
    zoom_y = boxh > 1.0f ? 0.0f : CLAMP(zoom_y, boxh / 2 - .5, .5 - boxh / 2);
  }

  pts[0] = (zoom_x + 0.5f) * procw;
  pts[1] = (zoom_y + 0.5f) * proch;
  gboolean has_moved = fabsf(pts[0] - old_pts0) + fabsf(pts[1] - old_pts1) > 0.5f;
  if(has_moved)
  {
    _dev_distort_backtransform_locked(dev, port->pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, pts, 1);
    port->zoom_x = pts[0];
    port->zoom_y = pts[1];
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_pthread_mutex_unlock(&darktable.control->global_mutex);

  if(!has_moved
     && fabsf(old_zoom_scale - port->zoom_scale) < 0.01f
     && old_zoom == port->zoom
     && old_closeup == port->closeup)
    return;

  if(port->widget)
    dt_control_queue_redraw_widget(port->widget);
  if(port == &dev->full)
    dt_control_navigation_redraw();
}

void dt_dev_get_pointer_zoom_pos(dt_dev_viewport_t *port,
                                 const float px,
                                 const float py,
                                 float *zoom_x,
                                 float *zoom_y,
                                 float *zoom_scale)
{
  dt_dev_zoom_t zoom;
  int closeup = 0, procw = 0, proch = 0;
  float zoom2_x = 0.0f, zoom2_y = 0.0f;
  dt_dev_get_viewport_params(port, &zoom, &closeup, &zoom2_x, &zoom2_y);
  dt_dev_get_processed_size(port, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 0);
  const double tb = port->border_size;
  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = px - tb - .5 * port->width;
  const float mouse_off_y = py - tb - .5 * port->height;
  zoom2_x += mouse_off_x / (procw * scale);
  zoom2_y += mouse_off_y / (proch * scale);
  *zoom_x = zoom2_x + 0.5f;
  *zoom_y = zoom2_y + 0.5f;
  *zoom_scale = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 1);
}

void dt_dev_get_pointer_zoom_pos_from_bounds(dt_dev_viewport_t *port,
                                             const float px,
                                             const float py,
                                             const float zbound_x,
                                             const float zbound_y,
                                             float *zoom_x,
                                             float *zoom_y,
                                             float *zoom_scale)
{
  dt_dev_zoom_t zoom;
  int closeup = 0, procw = 0, proch = 0;
  float zoom2_x = zbound_x, zoom2_y = zbound_y;
  dt_dev_get_viewport_params(port, &zoom, &closeup, NULL, NULL);
  dt_dev_get_processed_size(port, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 0);
  const double tb = port->border_size;
  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = px - tb - .5 * port->width;
  const float mouse_off_y = py - tb - .5 * port->height;
  zoom2_x += mouse_off_x / (procw * scale);
  zoom2_y += mouse_off_y / (proch * scale);
  *zoom_x = zoom2_x + 0.5f;
  *zoom_y = zoom2_y + 0.5f;
  *zoom_scale = dt_dev_get_zoom_scale(port, zoom, 1<<closeup, 1);
}

void dt_dev_get_viewport_params(dt_dev_viewport_t *port,
                                dt_dev_zoom_t *zoom,
                                int *closeup,
                                float *x,
                                float *y)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));

  if(zoom)    *zoom = port->zoom;
  if(closeup) *closeup = port->closeup;

  if(x && y && port->pipe)
  {
    float pts[2] = { port->zoom_x, port->zoom_y };
    dt_dev_distort_transform_plus(darktable.develop, port->pipe,
                                  0.0f, DT_DEV_TRANSFORM_DIR_ALL, pts, 1);
    *x = pts[0] / port->pipe->processed_width - 0.5f;
    *y = pts[1] / port->pipe->processed_height - 0.5f;
  }
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

gboolean dt_dev_is_current_image(const dt_develop_t *dev,
                                 const dt_imgid_t imgid)
{
  return (dev->image_storage.id == imgid) ? TRUE : FALSE;
}

static dt_dev_proxy_exposure_t *_dev_exposure_proxy_available(dt_develop_t *dev)
{
  if(!dev->proxy.exposure.module || dt_view_get_current() != DT_VIEW_DARKROOM) return NULL;

  dt_dev_proxy_exposure_t *instance = &dev->proxy.exposure;
  return instance && instance->module ? instance : NULL;
}

float dt_dev_exposure_get_exposure(dt_develop_t *dev)
{
  const dt_dev_proxy_exposure_t *instance = _dev_exposure_proxy_available(dev);
  return instance && instance->get_exposure && instance->module->enabled ? instance->get_exposure(instance->module) : 0.0f;
}

float dt_dev_exposure_get_black(dt_develop_t *dev)
{
  const dt_dev_proxy_exposure_t *instance = _dev_exposure_proxy_available(dev);
  return instance && instance->get_black  && instance->module->enabled ? instance->get_black(instance->module) : 0.0f;
}

void dt_dev_exposure_handle_event(GdkEvent *event, gboolean blackwhite)
{
  if(darktable.develop->proxy.exposure.handle_event)
    darktable.develop->proxy.exposure.handle_event(event, blackwhite);
}

void dt_dev_modulegroups_set(dt_develop_t *dev,
                             const uint32_t group)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.set
     && dev->first_load == FALSE)
    dev->proxy.modulegroups.set(dev->proxy.modulegroups.module, group);
}

uint32_t dt_dev_modulegroups_get_activated(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get_activated)
    return dev->proxy.modulegroups.get_activated(dev->proxy.modulegroups.module);

  return 0;
}

uint32_t dt_dev_modulegroups_get(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get)
    return dev->proxy.modulegroups.get(dev->proxy.modulegroups.module);

  return 0;
}

gboolean dt_dev_modulegroups_test_activated(dt_develop_t *dev)
{
  const uint32_t activated = dt_dev_modulegroups_get_activated(dev);
  return activated != DT_MODULEGROUP_BASICS;
}

gboolean dt_dev_modulegroups_test(dt_develop_t *dev,
                                  const uint32_t group,
                                  dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.test)
    return dev->proxy.modulegroups.test(dev->proxy.modulegroups.module, group, module);
  return FALSE;
}

void dt_dev_modulegroups_switch(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.switch_group && dev->first_load == FALSE)
    dev->proxy.modulegroups.switch_group(dev->proxy.modulegroups.module, module);
}

void dt_dev_modulegroups_update_visibility(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.switch_group
     && dev->first_load == FALSE)
    dev->proxy.modulegroups.update_visibility(dev->proxy.modulegroups.module);
}

gboolean dt_dev_modulegroups_is_visible(dt_develop_t *dev,
                                        gchar *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.test_visible)
    return dev->proxy.modulegroups.test_visible(dev->proxy.modulegroups.module, module);
  return FALSE;
}

int dt_dev_modulegroups_basics_module_toggle(dt_develop_t *dev,
                                             GtkWidget *widget,
                                             const gboolean doit)
{
  if(dev->proxy.modulegroups.module
     && dev->proxy.modulegroups.basics_module_toggle)
    return dev->proxy.modulegroups.basics_module_toggle
      (dev->proxy.modulegroups.module, widget, doit);
  return 0;
}

void dt_dev_masks_list_change(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_change)
    dev->proxy.masks.list_change(dev->proxy.masks.module);
}
void dt_dev_masks_list_update(dt_develop_t *dev)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_update)
    dev->proxy.masks.list_update(dev->proxy.masks.module);
}

void dt_dev_masks_list_remove(dt_develop_t *dev,
                              const dt_mask_id_t formid,
                              const dt_mask_id_t parentid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_remove)
    dev->proxy.masks.list_remove(dev->proxy.masks.module, formid, parentid);
}
void dt_dev_masks_selection_change(dt_develop_t *dev,
                                   dt_iop_module_t *module,
                                   const dt_mask_id_t selectid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.selection_change)
    dev->proxy.masks.selection_change(dev->proxy.masks.module, module, selectid);
}

/** duplicate a existent module */
dt_iop_module_t *dt_dev_module_duplicate_ext(dt_develop_t *dev,
                                             dt_iop_module_t *base,
                                             const gboolean reorder_iop)
{
  // we create the new module
  dt_iop_module_t *module = calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, base->so, base->dev))
    return NULL;
  module->instance = base->instance;

  // we set the multi-instance priority and the iop order
  int pmax = 0;
  for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;
    if(mod->instance == base->instance)
    {
      if(pmax < mod->multi_priority) pmax = mod->multi_priority;
    }
  }
  // create a unique multi-priority
  pmax += 1;
  dt_iop_update_multi_priority(module, pmax);

  // add this new module position into the iop-order-list
  dt_ioppr_insert_module_instance(dev, module);

  // since we do not rename the module we need to check that an old
  // module does not have the same name. Indeed the multi_priority are
  // always rebased to start from 0, to it may be the case that the
  // same multi_name be generated when duplicating a module.
  int pname = module->multi_priority;
  char mname[128];

  do
  {
    snprintf(mname, sizeof(mname), "%d", pname);
    gboolean dup = FALSE;

    for(GList *modules = base->dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *mod = modules->data;
      if(mod->instance == base->instance)
      {
        if(strcmp(mname, mod->multi_name) == 0)
        {
          dup = TRUE;
          break;
        }
      }
    }

    if(dup)
      pname++;
    else
      break;
  } while(1);

  // the multi instance name
  g_strlcpy(module->multi_name, mname, sizeof(module->multi_name));
  module->multi_name_hand_edited = FALSE;

  // we insert this module into dev->iop
  base->dev->iop = g_list_insert_sorted(base->dev->iop, module, dt_sort_iop_by_order);

  // always place the new instance after the base one
  if(reorder_iop && !dt_ioppr_move_iop_after(base->dev, module, base))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_dev_module_duplicate] can't move new instance after the base one");
    dt_control_log(_("duplicate module, can't move new instance after the base one\n"));
  }

  // that's all. rest of insertion is gui work !
  return module;
}

dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, dt_iop_module_t *base)
{
  return dt_dev_module_duplicate_ext(dev, base, TRUE);
}

void dt_dev_invalidate_history_module(GList *list,
                                      dt_iop_module_t *module)
{
  for(; list; list = g_list_next(list))
  {
    dt_dev_history_item_t *hitem = list->data;
    if(hitem->module == module)
    {
      hitem->module = NULL;
    }
  }
}

void dt_dev_module_remove(dt_develop_t *dev,
                          dt_iop_module_t *module)
{
  // if(darktable.gui->reset) return;
  dt_pthread_mutex_lock(&dev->history_mutex);
  int del = 0;

  if(dev->gui_attached)
  {
    dt_dev_undo_start_record(dev);

    GList *elem = dev->history;
    while(elem != NULL)
    {
      GList *next = g_list_next(elem);
      dt_dev_history_item_t *hist = elem->data;

      if(module == hist->module)
      {
        dt_dev_free_history_item(hist);
        dev->history = g_list_delete_link(dev->history, elem);
        dev->history_end--;
        del = 1;
      }
      elem = next;
    }
  }

  // and we remove it from the list
  for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = modules->data;
    if(mod == module)
    {
      dev->iop = g_list_remove_link(dev->iop, modules);
      break;
    }
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(dev->gui_attached && del)
  {
    /* signal that history has changed */
    dt_dev_undo_end_record(dev);

    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MODULE_REMOVE, module);
    /* redraw */
    dt_control_queue_redraw_center();
  }
}

gchar *dt_history_item_get_name(const dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_strdup(module->name());
  else
    label = g_strdup_printf("%s • %s", module->name(), module->multi_name);
  return label;
}

gboolean dt_dev_distort_transform(dt_develop_t *dev,
                                  float *points,
                                  const size_t points_count)
{
  return dt_dev_distort_transform_plus(
    dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

gboolean dt_dev_distort_backtransform(dt_develop_t *dev,
                                      float *points,
                                      const size_t points_count)
{
  return dt_dev_distort_backtransform_plus(
    dev, dev->preview_pipe, 0.0f, DT_DEV_TRANSFORM_DIR_ALL, points, points_count);
}

gboolean dt_dev_distort_transform_plus(dt_develop_t *dev,
                                       dt_dev_pixelpipe_t *pipe,
                                       const double iop_order,
                                       const dt_dev_transform_direction_t transf_direction,
                                       float *points,
                                       const size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  _dev_distort_transform_locked(dev, pipe, iop_order, transf_direction,
                                points, points_count);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return TRUE;
}


gboolean dt_dev_distort_backtransform_plus(dt_develop_t *dev,
                                           dt_dev_pixelpipe_t *pipe,
                                           const double iop_order,
                                           const dt_dev_transform_direction_t transf_direction,
                                           float *points,
                                           const size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  const gboolean success = _dev_distort_backtransform_locked(dev, pipe, iop_order,
                                                            transf_direction, points, points_count);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return success;
}

dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev,
                                                    dt_dev_pixelpipe_t *pipe,
                                                    dt_iop_module_t *module)
{
  for(const GList *pieces = g_list_last(pipe->nodes);
      pieces;
      pieces = g_list_previous(pieces))
  {
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    if(piece->module == module)
    {
      return piece;
    }
  }
  return NULL;
}

dt_hash_t dt_dev_hash_plus(dt_develop_t *dev,
                           dt_dev_pixelpipe_t *pipe,
                           const double iop_order,
                           const dt_dev_transform_direction_t transf_direction)
{
  dt_hash_t hash = DT_INITHASH;
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = modules->data;
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    if(piece->enabled && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
                              && module->iop_order >= iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
                              && module->iop_order > iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
                              && module->iop_order <= iop_order)
                          || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
                              && module->iop_order < iop_order)))
    {
      hash = dt_hash(hash, &piece->hash, sizeof(piece->hash));
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return hash;
}

static gboolean _dev_wait_hash(dt_develop_t *dev,
                               dt_dev_pixelpipe_t *pipe,
                               const double iop_order,
                               const dt_dev_transform_direction_t transf_direction,
                               dt_pthread_mutex_t *lock,
                               const volatile dt_hash_t *const hash)
{
  const int usec = 5000;
  int nloop;

#ifdef HAVE_OPENCL
  if(pipe->devid >= 0)
    nloop = darktable.opencl->opencl_synchronization_timeout;
  else
    nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#else
  nloop = dt_conf_get_int("pixelpipe_synchronization_timeout");
#endif

  if(nloop <= 0) return TRUE;  // non-positive values omit pixelpipe synchronization

  for(int n = 0; n < nloop; n++)
  {
    if(dt_atomic_get_int(&pipe->shutdown))
      return TRUE;  // stop waiting if pipe shuts down

    dt_hash_t probehash;

    if(lock)
    {
      dt_pthread_mutex_lock(lock);
      probehash = *hash;
      dt_pthread_mutex_unlock(lock);
    }
    else
      probehash = *hash;

    if(probehash == dt_dev_hash_plus(dev, pipe, iop_order, transf_direction))
      return TRUE;

    dt_iop_nap(usec);
  }

  return FALSE;
}

gboolean dt_dev_sync_pixelpipe_hash(dt_develop_t *dev,
                                    dt_dev_pixelpipe_t *pipe,
                                    const double iop_order,
                                    const dt_dev_transform_direction_t transf_direction,
                                    dt_pthread_mutex_t *lock,
                                    const volatile dt_hash_t *const hash)
{
  // first wait for matching hash values
  if(_dev_wait_hash(dev, pipe, iop_order, transf_direction, lock, hash))
    return TRUE;

  // timed out. let's see if history stack has changed
  if(pipe->changed & (DT_DEV_PIPE_TOP_CHANGED | DT_DEV_PIPE_REMOVE | DT_DEV_PIPE_SYNCH))
  {
    // history stack has changed. let's trigger reprocessing
    dt_control_queue_redraw_center();
    // pretend that everything is fine
    return TRUE;
  }

  // no way to get pixelpipes in sync
  return FALSE;
}

dt_hash_t dt_dev_hash_distort_plus(dt_develop_t *dev,
                                   dt_dev_pixelpipe_t *pipe,
                                   const double iop_order,
                                   const dt_dev_transform_direction_t transf_direction)
{
  dt_hash_t hash = DT_INITHASH;
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_last(pipe->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = modules->data;
    dt_dev_pixelpipe_iop_t *piece = pieces->data;
    if(piece->enabled && module->operation_tags() & IOP_TAG_DISTORT
       && ((transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_INCL
               && module->iop_order >= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_FORW_EXCL
               && module->iop_order > iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_INCL
               && module->iop_order <= iop_order)
           || (transf_direction == DT_DEV_TRANSFORM_DIR_BACK_EXCL
               && module->iop_order < iop_order)))
    {
      hash = dt_hash(hash, &piece->hash, sizeof(piece->hash));
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return hash;
}

// set the module list order
void dt_dev_reorder_gui_module_list(dt_develop_t *dev)
{
  int pos_module = 0;
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_module_t *module = modules->data;

    GtkWidget *expander = module->expander;
    if(expander)
    {
      gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui,
                                                DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                            expander,
                            pos_module++);
    }
  }
}

void dt_dev_undo_start_record(dt_develop_t *dev)
{
  /* record current history state : before change (needed for undo) */
  if(dev->gui_attached && dt_view_get_current() == DT_VIEW_DARKROOM)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);

  dev->gui_previous_target = NULL;
}

void dt_dev_undo_end_record(dt_develop_t *dev)
{
  /* record current history state : after change (needed for undo) */
  if(dev->gui_attached && dt_view_get_current() == DT_VIEW_DARKROOM)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
}

void dt_dev_image(const dt_imgid_t imgid,
                  const size_t width,
                  const size_t height,
                  const int history_end,
                  uint8_t **buf,
                  float *scale,
                  size_t *buf_width,
                  size_t *buf_height,
                  float *zoom_x,
                  float *zoom_y,
                  const int snapshot_id,
                  GList *module_filter_out,
                  const int devid,
                  const gboolean finalscale)
{
  dt_develop_t dev;
  dt_dev_init(&dev, TRUE);
  dev.gui_attached = FALSE;
  dt_dev_pixelpipe_t *pipe = dev.full.pipe;

  pipe->type |= DT_DEV_PIXELPIPE_IMAGE | (finalscale ? DT_DEV_PIXELPIPE_IMAGE_FINAL : 0);
  // load image and set history_end

  dev.snapshot_id = snapshot_id;

  dt_dev_load_image(&dev, imgid);

  if(history_end != -1 && snapshot_id == -1)
    dt_dev_pop_history_items_ext(&dev, history_end);

  dev.full = darktable.develop->full;
  dev.full.pipe = pipe;

  if(!zoom_x && !zoom_y)
  {
    dev.full.zoom      = DT_ZOOM_FIT;
    dev.full.width     = width;
    dev.full.height    = height;
    dev.full.ppd       = 1.0;
    dev.full.iso_12646 = FALSE;
  }

  // process the pipe

  dev.module_filter_out = module_filter_out;

  dt_dev_process_image_job(&dev, &dev.full, pipe, -1, devid);

  // record resulting image and dimensions

  const uint32_t bufsize =
    sizeof(uint32_t) * pipe->backbuf_width * pipe->backbuf_height;
  *buf = dt_alloc_aligned(bufsize);
  memcpy(*buf, pipe->backbuf, bufsize);

  if(buf_width) *buf_width = pipe->backbuf_width;
  if(buf_height) *buf_height = pipe->backbuf_height;
  if(scale) *scale = pipe->backbuf_scale;
  if(zoom_x) *zoom_x = pipe->backbuf_zoom_x;
  if(zoom_y) *zoom_y = pipe->backbuf_zoom_y;

  dt_dev_cleanup(&dev);
}

gboolean dt_dev_equal_chroma(const float *f, const double *d)
{
  return feqf(f[0], (float)d[0], 0.00001)
      && feqf(f[1], (float)d[1], 0.00001)
      && feqf(f[2], (float)d[2], 0.00001);
}

gboolean dt_dev_is_D65_chroma(const dt_develop_t *dev)
{
  const dt_dev_chroma_t *chr = &dev->chroma;
  const float wb_coeffs[4] = { chr->wb_coeffs[0],
                               chr->wb_coeffs[1],
                               chr->wb_coeffs[2],
                               chr->wb_coeffs[3] };
  return chr->late_correction
    ? dt_dev_equal_chroma(wb_coeffs, chr->as_shot)
    : dt_dev_equal_chroma(wb_coeffs, chr->D65coeffs);
}

void dt_dev_clear_chroma_troubles(dt_develop_t *dev)
{
  if(!dev->gui_attached)
    return;

  dt_dev_chroma_t *chr = &dev->chroma;
  if(chr->temperature)
    dt_iop_set_module_trouble_message(chr->temperature, NULL, NULL, NULL);
  if(chr->adaptation)
    dt_iop_set_module_trouble_message(chr->adaptation, NULL, NULL, NULL);
}

void dt_dev_reset_chroma(dt_develop_t *dev)
{
  if(!dev)
    return;
  dt_dev_clear_chroma_troubles(dev);
  dt_dev_chroma_t *chr = &dev->chroma;
  chr->adaptation = NULL;
  chr->temperature = NULL;
  for_four_channels(c)
    chr->wb_coeffs[c] = 1.0;
}

void dt_dev_init_chroma(dt_develop_t *dev)
{
  dt_dev_reset_chroma(dev);
  dt_dev_chroma_t *chr = &dev->chroma;
  chr->late_correction = FALSE;
  for_four_channels(c)
  {
    chr->D65coeffs[c] = 1.0;
    chr->as_shot[c] = 1.0;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
