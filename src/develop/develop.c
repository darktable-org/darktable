/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson.

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
#include <glib/gprintf.h>
#include <stdlib.h>
#include <unistd.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <stdint.h>

#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/lightroom.h"
#include "control/jobs.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/imageio.h"
#include "common/tags.h"
#include "common/debug.h"
#include "develop/masks.h"
#include "gui/gtk.h"
#include "gui/presets.h"

#define DT_DEV_AVERAGE_DELAY_START 250
#define DT_DEV_PREVIEW_AVERAGE_DELAY_START 50
#define DT_DEV_AVERAGE_DELAY_COUNT 5

const gchar *dt_dev_histogram_type_names[DT_DEV_HISTOGRAM_N] = { "logarithmic", "linear", "waveform" };

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached)
{
  memset(dev, 0, sizeof(dt_develop_t));
  dev->full_preview = FALSE;
  dev->preview_downsampling = 1.0f;
  dev->gui_module = NULL;
  dev->timestamp = 0;
  dev->average_delay = DT_DEV_AVERAGE_DELAY_START;
  dev->preview_average_delay = DT_DEV_PREVIEW_AVERAGE_DELAY_START;
  dev->gui_leaving = 0;
  dev->gui_synch = 0;
  dt_pthread_mutex_init(&dev->history_mutex, NULL);
  dev->history_end = 0;
  dev->history = NULL; // empty list

  dev->gui_attached = gui_attached;
  dev->width = -1;
  dev->height = -1;

  dt_image_init(&dev->image_storage);
  dev->image_status = dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->image_loading = dev->preview_loading = 0;
  dev->image_force_reload = 0;
  dev->preview_input_changed = 0;

  dev->pipe = dev->preview_pipe = NULL;
  dt_pthread_mutex_init(&dev->pipe_mutex, NULL);
  dt_pthread_mutex_init(&dev->preview_pipe_mutex, NULL);
  //   dt_pthread_mutex_init(&dev->histogram_waveform_mutex, NULL);
  dev->histogram = NULL;
  dev->histogram_pre_tonecurve = NULL;
  dev->histogram_pre_levels = NULL;
  gchar *mode = dt_conf_get_string("plugins/darkroom/histogram/mode");
  if(g_strcmp0(mode, "linear") == 0)
    dev->histogram_type = DT_DEV_HISTOGRAM_LINEAR;
  else if(g_strcmp0(mode, "logarithmic") == 0)
    dev->histogram_type = DT_DEV_HISTOGRAM_LOGARITHMIC;
  else if(g_strcmp0(mode, "waveform") == 0)
    dev->histogram_type = DT_DEV_HISTOGRAM_WAVEFORM;
  g_free(mode);

  if(dev->gui_attached)
  {
    dev->pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dev->preview_pipe = (dt_dev_pixelpipe_t *)malloc(sizeof(dt_dev_pixelpipe_t));
    dt_dev_pixelpipe_init(dev->pipe);
    dt_dev_pixelpipe_init_preview(dev->preview_pipe);

    dev->histogram = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_tonecurve = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));
    dev->histogram_pre_levels = (uint32_t *)calloc(4 * 256, sizeof(uint32_t));

    dev->histogram_max = -1;
    dev->histogram_pre_tonecurve_max = -1;
    dev->histogram_pre_levels_max = -1;
  }

  dev->iop_instance = 0;
  dev->iop = NULL;

  dev->overexposed.enabled = FALSE;
  dev->overexposed.colorscheme = dt_conf_get_int("darkroom/ui/overexposed/colorscheme");
  dev->overexposed.lower = dt_conf_get_float("darkroom/ui/overexposed/lower");
  dev->overexposed.upper = dt_conf_get_float("darkroom/ui/overexposed/upper");
}

void dt_dev_cleanup(dt_develop_t *dev)
{
  if(!dev) return;
  // image_cache does not have to be unref'd, this is done outside develop module.
  dt_pthread_mutex_destroy(&dev->pipe_mutex);
  dt_pthread_mutex_destroy(&dev->preview_pipe_mutex);
  //   dt_pthread_mutex_destroy(&dev->histogram_waveform_mutex);
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
    free(((dt_dev_history_item_t *)dev->history->data)->blend_params);
    free((dt_dev_history_item_t *)dev->history->data);
    dev->history = g_list_delete_link(dev->history, dev->history);
  }
  while(dev->iop)
  {
    dt_iop_cleanup_module((dt_iop_module_t *)dev->iop->data);
    free(dev->iop->data);
    dev->iop = g_list_delete_link(dev->iop, dev->iop);
  }
  dt_pthread_mutex_destroy(&dev->history_mutex);
  free(dev->histogram);
  free(dev->histogram_pre_tonecurve);
  free(dev->histogram_pre_levels);

  dt_conf_set_int("darkroom/ui/overexposed/colorscheme", dev->overexposed.colorscheme);
  dt_conf_set_float("darkroom/ui/overexposed/lower", dev->overexposed.lower);
  dt_conf_set_float("darkroom/ui/overexposed/upper", dev->overexposed.upper);
}

void dt_dev_process_image(dt_develop_t *dev)
{
  if(!dev->gui_attached || dev->pipe->processing) return;
  int err
      = dt_control_add_job_res(darktable.control, dt_dev_process_image_job_create(dev), DT_CTL_WORKER_ZOOM_1);
  if(err) fprintf(stderr, "[dev_process_image] job queue exceeded!\n");
}

void dt_dev_process_preview(dt_develop_t *dev)
{
  if(!dev->gui_attached) return;
  int err = dt_control_add_job_res(darktable.control, dt_dev_process_preview_job_create(dev),
                                   DT_CTL_WORKER_ZOOM_FILL);
  if(err) fprintf(stderr, "[dev_process_preview] job queue exceeded!\n");
}

void dt_dev_invalidate(dt_develop_t *dev)
{
  dev->image_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
  if(dev->preview_pipe) dev->preview_pipe->input_timestamp = dev->timestamp;
}

void dt_dev_invalidate_all(dt_develop_t *dev)
{
  dev->image_status = dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
  dev->timestamp++;
}

