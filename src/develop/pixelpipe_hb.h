/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#pragma once

#include "common/atomic.h"
#include "common/image.h"
#include "common/iop_order.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_cache.h"
#include "imageio/imageio_common.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define DT_PIPECACHE_MIN 2

/**
 * struct used by iop modules to connect to pixelpipe.
 * data can be used to store whatever private data and
 * will be freed at the end.
 */
struct dt_iop_module_t;
struct dt_iop_order_iccprofile_info_t;

typedef struct dt_dev_pixelpipe_iop_t
{
  struct dt_iop_module_t *module;  // the module in the dev operation stack
  struct dt_dev_pixelpipe_t *pipe; // the pipe this piece belongs to
  void *data;                      // to be used by the module to store stuff per pipe piece
  void *blendop_data;              // to be used by the module to store blendop per pipe piece
  gboolean enabled; // used to disable parts of the pipe for export, independent on module itself.

  dt_dev_request_flags_t request_histogram;              // (bitwise) set if you want an histogram captured
  dt_dev_histogram_collection_params_t histogram_params; // set histogram generation params
  uint32_t *histogram; // pointer to histogram data; histogram_bins_count bins with 4 channels each
  dt_dev_histogram_stats_t histogram_stats; // stats of captured histogram
  uint32_t histogram_max[4];                // maximum levels in histogram, one per channel

  float iscale;                   // input actually just downscaled buffer? iscale*iwidth = actual width
  int iwidth, iheight;            // width and height of input buffer
  dt_hash_t hash;                 // hash of params and enabled.
  int bpc;                        // bits per channel, 32 means float
  int colors;                     // how many colors per pixel
  dt_iop_roi_t buf_in;            // theoretical full buffer regions of interest, as passed through modify_roi_out
  dt_iop_roi_t buf_out;
  dt_iop_roi_t processed_roi_in;  // the actual roi that was used for processing the piece
  dt_iop_roi_t processed_roi_out;
  gboolean process_cl_ready;      // set this to FALSE in commit_params to temporarily disable the use of process_cl
  gboolean process_tiling_ready;  // set this to FALSE in commit_params to temporarily disable tiling

  // the following are used internally for caching:
  dt_iop_buffer_dsc_t dsc_in;
  dt_iop_buffer_dsc_t dsc_out;

  GHashTable *raster_masks;
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

typedef enum dt_dev_pixelpipe_status_t
{
  DT_DEV_PIXELPIPE_DIRTY = 0,   // history stack changed or image new
  DT_DEV_PIXELPIPE_RUNNING = 1, // pixelpipe is running
  DT_DEV_PIXELPIPE_VALID = 2,   // pixelpipe has finished; valid result
  DT_DEV_PIXELPIPE_INVALID = 3  // pixelpipe has finished; invalid result
} dt_dev_pixelpipe_status_t;

typedef struct dt_dev_detail_mask_t
{
  dt_iop_roi_t roi;
  dt_hash_t hash;
  float *data;
} dt_dev_detail_mask_t;

/**
 * this encapsulates the pixelpipe.
 * a develop module will need several of these:
 * for previews and full blits to cairo and for
 * the export function.
 */
typedef struct dt_dev_pixelpipe_t
{
  // store history/zoom caches
  dt_dev_pixelpipe_cache_t cache;
  // set to TRUE in order to obsolete old cache entries on next pixelpipe run
  gboolean cache_obsolete;
  uint64_t runs; // used only for pixelpipe cache statistics
  // input buffer
  float *input;
  // width and height of input buffer
  int iwidth, iheight;
  // input actually just downscaled buffer? iscale*iwidth = actual width
  float iscale;
  // dimensions of processed buffer
  int processed_width, processed_height;

  // this one actually contains the expected output format,
  // and should be modified by process*(), if necessary.
  dt_iop_buffer_dsc_t dsc;

  /** work profile info of the image */
  struct dt_iop_order_iccprofile_info_t *work_profile_info;
  /** input profile info **/
  struct dt_iop_order_iccprofile_info_t *input_profile_info;
  /** output profile info **/
  struct dt_iop_order_iccprofile_info_t *output_profile_info;

  // instances of pixelpipe, stored in GList of dt_dev_pixelpipe_iop_t
  GList *nodes;
  // event flag
  dt_dev_pixelpipe_change_t changed;
  // pipe status
  dt_dev_pixelpipe_status_t status;
  gboolean loading;
  gboolean input_changed;
  // backbuffer (output)
  uint8_t *backbuf;
  size_t backbuf_size;
  int backbuf_width, backbuf_height;
  float backbuf_scale;
  float backbuf_zoom_x, backbuf_zoom_y;
  dt_hash_t backbuf_hash;
  dt_pthread_mutex_t mutex, backbuf_mutex, busy_mutex;
  int final_width, final_height;

  // the data for the luminance mask are kept in a buffer written by demosaic or rawprepare
  // as we have to scale the mask later we keep size at that stage
  gboolean want_detail_mask;
  struct dt_dev_detail_mask_t scharr;

  // avoid cached data for processed module
  gboolean nocache;

  dt_imgid_t output_imgid;
  // working?
  gboolean processing;
  // shutting down?
  dt_atomic_int shutdown;
  // opencl enabled for this pixelpipe?
  gboolean opencl_enabled;
  // opencl error detected?
  gboolean opencl_error;
  // running in a tiling context?
  gboolean tiling;
  // should this pixelpipe display a mask in the end?
  dt_dev_pixelpipe_display_mask_t mask_display;
  // should this pixelpipe completely suppressed the blendif module?
  gboolean bypass_blendif;
  // input data based on this timestamp:
  int input_timestamp;
  uint32_t average_delay;
  dt_dev_pixelpipe_type_t type;
  // the final output pixel format this pixelpipe will be converted to
  dt_imageio_levels_t levels;
  // opencl device that has been locked for this pipe.
  int devid;
  // image struct as it was when the pixelpipe was initialized. copied to avoid race conditions.
  dt_image_t image;
  // the user might choose to overwrite the output color space and rendering intent.
  dt_colorspaces_color_profile_type_t icc_type;
  gchar *icc_filename;
  dt_iop_color_intent_t icc_intent;
  // snapshot of modules
  GList *iop;
  // snapshot of modules iop_order
  GList *iop_order_list;
  // snapshot of mask list
  GList *forms;
  // the masks generated in the pipe for later reusal are inside dt_dev_pixelpipe_iop_t
  gboolean store_all_raster_masks;
} dt_dev_pixelpipe_t;

struct dt_develop_t;

// report pipe->type as textual string
const char *dt_dev_pixelpipe_type_to_str(dt_dev_pixelpipe_type_t pipe_type);

// inits the pixelpipe with plain passthrough input/output and empty input and default caching settings.
gboolean dt_dev_pixelpipe_init(dt_dev_pixelpipe_t *pipe);
// inits the preview pixelpipe with plain passthrough input/output and empty input and default caching
// settings.
gboolean dt_dev_pixelpipe_init_preview(dt_dev_pixelpipe_t *pipe);
gboolean dt_dev_pixelpipe_init_preview2(dt_dev_pixelpipe_t *pipe);
// inits the pixelpipe with settings optimized for full-image export
// (no history stack cache)
gboolean dt_dev_pixelpipe_init_export(dt_dev_pixelpipe_t *pipe,
                                      const int32_t width,
                                      const int32_t height,
                                      const int levels,
                                      const gboolean store_masks);
// inits the pixelpipe with settings optimized for thumbnail export
// (no history stack cache)
gboolean dt_dev_pixelpipe_init_thumbnail(dt_dev_pixelpipe_t *pipe,
                                         const int32_t width,
                                         const int32_t height);
// inits all but the pixel caches, so you can't actually process an
// image (just get dimensions and distortions)
gboolean dt_dev_pixelpipe_init_dummy(dt_dev_pixelpipe_t *pipe,
                                     const int32_t width,
                                     const int32_t height);
// inits the pixelpipe with given cacheline size and number of
// entries. returns TRUE in case of success
gboolean dt_dev_pixelpipe_init_cached(dt_dev_pixelpipe_t *pipe,
                                      const size_t size,
                                      const int32_t entries,
                                      const size_t memlimit);
// returns available memory for the pipe
size_t dt_get_available_pipe_mem(const dt_dev_pixelpipe_t *pipe);
// constructs a new input buffer from given RGB float array.
void dt_dev_pixelpipe_set_input(dt_dev_pixelpipe_t *pipe,
                                struct dt_develop_t *dev,
                                float *input,
                                const int width,
                                const int height,
                                const float iscale);
// set some metadata for colorout to avoid race conditions.
void dt_dev_pixelpipe_set_icc(dt_dev_pixelpipe_t *pipe,
                              const dt_colorspaces_color_profile_type_t icc_type,
                              const gchar *icc_filename,
                              const dt_iop_color_intent_t icc_intent);

// returns the dimensions of the full image after processing.
void dt_dev_pixelpipe_get_dimensions(dt_dev_pixelpipe_t *pipe,
                                     struct dt_develop_t *dev,
                                     const int width_in,
                                     const int height_in,
                                     int *width,
                                     int *height);

// destroys all allocated data.
void dt_dev_pixelpipe_cleanup(dt_dev_pixelpipe_t *pipe);

// wrapper for cleanup_nodes, create_nodes, synch_all and synch_top,
// decides upon changed event which one to take on. also locks
// dev->history_mutex.
void dt_dev_pixelpipe_change(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// cleanup all nodes except clean input/output
void dt_dev_pixelpipe_cleanup_nodes(dt_dev_pixelpipe_t *pipe);
// sync with develop_t history stack from scratch (new node added, have to pop old ones)
void dt_dev_pixelpipe_create_nodes(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// sync with develop_t history stack by just copying the top item params (same op, new params on top)
void dt_dev_pixelpipe_synch_all(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// adjust output node according to history stack (history pop event)
void dt_dev_pixelpipe_synch_top(dt_dev_pixelpipe_t *pipe, struct dt_develop_t *dev);
// force a rebuild of the pipe, needed when a module order is changed for example
void dt_dev_pixelpipe_rebuild(struct dt_develop_t *dev);

// switch on details mask processing
void dt_dev_pixelpipe_usedetails(dt_dev_pixelpipe_t *pipe);
// process region of interest of pixels. returns TRUE if pipe was altered during processing.
gboolean dt_dev_pixelpipe_process(dt_dev_pixelpipe_t *pipe,
                             struct dt_develop_t *dev,
                             const int x,
                             const int y,
                             const int width,
                             const int height,
                             const float scale,
                             const int devid);
// convenience method that does not gamma-compress the image.
gboolean dt_dev_pixelpipe_process_no_gamma(dt_dev_pixelpipe_t *pipe,
                                      struct dt_develop_t *dev,
                                      const int x,
                                      const int y,
                                      const int width,
                                      const int height,
                                      const float scale);

// disable given op and all that comes after it in the pipe:
void dt_dev_pixelpipe_disable_after(dt_dev_pixelpipe_t *pipe, const char *op);
// disable given op and all that comes before it in the pipe:
void dt_dev_pixelpipe_disable_before(dt_dev_pixelpipe_t *pipe, const char *op);

// helper function to pass a raster mask through a (so far) processed pipe
float *dt_dev_get_raster_mask(dt_dev_pixelpipe_iop_t *piece,
                              const struct dt_iop_module_t *raster_mask_source,
                              const dt_mask_id_t raster_mask_id,
                              const struct dt_iop_module_t *target_module,
                              gboolean *free_mask);
// some helper functions related to the details mask interface
void dt_dev_clear_scharr_mask(dt_dev_pixelpipe_t *pipe);

gboolean dt_dev_write_scharr_mask(dt_dev_pixelpipe_iop_t *piece,
                                  float *const rgb,
                                  const dt_iop_roi_t *const roi_in,
                                  const gboolean mode);
#ifdef HAVE_OPENCL
int dt_dev_write_scharr_mask_cl(dt_dev_pixelpipe_iop_t *piece,
                                cl_mem in,
                                const dt_iop_roi_t *const roi_in,
                                const gboolean mode);
#endif

/* specialized version of dt_print for pixelpipe debugging */
void dt_print_pipe_ext(const char *title,
                       const dt_dev_pixelpipe_t *pipe,
                       const struct dt_iop_module_t *mod,
                       const int device,
                       const dt_iop_roi_t *roi_in,
                       const dt_iop_roi_t *roi_out,
                       const char *msg, ...)
  __attribute__((format(printf, 7, 8)));

// helper function writing the pipe-processed ctmask data to dest
float *dt_dev_distort_detail_mask(dt_dev_pixelpipe_iop_t *piece,
                                  float *src,
                                  const struct dt_iop_module_t *target_module);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
