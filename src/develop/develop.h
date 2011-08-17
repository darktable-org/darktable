/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DARKTABLE_DEVELOP_H
#define DARKTABLE_DEVELOP_H

#include <inttypes.h>
#include <cairo.h>
#include <glib.h>

#include "common/dtpthread.h"
#include "control/settings.h"
#include "develop/imageop.h"
#include "common/image.h"

extern uint8_t dt_dev_default_gamma[0x10000];
extern float dt_dev_de_gamma[0x100];

struct dt_iop_module_t;
struct dt_iop_params_t;
typedef struct dt_dev_history_item_t
{
  struct dt_iop_module_t *module; // pointer to image operation module
  int32_t enabled;                // switched respective module on/off
  struct dt_iop_params_t *params; // parameters for this operation
  struct dt_develop_blend_params_t *blend_params;
}
dt_dev_history_item_t;

struct dt_dev_pixelpipe_t;
struct dt_iop_module_t;
typedef struct dt_develop_t
{
  int32_t gui_attached; // != 0 if the gui should be notified of changes in hist stack and modules should be gui_init'ed.
  int32_t gui_leaving;  // set if everything is scheduled to shut down.
  int32_t gui_synch; // set by the render threads if gui_update should be called in the modules.
  int32_t image_loading, image_dirty;
  int32_t image_force_reload;
  int32_t preview_loading, preview_dirty, preview_input_changed;
  uint32_t timestamp;
  struct dt_iop_module_t *gui_module; // this module claims gui expose/event callbacks.
  float preview_downsampling; // < 1.0: optionally downsample preview

  // width, height: dimensions of window
  // capwidth, capheight: actual dimensions of scaled image inside window.
  int32_t width, height, capwidth, capheight, capwidth_preview, capheight_preview;

  // image processing pipeline with caching
  struct dt_dev_pixelpipe_t *pipe, *preview_pipe;

  // image under consideration.
  dt_image_t *image;
  int32_t mipf_width, mipf_height;
  float   *mipf, mipf_exact_width, mipf_exact_height;

  // history stack
  dt_pthread_mutex_t history_mutex;
  int32_t history_end;
  GList *history;
  // operations pipeline
  int32_t iop_instance;
  GList *iop;

  // histogram for display.
  float *histogram, *histogram_pre;
  float histogram_max, histogram_pre_max;
  uint8_t gamma[0x100];

}
dt_develop_t;

void dt_dev_init(dt_develop_t *dev, int32_t gui_attached);
void dt_dev_cleanup(dt_develop_t *dev);

void dt_dev_raw_load(dt_develop_t *dev, dt_image_t *img);
void dt_dev_raw_reload(dt_develop_t *dev);
void dt_dev_process_image_job(dt_develop_t *dev);
void dt_dev_process_preview_job(dt_develop_t *dev);
// launch jobs above
void dt_dev_process_image(dt_develop_t *dev);
void dt_dev_process_preview(dt_develop_t *dev);
// directly in this thread process mipf->mip4..0
void dt_dev_process_to_mip(dt_develop_t *dev);

void dt_dev_load_image(dt_develop_t *dev, struct dt_image_t *img);
void dt_dev_load_preview(dt_develop_t *dev, struct dt_image_t *image);
/** checks if provided imgid is the image currently in develop */
int dt_dev_is_current_image(dt_develop_t *dev, int imgid);
void dt_dev_add_history_item(dt_develop_t *dev, struct dt_iop_module_t *module, gboolean enable);
void dt_dev_reload_history_items(dt_develop_t *dev);
void dt_dev_pop_history_items(dt_develop_t *dev, int32_t cnt);
void dt_dev_write_history(dt_develop_t *dev);
void dt_dev_read_history(dt_develop_t *dev);

void dt_dev_invalidate(dt_develop_t *dev);
// also invalidates preview (which is unaffected by resize/zoom/pan)
void dt_dev_invalidate_all(dt_develop_t *dev);
void dt_dev_set_gamma(dt_develop_t *dev);
void dt_dev_set_histogram(dt_develop_t *dev);
void dt_dev_set_histogram_pre(dt_develop_t *dev);
void dt_dev_get_history_item_label(dt_dev_history_item_t *hist, char *label, const int cnt);


void dt_dev_get_processed_size(const dt_develop_t *dev, int *procw, int *proch);
void dt_dev_check_zoom_bounds(dt_develop_t *dev, float *zoom_x, float *zoom_y, dt_dev_zoom_t zoom, int closeup, float *boxw, float *boxh);
float dt_dev_get_zoom_scale(dt_develop_t *dev, dt_dev_zoom_t zoom, int closeup_factor, int mode);
void dt_dev_get_pointer_zoom_pos(dt_develop_t *dev, const float px, const float py, float *zoom_x, float *zoom_y);


void dt_dev_configure (dt_develop_t *dev, int wd, int ht);
void dt_dev_invalidate_from_gui (dt_develop_t *dev);

#endif