void dt_dev_process_preview_job(dt_develop_t *dev)
{
  dt_mipmap_buffer_t buf;
  if(dev->image_loading)
  {
    // raw is already loading, no use starting another file access, we wait.
    return;
  }

  dt_pthread_mutex_lock(&dev->preview_pipe_mutex);
  dt_control_log_busy_enter();
  dev->preview_pipe->input_timestamp = dev->timestamp;
  dev->preview_status = DT_DEV_PIXELPIPE_RUNNING;

  // lock if there, issue a background load, if not (best-effort for mip f).
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, dev->image_storage.id, DT_MIPMAP_F, 0, 'r');
  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
    return; // not loaded yet. load will issue a gtk redraw on completion, which in turn will trigger us again
            // later.
  }
  // init pixel pipeline for preview.
  dt_dev_pixelpipe_set_input(dev->preview_pipe, dev, (float *)buf.buf, buf.width, buf.height,
                             dev->image_storage.width / (float)buf.width);

  if(dev->preview_loading)
  {
    dt_dev_pixelpipe_cleanup_nodes(dev->preview_pipe);
    dt_dev_pixelpipe_create_nodes(dev->preview_pipe, dev);
    dt_dev_pixelpipe_flush_caches(dev->preview_pipe);
    dev->preview_loading = 0;
  }

  // if raw loaded, get new mipf
  if(dev->preview_input_changed)
  {
    dt_dev_pixelpipe_flush_caches(dev->preview_pipe);
    dev->preview_input_changed = 0;
  }

// always process the whole downsampled mipf buffer, to allow for fast scrolling and mip4 write-through.
restart:
  if(dev->gui_leaving)
  {
    dt_control_log_busy_leave();
    dev->preview_status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }
  // adjust pipeline according to changed flag set by {add,pop}_history_item.
  // this locks dev->history_mutex.
  dt_times_t start;
  dt_get_times(&start);
  dt_dev_pixelpipe_change(dev->preview_pipe, dev);
  if(dt_dev_pixelpipe_process(
         dev->preview_pipe, dev, 0, 0, dev->preview_pipe->processed_width * dev->preview_downsampling,
         dev->preview_pipe->processed_height * dev->preview_downsampling, dev->preview_downsampling))
  {
    if(dev->preview_loading || dev->preview_input_changed)
    {
      dt_control_log_busy_leave();
      dev->preview_status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      return;
    }
    else
      goto restart;
  }

  dev->preview_status = DT_DEV_PIXELPIPE_VALID;

  dt_show_times(&start, "[dev_process_preview] pixel pipeline processing", NULL);
  dt_dev_average_delay_update(&start, &dev->preview_average_delay);

  // redraw the whole thing, to also update color picker values and histograms etc.
  if(dev->gui_attached) dt_control_queue_redraw();
  dt_control_log_busy_leave();
  dt_pthread_mutex_unlock(&dev->preview_pipe_mutex);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
}

void dt_dev_process_image_job(dt_develop_t *dev)
{
  dt_pthread_mutex_lock(&dev->pipe_mutex);
  dt_control_log_busy_enter();
  // let gui know to draw preview instead of us, if it's there:
  dev->image_status = DT_DEV_PIXELPIPE_RUNNING;

  dt_mipmap_buffer_t buf;
  dt_times_t start;
  dt_get_times(&start);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, dev->image_storage.id, DT_MIPMAP_FULL,
                           DT_MIPMAP_BLOCKING, 'r');
  dt_show_times(&start, "[dev]", "to load the image.");

  // failed to load raw?
  if(!buf.buf)
  {
    dt_control_log_busy_leave();
    dev->image_status = DT_DEV_PIXELPIPE_DIRTY;
    dt_pthread_mutex_unlock(&dev->pipe_mutex);
    return;
  }

  dt_dev_pixelpipe_set_input(dev->pipe, dev, (float *)buf.buf, buf.width, buf.height, 1.0);

  if(dev->image_loading)
  {
    // init pixel pipeline
    dt_dev_pixelpipe_cleanup_nodes(dev->pipe);
    dt_dev_pixelpipe_create_nodes(dev->pipe, dev);
    if(dev->image_force_reload) dt_dev_pixelpipe_flush_caches(dev->pipe);
    dev->image_force_reload = 0;
    if(dev->gui_attached)
    {
      // during load, a mipf update could have been issued.
      dev->preview_input_changed = 1;
      dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;
      dev->gui_synch = 1; // notify gui thread we want to synch (call gui_update in the modules)
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    }
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  }

  dt_dev_zoom_t zoom;
  float zoom_x, zoom_y, scale;
  int window_width, window_height, x, y, closeup;
  dt_dev_pixelpipe_change_t pipe_changed;

// adjust pipeline according to changed flag set by {add,pop}_history_item.
restart:
  if(dev->gui_leaving)
  {
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    dt_control_log_busy_leave();
    dev->image_status = DT_DEV_PIXELPIPE_INVALID;
    dt_pthread_mutex_unlock(&dev->pipe_mutex);
    return;
  }
  dev->pipe->input_timestamp = dev->timestamp;
  // dt_dev_pixelpipe_change() will clear the changed value
  pipe_changed = dev->pipe->changed;
  // this locks dev->history_mutex.
  dt_dev_pixelpipe_change(dev->pipe, dev);
  // determine scale according to new dimensions
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom_x = dt_control_get_dev_zoom_x();
  zoom_y = dt_control_get_dev_zoom_y();
  // if just changed to an image with a different aspect ratio or
  // altered image orientation, the prior zoom xy could now be beyond
  // the image boundary
  if(dev->image_loading || (pipe_changed != DT_DEV_PIPE_UNCHANGED))
  {
    dt_dev_check_zoom_bounds(dev, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
    dt_control_set_dev_zoom_x(zoom_x);
    dt_control_set_dev_zoom_y(zoom_y);
  }

  scale = dt_dev_get_zoom_scale(dev, zoom, 1.0f, 0) * darktable.gui->ppd;
  window_width = dev->width * darktable.gui->ppd;
  window_height = dev->height * darktable.gui->ppd;
  if(closeup)
  {
    window_width /= 2;
    window_height /= 2;
  }
  const int wd = MIN(window_width, dev->pipe->processed_width * scale);
  const int ht = MIN(window_height, dev->pipe->processed_height * scale);
  x = MAX(0, scale * dev->pipe->processed_width  * (.5 + zoom_x) - wd / 2);
  y = MAX(0, scale * dev->pipe->processed_height * (.5 + zoom_y) - ht / 2);

  dt_get_times(&start);
  if(dt_dev_pixelpipe_process(dev->pipe, dev, x, y, wd, ht, scale))
  {
    // interrupted because image changed?
    if(dev->image_force_reload)
    {
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      dt_control_log_busy_leave();
      dev->image_status = DT_DEV_PIXELPIPE_INVALID;
      dt_pthread_mutex_unlock(&dev->pipe_mutex);
      return;
    }
    // or because the pipeline changed?
    else
      goto restart;
  }
  dt_show_times(&start, "[dev_process_image] pixel pipeline processing", NULL);
  dt_dev_average_delay_update(&start, &dev->average_delay);

  // maybe we got zoomed/panned in the meantime?
  if(dev->pipe->changed != DT_DEV_PIPE_UNCHANGED) goto restart;

  // cool, we got a new image!
  dev->image_status = DT_DEV_PIXELPIPE_VALID;
  dev->image_loading = 0;

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  // redraw the whole thing, to also update color picker values and histograms etc.
  if(dev->gui_attached) dt_control_queue_redraw();
  dt_control_log_busy_leave();
  dt_pthread_mutex_unlock(&dev->pipe_mutex);
}

