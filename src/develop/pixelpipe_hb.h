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
#ifndef DT_DEV_PIXELPIPE
#define DT_DEV_PIXELPIPE

#include "common/image.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "develop/imageop.h"
#include "develop/develop.h"
#include "develop/pixelpipe_cache.h"

/**
 * struct used by iop modules to connect to pixelpipe.
 * input and output nodes will be connected to gegl graph.
 * data can be used to store whatever private data and
 * will be freed at the end.
 */
struct dt_iop_module_t;

/** region of interest */
typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t *module;  // the module in the dev operation stack
  struct dt_dev_pixelpipe_t *pipe; // the pipe this piece belongs to
  void *data;                      // to be used by the module to store stuff per pipe piece
  void *blendop_data;              // to be used by the module to store blendop per pipe piece
  int enabled;         // used to disable parts of the pipe for export, independent on module itself.
  float iscale;        // input actually just downscaled buffer? iscale*iwidth = actual width
  int iwidth, iheight; // width and height of input buffer
  uint64_t hash;       // hash of params and enabled.
  int bpc;             // bits per channel, 32 means float
  int colors;          // how many colors per pixel
  dt_iop_roi_t buf_in,
      buf_out;                // theoretical full buffer regions of interest, as passed through modify_roi_out
  int process_cl_ready;       // set this to 0 in commit_params to temporarily disable the use of process_cl
  float processed_maximum[3]; // sensor saturation after this iop, used internally for caching
} dt_dev_pixelpipe_iop_t;

typedef enum dt_dev_pixelpipe_change_t
{
  DT_DEV_PIPE_UNCHANGED = 0,        // no event
  DT_DEV_PIPE_TOP_CHANGED = 1 << 0, // only params of top element changed
  DT_DEV_PIPE_REMOVE = 1 << 1,      // possibly elements of the pipe have to be removed
  DT_DEV_PIPE_SYNCH
  = 1 << 2, // all nodes up to end need to be synched, but no removal of module pieces is necessary
  DT_DEV_PIPE_ZOOMED = 1 << 3 // zoom event, preview pipe does not need changes
} dt_dev_pixelpipe_change_t;

/**
 * this encapsulates the gegl pixel pipeline.
 * a develop module will need several of these:
 * for previews and full blits to cairo and for
 * the export function.
 */
typedef struct dt_dev_pixelpipe_t
{
  // store history/zoom caches
  dt_dev_pixelpipe_cache_t cache;
  // set to non-zero in order to obsolete old cache entries on next pixelpipe run
  int cache_obsolete;
  // input buffer
  float *input;
  // width and height of input buffer
  int iwidth, iheight;
  // input actually just downscaled buffer? iscale*iwidth = actual width
  float iscale;
  // dimensions of processed buffer
  int processed_width, processed_height;
  // sensor saturation, propagated through the operations:
  float processed_maximum[3];
  // gegl instances of pixel pipeline, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
  // event flag
  dt_dev_pixelpipe_change_t changed;
  // backbuffer (output)
  uint8_t *backbuf;
  size_t backbuf_size;
  int backbuf_width, backbuf_height;
  uint64_t backbuf_hash;
  dt_pthread_mutex_t backbuf_mutex, busy_mutex;
  // working?
  int processing;
  // shutting down?
  int shutdown;
  // opencl enabled for this pixelpipe?
  int opencl_enabled;
  // opencl error detected?
  int opencl_error;
  // running in a tiling context?
  int tiling;
  // should this pixelpipe display a mask in the end?
  int mask_display;
  // input data based on this timestamp:
  int input_timestamp;
  dt_dev_pixelpipe_type_t type;
  // the final output pixel format this pixelpipe will be converted to
  dt_imageio_levels_t levels;
  // opencl device that has been locked for this pipe.
  int devid;
  // image struct as it was when the pixelpipe was initialized. copied to avoid race conditions.
  dt_image_t image;
} dt_dev_pixelpipe_t;

struct dt_develop_t;

// inits the pixelpipe with plain passthrough input/output and empty input and default caching settings.
int dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe);
// inits the preview pixelpipe with plain passthrough input/output and empty input and default caching
// settings.
int dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe);
// inits the pixelpipe with settings optimized for full-image export (no history stack cache)
int dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height, int levels);
// inits the pixelpipe with settings optimized for thumbnail export (no history stack cache)
int dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height);
// inits all but the pixel caches, so you can't actually process an image (just get dimensions and
// distortions)
int dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe, int32_t width, int32_t height);
// inits the pixelpipe with given cacheline size and number of entries.
int dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe, size_t size, int32_t entries);
// constructs a new input gegl_buffer from given RGB float array.
void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, float *input, int width,
                                int height, float iscale);

// returns the dimensions of the full image after processing.
void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int width_in,
                                     int height_in, int *width, int *height);

// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);

// flushes all cached data. useful if input pixels unexpectedly change.
void dt_dev_pixelpipe_flush_caches(dt_dev_pixelpipe_t *pipe);

// wrapper for cleanup_nodes, create_nodes, synch_all and synch_top, decides upon changed event which one to
// take on. also locks dev->history_mutex.
void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// cleanup all gegl nodes except clean input/output
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// sync with develop_t history stack from scratch (new node added, have to pop old ones)
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// adjust gegl:nop output node according to history stack (history pop event)
void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);

// process region of interest of pixels. returns 1 if pipe was altered during processing.
int dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int x, int y, int width,
                             int height, float scale);
// convenience method that does not gamma-compress the image.
int dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int x, int y,
                                      int width, int height, float scale);

// disable given op and all that comes after it in the pipe:
void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op);
// disable given op and all that comes before it in the pipe:
void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op);


// TODO: future application: remove/add modules from list, load from disk, user programmable etc
// TODO: add n-th module in dev list to gegl pipeline
void dt_dev_pixelpipe_add_node(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int n);
// TODO: remove n-th module from gegl pipeline
void dt_dev_pixelpipe_remove_node(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev, int n);

// signifies that this pipeline uses the MIP_F buffer instead of MIP_FULL
// i.e. four floats per pixel already demosaiced/downsampled
static inline int dt_dev_pixelpipe_uses_downsampled_input(dt_dev_pixelpipe_t *pipe)
{
  if(!dt_conf_get_bool("plugins/lighttable/low_quality_thumbnails"))
    return pipe->type == DT_DEV_PIXELPIPE_PREVIEW;
  else
    return (pipe->type == DT_DEV_PIXELPIPE_PREVIEW) || (pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL);
}

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