// load the raw and get the new image struct, blocking in gui thread
static inline void _dt_dev_load_raw(dt_develop_t *dev, const uint32_t imgid)
{
  // first load the raw, to make sure dt_image_t will contain all and correct data.
  dt_mipmap_buffer_t buf;
  dt_times_t start;
  dt_get_times(&start);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  dt_show_times(&start, "[dev]", "to load the image.");

  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  dev->image_storage = *image;
  dt_image_cache_read_release(darktable.image_cache, image);
}

void dt_dev_reload_image(dt_develop_t *dev, const uint32_t imgid)
{
  _dt_dev_load_raw(dev, imgid);
  dev->image_force_reload = dev->image_loading = dev->preview_loading = 1;

  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate(dev); // only invalidate image, preview will follow once it's loaded.
}

float dt_dev_get_zoom_scale(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup_factor, int preview)
{
  float zoom_scale;

  const float w = preview ? dev->preview_pipe->processed_width : dev->pipe->processed_width;
  const float h = preview ? dev->preview_pipe->processed_height : dev->pipe->processed_height;
  const float ps = dev->pipe->backbuf_width
                       ? dev->pipe->processed_width / (float)dev->preview_pipe->processed_width
                       : dev->preview_pipe->iscale / dev->preview_downsampling;

  switch(zoom)
  {
    case DT_ZOOM_FIT:
      zoom_scale = fminf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_FILL:
      zoom_scale = fmaxf(dev->width / w, dev->height / h);
      break;
    case DT_ZOOM_1:
      zoom_scale = closeup_factor;
      if(preview) zoom_scale *= ps;
      break;
    default: // DT_ZOOM_FREE
      zoom_scale = dt_control_get_dev_zoom_scale();
      if(preview) zoom_scale *= ps;
      break;
  }
  return zoom_scale;
}

void dt_dev_load_image(dt_develop_t *dev, const uint32_t imgid)
{
  _dt_dev_load_raw(dev, imgid);

  if(dev->pipe)
  {
    dev->pipe->processed_width = 0;
    dev->pipe->processed_height = 0;
  }
  dev->image_loading = 1;
  dev->preview_loading = 1;
  dev->first_load = 1;
  dev->image_status = dev->preview_status = DT_DEV_PIXELPIPE_DIRTY;

  dev->iop = dt_iop_load_modules(dev);

  dt_masks_read_forms(dev);
  dev->form_visible = NULL;

  dt_dev_read_history(dev);

  dev->first_load = 0;
}

void dt_dev_configure(dt_develop_t *dev, int wd, int ht)
{
  // fixed border on every side
  const int tb = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  wd -= 2*tb;
  ht -= 2*tb;
  if(dev->width != wd || dev->height != ht)
  {
    dev->width = wd;
    dev->height = ht;
    dev->preview_pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dev->pipe->changed |= DT_DEV_PIPE_ZOOMED;
    dt_dev_invalidate(dev);
  }
}

// helper used to synch a single history item with db
int dt_dev_write_history_item(const dt_image_t *image, dt_dev_history_item_t *h, int32_t num)
{
  if(!image) return 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select num from history where imgid = ?1 and num = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, image->id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
  if(sqlite3_step(stmt) != SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "insert into history (imgid, num) values (?1, ?2)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, image->id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);
    sqlite3_step(stmt);
  }
  // printf("[dev write history item] writing %d - %s params %f %f\n", h->module->instance, h->module->op,
  // *(float *)h->params, *(((float *)h->params)+1));
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "update history set operation = ?1, op_params = ?2, module = ?3, enabled = ?4, "
                              "blendop_params = ?7, blendop_version = ?8, multi_priority = ?9, multi_name = "
                              "?10 where imgid = ?5 and num = ?6",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, h->module->op, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 2, h->params, h->module->params_size, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, h->module->version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, h->enabled);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, image->id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 6, num);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 7, h->blend_params, sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, dt_develop_blend_version());
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, h->multi_priority);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 10, h->multi_name, -1, SQLITE_TRANSIENT);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return 0;
}

void dt_dev_add_history_item(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable)
{
  if(darktable.gui->reset) return;
  dt_pthread_mutex_lock(&dev->history_mutex);
  if(dev->gui_attached)
  {
    GList *history = g_list_nth(dev->history, dev->history_end);
    while(history)
    {
      GList *next = g_list_next(history);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
      // printf("removing obsoleted history item: %s\n", hist->module->op);
      free(hist->params);
      free(hist->blend_params);
      free(history->data);
      dev->history = g_list_delete_link(dev->history, history);
      history = next;
    }
    history = g_list_nth(dev->history, dev->history_end - 1);
    dt_dev_history_item_t *hist = history ? (dt_dev_history_item_t *)(history->data) : 0;
    if(!history || // if no history yet, push new item for sure.
       (( module->instance != hist->module->instance             // add new item for different op
       || module->multi_priority != hist->module->multi_priority // or instance
       || dev->focus_hash != hist->focus_hash)                   // or if focused out and in
       && (// but only add item if there is a difference at all for the same module
         (module->params_size != hist->module->params_size) ||
         (module->params_size == hist->module->params_size && memcmp(hist->params, module->params, module->params_size)))))
    {
      // new operation, push new item
      // printf("adding new history item %d - %s\n", dev->history_end, module->op);
      // if(history) printf("because item %d - %s is different operation.\n", dev->history_end-1,
      // ((dt_dev_history_item_t *)history->data)->module->op);
      dev->history_end++;
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));
      if(enable)
      {
        module->enabled = TRUE;
        if(module->off)
        {
          darktable.gui->reset = 1;
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
          darktable.gui->reset = 0;
        }
      }
      hist->focus_hash = dev->focus_hash;
      hist->enabled = module->enabled;
      hist->module = module;
      hist->params = malloc(module->params_size);
      hist->multi_priority = module->multi_priority;
      snprintf(hist->multi_name, sizeof(hist->multi_name), "%s", module->multi_name);
      /* allocate and set hist blend_params */
      hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
      memcpy(hist->params, module->params, module->params_size);
      memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));

      dev->history = g_list_append(dev->history, hist);
      dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
      dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // topology remains, as modules are fixed for now.
    }
    else
    {
      // same operation, change params
      // printf("changing same history item %d - %s\n", dev->history_end-1, module->op);
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
      memcpy(hist->params, module->params, module->params_size);

      if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
        memcpy(hist->blend_params, module->blend_params, sizeof(dt_develop_blend_params_t));

      // if the user changed stuff and the module is still not enabled, do it:
      if(!hist->enabled && !module->enabled)
      {
        module->enabled = 1;
        if(module->off)
        {
          darktable.gui->reset = 1;
          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(module->off), module->enabled);
          darktable.gui->reset = 0;
        }
      }
      hist->multi_priority = module->multi_priority;
      memcpy(hist->multi_name, module->multi_name, sizeof(module->multi_name));
      hist->enabled = module->enabled;
      dev->pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
      dev->preview_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
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

  // invalidate buffers and force redraw of darkroom
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);

  if(dev->gui_attached)
  {
    /* signal that history has changed */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_reload_history_items(dt_develop_t *dev)
{
  dev->focus_hash = 0;
  dt_dev_pop_history_items(dev, 0);
  // remove unused history items:
  GList *history = g_list_nth(dev->history, dev->history_end);
  while(history)
  {
    GList *next = g_list_next(history);
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    free(hist->params);
    free(hist->blend_params);
    free(history->data);
    dev->history = g_list_delete_link(dev->history, history);
    history = next;
  }
  dt_dev_read_history(dev);

  // we have to add new module instances first
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    if(module->multi_priority > 0)
    {
      if(!dt_iop_is_hidden(module) && !module->expander)
      {
        module->gui_init(module);
        dt_iop_reload_defaults(module);
        // we search the base iop corresponding
        GList *mods = g_list_first(dev->iop);
        dt_iop_module_t *base = NULL;
        int pos_module = 0;
        int pos_base = 0;
        int pos = 0;
        while(mods)
        {
          dt_iop_module_t *mod = (dt_iop_module_t *)(mods->data);
          if(mod->multi_priority == 0 && mod->instance == module->instance)
          {
            base = mod;
            pos_base = pos;
          }
          else if(mod == module)
            pos_module = pos;
          mods = g_list_next(mods);
          pos++;
        }
        if(!base) continue;

        /* add module to right panel */
        GtkWidget *expander = dt_iop_gui_get_expander(module);
        dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER, expander);
        GValue gv = { 0, { { 0 } } };
        g_value_init(&gv, G_TYPE_INT);
        gtk_container_child_get_property(
            GTK_CONTAINER(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER)),
            base->expander, "position", &gv);
        gtk_box_reorder_child(dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER),
                              expander, g_value_get_int(&gv) + pos_base - pos_module);
        dt_iop_gui_set_expanded(module, TRUE, FALSE);
        dt_iop_gui_update_blending(module);

        // the pipe need to be reconstruct
        dev->pipe->changed |= DT_DEV_PIPE_REMOVE;
        dev->preview_pipe->changed |= DT_DEV_PIPE_REMOVE;
      }
    }
    else if(!dt_iop_is_hidden(module) && module->expander)
    {
      // we have to ensure that the name of the widget is correct
      GtkWidget *wlabel;
      GtkWidget *header = gtk_bin_get_child(
          GTK_BIN(g_list_nth_data(gtk_container_get_children(GTK_CONTAINER(module->expander)), 0)));

      /* get arrow icon widget */
      wlabel = g_list_nth(gtk_container_get_children(GTK_CONTAINER(header)), 5)->data;
      gchar *label = dt_history_item_get_name_html(module);
      gtk_label_set_markup(GTK_LABEL(wlabel), label);
      g_free(label);
    }
    modules = g_list_next(modules);
  }

  dt_dev_pop_history_items(dev, dev->history_end);
}

void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt)
{
  // printf("dev popping all history items >= %d\n", cnt);
  dt_pthread_mutex_lock(&dev->history_mutex);
  darktable.gui->reset = 1;
  dev->history_end = cnt;
  // reset gui params for all modules
  GList *modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    memcpy(module->params, module->default_params, module->params_size);
    memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
    module->enabled = module->default_enabled;
    modules = g_list_next(modules);
  }
  // go through history and set gui params
  GList *history = dev->history;
  for(int i = 0; i < cnt && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    memcpy(hist->module->params, hist->params, hist->module->params_size);
    memcpy(hist->module->blend_params, hist->blend_params, sizeof(dt_develop_blend_params_t));

    hist->module->enabled = hist->enabled;
    snprintf(hist->module->multi_name, sizeof(hist->module->multi_name), "%s", hist->multi_name);

    history = g_list_next(history);
  }
  // update all gui modules
  modules = dev->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_iop_gui_update(module);
    modules = g_list_next(modules);
  }
  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
  darktable.gui->reset = 0;
  dt_dev_invalidate_all(dev);
  dt_pthread_mutex_unlock(&dev->history_mutex);
  dt_control_queue_redraw_center();
}

void dt_dev_write_history(dt_develop_t *dev)
{
  sqlite3_stmt *stmt;

  gboolean changed = FALSE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  GList *history = dev->history;
  for(int i = 0; i < dev->history_end && history; i++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(history->data);
    (void)dt_dev_write_history_item(&dev->image_storage, hist, i);
    history = g_list_next(history);
    changed = TRUE;
  }

  /* attach / detach changed tag reflecting actual change */
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  if(changed)
    dt_tag_attach(tagid, dev->image_storage.id);
  else
    dt_tag_detach(tagid, dev->image_storage.id);
}

static void auto_apply_presets(dt_develop_t *dev)
{
  const int imgid = dev->image_storage.id;

  if(imgid <= 0) return;

  // be extra sure that we don't mess up history in separate threads:
  dt_pthread_mutex_lock(&darktable.db_insert);

  int run = 0;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');
  if(!(image->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)) run = 1;

  // flag was already set? only apply presets once in the lifetime of a history stack.
  // (the flag will be cleared when removing it)
  if(!run || image->id <= 0)
  {
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
    dt_pthread_mutex_unlock(&darktable.db_insert);
    return;
  }

  // cleanup
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "delete from memory.history", NULL, NULL, NULL);
  const char *preset_table[2] = { "presets", "legacy_presets" };
  const int legacy = (image->flags & DT_IMAGE_NO_LEGACY_PRESETS) ? 0 : 1;
  char query[1024];
  snprintf(query, sizeof(query), "insert into memory.history select ?1, 0, op_version, operation, op_params, "
                                 "enabled, blendop_params, blendop_version, multi_priority, multi_name "
                                 "from %s where autoapply=1 and "
                                 "?2 like model and ?3 like maker and ?4 like lens and "
                                 "?5 between iso_min and iso_max and "
                                 "?6 between exposure_min and exposure_max and "
                                 "?7 between aperture_min and aperture_max and "
                                 "?8 between focal_length_min and focal_length_max and "
                                 "(format = 0 or format&?9!=0) order by writeprotect desc, "
                                 "length(model), length(maker), length(lens)",
           preset_table[legacy]);
  // query for all modules at once:
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, image->exif_model, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, image->exif_maker, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, image->exif_lens, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 5, fmaxf(0.0f, fminf(1000000, image->exif_iso)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, fmaxf(0.0f, fminf(1000000, image->exif_exposure)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, fmaxf(0.0f, fminf(1000000, image->exif_aperture)));
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 8, fmaxf(0.0f, fminf(1000000, image->exif_focal_length)));
  // 0: dontcare, 1: ldr, 2: raw
  DT_DEBUG_SQLITE3_BIND_DOUBLE(
      stmt, 9, dt_image_is_ldr(image) ? FOR_LDR : (dt_image_is_raw(image) ? FOR_RAW : FOR_HDR));

  if(sqlite3_step(stmt) == SQLITE_DONE)
  {
    sqlite3_finalize(stmt);
    int cnt = 0;
    // count what we found:
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(*) from memory.history", -1,
                                &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // if there is anything..
      cnt = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
      // fprintf(stderr, "[auto_apply_presets] imageid %d found %d matching presets (legacy %d)\n", imgid,
      // cnt, legacy);
      // advance the current history by that amount:
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "update history set num=num+?1 where imgid=?2", -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, cnt);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);

      if(sqlite3_step(stmt) == SQLITE_DONE)
      {
        // and finally prepend the rest with increasing numbers (starting at 0)
        sqlite3_finalize(stmt);
        DT_DEBUG_SQLITE3_PREPARE_V2(
            dt_database_get(darktable.db),
            "insert into history select imgid, rowid-1, module, operation, op_params, enabled, "
            "blendop_params, blendop_version, multi_priority, multi_name from memory.history",
            -1, &stmt, NULL);
        sqlite3_step(stmt);
      }
    }
  }
  sqlite3_finalize(stmt);

  //  first time we are loading the image, try to import lightroom .xmp if any
  if(dev->image_loading) dt_lightroom_import(dev->image_storage.id, dev, TRUE);

  image->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED | DT_IMAGE_NO_LEGACY_PRESETS;
  dt_pthread_mutex_unlock(&darktable.db_insert);

  // make sure these end up in the image_cache + xmp (sync through here if we set the flag)
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
}

void dt_dev_read_history(dt_develop_t *dev)
{
  if(dev->image_storage.id <= 0) return;
  if(!dev->iop) return;

  // maybe prepend auto-presets to history before loading it:
  auto_apply_presets(dev);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select imgid, num, module, operation, "
                                                             "op_params, enabled, blendop_params, "
                                                             "blendop_version, multi_priority, multi_name "
                                                             "from history where imgid = ?1 order by num",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  dev->history_end = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-num, 2-module_instance, 3-operation char, 4-params blob, 5-enabled, 6-blend_params,
    // 7-blendop_version, 8 multi_priority, 9 multi_name
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)malloc(sizeof(dt_dev_history_item_t));
    hist->enabled = sqlite3_column_int(stmt, 5);

    GList *modules = dev->iop;
    const char *opname = (const char *)sqlite3_column_text(stmt, 3);
    int multi_priority = sqlite3_column_int(stmt, 8);
    const char *multi_name = (const char *)sqlite3_column_text(stmt, 9);
    if(!opname)
    {
      fprintf(stderr, "[dev_read_history] database history for image `%s' seems to be corrupted!\n",
              dev->image_storage.filename);
      free(hist);
      continue;
    }

    hist->module = NULL;
    dt_iop_module_t *find_op = NULL;
    while(opname && modules)
    {
      dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
      if(!strcmp(module->op, opname))
      {
        if(module->multi_priority == multi_priority)
        {
          hist->module = module;
          if(multi_name)
            snprintf(module->multi_name, sizeof(module->multi_name), "%s", multi_name);
          else
            memset(module->multi_name, 0, sizeof(module->multi_name));
          break;
        }
        else if(multi_priority > 0)
        {
          // we just say that we find the name, so we just have to add new instance of this module
          find_op = module;
        }
      }
      modules = g_list_next(modules);
    }
    if(!hist->module && find_op)
    {
      // we have to add a new instance of this module and set index to modindex
      dt_iop_module_t *new_module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
      if(!dt_iop_load_module(new_module, find_op->so, dev))
      {
        new_module->multi_priority = multi_priority;

        snprintf(new_module->multi_name, sizeof(new_module->multi_name), "%s", multi_name);

        dev->iop = g_list_insert_sorted(dev->iop, new_module, sort_plugins);

        new_module->instance = find_op->instance;
        hist->module = new_module;
      }
    }

    if(!hist->module && opname)
    {
      fprintf(
          stderr,
          "[dev_read_history] the module `%s' requested by image `%s' is not installed on this computer!\n",
          opname, dev->image_storage.filename);
      free(hist);
      continue;
    }

    if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK)
    {
      free(hist);
      continue;
    }

    int modversion = sqlite3_column_int(stmt, 2);
    assert(strcmp((char *)sqlite3_column_text(stmt, 3), hist->module->op) == 0);
    hist->params = malloc(hist->module->params_size);
    hist->blend_params = malloc(sizeof(dt_develop_blend_params_t));
    snprintf(hist->multi_name, sizeof(hist->multi_name), "%s", multi_name);
    hist->multi_priority = multi_priority;

    const void *blendop_params = sqlite3_column_blob(stmt, 6);
    int bl_length = sqlite3_column_bytes(stmt, 6);
    int blendop_version = sqlite3_column_int(stmt, 7);

    if(blendop_params && (blendop_version == dt_develop_blend_version())
       && (bl_length == sizeof(dt_develop_blend_params_t)))
    {
      memcpy(hist->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));
    }
    else if(blendop_params
            && dt_develop_blend_legacy_params(hist->module, blendop_params, blendop_version,
                                              hist->blend_params, dt_develop_blend_version(), bl_length) == 0)
    {
      // do nothing
    }
    else
    {
      memcpy(hist->blend_params, hist->module->default_blendop_params, sizeof(dt_develop_blend_params_t));
    }

    if(hist->module->version() != modversion || hist->module->params_size != sqlite3_column_bytes(stmt, 4)
       || strcmp((char *)sqlite3_column_text(stmt, 3), hist->module->op))
    {
      if(!hist->module->legacy_params
         || hist->module->legacy_params(hist->module, sqlite3_column_blob(stmt, 4), labs(modversion),
                                        hist->params, labs(hist->module->version())))
      {
        free(hist->params);
        free(hist->blend_params);
        fprintf(stderr, "[dev_read_history] module `%s' version mismatch: history is %d, dt %d.\n",
                hist->module->op, modversion, hist->module->version());
        const char *fname = dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;
        if(fname > dev->image_storage.filename) fname++;
        dt_control_log(_("%s: module `%s' version mismatch: %d != %d"), fname, hist->module->op,
                       hist->module->version(), modversion);
        free(hist);
        continue;
      }
      else
      {
        if(!strcmp(hist->module->op, "spots") && modversion == 1)
        {
          // quick and dirty hack to handle spot removal legacy_params
          memcpy(hist->blend_params, hist->module->blend_params, sizeof(dt_develop_blend_params_t));
          memcpy(hist->module->blend_params, hist->module->default_blendop_params,
                 sizeof(dt_develop_blend_params_t));
        }
      }

      /*
       * Fix for flip iop: previously it was not always needed, but it might be
       * in history stack as "orientation (off)", but now we always want it
       * by default, so if it is disabled, enable it, and replace params with
       * default_params. if user want to, he can disable it.
       */
      if(!strcmp(hist->module->op, "flip") && hist->enabled == 0 && labs(modversion) == 1)
      {
        memcpy(hist->params, hist->module->default_params, hist->module->params_size);
        hist->enabled = 1;
      }
    }
    else
    {
      memcpy(hist->params, sqlite3_column_blob(stmt, 4), hist->module->params_size);
    }

    // make sure that always-on modules are always on. duh.
    if(hist->module->default_enabled == 1 && hist->module->hide_enable_button == 1)
    {
      hist->enabled = 1;
    }

    // memcpy(hist->module->params, hist->params, hist->module->params_size);
    // hist->module->enabled = hist->enabled;
    // printf("[dev read history] img %d number %d for operation %d - %s params %f %f\n",
    // sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1), instance, hist->module->op, *(float
    // *)hist->params, *(((float*)hist->params)+1));
    dev->history = g_list_append(dev->history, hist);
    dev->history_end++;
  }

  if(dev->gui_attached)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH; // again, fixed topology for now.
    dt_dev_invalidate_all(dev);

    /* signal history changed */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  }
  sqlite3_finalize(stmt);
}


void dt_dev_reprocess_all(dt_develop_t *dev)
{
  if(darktable.gui->reset) return;
  if(dev && dev->gui_attached)
  {
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->pipe->cache_obsolete = 1;
    dev->preview_pipe->cache_obsolete = 1;

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
    dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
    dev->pipe->cache_obsolete = 1;

    // invalidate buffers and force redraw of darkroom
    dt_dev_invalidate_all(dev);

    /* redraw */
    dt_control_queue_redraw_center();
  }
}


void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom,
                              int closeup, float *boxww, float *boxhh)
{
  int procw = 0, proch = 0;
  dt_dev_get_processed_size(dev, &procw, &proch);
  float boxw = 1, boxh = 1; // viewport in normalised space
                            //   if(zoom == DT_ZOOM_1)
                            //   {
                            //     const float imgw = (closeup ? 2 : 1)*procw;
                            //     const float imgh = (closeup ? 2 : 1)*proch;
                            //     const float devw = MIN(imgw, dev->width);
                            //     const float devh = MIN(imgh, dev->height);
                            //     boxw = fminf(1.0, devw/imgw);
                            //     boxh = fminf(1.0, devh/imgh);
                            //   }
  if(zoom == DT_ZOOM_FIT)
  {
    *zoom_x = *zoom_y = 0.0f;
    boxw = boxh = 1.0f;
  }
  else
  {
    const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 0);
    const float imgw = procw;
    const float imgh = proch;
    const float devw = dev->width;
    const float devh = dev->height;
    boxw = devw / (imgw * scale);
    boxh = devh / (imgh * scale);
  }

  if(*zoom_x < boxw / 2 - .5) *zoom_x = boxw / 2 - .5;
  if(*zoom_x > .5 - boxw / 2) *zoom_x = .5 - boxw / 2;
  if(*zoom_y < boxh / 2 - .5) *zoom_y = boxh / 2 - .5;
  if(*zoom_y > .5 - boxh / 2) *zoom_y = .5 - boxh / 2;
  if(boxw > 1.0) *zoom_x = 0.f;
  if(boxh > 1.0) *zoom_y = 0.f;

  if(boxww) *boxww = boxw;
  if(boxhh) *boxhh = boxh;
}

void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch)
{
  if(!dev) return;

  // if pipe is processed, lets return its size
  if(dev->pipe && dev->pipe->processed_width)
  {
    *procw = dev->pipe->processed_width;
    *proch = dev->pipe->processed_height;
    return;
  }

  // fallback on preview pipe
  if(dev->preview_pipe && dev->preview_pipe->processed_width)
  {
    const float scale = (dev->preview_pipe->iscale / dev->preview_downsampling);
    *procw = scale * dev->preview_pipe->processed_width;
    *proch = scale * dev->preview_pipe->processed_height;
    return;
  }

  // no processed pipes, lets return 0 size
  *procw = *proch = 0;
  return;
}

void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev, const float px, const float py, float *zoom_x,
                                 float *zoom_y)
{
  dt_dev_zoom_t zoom;
  int closeup, procw = 0, proch = 0;
  float zoom2_x, zoom2_y;
  zoom = dt_control_get_dev_zoom();
  closeup = dt_control_get_dev_closeup();
  zoom2_x = dt_control_get_dev_zoom_x();
  zoom2_y = dt_control_get_dev_zoom_y();
  dt_dev_get_processed_size(dev, &procw, &proch);
  const float scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2.0 : 1.0, 0);
  // offset from center now (current zoom_{x,y} points there)
  const float mouse_off_x = px - .5 * dev->width, mouse_off_y = py - .5 * dev->height;
  zoom2_x += mouse_off_x / (procw * scale);
  zoom2_y += mouse_off_y / (proch * scale);
  *zoom_x = zoom2_x;
  *zoom_y = zoom2_y;
}

void dt_dev_get_history_item_label(dt_dev_history_item_t *hist, char *label, const int cnt)
{
  gchar *module_label = dt_history_item_get_name(hist->module);
  g_snprintf(label, cnt, "%s (%s)", module_label, hist->enabled ? _("on") : _("off"));
  g_free(module_label);
}

int dt_dev_is_current_image(dt_develop_t *dev, uint32_t imgid)
{
  return (dev->image_storage.id == imgid) ? 1 : 0;
}

gboolean dt_dev_exposure_hooks_available(dt_develop_t *dev)
{
  /* check if exposure iop module has registered its hooks */
  if(dev->proxy.exposure.module && dev->proxy.exposure.set_black && dev->proxy.exposure.get_black
     && dev->proxy.exposure.set_white && dev->proxy.exposure.get_white)
    return TRUE;

  return FALSE;
}

void dt_dev_exposure_reset_defaults(dt_develop_t *dev)
{
  if(!dev->proxy.exposure.module) return;

  dt_iop_module_t *exposure = dev->proxy.exposure.module;
  memcpy(exposure->params, exposure->default_params, exposure->params_size);
  exposure->gui_update(exposure);
  dt_dev_add_history_item(exposure->dev, exposure, TRUE);
}

void dt_dev_exposure_set_white(dt_develop_t *dev, const float white)
{
  if(dev->proxy.exposure.module && dev->proxy.exposure.set_white)
    dev->proxy.exposure.set_white(dev->proxy.exposure.module, white);
}

float dt_dev_exposure_get_white(dt_develop_t *dev)
{
  if(dev->proxy.exposure.module && dev->proxy.exposure.set_white)
    return dev->proxy.exposure.get_white(dev->proxy.exposure.module);

  return 0.0;
}

void dt_dev_exposure_set_black(dt_develop_t *dev, const float black)
{
  if(dev->proxy.exposure.module && dev->proxy.exposure.set_black)
    dev->proxy.exposure.set_black(dev->proxy.exposure.module, black);
}

float dt_dev_exposure_get_black(dt_develop_t *dev)
{
  if(dev->proxy.exposure.module && dev->proxy.exposure.set_black)
    return dev->proxy.exposure.get_black(dev->proxy.exposure.module);

  return 0.0;
}

gboolean dt_dev_modulegroups_available(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.set && dev->proxy.modulegroups.get)
    return TRUE;

  return FALSE;
}

void dt_dev_modulegroups_set(dt_develop_t *dev, uint32_t group)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.set && dev->first_load == 0)
    dev->proxy.modulegroups.set(dev->proxy.modulegroups.module, group);
}

uint32_t dt_dev_modulegroups_get(dt_develop_t *dev)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.get)
    return dev->proxy.modulegroups.get(dev->proxy.modulegroups.module);

  return 0;
}

gboolean dt_dev_modulegroups_test(dt_develop_t *dev, uint32_t group, uint32_t iop_group)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.test)
    return dev->proxy.modulegroups.test(dev->proxy.modulegroups.module, group, iop_group);
  return FALSE;
}

void dt_dev_modulegroups_switch(dt_develop_t *dev, dt_iop_module_t *module)
{
  if(dev->proxy.modulegroups.module && dev->proxy.modulegroups.switch_group && dev->first_load == 0)
    dev->proxy.modulegroups.switch_group(dev->proxy.modulegroups.module, module);
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
void dt_dev_masks_list_remove(dt_develop_t *dev, int formid, int parentid)
{
  if(dev->proxy.masks.module && dev->proxy.masks.list_remove)
    dev->proxy.masks.list_remove(dev->proxy.masks.module, formid, parentid);
}
void dt_dev_masks_selection_change(dt_develop_t *dev, int selectid, int throw_event)
{
  if(dev->proxy.masks.module && dev->proxy.masks.selection_change)
    dev->proxy.masks.selection_change(dev->proxy.masks.module, selectid, throw_event);
}

void dt_dev_snapshot_request(dt_develop_t *dev, const char *filename)
{
  dev->proxy.snapshot.filename = filename;
  dev->proxy.snapshot.request = TRUE;
  dt_control_queue_redraw_center();
}

void dt_dev_invalidate_from_gui(dt_develop_t *dev)
{
  dt_dev_pop_history_items(darktable.develop, darktable.develop->history_end);
}

void dt_dev_average_delay_update(const dt_times_t *start, uint32_t *average_delay)
{
  dt_times_t end;
  dt_get_times(&end);

  *average_delay += ((end.clock - start->clock) * 1000 / DT_DEV_AVERAGE_DELAY_COUNT
                     - *average_delay / DT_DEV_AVERAGE_DELAY_COUNT);
}


/** duplicate a existant module */
dt_iop_module_t *dt_dev_module_duplicate(dt_develop_t *dev, dt_iop_module_t *base, int priority)
{
  // we create the new module
  dt_iop_module_t *module = (dt_iop_module_t *)malloc(sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, base->so, base->dev)) return NULL;
  module->instance = base->instance;

  // we set the multi-instance priority
  GList *modules = g_list_first(base->dev->iop);
  int pmax = 0;
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod->instance == base->instance)
    {
      // if the module is after the new one, we have to increment his priority
      if(mod->multi_priority >= priority)
      {
        mod->multi_priority += 1;
      }
      if(pmax < mod->multi_priority) pmax = mod->multi_priority;
    }
    modules = g_list_next(modules);
  }
  pmax += 1;
  if(priority < pmax) pmax = priority;
  module->multi_priority = pmax;

  // since we do not rename the module we need to check that an old module does not have the same name. Indeed
  // the multi_priority
  // are always rebased to start from 0, to it may be the case that the same multi_name be generated when
  // duplicating a module.
  int pname = module->multi_priority + 1;
  char mname[128];

  do
  {
    snprintf(mname, sizeof(mname), "%d", pname);
    gboolean dup = FALSE;

    GList *modules = g_list_first(base->dev->iop);
    while(modules)
    {
      dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
      if(mod->instance == base->instance)
      {
        if(strcmp(mname, mod->multi_name) == 0)
        {
          dup = TRUE;
          break;
        }
      }
      modules = g_list_next(modules);
    }

    if(dup)
      pname++;
    else
      break;
  } while(1);

  // the multi instance name
  g_strlcpy(module->multi_name, mname, sizeof(module->multi_name));
  // we insert this module into dev->iop
  base->dev->iop = g_list_insert_sorted(base->dev->iop, module, sort_plugins);

  // that's all. rest of insertion is gui work !
  return module;
}

void dt_dev_module_remove(dt_develop_t *dev, dt_iop_module_t *module)
{
  // if(darktable.gui->reset) return;
  dt_pthread_mutex_lock(&dev->history_mutex);
  int del = 0;
  if(dev->gui_attached)
  {
    int pos = 0;
    for(guint i = 0; i < g_list_length(dev->history); i++)
    {
      GList *elem = g_list_nth(dev->history, pos);

      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)(elem->data);

      if(module->instance == hist->module->instance && module->multi_priority == hist->module->multi_priority)
      {
        free(hist->params);
        free(hist->blend_params);
        free(hist);
        dev->history = g_list_delete_link(dev->history, elem);
        dev->history_end--;
        del = 1;
      }
      else
      {
        pos++;
      }
    }
  }

  dt_pthread_mutex_unlock(&dev->history_mutex);

  // and we remove it from the list
  GList *modules = g_list_first(dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
    {
      dev->iop = g_list_remove_link(dev->iop, modules);
      break;
    }
    modules = g_list_next(modules);
  }

  if(dev->gui_attached && del)
  {
    /* signal that history has changed */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
    /* redraw */
    dt_control_queue_redraw_center();
  }
}

void dt_dev_module_update_multishow(dt_develop_t *dev, struct dt_iop_module_t *module)
{
  // if the module is not multi instances compatible, then exit

  // We count the number of other instances
  int nb_before = 0;
  int nb_after = 0;
  int pos = 0;
  int pos_module = -1;
  GList *modules = g_list_first(dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(mod == module)
      pos_module = pos;
    else if(mod->instance == module->instance)
    {
      if(pos_module < 0)
        nb_before++;
      else
        nb_after++;
    }
    modules = g_list_next(modules);
    pos++;
  }

  module->multi_show_close = (nb_after + nb_before > 0);
  module->multi_show_up = (nb_after > 0);
  module->multi_show_down = (nb_before > 0);
}
void dt_dev_modules_update_multishow(dt_develop_t *dev)
{
  GList *modules = g_list_first(dev->iop);
  while(modules)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    dt_dev_module_update_multishow(dev, mod);
    modules = g_list_next(modules);
  }
}
gchar *dt_history_item_get_name(struct dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_strdup_printf("%s", module->name());
  else
    label = g_strdup_printf("%s %s", module->name(), module->multi_name);
  return label;
}
gchar *dt_history_item_get_name_html(struct dt_iop_module_t *module)
{
  gchar *label;
  /* create a history button and add to box */
  if(!module->multi_name[0] || strcmp(module->multi_name, "0") == 0)
    label = g_strdup_printf("<span size=\"larger\">%s</span>", module->name());
  else
    label = g_strdup_printf("<span size=\"larger\">%s</span> %s", module->name(), module->multi_name);
  return label;
}

int dt_dev_distort_transform(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_transform_plus(dev, dev->preview_pipe, 0, 99999, points, points_count);
}
int dt_dev_distort_backtransform(dt_develop_t *dev, float *points, size_t points_count)
{
  return dt_dev_distort_backtransform_plus(dev, dev->preview_pipe, 0, 99999, points, points_count);
}

int dt_dev_distort_transform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int pmin, int pmax,
                                  float *points, size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_first(dev->iop);
  GList *pieces = g_list_first(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled && module->priority <= pmax && module->priority >= pmin)
    {
      module->distort_transform(module, piece, points, points_count);
    }
    modules = g_list_next(modules);
    pieces = g_list_next(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return 1;
}

int dt_dev_distort_backtransform_plus(dt_develop_t *dev, dt_dev_pixelpipe_t *pipe, int pmin, int pmax,
                                      float *points, size_t points_count)
{
  dt_pthread_mutex_lock(&dev->history_mutex);
  GList *modules = g_list_last(dev->iop);
  GList *pieces = g_list_last(pipe->nodes);
  while(modules)
  {
    if(!pieces)
    {
      dt_pthread_mutex_unlock(&dev->history_mutex);
      return 0;
    }
    dt_iop_module_t *module = (dt_iop_module_t *)(modules->data);
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->enabled && module->priority <= pmax && module->priority >= pmin)
    {
      module->distort_backtransform(module, piece, points, points_count);
    }
    modules = g_list_previous(modules);
    pieces = g_list_previous(pieces);
  }
  dt_pthread_mutex_unlock(&dev->history_mutex);
  return 1;
}

dt_dev_pixelpipe_iop_t *dt_dev_distort_get_iop_pipe(dt_develop_t *dev, struct dt_dev_pixelpipe_t *pipe,
                                                    struct dt_iop_module_t *module)
{
  GList *pieces = g_list_last(pipe->nodes);
  while(pieces)
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)(pieces->data);
    if(piece->module == module)
    {
      return piece;
    }
    pieces = g_list_previous(pieces);
  }
  return NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
