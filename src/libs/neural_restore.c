/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// neural restore — lighttable module for AI-based image restoration
//
// overview
// --------
// provides three operations via a tabbed notebook UI:
//   - raw denoise: run a RawNIND UtNet2 denoiser pre-demosaic (Bayer
//     CFA) or post-demosaic (X-Trans / Foveon, via lin_rec2020).
//     output is a DNG (CFA Bayer or LinearRaw) that re-imports into
//     the user's full pipeline.
//   - denoise: run an ONNX denoiser (e.g. NIND UNet) on the user's
//     processed/exported image. output is a TIFF.
//   - upscale: run an ONNX super-resolution model (e.g. BSRGAN) at
//     2x or 4x. output is a TIFF.
//
// the module lives in the right panel (DT_UI_CONTAINER_PANEL_RIGHT_CENTER)
// and is available in both lighttable and darkroom views. it is only built
// when cmake option USE_AI=ON.
//
// architecture
// ------------
// the core AI inference, tiling, color management and DWT detail
// recovery logic lives in the darktable_ai library, split across:
//   - src/common/ai/restore.{c,h}        env/ctx lifecycle, model
//                                         loaders, raw patch runners,
//                                         shared user-pipe ROI bridge
//   - src/common/ai/restore_rgb.{c,h}    RGB denoise + upscale tiled
//                                         driver, sRGB wrapper, shadow
//                                         boost, DWT detail recovery
//   - src/common/ai/restore_raw_bayer.   RawNIND Bayer end-to-end
//     {c,h}                               (batch + piped preview)
//   - src/common/ai/restore_raw_linear.  RawNIND linear / X-Trans
//     {c,h}                               end-to-end
//
// this module handles UI, threading, output writing, and the per-task
// preview cache.
//
// 1. preview (interactive, single-image)
//    triggered by clicking the picker thumbnail (which sets a
//    "preview requested" flag) or by tab switching afterwards.
//    two worker functions, dispatched via _preview_thread_dispatch:
//      - _preview_thread:     RGB denoise + upscale. exports the
//                             image at reduced resolution and runs
//                             dt_restore_run_patch() on a crop.
//      - _preview_thread_raw: raw denoise. reads the mipmap CFA
//                             (Bayer) or runs darktable's demosaic
//                             pipe (X-Trans → lin_rec2020), feeds
//                             one inference tile, then runs the
//                             user's full pipe twice (with
//                             rawdenoise disabled) on the patched
//                             vs original CFA so before/after match
//                             a re-imported DNG.
//    both deliver result buffers to the main thread via g_idle_add.
//    the preview widget draws a split before/after view with a
//    draggable divider. for RGB denoise, DWT-filtered detail is
//    pre-computed once per inference so the strength slider can
//    re-blend interactively without re-running the model.
//
//    responsiveness:
//      - tab switch routes through a 150 ms debounce
//        (_schedule_preview_refresh) so rapid cycling collapses to
//        one preview run
//      - per-task preview cache keyed by (imgid, patch_center): on
//        tab switch back to a previously-seen state we install the
//        cached buffers and skip inference entirely
//      - new triggers do NOT join the previous worker (would freeze
//        the UI for the duration of an in-flight pipe call). the
//        previous thread is detached; preview_inference_lock
//        serialises inference so the new worker queues without
//        fighting for the GPU; preview_sequence is bumped so any
//        in-flight result is discarded by its idle callback when it
//        eventually arrives.
//
// 2. batch processing (multi-image)
//    runs as a dt_control_job on the user background queue.
//    for each selected image, dispatches by task:
//      - raw denoise (Bayer): pre-process the sensor CFA (black/WB/
//        pack), run tiled inference via dt_restore_raw_bayer(), un-
//        process and re-mosaic, write a CFA DNG via
//        dt_imageio_dng_write_cfa_bayer().
//      - raw denoise (X-Trans / linear): demosaic via the darktable
//        pipe (rawprepare + highlights + demosaic only), run
//        dt_restore_raw_linear(), write a LinearRaw DNG via
//        dt_imageio_dng_write_linear().
//      - denoise / upscale (RGB): export via the darktable pipeline
//        with a custom format module that intercepts the pixel
//        buffer in _ai_write_image(). when strength < 100 (so the
//        DWT detail recovery is active), buffer the full denoised
//        output, apply dt_restore_apply_detail_recovery(), then
//        write TIFF. otherwise stream tiles directly to TIFF via
//        _process_tiled_tiff() to avoid buffering the full output.
//        output TIFF embeds the selected output ICC profile and
//        source EXIF.
//    in all cases, the output is imported into the darktable library
//    and grouped with the source image (when add-to-catalog is on).
//
//    when the batch finishes, a single completion toast is shown via
//    dt_control_log (e.g. "neural denoise: 3 images processed"). the
//    module deliberately does NOT raise DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE:
//    lighttable ignores that signal while darkroom / map / culling /
//    tethering / print_settings would swap the user's current view to
//    the freshly-imported image and clobber any in-progress edit.
//
// 3. output parameters (collapsible section)
//    common (all three tabs):
//      - add to catalog: auto-import output into darktable library
//      - output directory: supports darktable variables (e.g. $(FILE_FOLDER))
//    RGB tabs only (denoise / upscale — raw denoise writes DNG):
//      - bit depth: 8/16/32-bit TIFF (default 16-bit)
//      - output ICC profile: pick any installed profile, or keep
//        image settings
//      - preserve wide-gamut: when on, out-of-sRGB-gamut pixels pass
//        through the denoise model unchanged (wide-gamut colors
//        preserved exactly); when off, those pixels are clipped to
//        sRGB and denoised like the rest
//
// threading
// ---------
// - preview: background GThread spawned per refresh. previous worker
//   is detached (not joined) so the UI thread never blocks on tab
//   switch. preview_inference_lock (GMutex) serialises the actual
//   inference / pipe work so two workers don't fight for the GPU.
//   stale workers are discarded via the atomic preview_sequence
//   counter, checked at the dispatcher entry and at key points
//   inside the worker; idle callbacks re-check before installing.
//   gui_cleanup joins the latest worker and drains the main context
//   to flush any pending idle callbacks before freeing module state.
// - batch: dt_control_job on DT_JOB_QUEUE_USER_BG. supports
//   cancellation via dt_control_job_get_state().
// - ai_registry->lock: held briefly to read provider setting.
// - all GTK updates go through g_idle_add to stay on the main thread.
//
// key structs
// -----------
// dt_lib_neural_restore_t        — module GUI state, preview cache,
//                                   inference lock, debounce timer
// dt_neural_job_t                — batch processing job parameters
// dt_neural_format_params_t      — custom format module for export
//                                   interception (RGB tabs)
// dt_neural_preview_data_t       — preview thread input parameters
//                                   (shared by both workers)
// dt_neural_preview_result_t     — RGB-tab preview thread output
// dt_neural_preview_result_raw_t — raw-tab preview thread output
//                                   (also carries the cached
//                                   full-image buffers for re-pick)
// dt_neural_preview_capture_t    — captures export pixels for the
//                                   RGB-tab preview
//
// preferences
// -----------
// CONF_STRENGTH            — RGB denoise strength slider (0 = source,
//                            100 = full denoise; internally mapped to
//                            a DWT-filtered residual recovery amount
//                            so lowering strength brings back texture
//                            without reintroducing noise-frequency
//                            content)
// CONF_RAW_STRENGTH        — raw denoise strength slider (0 = source
//                            CFA, 100 = full denoised CFA; uniform
//                            blend at the re-mosaic sample level)
// CONF_ACTIVE_PAGE         — last active notebook tab
// CONF_BIT_DEPTH           — output TIFF bit depth (0=8, 1=16, 2=32)
// CONF_ADD_CATALOG         — auto-import output into library
// CONF_OUTPUT_DIR          — output directory pattern (supports variables)
// CONF_ICC_TYPE            — output ICC profile type (image settings by default)
// CONF_ICC_FILE            — filename for file-type ICC profiles
// CONF_PRESERVE_WIDE_GAMUT — pass-through out-of-sRGB-gamut pixels during denoise
// CONF_PREVIEW_EXPORT_SIZE — preview export longest-edge in pixels
// CONF_PREVIEW_HEIGHT      — preview widget height in pixels
// CONF_EXPAND_OUTPUT       — output section collapsed/expanded state

#include "common/ai/restore.h"
#include "common/ai/restore_rgb.h"
#include "common/ai/restore_raw_bayer.h"
#include "common/ai/restore_raw_linear.h"
#include "control/conf.h"
#include "bauhaus/bauhaus.h"
#include "common/act_on.h"
#include "common/collection.h"
#include "common/variables.h"
#include "common/colorspaces.h"
#include "imageio/imageio_dng.h"
#include "imageio/imageio_jpeg.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/grouping.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/jobs/control_jobs.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/format.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

DT_MODULE(1)

#define CONF_PREVIEW_EXPORT_SIZE "plugins/lighttable/neural_restore/preview_export_size"
// warn the user when upscaled output exceeds this many megapixels
#define LARGE_OUTPUT_MP 60.0

#define CONF_STRENGTH "plugins/lighttable/neural_restore/strength"
#define CONF_RAW_STRENGTH "plugins/lighttable/neural_restore/raw_strength"
#define CONF_ACTIVE_PAGE "plugins/lighttable/neural_restore/active_page"
#define CONF_BIT_DEPTH "plugins/lighttable/neural_restore/bit_depth"
#define CONF_ADD_CATALOG "plugins/lighttable/neural_restore/add_to_catalog"
#define CONF_OUTPUT_DIR "plugins/lighttable/neural_restore/output_directory"
#define CONF_ICC_TYPE "plugins/lighttable/neural_restore/icc_type"
#define CONF_ICC_FILE "plugins/lighttable/neural_restore/icc_filename"
#define CONF_PRESERVE_WIDE_GAMUT "plugins/lighttable/neural_restore/preserve_wide_gamut"
#define CONF_EXPAND_OUTPUT "plugins/lighttable/neural_restore/expand_output"
#define CONF_PREVIEW_HEIGHT "plugins/lighttable/neural_restore/preview_height"

typedef enum dt_neural_task_t
{
  NEURAL_TASK_DENOISE = 0,
  NEURAL_TASK_UPSCALE_2X,
  NEURAL_TASK_UPSCALE_4X,
  NEURAL_TASK_RAW_DENOISE,
  NEURAL_TASK_COUNT,         // sentinel; used to size per-task arrays
} dt_neural_task_t;

typedef enum dt_neural_bpp_t
{
  NEURAL_BPP_8 = 0,
  NEURAL_BPP_16 = 1,
  NEURAL_BPP_32 = 2,
} dt_neural_bpp_t;

// preview-area placeholder state when no rendered preview exists
typedef enum dt_nr_preview_err_t
{
  DT_NR_PREVIEW_ERR_NONE = 0,
  DT_NR_PREVIEW_ERR_UNSUPPORTED,  // sensor class not handled by task
  DT_NR_PREVIEW_ERR_INIT_FAILED,  // mipmap / model / cache load bailed
} dt_nr_preview_err_t;

typedef struct dt_lib_neural_restore_t
{
  GtkNotebook *notebook;
  GtkWidget *raw_denoise_page;
  GtkWidget *denoise_page;
  GtkWidget *upscale_page;
  GtkWidget *scale_combo;
  GtkWidget *preview_area;
  GtkWidget *process_button;
  char info_text_left[64];
  char info_text_right[128];
  char warning_text[128];
  GtkWidget *recovery_slider;
  GtkWidget *raw_strength_slider;
  dt_neural_task_t task;
  dt_restore_env_t *env;
  dt_restore_context_t *cached_ctx;
  dt_neural_task_t cached_task;
  dt_pthread_mutex_t ctx_lock;
  gboolean model_available;
  GHashTable *processing_images;
  float *preview_before;
  float *preview_after;
  float *preview_detail;
  int preview_w;
  int preview_h;
  float split_pos;
  gboolean preview_ready;
  gboolean preview_requested;
  gboolean dragging_split;
  gboolean preview_generating;
  dt_nr_preview_err_t preview_error;
  gboolean recovery_changing;
  GThread *preview_thread;
  gint preview_sequence;
  unsigned char *cairo_before;
  unsigned char *cairo_after;
  int cairo_stride;

  // preview area selector
  float patch_center[2];
  gboolean picking_thumbnail;
  GtkWidget *pick_button;
  // cached export image for re-picking without re-exporting
  float *export_pixels;
  int export_w;
  int export_h;
  unsigned char *export_cairo;
  int export_cairo_stride;

  // raw denoise preview state — disjoint from the export-based preview
  // above. cached per-image (CFA for Bayer, demosaicked lin_rec2020 for
  // X-Trans / linear) so re-picking a new crop on the same image skips
  // the slow load + demosaic; freed on imgid or sensor-type change.
  dt_imgid_t preview_raw_imgid;
  dt_restore_sensor_class_t preview_raw_sensor_class;
  float *preview_full_cfa;       // Bayer: full sensor (w*h floats)
  int preview_full_w;
  int preview_full_h;
  float *preview_full_lin;       // linear: 3ch interleaved (w*h*3 floats)
  int preview_lin_w;
  int preview_lin_h;
  // per-refresh inference output (3ch interleaved at the displayed crop
  // dims, both in lin_rec2020). cached so the strength slider can blend
  // these without re-running inference.
  float *preview_raw_src_rgb;
  float *preview_raw_denoised_rgb;
  int preview_raw_crop_w;
  int preview_raw_crop_h;
  // strength slider debounce: re-blend on UI thread 50 ms after the
  // last value-changed emit. set/replaced via g_timeout_add.
  guint preview_strength_timer;
  // debounce timer for `_trigger_preview`. tab switches and rapid
  // re-triggers schedule via this so a quick burst of switches
  // doesn't pile up worker threads. set/replaced via g_timeout_add;
  // 0 means no pending trigger
  guint preview_trigger_timer;
  // serializes the expensive inference / pipe work in worker threads.
  // a stale worker (sequence bumped while it was in flight) holds this
  // until it finishes, so a freshly-spawned worker waits its turn
  // rather than competing for the same GPU/CPU
  GMutex preview_inference_lock;
  // per-task cache of the last successful preview, keyed by
  // (imgid, patch_center). on tab switch we look up the new task's
  // slot; if it matches the current image+patch we install the
  // cached buffers and skip inference entirely. invalidated on
  // image change or patch move
  struct {
    gboolean valid;
    dt_imgid_t imgid;
    float patch_center[2];
    int crop_w, crop_h;
    float *before_rgb;       // 3ch interleaved, crop_w*crop_h*3 floats
    float *after_rgb;        // same
    float *detail;           // denoise: DWT luminance detail; NULL otherwise
  } preview_cache[NEURAL_TASK_COUNT];

  // output settings (collapsible)
  dt_gui_collapsible_section_t cs_output;
  GtkWidget *bpp_combo;
  GtkWidget *profile_combo;
  GtkWidget *preserve_wide_gamut_toggle;
  GtkWidget *catalog_toggle;
  GtkWidget *output_dir_entry;
  GtkWidget *output_dir_button;
} dt_lib_neural_restore_t;

typedef struct dt_neural_job_t
{
  dt_neural_task_t task;
  dt_restore_env_t *env;
  GList *images;
  dt_job_t *control_job;
  dt_restore_context_t *ctx;
  int scale;
  float strength;
  float raw_strength;  // 0..1 blend for raw denoise
  // raw denoise only: sensor class of the currently-loaded rawdenoise
  // ctx. lets us reuse ctx across images of the same class in a batch
  // and avoid reloading the ORT session for every image
  dt_restore_sensor_class_t raw_ctx_sensor_class;
  dt_lib_module_t *self;
  dt_neural_bpp_t bpp;
  gboolean add_to_catalog;
  char *output_dir;  // NULL = same as source
  // output color profile. DT_COLORSPACE_NONE means "use image's working profile"
  dt_colorspaces_color_profile_type_t icc_type;
  char *icc_filename;  // only used when icc_type == DT_COLORSPACE_FILE
  // when TRUE, wide-gamut pixels pass through unchanged on denoise
  gboolean preserve_wide_gamut;
} dt_neural_job_t;

typedef struct dt_neural_format_params_t
{
  dt_imageio_module_data_t parent;
  dt_neural_job_t *job;
} dt_neural_format_params_t;

typedef struct dt_neural_preview_capture_t
{
  dt_imageio_module_data_t parent;
  float *pixels;
  int cap_w;
  int cap_h;
} dt_neural_preview_capture_t;

typedef struct dt_neural_preview_data_t
{
  dt_lib_module_t *self;
  dt_imgid_t imgid;
  dt_neural_task_t task;
  int scale;
  dt_restore_env_t *env;
  int sequence;
  int preview_w;
  int preview_h;
  float patch_center[2];
  // when re-picking, borrow cached export instead of re-exporting.
  // pointer is valid for the thread's lifetime (main thread joins
  // before freeing). thread must NOT free this
  const float *reuse_pixels;
  int reuse_w;
  int reuse_h;
} dt_neural_preview_data_t;

typedef struct dt_neural_preview_result_t
{
  dt_lib_module_t *self;
  float *before;
  float *after;
  float *export_pixels;
  int export_w;
  int export_h;
  int sequence;
  int width;
  int height;
  // cache key components copied from the originating preview request:
  // the worker may run after the user has switched tabs/images, so
  // the idle callback uses these (not d->* live values) to decide
  // whether the result is still applicable to the current state and
  // to populate the per-task preview cache slot
  dt_neural_task_t task;
  dt_imgid_t imgid;
  float patch_center[2];
} dt_neural_preview_result_t;
const char *name(dt_lib_module_t *self) { return _("neural restore"); }

const char *description(dt_lib_module_t *self)
{
  return _("AI-based image restoration: denoise and upscale");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self) { return 400; }
static int _ai_check_bpp(dt_imageio_module_data_t *data) { return 32; }

static int _ai_check_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_FLOAT;
}

static const char *_ai_get_mime(dt_imageio_module_data_t *data) { return "memory"; }

static int _preview_capture_write_image(dt_imageio_module_data_t *data,
                                        const char *filename,
                                        const void *in_void,
                                        dt_colorspaces_color_profile_type_t over_type,
                                        const char *over_filename,
                                        void *exif, int exif_len,
                                        dt_imgid_t imgid,
                                        int num, int total,
                                        dt_dev_pixelpipe_t *pipe,
                                        const gboolean export_masks)
{
  dt_neural_preview_capture_t *cap = (dt_neural_preview_capture_t *)data;
  const int w = data->width;
  const int h = data->height;
  const size_t buf_size = (size_t)w * h * 4 * sizeof(float);
  cap->pixels = g_try_malloc(buf_size);
  if(cap->pixels)
  {
    memcpy(cap->pixels, in_void, buf_size);
    cap->cap_w = w;
    cap->cap_h = h;
  }
  return 0;
}

static inline float _linear_to_srgb(float v)
{
  if(v <= 0.0f) return 0.0f;
  if(v >= 1.0f) return 1.0f;
  return (v <= 0.0031308f) ? 12.92f * v : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

// pull the camera's embedded JPEG preview from the source raw, to embed
// as the DNG thumbnail; output buffer is g_malloc'd (caller frees)
static int _extract_source_jpeg_preview(const char *src_path,
                                        uint8_t **out_jpeg,
                                        int *out_jpeg_len,
                                        int *out_w,
                                        int *out_h)
{
  *out_jpeg = NULL;
  *out_jpeg_len = 0;
  *out_w = 0;
  *out_h = 0;

  uint8_t *raw_buf = NULL;
  size_t   raw_size = 0;
  char    *mime = NULL;
  // dt_exif_get_thumbnail returns FALSE on success, allocates via malloc/strdup
  if(dt_exif_get_thumbnail(src_path, &raw_buf, &raw_size, &mime) || !raw_buf)
  {
    free(raw_buf);
    free(mime);
    return 1;
  }
  const gboolean is_jpeg = mime && (strcmp(mime, "image/jpeg") == 0);
  free(mime);
  if(!is_jpeg || raw_size == 0 || raw_size > (size_t)INT_MAX)
  {
    free(raw_buf);
    return 1;
  }

  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_decompress_header(raw_buf, raw_size, &jpg) != 0
     || jpg.width <= 0 || jpg.height <= 0)
  {
    free(raw_buf);
    return 1;
  }

  // re-allocate via glib so caller can g_free uniformly
  uint8_t *jpeg = g_try_malloc(raw_size);
  if(!jpeg)
  {
    free(raw_buf);
    return 1;
  }
  memcpy(jpeg, raw_buf, raw_size);
  free(raw_buf);

  *out_jpeg     = jpeg;
  *out_jpeg_len = (int)raw_size;
  *out_w        = jpg.width;
  *out_h        = jpg.height;
  return 0;
}

// convert float RGB (3ch interleaved, linear) to cairo RGB24 surface data
static void _float_rgb_to_cairo(const float *const restrict src,
                                unsigned char *const restrict dst,
                                int width, int height, int stride)
{
  for(int y = 0; y < height; y++)
  {
    uint32_t *row = (uint32_t *)(dst + y * stride);
    for(int x = 0; x < width; x++)
    {
      const int si = (y * width + x) * 3;
      const uint8_t r = (uint8_t)(_linear_to_srgb(src[si + 0]) * 255.0f + 0.5f);
      const uint8_t g = (uint8_t)(_linear_to_srgb(src[si + 1]) * 255.0f + 0.5f);
      const uint8_t b = (uint8_t)(_linear_to_srgb(src[si + 2]) * 255.0f + 0.5f);
      row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
}

// nearest-neighbor upscale for before preview in upscale mode
static void _nn_upscale(const float *const restrict src,
                        int src_w, int src_h,
                        float *const restrict dst,
                        int dst_w, int dst_h)
{
  for(int y = 0; y < dst_h; y++)
  {
    const int sy = y * src_h / dst_h;
    for(int x = 0; x < dst_w; x++)
    {
      const int sx = x * src_w / dst_w;
      const size_t di = ((size_t)y * dst_w + x) * 3;
      const size_t si = ((size_t)sy * src_w + sx) * 3;
      dst[di + 0] = src[si + 0];
      dst[di + 1] = src[si + 1];
      dst[di + 2] = src[si + 2];
    }
  }
}

// write a float 3ch scanline to TIFF, converting to the
// target bit depth. scratch must be at least width*3*4 bytes
static int _write_tiff_scanline(TIFF *tif,
                                const float *src,
                                int width,
                                int bpp,
                                int row,
                                void *scratch)
{
  if(bpp == 32)
    return TIFFWriteScanline(tif, (void *)src, row, 0);

  if(bpp == 16)
  {
    uint16_t *dst = (uint16_t *)scratch;
    for(int i = 0; i < width * 3; i++)
    {
      float v = CLAMPF(src[i], 0.0f, 1.0f);
      dst[i] = (uint16_t)(v * 65535.0f + 0.5f);
    }
    return TIFFWriteScanline(tif, dst, row, 0);
  }

  // 8 bit
  uint8_t *dst = (uint8_t *)scratch;
  for(int i = 0; i < width * 3; i++)
  {
    float v = CLAMPF(src[i], 0.0f, 1.0f);
    dst[i] = (uint8_t)(v * 255.0f + 0.5f);
  }
  return TIFFWriteScanline(tif, dst, row, 0);
}

// load the right model for a task
static dt_restore_context_t *_load_for_task(
  dt_restore_env_t *env,
  dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_DENOISE:
      return dt_restore_load_denoise(env);
    case NEURAL_TASK_RAW_DENOISE:
      // focus on bayer for now; auto-pick bayer vs linear per image
      // sensor is a follow-up (see dt_restore_load_rawdenoise_linear)
      return dt_restore_load_rawdenoise_bayer(env);
    case NEURAL_TASK_UPSCALE_2X:
      return dt_restore_load_upscale_x2(env);
    case NEURAL_TASK_UPSCALE_4X:
      return dt_restore_load_upscale_x4(env);
    default:
      return NULL;
  }
}

// short, untranslated task names for debug logs (use the localised
// labels at line ~1022 for user-visible strings)
static const char *_task_log_name(dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_DENOISE:     return "denoise";
    case NEURAL_TASK_RAW_DENOISE: return "raw denoise";
    case NEURAL_TASK_UPSCALE_2X:  return "upscale 2x";
    case NEURAL_TASK_UPSCALE_4X:  return "upscale 4x";
    default:                      return "?";
  }
}

// check if a model is available for a task
static gboolean _task_model_available(
  dt_restore_env_t *env,
  dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_DENOISE:
      return dt_restore_denoise_available(env);
    case NEURAL_TASK_RAW_DENOISE:
      return dt_restore_rawdenoise_available(env);
    default:
      return dt_restore_upscale_available(env);
  }
}

// row writer: copy 3ch float scanline to float4 RGBA buffer
typedef struct _buf_writer_data_t
{
  float *out_buf;
  int out_w;
} _buf_writer_data_t;

static int _buf_row_writer(const float *scanline,
                           int out_w, int y,
                           void *user_data)
{
  _buf_writer_data_t *wd = (_buf_writer_data_t *)user_data;
  float *dst = wd->out_buf + (size_t)y * wd->out_w * 4;
  for(int x = 0; x < out_w; x++)
  {
    dst[x * 4 + 0] = scanline[x * 3 + 0];
    dst[x * 4 + 1] = scanline[x * 3 + 1];
    dst[x * 4 + 2] = scanline[x * 3 + 2];
    dst[x * 4 + 3] = 0.0f;
  }
  return 0;
}

// row writer: convert and write 3ch float scanline to TIFF
typedef struct _tiff_writer_data_t
{
  TIFF *tif;
  int bpp;
  void *scratch; // bpp conversion buffer
} _tiff_writer_data_t;

static int _tiff_row_writer(const float *scanline,
                            int out_w, int y,
                            void *user_data)
{
  _tiff_writer_data_t *wd = (_tiff_writer_data_t *)user_data;
  return (_write_tiff_scanline(wd->tif, scanline,
                               out_w, wd->bpp,
                               y, wd->scratch) < 0)
    ? 1 : 0;
}

static int _ai_write_image(dt_imageio_module_data_t *data,
                           const char *filename,
                           const void *in_void,
                           dt_colorspaces_color_profile_type_t over_type,
                           const char *over_filename,
                           void *exif, int exif_len,
                           dt_imgid_t imgid,
                           int num, int total,
                           dt_dev_pixelpipe_t *pipe,
                           const gboolean export_masks)
{
  dt_neural_format_params_t *params = (dt_neural_format_params_t *)data;
  dt_neural_job_t *job = params->job;

  if(!job->ctx)
    return 1;

  // inform the restore pipeline of the working profile so it can
  // convert to sRGB primaries before inference (the model was trained
  // on sRGB data) and back after. without this the model treats the
  // working-profile RGB values as sRGB and shifts hues
  const dt_colorspaces_color_profile_t *work_cp
    = dt_colorspaces_get_work_profile(imgid);
  dt_restore_set_profile(job->ctx, work_cp ? work_cp->profile : NULL);
  dt_restore_set_preserve_wide_gamut(job->ctx, job->preserve_wide_gamut);

  const int width = params->parent.width;
  const int height = params->parent.height;
  const int S = job->scale;
  const int out_w = width * S;
  const int out_h = height * S;
  const float *in_data = (const float *)in_void;

  dt_print(DT_DEBUG_AI,
           "[neural_restore] processing %dx%d -> %dx%d (scale=%d)",
           width, height, out_w, out_h, S);

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "w");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "w");
#endif
  if(!tif)
  {
    dt_control_log(_("failed to open TIFF for writing: %s"), filename);
    return 1;
  }

  const int bpp = (job->bpp == NEURAL_BPP_8) ? 8
    : (job->bpp == NEURAL_BPP_16) ? 16 : 32;
  const int sample_fmt = (bpp == 32)
    ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT;

  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, out_w);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, out_h);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, bpp);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, sample_fmt);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP,
               TIFFDefaultStripSize(tif, 0));

  // embed the darktable working profile ICC so wide-gamut
  // colors are preserved (the restore pipeline applies only the
  // sRGB transfer function, not a primaries conversion)
  const dt_colorspaces_color_profile_t *cp
    = dt_colorspaces_get_work_profile(imgid);
  if(cp && cp->profile)
  {
    uint32_t icc_len = 0;
    cmsSaveProfileToMem(cp->profile, NULL, &icc_len);
    if(icc_len > 0)
    {
      uint8_t *icc_buf = g_malloc(icc_len);
      if(icc_buf)
      {
        cmsSaveProfileToMem(cp->profile, icc_buf, &icc_len);
        TIFFSetField(tif, TIFFTAG_ICCPROFILE, icc_len, icc_buf);
        g_free(icc_buf);
      }
    }
  }

  // strength: 100 = full denoise (no recovery), 0 = source-like.
  // DWT detail recovery runs whenever strength < 100, mixing
  // (1 - strength/100) of the filtered residual back into the output.
  const float recovery_alpha = 1.0f - job->strength / 100.0f;
  const gboolean need_buffer = (recovery_alpha > 0.0f && S == 1);

  int res;
  if(need_buffer)
  {
    // buffer full denoised output for detail recovery
    float *out_4ch = g_try_malloc((size_t)out_w * out_h * 4 * sizeof(float));
    if(!out_4ch)
    {
      TIFFClose(tif);
      dt_control_log(_("out of memory for detail recovery buffer"));
      return 1;
    }

    _buf_writer_data_t bwd = { .out_buf = out_4ch,
                               .out_w = out_w };
    res = dt_restore_process_tiled(job->ctx, in_data,
                                   width, height, S,
                                   _buf_row_writer, &bwd,
                                   job->control_job);

    if(res == 0)
    {
      dt_restore_apply_detail_recovery(in_data, out_4ch, width, height, recovery_alpha);

      // write buffered result to TIFF
      const size_t row_bytes = (size_t)out_w * 3 * sizeof(float);
      float *scan = g_malloc(row_bytes);
      void *cvt = (bpp < 32)
        ? g_malloc((size_t)out_w * 3 * sizeof(uint16_t))
        : NULL;
      for(int y = 0; y < out_h && res == 0; y++)
      {
        const float *row = out_4ch + (size_t)y * out_w * 4;
        for(int x = 0; x < out_w; x++)
        {
          scan[x * 3 + 0] = row[x * 4 + 0];
          scan[x * 3 + 1] = row[x * 4 + 1];
          scan[x * 3 + 2] = row[x * 4 + 2];
        }
        if(_write_tiff_scanline(tif, scan, out_w, bpp, y, cvt) < 0)
        {
          dt_print(DT_DEBUG_AI,
                   "[neural_restore] TIFF write error at scanline %d", y);
          res = 1;
        }
      }
      g_free(cvt);
      g_free(scan);
    }
    g_free(out_4ch);
  }
  else
  {
    void *scratch = (bpp < 32)
      ? g_try_malloc((size_t)out_w * 3 * sizeof(uint16_t))
      : NULL;
    _tiff_writer_data_t twd = { .tif = tif,
                                .bpp = bpp,
                                .scratch = scratch };
    res = dt_restore_process_tiled(job->ctx, in_data,
                                   width, height, S,
                                   _tiff_row_writer,
                                   &twd,
                                   job->control_job);
    g_free(scratch);
  }

  TIFFClose(tif);

  // write EXIF metadata from source image
  if(res == 0 && exif && exif_len > 0)
    dt_exif_write_blob(exif, exif_len, filename, 0);

  if(res != 0)
    g_unlink(filename);

  return res;
}

static void _import_image(const char *filename, dt_imgid_t source_imgid)
{
  dt_film_t film;
  dt_film_init(&film);
  char *dir = g_path_get_dirname(filename);
  dt_filmid_t filmid = dt_film_new(&film, dir);
  g_free(dir);
  const dt_imgid_t newid = dt_image_import(filmid, filename, FALSE, FALSE);
  dt_film_cleanup(&film);

  if(dt_is_valid_imgid(newid))
  {
    dt_print(DT_DEBUG_AI, "[neural_restore] imported imgid=%d: %s", newid, filename);
    if(dt_is_valid_imgid(source_imgid))
    {
      dt_grouping_add_to_group(source_imgid, newid);
      // promote the output as group leader, but only when the source
      // was the current leader — preserves any manually-set leader the
      // user deliberately chose
      const dt_image_t *src = dt_image_cache_get(source_imgid, 'r');
      const gboolean source_is_leader = src && src->group_id == source_imgid;
      dt_image_cache_read_release(src);
      if(source_is_leader)
        dt_grouping_change_representative(newid);
    }
    // refresh the collection so the new image appears in the thumb grid
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static const char *_task_suffix(dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_DENOISE:     return "_denoise";
    case NEURAL_TASK_RAW_DENOISE: return "_raw-denoise";
    case NEURAL_TASK_UPSCALE_2X:  return "_upscale-2x";
    case NEURAL_TASK_UPSCALE_4X:  return "_upscale-4x";
    default:                      return "_restore";
  }
}

static int _task_scale(dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_UPSCALE_2X: return 2;
    case NEURAL_TASK_UPSCALE_4X: return 4;
    default:                     return 1;
  }
}

static void _update_button_sensitivity(dt_lib_neural_restore_t *d);

typedef struct _job_finished_data_t
{
  dt_lib_module_t *self;
  GList *images; // image IDs to remove from processing set
} _job_finished_data_t;

static gboolean _job_finished_idle(gpointer data)
{
  _job_finished_data_t *fd = (_job_finished_data_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)fd->self->data;
  if(d)
  {
    for(GList *l = fd->images; l; l = g_list_next(l))
      g_hash_table_remove(d->processing_images, l->data);
    _update_button_sensitivity(d);
  }
  g_list_free(fd->images);
  g_free(fd);
  return G_SOURCE_REMOVE;
}

static void _job_cleanup(void *param)
{
  dt_neural_job_t *job = (dt_neural_job_t *)param;
  // return the model to the preview cache if it's empty
  // (avoids reloading the model for the next preview)
  dt_lib_neural_restore_t *d
    = (dt_lib_neural_restore_t *)job->self->data;
  dt_pthread_mutex_lock(&d->ctx_lock);
  if(!d->cached_ctx && job->ctx)
  {
    d->cached_ctx = dt_restore_ref(job->ctx);
    d->cached_task = job->task;
  }
  dt_pthread_mutex_unlock(&d->ctx_lock);
  dt_restore_unref(job->ctx);
  g_free(job->output_dir);
  g_free(job->icc_filename);
  g_list_free(job->images);
  g_free(job);
}

// raw-denoise batch path: bypasses the darktable export pipeline and
// goes directly from the source CFA mosaic to a denoised DNG. unlike
// RGB denoise/upscale, there is no demosaic / WB / tonemap / export
// involved — the raw pixels leave and re-enter the pipeline at the
// same stage, so the darktable re-import runs its normal pipeline on
// cleaner data. intentionally self-contained and free of interactions
// with the RGB denoise path
// ensure j->ctx is loaded with the rawdenoise variant matching the
// image's sensor class. reloads if needed; tracks the currently-loaded
// variant in j->raw_ctx_sensor_class so consecutive images of the same
// class don't pay the reload cost. returns 0 on success
static int _ensure_raw_ctx(dt_neural_job_t *j,
                           dt_restore_sensor_class_t cls)
{
  if(j->ctx && j->raw_ctx_sensor_class == cls)
    return 0;

  if(j->ctx)
  {
    dt_restore_unref(j->ctx);
    j->ctx = NULL;
  }
  const char *label = NULL;
  switch(cls)
  {
    case DT_RESTORE_SENSOR_CLASS_BAYER:
      j->ctx = dt_restore_load_rawdenoise_bayer(j->env);
      label = _("bayer");
      break;
    case DT_RESTORE_SENSOR_CLASS_XTRANS:
      j->ctx = dt_restore_load_rawdenoise_xtrans(j->env);
      label = _("x-trans");
      break;
    case DT_RESTORE_SENSOR_CLASS_LINEAR:
      j->ctx = dt_restore_load_rawdenoise_linear(j->env);
      label = _("linear");
      break;
    default:
      return 1;
  }
  if(!j->ctx)
  {
    dt_control_log(_("failed to load AI raw denoise %s model"), label);
    return 1;
  }
  j->raw_ctx_sensor_class = cls;
  return 0;
}

// bayer variant: source CFA (single-channel) -> denoise -> CFA DNG
static int _process_raw_denoise_bayer(dt_neural_job_t *j,
                                      dt_imgid_t imgid,
                                      const char *out_filename,
                                      const char *src_path,
                                      const dt_image_t *img_meta)
{
  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!mbuf.buf)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] failed to load raw mosaic for imgid %d",
             imgid);
    dt_mipmap_cache_release(&mbuf);
    return 1;
  }

  const int width = img_meta->width;
  const int height = img_meta->height;
  const size_t npix = (size_t)width * height;
  float *cfa_in = g_try_malloc(npix * sizeof(float));
  if(!cfa_in)
  {
    dt_mipmap_cache_release(&mbuf);
    return 1;
  }

  if(img_meta->buf_dsc.datatype == TYPE_UINT16)
  {
    const uint16_t *const src = (const uint16_t *)mbuf.buf;
    for(size_t i = 0; i < npix; i++) cfa_in[i] = (float)src[i];
  }
  else if(img_meta->buf_dsc.datatype == TYPE_FLOAT)
  {
    memcpy(cfa_in, mbuf.buf, npix * sizeof(float));
  }
  else
  {
    dt_control_log(_("raw denoise: unsupported raw datatype"));
    g_free(cfa_in);
    dt_mipmap_cache_release(&mbuf);
    return 1;
  }

  dt_mipmap_cache_release(&mbuf);

  uint16_t *cfa_out = g_try_malloc(npix * sizeof(uint16_t));
  if(!cfa_out)
  {
    g_free(cfa_in);
    return 1;
  }

  int res = dt_restore_raw_bayer(j->ctx, img_meta, cfa_in,
                                 width, height, cfa_out,
                                 j->raw_strength,
                                 j->control_job);
  g_free(cfa_in);
  if(res != 0)
  {
    g_free(cfa_out);
    return res;
  }

  uint8_t *exif_blob = NULL;
  const int exif_len = dt_exif_read_blob(&exif_blob, src_path, imgid,
                                         FALSE, width, height, TRUE);
  uint8_t *jpeg_buf = NULL;
  int jpeg_len = 0, jpeg_w = 0, jpeg_h = 0;
  dt_imageio_dng_preview_t preview = {0};
  const int prev_rc = _extract_source_jpeg_preview(src_path, &jpeg_buf,
                                                   &jpeg_len, &jpeg_w, &jpeg_h);
  if(prev_rc == 0)
  {
    preview.data   = jpeg_buf;
    preview.len    = jpeg_len;
    preview.width  = jpeg_w;
    preview.height = jpeg_h;
    dt_print(DT_DEBUG_AI,
             "[neural_restore] embedded JPEG preview from source %dx%d (%d bytes)",
             jpeg_w, jpeg_h, jpeg_len);
  }
  else
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] no embedded preview in source (rc=%d) — "
             "writing DNG without thumbnail", prev_rc);
  }
  res = dt_imageio_dng_write_cfa_bayer(out_filename, cfa_out,
                                       width, height, img_meta,
                                       exif_blob, exif_len,
                                       jpeg_buf ? &preview : NULL);
  g_free(jpeg_buf);
  g_free(exif_blob);
  g_free(cfa_out);
  return res;
}

// linear variant: darktable's demosaic runs inside raw_restore_linear,
// so there's no CFA buffer to hand in. output is a 3ch linear DNG
static int _process_raw_denoise_linear(dt_neural_job_t *j,
                                       dt_imgid_t imgid,
                                       const char *out_filename,
                                       const char *src_path,
                                       const dt_image_t *img_meta)
{
  float *rgb = NULL;
  int w = 0, h = 0;
  int res = dt_restore_raw_linear(j->ctx, imgid, &rgb, &w, &h,
                                  j->raw_strength, j->control_job);
  if(res != 0 || !rgb)
  {
    g_free(rgb);
    return res ? res : 1;
  }

  uint8_t *exif_blob = NULL;
  const int exif_len = dt_exif_read_blob(&exif_blob, src_path, imgid,
                                         FALSE, w, h, TRUE);
  uint8_t *jpeg_buf = NULL;
  int jpeg_len = 0, jpeg_w = 0, jpeg_h = 0;
  dt_imageio_dng_preview_t preview = {0};
  const int prev_rc = _extract_source_jpeg_preview(src_path, &jpeg_buf,
                                                   &jpeg_len, &jpeg_w, &jpeg_h);
  if(prev_rc == 0)
  {
    preview.data   = jpeg_buf;
    preview.len    = jpeg_len;
    preview.width  = jpeg_w;
    preview.height = jpeg_h;
    dt_print(DT_DEBUG_AI,
             "[neural_restore] embedded JPEG preview from source %dx%d (%d bytes)",
             jpeg_w, jpeg_h, jpeg_len);
  }
  else
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] no embedded preview in source (rc=%d) — "
             "writing DNG without thumbnail", prev_rc);
  }
  res = dt_imageio_dng_write_linear(out_filename, rgb, w, h, img_meta,
                                    exif_blob, exif_len,
                                    jpeg_buf ? &preview : NULL);
  g_free(jpeg_buf);
  g_free(exif_blob);
  dt_free_align(rgb);
  return res;
}

static int _process_raw_denoise_one(dt_neural_job_t *j,
                                    dt_imgid_t imgid,
                                    const char *out_filename,
                                    const char *src_path)
{
  // force the raw to be loaded so buf_dsc.{filters,channels,datatype}
  // are populated. for a fresh session, dt_image_cache_get alone may
  // return a dt_image_t whose buf_dsc is zeroed because rawspeed has
  // not been invoked on this id yet
  dt_mipmap_buffer_t warmup;
  dt_mipmap_cache_get(&warmup, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  const gboolean loaded = (warmup.buf != NULL);
  dt_mipmap_cache_release(&warmup);
  if(!loaded)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] mipmap warmup failed for imgid %d", imgid);
    dt_control_log(_("raw denoise: cannot load source image"));
    return 1;
  }

  const dt_image_t *cached = dt_image_cache_get(imgid, 'r');
  if(!cached) return 1;
  dt_image_t img_meta = *cached;
  dt_image_cache_read_release(cached);

  const uint32_t filters = img_meta.buf_dsc.filters;
  const uint32_t channels = img_meta.buf_dsc.channels;
  const uint32_t flags = img_meta.flags;
  const dt_restore_sensor_class_t cls = dt_restore_classify_sensor(&img_meta);

  const char *cls_name
    = (cls == DT_RESTORE_SENSOR_CLASS_BAYER)  ? "bayer"
    : (cls == DT_RESTORE_SENSOR_CLASS_XTRANS) ? "x-trans"
    : (cls == DT_RESTORE_SENSOR_CLASS_LINEAR) ? "linear"
    :                                           "unsupported";
  dt_print(DT_DEBUG_AI,
           "[neural_restore] imgid %d: flags=0x%x channels=%u "
           "filters=0x%x (%s)", imgid, flags, channels, filters,
           cls_name);

  if(cls == DT_RESTORE_SENSOR_CLASS_UNSUPPORTED)
  {
    dt_control_log(_("raw denoise: image is not a supported raw sensor format"));
    return 1;
  }

  if(_ensure_raw_ctx(j, cls)) return 1;

  switch(cls)
  {
    case DT_RESTORE_SENSOR_CLASS_BAYER:
      return _process_raw_denoise_bayer(j, imgid, out_filename,
                                        src_path, &img_meta);
    case DT_RESTORE_SENSOR_CLASS_XTRANS:
      // today: X-Trans runs through the linear pipeline. a future
      // dedicated xtrans_v1 model with a different input format would
      // get its own _process_raw_denoise_xtrans() here; the dispatch
      // structure stays, just the target function swaps
      return _process_raw_denoise_linear(j, imgid, out_filename,
                                         src_path, &img_meta);
    case DT_RESTORE_SENSOR_CLASS_LINEAR:
      return _process_raw_denoise_linear(j, imgid, out_filename,
                                         src_path, &img_meta);
    default:
      return 1;
  }
}

static int32_t _process_job_run(dt_job_t *job)
{
  dt_neural_job_t *j = dt_control_job_get_params(job);

  const char *task_name = (j->task == NEURAL_TASK_DENOISE)     ? _("denoise")
                        : (j->task == NEURAL_TASK_RAW_DENOISE) ? _("raw denoise")
                        :                                        _("upscale");
  char msg[256];
  snprintf(msg, sizeof(msg), _("loading %s model..."), task_name);
  dt_control_job_set_progress_message(job, msg);

  j->control_job = job;

  // steal cached model if available - take a ref for the batch
  // and release the cache's ref. this prevents the preview thread
  // from using it concurrently (ORT sessions are not thread-safe
  // for simultaneous inference). if the user triggers a preview
  // while batch is running, the preview will load its own session
  // since cached_ctx is NULL
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)j->self->data;
  dt_pthread_mutex_lock(&d->ctx_lock);
  if(d->cached_ctx && d->cached_task == j->task)
  {
    j->ctx = dt_restore_ref(d->cached_ctx);
    dt_restore_unref(d->cached_ctx);
    d->cached_ctx = NULL;
  }
  else
  {
    dt_restore_unref(d->cached_ctx);
    d->cached_ctx = NULL;
  }
  dt_pthread_mutex_unlock(&d->ctx_lock);
  if(!j->ctx)
    j->ctx = _load_for_task(j->env, j->task);

  if(!j->ctx)
  {
    dt_control_log(_("failed to load AI %s model"), task_name);
    return 1;
  }

  dt_print(DT_DEBUG_AI,
           "[neural_restore] job started: task=%s, scale=%d, images=%d",
           task_name, j->scale, g_list_length(j->images));

  dt_imageio_module_format_t fmt = {
    .mime = _ai_get_mime,
    .levels = _ai_check_levels,
    .bpp = _ai_check_bpp,
    .write_image = _ai_write_image};

  dt_neural_format_params_t fmt_params = {.job = j};

  const int total = g_list_length(j->images);
  int count = 0;
  int successes = 0;  // images that made it through export (for the completion toast)
  const char *suffix = _task_suffix(j->task);

  for(GList *iter = j->images; iter; iter = g_list_next(iter))
  {
    if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED)
      break;

    dt_imgid_t imgid = GPOINTER_TO_INT(iter->data);
    char srcpath[PATH_MAX];
    dt_image_full_path(imgid, srcpath, sizeof(srcpath), NULL);

    // build base name (strip extension)
    char *basename = g_path_get_basename(srcpath);
    char *dot = strrchr(basename, '.');
    if(dot) *dot = '\0';

    // expand output directory variables (e.g. $(FILE_FOLDER))
    char *dir_pattern = (j->output_dir && j->output_dir[0])
      ? j->output_dir : "$(FILE_FOLDER)";
    dt_variables_params_t *vp = NULL;
    dt_variables_params_init(&vp);
    vp->filename = srcpath;
    vp->imgid = imgid;
    char *out_dir = dt_variables_expand(vp,
                                        (gchar *)dir_pattern,
                                        FALSE);
    dt_variables_params_destroy(vp);

    // if basename already ends with the suffix, don't
    // append it again (e.g. re-processing a denoised file)
    const size_t blen = strlen(basename);
    const size_t slen = strlen(suffix);
    const gboolean has_suffix
      = (blen >= slen) && strcmp(basename + blen - slen, suffix) == 0;

    // build base path without .tif for collision loop
    char base[PATH_MAX];
    if(has_suffix)
      snprintf(base, sizeof(base), "%s/%s", out_dir, basename);
    else
      snprintf(base, sizeof(base), "%s/%s%s", out_dir, basename, suffix);

    g_free(out_dir);
    g_free(basename);

    // ensure output directory exists
    char *out_dir_resolved = g_path_get_dirname(base);
    if(g_mkdir_with_parents(out_dir_resolved, 0750) != 0)
    {
      dt_print(DT_DEBUG_AI,
               "[neural_restore] failed to create output directory: %s",
               out_dir_resolved);
      dt_control_log(_("neural restore: cannot create output directory"));
      g_free(out_dir_resolved);
      dt_control_job_set_progress(job, (double)++count / total);
      continue;
    }
    g_free(out_dir_resolved);

    // raw denoise writes DNG; RGB denoise/upscale writes TIFF
    const char *ext
      = (j->task == NEURAL_TASK_RAW_DENOISE) ? "dng" : "tif";

    // find unique filename: base.<ext>, base_1.<ext>, ...
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s.%s", base, ext);

    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      gboolean found = FALSE;
      for(int s = 1; s < 10000; s++)
      {
        snprintf(filename, sizeof(filename), "%s_%d.%s", base, s, ext);
        if(!g_file_test(filename, G_FILE_TEST_EXISTS))
        {
          found = TRUE;
          break;
        }
      }
      if(!found)
      {
        dt_print(DT_DEBUG_AI,
                 "[neural_restore] could not find unique filename for imgid %d",
                 imgid);
        dt_control_log(_("neural restore: too many output files"));
        dt_control_job_set_progress(job, (double)++count / total);
        continue;
      }
    }

    dt_print(DT_DEBUG_AI,
             "[neural_restore] processing imgid %d -> %s", imgid, filename);
    snprintf(msg, sizeof(msg),
             (j->task == NEURAL_TASK_DENOISE)      ? _("denoising image %d/%d...")
             : (j->task == NEURAL_TASK_RAW_DENOISE) ? _("raw denoising image %d/%d...")
             : (j->task == NEURAL_TASK_UPSCALE_2X)  ? _("upscaling 2x image %d/%d...")
             :                                       _("upscaling 4x image %d/%d..."),
             count + 1, total);
    dt_control_job_set_progress_message(job, msg);

    int step_err = 0;
    if(j->task == NEURAL_TASK_RAW_DENOISE)
    {
      step_err = _process_raw_denoise_one(j, imgid, filename, srcpath);
      if(step_err)
        dt_control_log(_("raw denoise failed for image %d"), imgid);
    }
    else
    {
      step_err = dt_imageio_export_with_flags(
        imgid,
        filename,
        &fmt,
        (dt_imageio_module_data_t *)&fmt_params,
        FALSE,  // ignore_exif — pass EXIF to write_image
        FALSE,  // display_byteorder
        TRUE,   // high_quality
        TRUE,   // upscale
        FALSE,  // is_scaling
        1.0,    // scale_factor
        FALSE,  // thumbnail_export
        NULL,   // filter
        FALSE,  // copy_metadata
        FALSE,  // export_masks
        (j->icc_type == DT_COLORSPACE_NONE)
          ? dt_colorspaces_get_work_profile(imgid)->type
          : j->icc_type,
        j->icc_filename,
        DT_INTENT_PERCEPTUAL,
        NULL, NULL,
        count, total, NULL, -1);
      if(step_err)
      {
        dt_print(DT_DEBUG_AI,
                 "[neural_restore] export failed for imgid %d", imgid);
        dt_control_log(_("neural restore: export failed"));
      }
    }

    if(step_err)
    {
      dt_control_job_set_progress(job, (double)++count / total);
      continue;
    }

    if(j->add_to_catalog)
      _import_image(filename, imgid);
    successes++;
    dt_control_job_set_progress(job, (double)++count / total);
  }

  dt_restore_unref(j->ctx);
  j->ctx = NULL;

  // single completion toast, shown in whichever view the user is in;
  // covers the batch cleanly and avoids the per-image navigation that
  // DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE would trigger
  if(successes > 0)
    dt_control_log(ngettext("neural %s: %d image processed",
                            "neural %s: %d images processed",
                            successes),
                   task_name, successes);

  _job_finished_data_t *fd = g_new(_job_finished_data_t, 1);
  fd->self = j->self;
  fd->images = g_list_copy(j->images);
  g_idle_add(_job_finished_idle, fd);
  return 0;
}

static gboolean _check_model_available(
  dt_lib_neural_restore_t *d,
  dt_neural_task_t task)
{
  return _task_model_available(d->env, task);
}

// check if any of the currently selected images are already being processed
static gboolean _any_selected_processing(dt_lib_neural_restore_t *d)
{
  GList *images = dt_act_on_get_images(TRUE, TRUE, FALSE);
  gboolean found = FALSE;
  for(GList *l = images; l; l = g_list_next(l))
  {
    if(g_hash_table_contains(d->processing_images, l->data))
    {
      found = TRUE;
      break;
    }
  }
  g_list_free(images);
  return found;
}

static void _update_button_sensitivity(dt_lib_neural_restore_t *d)
{
  const gboolean has_images = (dt_act_on_get_images_nb(TRUE, FALSE) > 0);
  const gboolean sensitive = d->model_available && has_images
                             && !_any_selected_processing(d);
  gtk_widget_set_sensitive(d->process_button, sensitive);
  // picker only available after preview has been generated
  gtk_widget_set_sensitive(d->pick_button,
                           d->export_pixels != NULL);
}

static void _update_info_label(dt_lib_neural_restore_t *d)
{
  d->info_text_left[0] = '\0';
  d->info_text_right[0] = '\0';
  d->warning_text[0] = '\0';

  if(!d->model_available)
    return;

  const int scale = _task_scale(d->task);

  // pick the first selected/active image to resolve "image settings"
  // profile and to compute upscale output dimensions
  GList *imgs = dt_act_on_get_images(TRUE, FALSE, FALSE);
  const dt_imgid_t imgid = imgs ? GPOINTER_TO_INT(imgs->data) : NO_IMGID;

  // show output dimensions for upscale using final developed size
  // (respects crop, rotation, lens correction)
  if(scale > 1 && dt_is_valid_imgid(imgid))
  {
    int fw = 0, fh = 0;
    dt_image_get_final_size(imgid, &fw, &fh);
    if(fw > 0 && fh > 0)
    {
      const int out_w = fw * scale;
      const int out_h = fh * scale;
      const double in_mp = (double)fw * fh / 1e6;
      const double out_mp = (double)out_w * out_h / 1e6;
      snprintf(d->info_text_left, sizeof(d->info_text_left),
               "%.0fMP", in_mp);
      snprintf(d->info_text_right, sizeof(d->info_text_right),
               "%.0fMP", out_mp);

      if(out_mp >= LARGE_OUTPUT_MP)
        snprintf(d->warning_text, sizeof(d->warning_text),
                 "%s",
                 _("large output - processing will be slow"));
    }
  }

  // raw denoise: DNG variant batch will produce. Source of truth is
  // preview_raw_sensor_class — buf_dsc.filters is zeroed until rawspeed
  // decodes the image, so probing it here would misclassify an
  // unloaded X-Trans RAF. both X-Trans and the linear-fallback class
  // currently write LinearRaw DNG; only Bayer writes CFA DNG
  if(d->task == NEURAL_TASK_RAW_DENOISE
     && dt_is_valid_imgid(imgid)
     && d->preview_raw_imgid == imgid)
  {
    const gboolean is_bayer_out
      = (d->preview_raw_sensor_class == DT_RESTORE_SENSOR_CLASS_BAYER);
    snprintf(d->info_text_left, sizeof(d->info_text_left), "%s",
             is_bayer_out ? _("output: Bayer CFA DNG")
                          : _("output: LinearRaw DNG"));
  }

  // gamut note (informational, not a warning): reuse the same info
  // line as the upscale size display. for denoise, shows standalone
  // in info_text_left; for upscale, appended to the size info. not
  // applicable to raw denoise — that path writes camRGB DNG without
  // any sRGB wrapper, so there's no gamut clipping to warn about
  if(d->task != NEURAL_TASK_RAW_DENOISE)
  {
    const dt_colorspaces_color_profile_type_t icc_type
      = dt_conf_key_exists(CONF_ICC_TYPE)
        ? dt_conf_get_int(CONF_ICC_TYPE)
        : DT_COLORSPACE_NONE;
    if(dt_image_has_wide_gamut_output_profile(imgid, icc_type))
    {
      const gboolean preserve = dt_conf_key_exists(CONF_PRESERVE_WIDE_GAMUT)
        ? dt_conf_get_bool(CONF_PRESERVE_WIDE_GAMUT) : TRUE;
      const char *msg = (scale == 1 && preserve)
        ? _("wide-gamut preserved, not denoised")
        : _("wide-gamut clipped");
      if(d->info_text_right[0])
      {
        const size_t used = strlen(d->info_text_right);
        snprintf(d->info_text_right + used, sizeof(d->info_text_right) - used,
                 "  ·  %s", msg);
      }
      else
      {
        snprintf(d->info_text_left, sizeof(d->info_text_left), "%s", msg);
      }
    }
  }

  g_list_free(imgs);
  gtk_widget_queue_draw(d->preview_area);
}

static void _trigger_preview(dt_lib_module_t *self);
static void _cancel_preview(dt_lib_module_t *self);
static void _schedule_preview_failed(dt_lib_module_t *self,
                                     dt_nr_preview_err_t err);

static void _task_changed(dt_lib_neural_restore_t *d)
{
  d->model_available = _check_model_available(d, d->task);
  if(!d->model_available)
  {
    d->preview_ready = FALSE;
    d->preview_generating = FALSE;
    gtk_widget_queue_draw(d->preview_area);
  }

  // restore strength slider from conf when switching to a scale==1
  // task (denoise / raw denoise — both benefit from the strength
  // knob), snap to 100 when switching to upscale (upscale has no
  // strength semantics — see note below). use _strength_changing
  // flag to avoid redundant conf writes from the slider's
  // value-changed callback.
  // raw denoise has no DWT analysis of exported preview pixels, so
  // the slider is hidden there — but for DENOISE and UPSCALE we
  // preserve the master behaviour (slider visible, value restored /
  // reset).
  d->recovery_changing = TRUE;
  if(d->task == NEURAL_TASK_DENOISE || d->task == NEURAL_TASK_RAW_DENOISE)
  {
    const float saved = dt_conf_key_exists(CONF_STRENGTH)
      ? dt_conf_get_float(CONF_STRENGTH) : 100.0f;
    dt_bauhaus_slider_set(d->recovery_slider, saved);
  }
  else
  {
    dt_bauhaus_slider_set(d->recovery_slider, 100.0f);
  }
  gtk_widget_set_visible(d->recovery_slider,
                         d->task != NEURAL_TASK_RAW_DENOISE);
  d->recovery_changing = FALSE;

  // output settings that only apply to the RGB-export-based tasks:
  // bit depth selects TIFF bits-per-pixel (raw denoise writes DNG),
  // output ICC profile applies to the exported TIFF, and
  // preserve-wide-gamut is part of the sRGB-gamma wrapper around the
  // denoise-nind model. bpp + ICC apply to any TIFF-emitting task
  // (denoise + upscale); preserve-wide-gamut only matters for denoise
  // (upscale has no pixel-to-pixel correspondence to pass through).
  // raw denoise writes a DNG directly from the sensor-space inference
  // result, so none of these knobs apply to it
  const gboolean tiff_knobs_visible
    = (d->task != NEURAL_TASK_RAW_DENOISE);
  const gboolean wide_gamut_visible
    = (d->task == NEURAL_TASK_DENOISE);
  if(d->bpp_combo)
    gtk_widget_set_visible(d->bpp_combo, tiff_knobs_visible);
  if(d->profile_combo)
    gtk_widget_set_visible(d->profile_combo, tiff_knobs_visible);
  if(d->preserve_wide_gamut_toggle)
    gtk_widget_set_visible(d->preserve_wide_gamut_toggle, wide_gamut_visible);

  _update_info_label(d);
  _update_button_sensitivity(d);
}

// per-task preview cache helpers
//
// the cache holds, per task, the buffers needed to redisplay the most
// recent successful preview without re-running inference. on tab switch
// we look up the new task's slot keyed by (imgid, patch_center). on
// hit we install the cached buffers and skip the worker; on miss we
// schedule one. invalidated wholesale on image / patch change
//
// for raw denoise, before_rgb / after_rgb hold the unblended source vs
// fully-denoised lin_rec2020 crops (= preview_raw_src_rgb /
// preview_raw_denoised_rgb). detail is always NULL there
//
// for RGB denoise + upscale, before_rgb / after_rgb hold the displayed
// preview_before / preview_after, and detail (denoise only) holds the
// DWT luminance residual used by the strength slider

static void _preview_cache_free_slot(dt_lib_neural_restore_t *d, int task)
{
  g_free(d->preview_cache[task].before_rgb);
  d->preview_cache[task].before_rgb = NULL;
  g_free(d->preview_cache[task].after_rgb);
  d->preview_cache[task].after_rgb = NULL;
  dt_free_align(d->preview_cache[task].detail);
  d->preview_cache[task].detail = NULL;
  d->preview_cache[task].valid = FALSE;
}

static void _preview_cache_invalidate_all(dt_lib_neural_restore_t *d)
{
  for(int t = 0; t < NEURAL_TASK_COUNT; t++)
    _preview_cache_free_slot(d, t);
}

static gboolean _preview_cache_hit(dt_lib_neural_restore_t *d,
                                   dt_neural_task_t task,
                                   dt_imgid_t imgid)
{
  if(task >= NEURAL_TASK_COUNT) return FALSE;
  const __typeof__(d->preview_cache[0]) *e = &d->preview_cache[task];
  // exact match on patch_center (no fp tolerance: we store the exact
  // value the worker received, so equality is reliable)
  return e->valid
      && e->imgid == imgid
      && e->patch_center[0] == d->patch_center[0]
      && e->patch_center[1] == d->patch_center[1];
}

// memcpy buffers into the cache slot for `task`. caller retains
// ownership of the source pointers (we duplicate)
static void _preview_cache_store(dt_lib_neural_restore_t *d,
                                 dt_neural_task_t task,
                                 dt_imgid_t imgid,
                                 const float patch_center[2],
                                 int crop_w, int crop_h,
                                 const float *before, const float *after,
                                 const float *detail)
{
  // task is an unsigned enum, no need for < 0 check
  if(task >= NEURAL_TASK_COUNT) return;
  if(crop_w <= 0 || crop_h <= 0 || !before || !after) return;
  _preview_cache_free_slot(d, task);
  const size_t n3 = (size_t)crop_w * crop_h * 3;
  d->preview_cache[task].before_rgb = g_try_malloc(n3 * sizeof(float));
  d->preview_cache[task].after_rgb  = g_try_malloc(n3 * sizeof(float));
  if(!d->preview_cache[task].before_rgb || !d->preview_cache[task].after_rgb)
  {
    _preview_cache_free_slot(d, task);
    return;
  }
  memcpy(d->preview_cache[task].before_rgb, before, n3 * sizeof(float));
  memcpy(d->preview_cache[task].after_rgb,  after,  n3 * sizeof(float));
  if(detail)
  {
    const size_t n1 = (size_t)crop_w * crop_h;
    d->preview_cache[task].detail = dt_alloc_align_float(n1);
    if(d->preview_cache[task].detail)
      memcpy(d->preview_cache[task].detail, detail, n1 * sizeof(float));
  }
  d->preview_cache[task].imgid = imgid;
  d->preview_cache[task].patch_center[0] = patch_center[0];
  d->preview_cache[task].patch_center[1] = patch_center[1];
  d->preview_cache[task].crop_w = crop_w;
  d->preview_cache[task].crop_h = crop_h;
  d->preview_cache[task].valid = TRUE;
}

// rebuild the "after" cairo surface from cached float buffers, applying
// DWT-filtered detail recovery so that slider changes don't re-run inference
static void _rebuild_cairo_after(dt_lib_neural_restore_t *d)
{
  if(!d->preview_after || !d->cairo_after) return;

  const int w = d->preview_w;
  const int h = d->preview_h;
  const int stride = d->cairo_stride;
  // strength = 100 → no recovery (max denoise visible); strength = 0
  // → full filtered detail back. preview mirrors batch semantics.
  const float strength = dt_conf_key_exists(CONF_STRENGTH)
    ? dt_conf_get_float(CONF_STRENGTH) : 100.0f;
  const float alpha = 1.0f - strength / 100.0f;
  const gboolean recover = (alpha > 0.0f && d->preview_detail);

  for(int y = 0; y < h; y++)
  {
    uint32_t *row = (uint32_t *)(d->cairo_after + y * stride);
    for(int x = 0; x < w; x++)
    {
      const int si = (y * w + x) * 3;
      const int pi = y * w + x;
      float r = d->preview_after[si + 0];
      float g = d->preview_after[si + 1];
      float b = d->preview_after[si + 2];
      if(recover)
      {
        const float detail = alpha * d->preview_detail[pi];
        r += detail;
        g += detail;
        b += detail;
      }
      const uint8_t cr = (uint8_t)(_linear_to_srgb(r) * 255.0f + 0.5f);
      const uint8_t cg = (uint8_t)(_linear_to_srgb(g) * 255.0f + 0.5f);
      const uint8_t cb = (uint8_t)(_linear_to_srgb(b) * 255.0f + 0.5f);
      row[x] = ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
    }
  }
}

static gboolean _preview_result_idle(gpointer data)
{
  dt_neural_preview_result_t *res = (dt_neural_preview_result_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)res->self->data;

  // discard stale results
  if(res->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    g_free(res->before);
    g_free(res->after);
    g_free(res->export_pixels);
    g_free(res);
    return G_SOURCE_REMOVE;
  }

  g_free(d->preview_before);
  g_free(d->preview_after);
  dt_free_align(d->preview_detail);
  d->preview_before = res->before;
  d->preview_after = res->after;
  d->preview_w = res->width;
  d->preview_h = res->height;

  // cache the export image for picker re-cropping
  if(res->export_pixels)
  {
    g_free(d->export_pixels);
    g_free(d->export_cairo);
    d->export_pixels = res->export_pixels;
    d->export_w = res->export_w;
    d->export_h = res->export_h;
    d->export_cairo = NULL;
  }

  // pre-compute DWT-filtered luminance detail for instant slider response
  d->preview_detail = dt_restore_compute_dwt_detail(res->before, res->after,
                                                    res->width, res->height);

  // rebuild cached cairo surface data
  g_free(d->cairo_before);
  g_free(d->cairo_after);
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, res->width);
  d->cairo_before = g_malloc(stride * res->height);
  d->cairo_after = g_malloc(stride * res->height);
  d->cairo_stride = stride;
  _float_rgb_to_cairo(d->preview_before, d->cairo_before,
                      res->width, res->height, stride);
  _rebuild_cairo_after(d);

  // store this result in the per-task cache so a later tab switch
  // back to the same task / image / patch skips inference
  _preview_cache_store(d, res->task, res->imgid, res->patch_center,
                       res->width, res->height,
                       d->preview_before, d->preview_after,
                       d->preview_detail);

  d->preview_ready = TRUE;
  d->preview_generating = FALSE;
  _update_button_sensitivity(d);
  gtk_widget_queue_draw(d->preview_area);
  g_free(res);
  return G_SOURCE_REMOVE;
}

static gpointer _preview_thread(gpointer data)
{
  dt_neural_preview_data_t *pd = (dt_neural_preview_data_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)pd->self->data;

  // bail reason if we hit cleanup; stale-sequence bails are dropped
  dt_nr_preview_err_t err = DT_NR_PREVIEW_ERR_INIT_FAILED;

  // reuse borrowed export if available (re-pick), otherwise export.
  // pixels points to either the borrowed buffer (not owned) or
  // cap.pixels (owned, must be freed on error or passed to result)
  dt_neural_preview_capture_t cap = {0};
  const float *pixels = NULL;
  int pixels_w = 0, pixels_h = 0;
  gboolean owns_pixels = FALSE;

  if(pd->reuse_pixels)
  {
    pixels = pd->reuse_pixels;
    pixels_w = pd->reuse_w;
    pixels_h = pd->reuse_h;
  }
  else
  {
    const int export_size = dt_conf_get_int(CONF_PREVIEW_EXPORT_SIZE);
    cap.parent.max_width = export_size;
    cap.parent.max_height = export_size;

    dt_imageio_module_format_t fmt = {
      .mime = _ai_get_mime,
      .levels = _ai_check_levels,
      .bpp = _ai_check_bpp,
      .write_image = _preview_capture_write_image};

    const dt_colorspaces_color_profile_type_t cfg_type
      = dt_conf_key_exists(CONF_ICC_TYPE)
        ? dt_conf_get_int(CONF_ICC_TYPE)
        : DT_COLORSPACE_NONE;
    gchar *cfg_file = (cfg_type == DT_COLORSPACE_FILE)
      ? dt_conf_get_string(CONF_ICC_FILE)
      : NULL;
    dt_imageio_export_with_flags(pd->imgid,
                                 "unused",
                                 &fmt,
                                 (dt_imageio_module_data_t *)&cap,
                                 TRUE,   // ignore_exif
                                 FALSE,  // display_byteorder
                                 TRUE,   // high_quality
                                 FALSE,  // upscale
                                 FALSE,  // is_scaling
                                 1.0,    // scale_factor
                                 FALSE,  // thumbnail_export
                                 NULL,   // filter
                                 FALSE,  // copy_metadata
                                 FALSE,  // export_masks
                                 (cfg_type == DT_COLORSPACE_NONE)
                                   ? dt_colorspaces_get_work_profile(pd->imgid)->type
                                   : cfg_type,
                                 cfg_file,
                                 DT_INTENT_PERCEPTUAL,
                                 NULL, NULL, 1, 1, NULL, -1);
    g_free(cfg_file);

    pixels = cap.pixels;
    pixels_w = cap.cap_w;
    pixels_h = cap.cap_h;
    owns_pixels = TRUE;
  }

  if(!pixels || pd->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }

  dt_print(DT_DEBUG_AI,
           "[neural_restore] preview: %s %dx%d, scale=%d, "
           "export_size=%d",
           owns_pixels ? "exported" : "reusing",
           pixels_w, pixels_h, pd->scale,
           owns_pixels ? cap.parent.max_width : 0);

  // crop region matching widget aspect ratio
  // clamp crop to export dimensions and max preview size to keep
  // inference responsive. the result is scaled to fill the widget
  const int max_crop = 512;
  // pw/ph are output dimensions (divisible by scale),
  // crop_w/crop_h are input dimensions to the model
  int pw = MIN(MIN(pd->preview_w, pixels_w * pd->scale),
               max_crop * pd->scale);
  int ph = MIN(MIN(pd->preview_h, pixels_h * pd->scale),
               max_crop * pd->scale);
  pw = (pw / pd->scale) * pd->scale;
  ph = (ph / pd->scale) * pd->scale;
  const int crop_w = pw / pd->scale;
  const int crop_h = ph / pd->scale;

  if(crop_w <= 0 || crop_h <= 0)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] preview: export too small (%dx%d)",
             pixels_w, pixels_h);
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }

  // compute crop position in export image
  int crop_x = (int)(pd->patch_center[0] * pixels_w) - crop_w / 2;
  int crop_y = (int)(pd->patch_center[1] * pixels_h) - crop_h / 2;
  crop_x = CLAMP(crop_x, 0, pixels_w - crop_w);
  crop_y = CLAMP(crop_y, 0, pixels_h - crop_h);

  // extract crop as interleaved RGBx (4ch) for dt_restore_process_tiled
  float *crop_4ch = g_try_malloc((size_t)crop_w * crop_h * 4 * sizeof(float));
  if(!crop_4ch)
  {
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }
  for(int y = 0; y < crop_h; y++)
    memcpy(crop_4ch + (size_t)y * crop_w * 4,
           pixels + ((size_t)(crop_y + y) * pixels_w + crop_x) * 4,
           (size_t)crop_w * 4 * sizeof(float));

  // extract "before" as interleaved RGB (3ch) for display
  float *crop_rgb = g_try_malloc((size_t)crop_w * crop_h * 3 * sizeof(float));
  if(!crop_rgb)
  {
    g_free(crop_4ch);
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }
  for(int y = 0; y < crop_h; y++)
  {
    for(int x = 0; x < crop_w; x++)
    {
      const size_t si = ((size_t)(crop_y + y) * pixels_w + (crop_x + x)) * 4;
      const size_t di = ((size_t)y * crop_w + x) * 3;
      crop_rgb[di + 0] = pixels[si + 0];
      crop_rgb[di + 1] = pixels[si + 1];
      crop_rgb[di + 2] = pixels[si + 2];
    }
  }

  // check for cancellation before expensive inference
  if(pd->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    g_free(crop_4ch);
    g_free(crop_rgb);
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }

  // take a ref on the cached model (or load a new one).
  // hold the lock only for the ref/swap, not during inference
  dt_restore_context_t *ctx = NULL;
  dt_pthread_mutex_lock(&d->ctx_lock);
  if(!d->cached_ctx || d->cached_task != pd->task)
  {
    dt_restore_unref(d->cached_ctx);
    d->cached_ctx = _load_for_task(pd->env, pd->task);
    d->cached_task = pd->task;
  }
  if(d->cached_ctx)
    ctx = dt_restore_ref(d->cached_ctx);
  dt_pthread_mutex_unlock(&d->ctx_lock);

  if(!ctx)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] preview: failed to load model");
    g_free(crop_4ch);
    g_free(crop_rgb);
    if(owns_pixels) g_free(cap.pixels);
    goto cleanup;
  }

  // use the same tiled processing as batch — this handles mirror
  // padding, overlap blending, and planar conversion identically.
  // tile size is fixed at model load time (ctx->tile_size) and shared
  // across preview and batch so JIT-compiling EPs (MIGraphX, CoreML,
  // TensorRT) compile the kernel only once per session.
  float *out_4ch = g_try_malloc((size_t)pw * ph * 4 * sizeof(float));
  if(!out_4ch)
  {
    g_free(crop_4ch);
    g_free(crop_rgb);
    goto cleanup;
  }

  dt_print(DT_DEBUG_AI,
           "[neural_restore] preview: tiled inference %dx%d",
           crop_w, crop_h);

  // set working profile on context so the model sees sRGB primaries
  const dt_colorspaces_color_profile_t *work_cp
    = dt_colorspaces_get_work_profile(pd->imgid);
  dt_restore_set_profile(ctx, work_cp ? work_cp->profile : NULL);
  const gboolean pres = dt_conf_key_exists(CONF_PRESERVE_WIDE_GAMUT)
    ? dt_conf_get_bool(CONF_PRESERVE_WIDE_GAMUT) : TRUE;
  dt_restore_set_preserve_wide_gamut(ctx, pres);

  _buf_writer_data_t bwd = { .out_buf = out_4ch, .out_w = pw };
  const int ret = dt_restore_process_tiled(
    ctx, crop_4ch, crop_w, crop_h, pd->scale,
    _buf_row_writer, &bwd, NULL);
  g_free(crop_4ch);
  dt_restore_unref(ctx);

  if(ret != 0)
  {
    dt_print(DT_DEBUG_AI, "[neural_restore] preview: inference failed");
    g_free(out_4ch);
    g_free(crop_rgb);
    goto cleanup;
  }

  // build "before" buffer: pw × ph interleaved RGB
  float *before_buf = NULL;
  if(pd->scale > 1)
  {
    before_buf = g_malloc((size_t)pw * ph * 3 * sizeof(float));
    _nn_upscale(crop_rgb, crop_w, crop_h, before_buf, pw, ph);
    g_free(crop_rgb);
  }
  else
  {
    before_buf = crop_rgb;
  }

  // build "after" buffer: extract RGB from RGBx output
  float *after_buf = g_malloc((size_t)pw * ph * 3 * sizeof(float));
  for(int y = 0; y < ph; y++)
  {
    for(int x = 0; x < pw; x++)
    {
      const size_t si = ((size_t)y * pw + x) * 4;
      const size_t di = ((size_t)y * pw + x) * 3;
      after_buf[di + 0] = out_4ch[si + 0];
      after_buf[di + 1] = out_4ch[si + 1];
      after_buf[di + 2] = out_4ch[si + 2];
    }
  }
  g_free(out_4ch);

  // deliver result to main thread
  dt_neural_preview_result_t *result = g_new(dt_neural_preview_result_t, 1);
  result->self = pd->self;
  result->before = before_buf;
  result->after = after_buf;
  // only pass export pixels on fresh export (owned by cap).
  // on reuse, the main thread already has them cached
  result->export_pixels = owns_pixels ? cap.pixels : NULL;
  result->export_w = pixels_w;
  result->export_h = pixels_h;
  result->sequence = pd->sequence;
  result->width = pw;
  result->height = ph;
  result->task = pd->task;
  result->imgid = pd->imgid;
  result->patch_center[0] = pd->patch_center[0];
  result->patch_center[1] = pd->patch_center[1];
  g_idle_add(_preview_result_idle, result);
  g_free(pd);
  return NULL;

cleanup:
  // bail: clear preview_generating on UI thread (stale-sequence bails dropped)
  if(pd->sequence == g_atomic_int_get(&d->preview_sequence))
    _schedule_preview_failed(pd->self, err);
  g_free(pd);
  return NULL;
}

static void _cancel_preview(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_ready = FALSE;
  d->preview_generating = FALSE;
  // bump sequence so any in-flight worker (and its idle callback)
  // discards its result. we DO NOT join here — that would block the
  // UI for the full duration of a running inference. the worker
  // keeps running in the background and exits silently. but: we DO
  // need to wait for it before freeing export_pixels below, since the
  // worker may still be reading from them. take + release the
  // inference lock as a synchronisation barrier — the worker holds
  // it during the heavy work, so once we get it we know it's done
  // touching shared buffers
  g_atomic_int_inc(&d->preview_sequence);
  if(d->preview_trigger_timer)
  {
    g_source_remove(d->preview_trigger_timer);
    d->preview_trigger_timer = 0;
  }
  if(d->preview_thread)
  {
    g_mutex_lock(&d->preview_inference_lock);
    g_mutex_unlock(&d->preview_inference_lock);
    g_thread_unref(d->preview_thread);
    d->preview_thread = NULL;
  }
  // invalidate cached export (image changed)
  g_free(d->export_pixels);
  d->export_pixels = NULL;
  g_free(d->export_cairo);
  d->export_cairo = NULL;
  d->picking_thumbnail = FALSE;
  gtk_widget_queue_draw(d->preview_area);
}

// ============================================================================
// raw denoise preview path. parallel to the export-based preview above:
//   * pixel source: full-resolution CFA (Bayer) or demosaicked lin_rec2020
//     (X-Trans / linear), cached per image so re-picks reuse it
//   * inference: dt_restore_raw_{bayer,linear}_preview returns 3ch
//     source + 3ch denoised crops (both lin_rec2020) for the displayed
//     region, using one fixed-size tile that matches the JIT-compiled
//     batch session
//   * strength slider: re-blends the cached src/denoised on the UI thread
//     (microseconds) without touching the model — debounced to 50 ms so
//     fast drags don't queue up redraws
// ============================================================================

#define RAW_PREVIEW_STRENGTH_DEBOUNCE_MS 50

typedef struct dt_neural_preview_result_raw_t
{
  dt_lib_module_t *self;
  float *src_rgb;       // crop_w * crop_h * 3, lin_rec2020
  float *denoised_rgb;  // same dims, lin_rec2020 (gain-matched)
  int width;
  int height;
  int sequence;
  // optional: full-image buffers to install into the cache (NULL when
  // the worker reused an already-cached buffer for this image).
  // ownership transfers to d on idle install
  float *take_full_cfa;       // Bayer; allocated with g_malloc
  float *take_full_lin;       // X-Trans/linear; allocated with dt_alloc_align
  int    full_w;
  int    full_h;
  dt_imgid_t full_imgid;
  dt_restore_sensor_class_t full_sensor_class;
  // optional: picker thumbnail (4ch interleaved float) produced via
  // dt_imageio_export_with_flags. matches whatever the user sees in
  // darkroom — identical colours to the preview's before/after. NULL
  // when we reused an already-cached export_pixels on d.
  float *take_export_pixels;
  int    export_thumb_w;
  int    export_thumb_h;
  // cache key (see comment on dt_neural_preview_result_t)
  float patch_center[2];
} dt_neural_preview_result_raw_t;

// blend cached src + denoised crops at the given strength, write into
// preview_before/after (allocating fresh buffers), rebuild cairo, and
// queue a redraw. UI thread only.
static void _blend_raw_into_preview(dt_lib_neural_restore_t *d,
                                    float strength)
{
  if(!d->preview_raw_src_rgb || !d->preview_raw_denoised_rgb) return;
  const int w = d->preview_raw_crop_w;
  const int h = d->preview_raw_crop_h;
  if(w <= 0 || h <= 0) return;

  if(strength < 0.0f) strength = 0.0f;
  if(strength > 1.0f) strength = 1.0f;
  const float a = strength;
  const float ia = 1.0f - strength;

  const size_t n3 = (size_t)w * h * 3;

  // preview_before stays at the source (split widget shows pre-denoise
  // on one side, blended-strength on the other)
  g_free(d->preview_before);
  d->preview_before = g_malloc(n3 * sizeof(float));
  memcpy(d->preview_before, d->preview_raw_src_rgb, n3 * sizeof(float));

  // preview_after = α · denoised + (1-α) · source, per channel
  g_free(d->preview_after);
  d->preview_after = g_malloc(n3 * sizeof(float));
  for(size_t i = 0; i < n3; i++)
    d->preview_after[i]
      = a  * d->preview_raw_denoised_rgb[i]
      + ia * d->preview_raw_src_rgb[i];

  // detail-recovery DWT does not apply to raw (different pipeline
  // position; would need its own analysis pass). leave NULL so
  // _rebuild_cairo_after takes the no-recovery branch.
  dt_free_align(d->preview_detail);
  d->preview_detail = NULL;

  d->preview_w = w;
  d->preview_h = h;

  // rebuild cached cairo surfaces
  g_free(d->cairo_before);
  g_free(d->cairo_after);
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
  d->cairo_before = g_malloc(stride * h);
  d->cairo_after = g_malloc(stride * h);
  d->cairo_stride = stride;
  _float_rgb_to_cairo(d->preview_before, d->cairo_before, w, h, stride);
  _rebuild_cairo_after(d);

  d->preview_ready = TRUE;
  gtk_widget_queue_draw(d->preview_area);
}

// install a cached preview slot into the active preview buffers and
// rebuild cairo so the widget displays it. dispatches by task: raw
// denoise needs to repopulate preview_raw_src/denoised_rgb and re-blend
// at the current strength; RGB denoise / upscale just install
// preview_before/after/detail and rebuild the after surface
static void _install_cache_slot_raw(dt_lib_module_t *self,
                                    dt_neural_task_t task)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  const __typeof__(d->preview_cache[0]) *e = &d->preview_cache[task];
  if(!e->valid) return;
  const size_t n3 = (size_t)e->crop_w * e->crop_h * 3;
  g_free(d->preview_raw_src_rgb);
  g_free(d->preview_raw_denoised_rgb);
  d->preview_raw_src_rgb = g_malloc(n3 * sizeof(float));
  d->preview_raw_denoised_rgb = g_malloc(n3 * sizeof(float));
  memcpy(d->preview_raw_src_rgb, e->before_rgb, n3 * sizeof(float));
  memcpy(d->preview_raw_denoised_rgb, e->after_rgb, n3 * sizeof(float));
  d->preview_raw_crop_w = e->crop_w;
  d->preview_raw_crop_h = e->crop_h;
  const float strength
    = dt_bauhaus_slider_get(d->raw_strength_slider) / 100.0f;
  _blend_raw_into_preview(d, strength);
}

static void _install_cache_slot_rgb(dt_lib_module_t *self,
                                    dt_neural_task_t task)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  const __typeof__(d->preview_cache[0]) *e = &d->preview_cache[task];
  if(!e->valid) return;
  const size_t n3 = (size_t)e->crop_w * e->crop_h * 3;
  g_free(d->preview_before);
  g_free(d->preview_after);
  dt_free_align(d->preview_detail);
  d->preview_before = g_malloc(n3 * sizeof(float));
  d->preview_after  = g_malloc(n3 * sizeof(float));
  memcpy(d->preview_before, e->before_rgb, n3 * sizeof(float));
  memcpy(d->preview_after,  e->after_rgb,  n3 * sizeof(float));
  d->preview_detail = NULL;
  if(e->detail)
  {
    const size_t n1 = (size_t)e->crop_w * e->crop_h;
    d->preview_detail = dt_alloc_align_float(n1);
    if(d->preview_detail)
      memcpy(d->preview_detail, e->detail, n1 * sizeof(float));
  }
  d->preview_w = e->crop_w;
  d->preview_h = e->crop_h;
  // rebuild cairo surfaces
  g_free(d->cairo_before);
  g_free(d->cairo_after);
  const int stride
    = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, e->crop_w);
  d->cairo_before = g_malloc(stride * e->crop_h);
  d->cairo_after  = g_malloc(stride * e->crop_h);
  d->cairo_stride = stride;
  _float_rgb_to_cairo(d->preview_before, d->cairo_before,
                      e->crop_w, e->crop_h, stride);
  _rebuild_cairo_after(d);
  d->preview_ready = TRUE;
  gtk_widget_queue_draw(d->preview_area);
}

static void _install_cache_slot(dt_lib_module_t *self, dt_neural_task_t task)
{
  if(task == NEURAL_TASK_RAW_DENOISE) _install_cache_slot_raw(self, task);
  else                                _install_cache_slot_rgb(self, task);
}

// debounced strength-slider re-blend. returns G_SOURCE_REMOVE so the
// timer fires once.
static gboolean _strength_blend_timer_cb(gpointer data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_strength_timer = 0;

  if(d->task != NEURAL_TASK_RAW_DENOISE) return G_SOURCE_REMOVE;
  if(!d->preview_raw_src_rgb || !d->preview_raw_denoised_rgb)
    return G_SOURCE_REMOVE;

  const float strength
    = dt_bauhaus_slider_get(d->raw_strength_slider) / 100.0f;
  _blend_raw_into_preview(d, strength);
  return G_SOURCE_REMOVE;
}

static void _schedule_raw_strength_reblend(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  if(d->preview_strength_timer)
    g_source_remove(d->preview_strength_timer);
  d->preview_strength_timer
    = g_timeout_add(RAW_PREVIEW_STRENGTH_DEBOUNCE_MS,
                    _strength_blend_timer_cb, self);
}

// fired when a preview worker bails: clears preview_generating and
// records the bail reason so the placeholder shows it
typedef struct
{
  dt_lib_module_t *self;
  dt_nr_preview_err_t err;
} _preview_failed_data_t;

static gboolean _preview_failed_idle(gpointer data)
{
  _preview_failed_data_t *fd = (_preview_failed_data_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)fd->self->data;
  d->preview_generating = FALSE;
  d->preview_error = fd->err;
  _update_button_sensitivity(d);
  gtk_widget_queue_draw(d->preview_area);
  g_free(fd);
  return G_SOURCE_REMOVE;
}

static void _schedule_preview_failed(dt_lib_module_t *self,
                                     dt_nr_preview_err_t err)
{
  _preview_failed_data_t *fd = g_new0(_preview_failed_data_t, 1);
  fd->self = self;
  fd->err = err;
  g_idle_add(_preview_failed_idle, fd);
}

static gboolean _preview_raw_result_idle(gpointer data)
{
  dt_neural_preview_result_raw_t *res
    = (dt_neural_preview_result_raw_t *)data;
  dt_lib_neural_restore_t *d
    = (dt_lib_neural_restore_t *)res->self->data;

  // discard stale results
  if(res->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    g_free(res->src_rgb);
    g_free(res->denoised_rgb);
    g_free(res->take_full_cfa);
    dt_free_align(res->take_full_lin);
    g_free(res->take_export_pixels);
    g_free(res);
    return G_SOURCE_REMOVE;
  }

  // install the per-image full buffer if the worker freshly loaded one.
  // also (re)build the patch-picker thumbnail (export_pixels) from it —
  // the raw path doesn't run a pipeline export so the picker needs us
  // to synthesise a whole-image 4ch RGBA buffer.
  gboolean refresh_thumbnail = FALSE;
  if(res->take_full_cfa)
  {
    g_free(d->preview_full_cfa);
    d->preview_full_cfa = res->take_full_cfa;
    d->preview_full_w = res->full_w;
    d->preview_full_h = res->full_h;
    d->preview_raw_imgid = res->full_imgid;
    d->preview_raw_sensor_class = res->full_sensor_class;
    // free the other variant's cache (we switched sensor type)
    dt_free_align(d->preview_full_lin);
    d->preview_full_lin = NULL;
    refresh_thumbnail = TRUE;
  }
  if(res->take_full_lin)
  {
    dt_free_align(d->preview_full_lin);
    d->preview_full_lin = res->take_full_lin;
    d->preview_lin_w = res->full_w;
    d->preview_lin_h = res->full_h;
    d->preview_raw_imgid = res->full_imgid;
    d->preview_raw_sensor_class = res->full_sensor_class;
    g_free(d->preview_full_cfa);
    d->preview_full_cfa = NULL;
    refresh_thumbnail = TRUE;
  }

  // install the picker thumbnail when the worker produced a fresh
  // export (triggered by new imgid / sensor-type change). matches
  // exactly what the user sees in darkroom — same pipeline the RGB
  // denoise preview uses for its picker thumbnail.
  if(res->take_export_pixels)
  {
    g_free(d->export_pixels);
    g_free(d->export_cairo);
    d->export_pixels = res->take_export_pixels;
    d->export_w = res->export_thumb_w;
    d->export_h = res->export_thumb_h;
    d->export_cairo = NULL;  // rebuilt on demand by picker
    res->take_export_pixels = NULL;
  }
  (void)refresh_thumbnail;  // legacy flag; export is handled above

  // install per-refresh inference output
  g_free(d->preview_raw_src_rgb);
  g_free(d->preview_raw_denoised_rgb);
  d->preview_raw_src_rgb = res->src_rgb;
  d->preview_raw_denoised_rgb = res->denoised_rgb;
  d->preview_raw_crop_w = res->width;
  d->preview_raw_crop_h = res->height;

  const float strength
    = dt_bauhaus_slider_get(d->raw_strength_slider) / 100.0f;
  _blend_raw_into_preview(d, strength);

  // preview_raw_sensor_class is now authoritative for this imgid;
  // refresh the overlay so it shows the correct DNG output format
  _update_info_label(d);

  // store unblended source / denoised in the cache. raw never has DWT
  // detail; the strength slider blends src ↔ denoised on the fly via
  // _blend_raw_into_preview, so the cache only needs the two anchors
  _preview_cache_store(d, NEURAL_TASK_RAW_DENOISE, res->full_imgid,
                       res->patch_center,
                       res->width, res->height,
                       d->preview_raw_src_rgb, d->preview_raw_denoised_rgb,
                       NULL);

  d->preview_generating = FALSE;
  _update_button_sensitivity(d);
  g_free(res);
  return G_SOURCE_REMOVE;
}

static gpointer _preview_thread_raw(gpointer data)
{
  dt_neural_preview_data_t *pd = (dt_neural_preview_data_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)pd->self->data;

  // bail reason for cleanup path; unsupported-sensor branch overrides
  dt_nr_preview_err_t bail_err = DT_NR_PREVIEW_ERR_INIT_FAILED;

  // 1. load source image metadata to determine sensor type.
  //    on a fresh session dt_image_cache_get returns img_meta with a
  //    zeroed buf_dsc until rawspeed has been invoked on this id. the
  //    batch path (_process_raw_denoise_one) does this same warmup
  //    BEFORE reading metadata for the same reason.
  if(pd->sequence != g_atomic_int_get(&d->preview_sequence)) goto cleanup;

  dt_mipmap_buffer_t warmup;
  dt_mipmap_cache_get(&warmup, pd->imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  const gboolean warm_loaded = (warmup.buf != NULL);
  dt_mipmap_cache_release(&warmup);
  if(!warm_loaded)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] raw preview: mipmap warmup failed for imgid %d",
             pd->imgid);
    goto cleanup;
  }

  const dt_image_t *cached = dt_image_cache_get(pd->imgid, 'r');
  if(!cached) goto cleanup;
  dt_image_t img_meta = *cached;
  dt_image_cache_read_release(cached);

  const uint32_t filters = img_meta.buf_dsc.filters;
  const dt_restore_sensor_class_t cls = dt_restore_classify_sensor(&img_meta);
  const gboolean is_xtrans = (cls == DT_RESTORE_SENSOR_CLASS_XTRANS);
  if(cls != DT_RESTORE_SENSOR_CLASS_BAYER
     && cls != DT_RESTORE_SENSOR_CLASS_XTRANS)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] raw preview: imgid %d is not bayer/xtrans "
             "(filters=0x%x class=%d)",
             pd->imgid, filters, cls);
    bail_err = DT_NR_PREVIEW_ERR_UNSUPPORTED;
    goto cleanup;
  }
  dt_print(DT_DEBUG_AI,
           "[neural_restore] raw preview: imgid=%d %s patch=(%.3f,%.3f) "
           "widget=%dx%d filters=0x%x",
           pd->imgid, is_xtrans ? "x-trans" : "bayer",
           pd->patch_center[0], pd->patch_center[1],
           pd->preview_w, pd->preview_h, filters);

  // 2. ensure the right ctx is loaded (matches batch logic in
  //    _ensure_raw_ctx). reload if cached_task is wrong or if the
  //    cached sensor class doesn't match this image.
  dt_restore_context_t *ctx = NULL;
  dt_pthread_mutex_lock(&d->ctx_lock);
  {
    const gboolean cached_is_raw_correct
      = d->cached_ctx
        && d->cached_task == NEURAL_TASK_RAW_DENOISE
        && (cls == d->preview_raw_sensor_class);
    if(!cached_is_raw_correct)
    {
      dt_restore_unref(d->cached_ctx);
      switch(cls)
      {
        case DT_RESTORE_SENSOR_CLASS_BAYER:
          d->cached_ctx = dt_restore_load_rawdenoise_bayer(pd->env);
          break;
        case DT_RESTORE_SENSOR_CLASS_XTRANS:
          d->cached_ctx = dt_restore_load_rawdenoise_xtrans(pd->env);
          break;
        default:
          d->cached_ctx = NULL;
          break;
      }
      d->cached_task = NEURAL_TASK_RAW_DENOISE;
      // mark cached sensor class so a follow-up preview matches; this
      // does NOT update preview_raw_imgid because we may not have a
      // fresh full-image buffer for this image yet
      d->preview_raw_sensor_class = cls;
    }
    if(d->cached_ctx) ctx = dt_restore_ref(d->cached_ctx);
  }
  dt_pthread_mutex_unlock(&d->ctx_lock);

  if(!ctx)
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] raw preview: failed to load model");
    goto cleanup;
  }
  if(pd->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    dt_restore_unref(ctx);
    goto cleanup;
  }

  // 3. acquire the full-image buffer. reuse cache if it matches imgid +
  //    sensor type; otherwise load fresh and stage into result for the
  //    UI to install on idle.
  float *take_full_cfa = NULL;
  float *take_full_lin = NULL;
  int full_w = 0, full_h = 0;
  const float *full_cfa_use = NULL;  // borrowed pointer (cache or take_*)
  const float *full_lin_use = NULL;

  const gboolean cache_matches
    = d->preview_raw_imgid == pd->imgid
      && d->preview_raw_sensor_class == cls
      && ((is_xtrans && d->preview_full_lin)
          || (!is_xtrans && d->preview_full_cfa));

  if(cache_matches)
  {
    if(is_xtrans)
    {
      full_lin_use = d->preview_full_lin;
      full_w = d->preview_lin_w;
      full_h = d->preview_lin_h;
    }
    else
    {
      full_cfa_use = d->preview_full_cfa;
      full_w = d->preview_full_w;
      full_h = d->preview_full_h;
    }
  }
  else if(is_xtrans)
  {
    if(dt_restore_raw_linear_prepare(ctx, pd->imgid, &take_full_lin,
                                     &full_w, &full_h) != 0
       || !take_full_lin)
    {
      dt_restore_unref(ctx);
      goto cleanup;
    }
    full_lin_use = take_full_lin;
  }
  else
  {
    // Bayer: read CFA from mipmap cache
    dt_mipmap_buffer_t mbuf;
    dt_mipmap_cache_get(&mbuf, pd->imgid, DT_MIPMAP_FULL,
                        DT_MIPMAP_BLOCKING, 'r');
    if(!mbuf.buf)
    {
      dt_mipmap_cache_release(&mbuf);
      dt_restore_unref(ctx);
      goto cleanup;
    }
    full_w = img_meta.width;
    full_h = img_meta.height;
    const size_t npix = (size_t)full_w * full_h;
    take_full_cfa = g_try_malloc(npix * sizeof(float));
    if(!take_full_cfa)
    {
      dt_mipmap_cache_release(&mbuf);
      dt_restore_unref(ctx);
      goto cleanup;
    }
    if(img_meta.buf_dsc.datatype == TYPE_UINT16)
    {
      const uint16_t *src = (const uint16_t *)mbuf.buf;
      for(size_t i = 0; i < npix; i++) take_full_cfa[i] = (float)src[i];
    }
    else if(img_meta.buf_dsc.datatype == TYPE_FLOAT)
    {
      memcpy(take_full_cfa, mbuf.buf, npix * sizeof(float));
    }
    else
    {
      dt_mipmap_cache_release(&mbuf);
      g_free(take_full_cfa);
      dt_restore_unref(ctx);
      goto cleanup;
    }
    dt_mipmap_cache_release(&mbuf);
    full_cfa_use = take_full_cfa;
  }

  if(pd->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    g_free(take_full_cfa);
    dt_free_align(take_full_lin);
    dt_restore_unref(ctx);
    goto cleanup;
  }

  // 3.5. refresh the picker-thumbnail export when we loaded a fresh
  //      full buffer (i.e. imgid / sensor-type changed). match the RGB
  //      preview path: dt_imageio_export_with_flags runs the user's
  //      full pipeline at ~1024 long edge, giving a display-accurate
  //      thumbnail whose colours match what the user sees in darkroom
  //      (and match our before/after ROI pipe outputs).
  float *take_export_pixels = NULL;
  int    export_thumb_w = 0;
  int    export_thumb_h = 0;
  if(!cache_matches)
  {
    dt_neural_preview_capture_t cap = {0};
    const int export_size = dt_conf_get_int(CONF_PREVIEW_EXPORT_SIZE);
    cap.parent.max_width = export_size;
    cap.parent.max_height = export_size;

    dt_imageio_module_format_t fmt = {
      .mime = _ai_get_mime,
      .levels = _ai_check_levels,
      .bpp = _ai_check_bpp,
      .write_image = _preview_capture_write_image};

    const dt_colorspaces_color_profile_type_t cfg_type
      = dt_conf_key_exists(CONF_ICC_TYPE)
        ? dt_conf_get_int(CONF_ICC_TYPE)
        : DT_COLORSPACE_NONE;
    gchar *cfg_file = (cfg_type == DT_COLORSPACE_FILE)
      ? dt_conf_get_string(CONF_ICC_FILE)
      : NULL;
    dt_imageio_export_with_flags(
      pd->imgid, "unused", &fmt,
      (dt_imageio_module_data_t *)&cap,
      TRUE,   // ignore_exif
      FALSE,  // display_byteorder
      TRUE,   // high_quality
      FALSE,  // upscale
      FALSE,  // is_scaling
      1.0,    // scale_factor
      FALSE,  // thumbnail_export
      NULL,   // filter
      FALSE,  // copy_metadata
      FALSE,  // export_masks
      (cfg_type == DT_COLORSPACE_NONE)
        ? dt_colorspaces_get_work_profile(pd->imgid)->type
        : cfg_type,
      cfg_file,
      DT_INTENT_PERCEPTUAL,
      NULL, NULL, 1, 1, NULL, -1);
    g_free(cfg_file);

    if(cap.pixels && cap.cap_w > 0 && cap.cap_h > 0)
    {
      take_export_pixels = cap.pixels;
      export_thumb_w = cap.cap_w;
      export_thumb_h = cap.cap_h;
    }
  }

  // 4. compute crop region. widget dims define the "100% preview" size,
  //    capped by the model's compiled tile size minus mandatory overlap.
  const int T = dt_restore_get_tile_size(ctx);
  // Bayer model upscales 2x; linear is 1:1. so the maximum displayed
  // crop in sensor pixels:
  //   bayer:  2*T - 4*overlap_packed = 2*T - 128  (for OVERLAP_PACKED=32)
  //   linear: T   - 2*overlap_linear = T   - 64   (for OVERLAP_LINEAR=32)
  const int max_disp = is_xtrans ? (T - 64) : (2 * T - 128);

  // the raw buffer is always landscape (sensor layout), but the preview
  // thumbnail the user clicks on is oriented per EXIF. un-rotate the
  // widget dims + click position into sensor coords before picking the
  // crop, otherwise portrait images end up sampling the wrong area
  const dt_image_orientation_t ori = dt_image_orientation(&img_meta);
  const gboolean swap_xy = (ori & ORIENTATION_SWAP_XY) != 0;

  int crop_w = MIN(swap_xy ? pd->preview_h : pd->preview_w, max_disp);
  int crop_h = MIN(swap_xy ? pd->preview_w : pd->preview_h, max_disp);
  // Bayer: snap to mod 2 (CFA grid)
  if(!is_xtrans)
  {
    crop_w = (crop_w / 2) * 2;
    crop_h = (crop_h / 2) * 2;
  }
  if(crop_w <= 0 || crop_h <= 0)
  {
    g_free(take_full_cfa);
    dt_free_align(take_full_lin);
    dt_restore_unref(ctx);
    goto cleanup;
  }

  // display-normalised click (u, v) -> sensor pixel, inverting whatever
  // combination of swap/flip the flip iop will apply during display.
  // matches dt_iop_flip:distort_backtransform semantics
  const int disp_w = swap_xy ? full_h : full_w;
  const int disp_h = swap_xy ? full_w : full_h;
  float dx_disp = pd->patch_center[0] * disp_w;
  float dy_disp = pd->patch_center[1] * disp_h;
  float sx, sy;
  if(swap_xy) { sx = dy_disp; sy = dx_disp; }
  else        { sx = dx_disp; sy = dy_disp; }
  if(ori & ORIENTATION_FLIP_X) sx = (float)full_w - sx;
  if(ori & ORIENTATION_FLIP_Y) sy = (float)full_h - sy;

  int crop_x = (int)sx - crop_w / 2;
  int crop_y = (int)sy - crop_h / 2;
  crop_x = CLAMP(crop_x, 0, full_w - crop_w);
  crop_y = CLAMP(crop_y, 0, full_h - crop_h);
  if(!is_xtrans)
  {
    crop_x = (crop_x / 2) * 2;
    crop_y = (crop_y / 2) * 2;
  }

  dt_print(DT_DEBUG_AI,
           "[neural_restore] raw preview: full=%dx%d ori=0x%x "
           "patch_center=(%.3f,%.3f) -> sensor=(%d,%d %dx%d) %s",
           full_w, full_h, (unsigned)ori,
           pd->patch_center[0], pd->patch_center[1],
           crop_x, crop_y, crop_w, crop_h,
           is_xtrans ? "linear" : "bayer");

  // 5. inference
  // Bayer path uses the _piped variant which runs darktable's full
  // pixelpipe on both the original CFA and a denoised-patched CFA, so
  // "before"/"after" match what the user would see after Process +
  // re-import (same history stack, same filmic/tone curve, same output
  // profile). Slower (~2-5 s for two pipes) but colour-accurate.
  // Linear path still uses the simpler in-space blend for now.
  float *src_rgb = NULL;
  float *denoised_rgb = NULL;
  int actual_w = 0, actual_h = 0;
  int err;
  if(is_xtrans)
    err = dt_restore_raw_linear_preview_piped(ctx, &img_meta, pd->imgid,
                                              full_lin_use,
                                              full_w, full_h,
                                              crop_x, crop_y,
                                              crop_w, crop_h,
                                              &src_rgb, &denoised_rgb,
                                              &actual_w, &actual_h);
  else
    err = dt_restore_raw_bayer_preview_piped(ctx, &img_meta, pd->imgid,
                                             full_cfa_use,
                                             full_w, full_h,
                                             crop_x, crop_y,
                                             crop_w, crop_h,
                                             &src_rgb, &denoised_rgb,
                                             &actual_w, &actual_h);

  dt_restore_unref(ctx);

  dt_print(DT_DEBUG_AI,
           "[neural_restore] raw preview: inference returned err=%d "
           "src=%p denoised=%p requested=%dx%d actual=%dx%d",
           err, (void *)src_rgb, (void *)denoised_rgb,
           crop_w, crop_h, actual_w, actual_h);

  if(err || !src_rgb || !denoised_rgb || actual_w <= 0 || actual_h <= 0)
  {
    g_free(src_rgb);
    g_free(denoised_rgb);
    g_free(take_full_cfa);
    dt_free_align(take_full_lin);
    goto cleanup;
  }

  // 6. ship to UI thread. width/height carry the ACTUAL rendered dims
  // from the pipe, which can be smaller than crop_w/crop_h when the
  // user's history includes geometry-modifying modules (clipping,
  // ashift, lens). downstream blend + cairo render must use these.
  dt_neural_preview_result_raw_t *res
    = g_new0(dt_neural_preview_result_raw_t, 1);
  res->self = pd->self;
  res->src_rgb = src_rgb;
  res->denoised_rgb = denoised_rgb;
  res->width = actual_w;
  res->height = actual_h;
  res->sequence = pd->sequence;
  res->take_full_cfa = take_full_cfa;
  res->take_full_lin = take_full_lin;
  res->full_w = full_w;
  res->full_h = full_h;
  res->full_imgid = pd->imgid;
  res->full_sensor_class = cls;
  res->patch_center[0] = pd->patch_center[0];
  res->patch_center[1] = pd->patch_center[1];
  res->take_export_pixels = take_export_pixels;
  res->export_thumb_w = export_thumb_w;
  res->export_thumb_h = export_thumb_h;
  g_idle_add(_preview_raw_result_idle, res);
  g_free(pd);
  return NULL;

cleanup:
  // bail: clear preview_generating on UI thread (stale-sequence bails dropped)
  if(pd->sequence == g_atomic_int_get(&d->preview_sequence))
    _schedule_preview_failed(pd->self, bail_err);
  g_free(pd);
  return NULL;
}

// thread dispatcher: serialises the actual inference / pipe work via
// preview_inference_lock so that even when an old worker is still
// running, the new one queues up rather than fighting for the GPU.
// also re-checks the sequence after acquiring the lock — if the
// trigger that spawned us has already been superseded while we were
// waiting, drop on the floor without doing anything expensive
static gpointer _preview_thread(gpointer data);
static gpointer _preview_thread_raw(gpointer data);
static gpointer _preview_thread_dispatch(gpointer data)
{
  dt_neural_preview_data_t *pd = (dt_neural_preview_data_t *)data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)pd->self->data;

  g_mutex_lock(&d->preview_inference_lock);

  if(pd->sequence != g_atomic_int_get(&d->preview_sequence))
  {
    g_mutex_unlock(&d->preview_inference_lock);
    g_free(pd);
    return NULL;
  }

  gpointer res = (pd->task == NEURAL_TASK_RAW_DENOISE)
    ? _preview_thread_raw(data)
    : _preview_thread(data);

  g_mutex_unlock(&d->preview_inference_lock);
  return res;
}

// debounced trigger: rapid tab switches collapse to one preview run.
// the timer handle in d->preview_trigger_timer is replaced (and the
// previous one removed) so the trigger only fires after the user
// settles on a tab for `delay_ms`
static gboolean _trigger_preview_from_timer(gpointer user_data);
static void _trigger_preview(dt_lib_module_t *self);

static void _schedule_preview_refresh(dt_lib_module_t *self, guint delay_ms)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  if(!d->model_available || !d->preview_requested) return;
  if(d->preview_trigger_timer)
    g_source_remove(d->preview_trigger_timer);
  d->preview_trigger_timer
    = g_timeout_add(delay_ms, _trigger_preview_from_timer, self);
}

static gboolean _trigger_preview_from_timer(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_trigger_timer = 0;
  _trigger_preview(self);
  return G_SOURCE_REMOVE;
}

static void _trigger_preview(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  if(!d->model_available || !d->preview_requested)
    return;

  // invalidate current preview and bump sequence so running thread exits early
  d->preview_ready = FALSE;
  d->preview_error = DT_NR_PREVIEW_ERR_NONE;
  g_atomic_int_inc(&d->preview_sequence);
  gtk_widget_queue_draw(d->preview_area);

  GList *imgs = dt_act_on_get_images(TRUE, FALSE, FALSE);
  if(!imgs) return;
  dt_imgid_t imgid = GPOINTER_TO_INT(imgs->data);
  g_list_free(imgs);

  if(!dt_is_valid_imgid(imgid)) return;

  // per-task cache lookup: if we already have a result for this exact
  // (task, imgid, patch_center) tuple, install it and skip the worker
  if(_preview_cache_hit(d, d->task, imgid))
  {
    dt_print(DT_DEBUG_AI,
             "[neural_restore] preview cache hit for %s",
             _task_log_name(d->task));
    _install_cache_slot(self, d->task);
    d->preview_generating = FALSE;
    _update_button_sensitivity(d);
    // cache hit means preview_raw_sensor_class was set by an earlier
    // worker run on this imgid — refresh the overlay so the DNG
    // output label appears when _task_changed cleared it on tab switch
    _update_info_label(d);
    return;
  }

  // compute preview dimensions matching widget aspect ratio
  const int widget_w = gtk_widget_get_allocated_width(d->preview_area);
  const int widget_h = gtk_widget_get_allocated_height(d->preview_area);
  if(widget_w <= 0 || widget_h <= 0)
    return;

  const int scale = _task_scale(d->task);
  // use widget dimensions directly for 1:1 pixel mapping
  int pw = (widget_w / scale) * scale;
  int ph = (widget_h / scale) * scale;
  if(pw < scale || ph < scale)
    return;

  d->preview_generating = TRUE;

  dt_neural_preview_data_t *pd = g_new0(dt_neural_preview_data_t, 1);
  pd->self = self;
  pd->imgid = imgid;
  pd->task = d->task;
  pd->scale = scale;
  pd->env = d->env;
  pd->sequence = g_atomic_int_get(&d->preview_sequence);
  pd->preview_w = pw;
  pd->preview_h = ph;
  pd->patch_center[0] = d->patch_center[0];
  pd->patch_center[1] = d->patch_center[1];

  // borrow cached export pixels if available (re-pick scenario).
  // the pointer is valid for the thread's lifetime because
  // _cancel_preview joins before freeing export_pixels
  if(d->export_pixels)
  {
    pd->reuse_pixels = d->export_pixels;
    pd->reuse_w = d->export_w;
    pd->reuse_h = d->export_h;
  }
  // detach the previous worker (don't join — that would block the
  // UI thread for the duration of the in-flight inference / pipe
  // call). preview_inference_lock serialises the actual heavy work,
  // and the bumped sequence + per-task cache lookup at the new
  // worker's entry guarantees we don't run two inferences for the
  // same target. gui_cleanup joins the latest worker for shutdown.
  if(d->preview_thread)
  {
    g_thread_unref(d->preview_thread);
    d->preview_thread = NULL;
  }
  d->preview_thread = g_thread_new("neural_preview",
                                   _preview_thread_dispatch, pd);
}

// map notebook page index to task. pages are ordered in the notebook as:
//   0 = raw denoise, 1 = denoise, 2 = upscale (with scale_combo picking 2x/4x)
static dt_neural_task_t _task_from_page(dt_lib_neural_restore_t *d, int page)
{
  switch(page)
  {
    case 0: return NEURAL_TASK_RAW_DENOISE;
    case 1: return NEURAL_TASK_DENOISE;
    default:
    {
      const int scale_pos = dt_bauhaus_combobox_get(d->scale_combo);
      return (scale_pos == 1) ? NEURAL_TASK_UPSCALE_4X : NEURAL_TASK_UPSCALE_2X;
    }
  }
}

static void _update_task_from_ui(dt_lib_neural_restore_t *d)
{
  d->task = _task_from_page(d, gtk_notebook_get_current_page(d->notebook));
}

static void _notebook_page_changed(GtkNotebook *notebook,
                                   GtkWidget *page,
                                   guint page_num,
                                   dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  // switch-page fires before the page changes, so use page_num
  d->task = _task_from_page(d, page_num);

  dt_conf_set_int(CONF_ACTIVE_PAGE, page_num);
  _task_changed(d);
  // debounced — rapid tab cycling won't pile up worker threads
  if(d->preview_requested)
    _schedule_preview_refresh(self, 150);
}

static void _scale_combo_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  _update_task_from_ui(d);
  _task_changed(d);
  if(d->preview_requested)
    _schedule_preview_refresh(self, 150);
}

static void _recovery_slider_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  if(d->recovery_changing) return;
  dt_conf_set_float(CONF_STRENGTH, dt_bauhaus_slider_get(d->recovery_slider));
  if(d->preview_ready)
  {
    _rebuild_cairo_after(d);
    gtk_widget_queue_draw(d->preview_area);
  }
}

static void _raw_strength_slider_changed(GtkWidget *widget,
                                         dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  dt_conf_set_float(CONF_RAW_STRENGTH,
                    dt_bauhaus_slider_get(d->raw_strength_slider));

  // live preview re-blend (debounced). only fires when raw denoise tab
  // is active and a preview is already cached — otherwise the model
  // hasn't run yet and there's nothing to blend.
  if(d->task == NEURAL_TASK_RAW_DENOISE
     && d->preview_raw_src_rgb
     && d->preview_raw_denoised_rgb)
    _schedule_raw_strength_reblend(self);
}

static void _process_clicked(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  if(!d->model_available || _any_selected_processing(d))
    return;

  GList *images = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!images)
    return;

  dt_neural_job_t *job_data = g_new0(dt_neural_job_t, 1);
  job_data->task = d->task;
  job_data->env = d->env;
  job_data->images = images;
  job_data->scale = _task_scale(d->task);
  job_data->strength = dt_conf_key_exists(CONF_STRENGTH)
    ? dt_conf_get_float(CONF_STRENGTH) : 100.0f;
  // raw denoise strength: 0..100 in UI, 0..1 for the pipeline
  job_data->raw_strength = dt_conf_key_exists(CONF_RAW_STRENGTH)
    ? dt_conf_get_float(CONF_RAW_STRENGTH) / 100.0f
    : 1.0f;
  job_data->bpp = dt_conf_key_exists(CONF_BIT_DEPTH)
    ? dt_conf_get_int(CONF_BIT_DEPTH)
    : NEURAL_BPP_16;
  job_data->add_to_catalog
    = dt_conf_key_exists(CONF_ADD_CATALOG)
      ? dt_conf_get_bool(CONF_ADD_CATALOG)
      : TRUE;
  char *out_dir = dt_conf_get_string(CONF_OUTPUT_DIR);
  job_data->output_dir
    = (out_dir && out_dir[0]) ? out_dir : NULL;
  if(!job_data->output_dir) g_free(out_dir);
  job_data->icc_type = dt_conf_key_exists(CONF_ICC_TYPE)
    ? dt_conf_get_int(CONF_ICC_TYPE)
    : DT_COLORSPACE_NONE;
  job_data->icc_filename = (job_data->icc_type == DT_COLORSPACE_FILE)
    ? dt_conf_get_string(CONF_ICC_FILE)
    : NULL;
  job_data->preserve_wide_gamut = dt_conf_key_exists(CONF_PRESERVE_WIDE_GAMUT)
    ? dt_conf_get_bool(CONF_PRESERVE_WIDE_GAMUT)
    : TRUE;
  job_data->self = self;

  // mark selected images as processing
  for(GList *l = images; l; l = g_list_next(l))
    g_hash_table_add(d->processing_images, l->data);
  _update_button_sensitivity(d);

  dt_job_t *job = dt_control_job_create(_process_job_run, "neural restore");
  dt_control_job_set_params(job, job_data, _job_cleanup);
  dt_control_job_add_progress(job, _("neural restore"), TRUE);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

// compute geometry for fitting the export image into the widget
static void _picking_geometry(const dt_lib_neural_restore_t *d,
                              const int w, const int h,
                              double *img_w, double *img_h,
                              double *ox, double *oy)
{
  const double sx = (double)w / d->export_w;
  const double sy = (double)h / d->export_h;
  const double scale = fmin(sx, sy);
  *img_w = d->export_w * scale;
  *img_h = d->export_h * scale;
  *ox = (w - *img_w) / 2.0;
  *oy = (h - *img_h) / 2.0;
}

// build cairo surface from cached export pixels (4ch linear float → sRGB 8-bit)
static void _build_export_cairo(dt_lib_neural_restore_t *d)
{
  g_free(d->export_cairo);
  d->export_cairo = NULL;

  if(!d->export_pixels || d->export_w <= 0 || d->export_h <= 0)
    return;

  const int ew = d->export_w;
  const int eh = d->export_h;
  const int stride
    = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, ew);
  d->export_cairo = g_malloc(stride * eh);
  d->export_cairo_stride = stride;

  for(int y = 0; y < eh; y++)
  {
    uint32_t *row = (uint32_t *)(d->export_cairo + y * stride);
    for(int x = 0; x < ew; x++)
    {
      const size_t si = ((size_t)y * ew + x) * 4;
      const uint8_t r
        = (uint8_t)(_linear_to_srgb(d->export_pixels[si + 0]) * 255.0f + 0.5f);
      const uint8_t g
        = (uint8_t)(_linear_to_srgb(d->export_pixels[si + 1]) * 255.0f + 0.5f);
      const uint8_t b
        = (uint8_t)(_linear_to_srgb(d->export_pixels[si + 2]) * 255.0f + 0.5f);
      row[x] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
}

static void _pick_toggled(GtkToggleButton *btn, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = self->data;
  const gboolean active = gtk_toggle_button_get_active(btn);

  if(active)
  {
    // use cached export image for area selection
    if(!d->export_pixels)
    {
      gtk_toggle_button_set_active(btn, FALSE);
      return;
    }
    if(!d->export_cairo)
      _build_export_cairo(d);
    if(!d->export_cairo)
    {
      gtk_toggle_button_set_active(btn, FALSE);
      return;
    }
    d->picking_thumbnail = TRUE;
  }
  else
  {
    d->picking_thumbnail = FALSE;
  }
  gtk_widget_queue_draw(d->preview_area);
}

static gboolean _pick_double_click(GtkWidget *widget,
                                   GdkEventButton *event,
                                   dt_lib_module_t *self)
{
  if(event->type != GDK_2BUTTON_PRESS) return FALSE;
  dt_lib_neural_restore_t *d = self->data;
  d->patch_center[0] = 0.5f;
  d->patch_center[1] = 0.5f;
  d->picking_thumbnail = FALSE;
  // free cairo cache (will be rebuilt), but keep export_pixels
  // so _trigger_preview can reuse them for the center crop
  g_free(d->export_cairo);
  d->export_cairo = NULL;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->pick_button), FALSE);
  d->preview_requested = TRUE;
  _trigger_preview(self);
  return TRUE;
}

static gboolean _preview_draw(GtkWidget *widget, cairo_t *cr, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  const int w = gtk_widget_get_allocated_width(widget);
  const int h = gtk_widget_get_allocated_height(widget);

  // background
  cairo_set_source_rgb(cr, 0.15, 0.15, 0.15);
  cairo_rectangle(cr, 0, 0, w, h);
  cairo_fill(cr);

  // thumbnail picking mode: show full image with crop rectangle
  if(d->picking_thumbnail && d->export_cairo)
  {
    cairo_surface_t *thumb_surf = cairo_image_surface_create_for_data(
      d->export_cairo, CAIRO_FORMAT_RGB24,
      d->export_w, d->export_h, d->export_cairo_stride);

    // fit thumbnail to widget
    double img_w, img_h, ox, oy;
    _picking_geometry(d, w, h, &img_w, &img_h, &ox, &oy);

    cairo_save(cr);
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, img_w / d->export_w, img_h / d->export_h);
    cairo_set_source_surface(cr, thumb_surf, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    // draw crop rectangle at current patch_center. the rectangle
    // represents the actual displayed patch, not the full tile size:
    //  * RGB denoise / upscale: the preview runs on the exported image
    //    at 1:1, so crop pixels == widget pixels, measured against the
    //    thumbnail's own resolution (export_w == source for this path)
    //  * raw denoise: preview_raw_crop_* carries the pipe's backbuf
    //    dims — already in display orientation (post-flip iop) — so we
    //    scale them against the display-oriented thumbnail (export_w /
    //    export_h), NOT the sensor buffer (preview_full_*). using the
    //    sensor dims would draw the wrong rectangle size on portrait
    //    images where sensor and display axes swap
    double rw, rh;
    if(d->task == NEURAL_TASK_RAW_DENOISE
       && d->preview_raw_crop_w > 0 && d->preview_raw_crop_h > 0
       && d->export_w > 0 && d->export_h > 0)
    {
      rw = (double)d->preview_raw_crop_w / d->export_w * img_w;
      rh = (double)d->preview_raw_crop_h / d->export_h * img_h;
    }
    else
    {
      const int task_scale = _task_scale(d->task);
      const int crop_w = w / task_scale;
      const int crop_h = h / task_scale;
      rw = (double)crop_w / d->export_w * img_w;
      rh = (double)crop_h / d->export_h * img_h;
    }
    // compute rectangle top-left. for RGB denoise / upscale, the click
    // / motion handlers already clamp patch_center with inner margins,
    // so the rectangle always fits — match master by NOT pushing here.
    // for raw denoise, patch_center is free-range in [0, 1]; push the
    // rectangle inward so it still fits (matches the worker's CLAMP on
    // crop_x / crop_y and keeps the picker visually honest).
    double rx = ox + d->patch_center[0] * img_w - rw / 2.0;
    double ry = oy + d->patch_center[1] * img_h - rh / 2.0;
    if(d->task == NEURAL_TASK_RAW_DENOISE)
    {
      if(rx < ox) rx = ox;
      if(ry < oy) ry = oy;
      if(rx + rw > ox + img_w) rx = ox + img_w - rw;
      if(ry + rh > oy + img_h) ry = oy + img_h - rh;
    }

    // dim area outside the rectangle
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
    cairo_rectangle(cr, 0, 0, w, h);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_fill(cr);
    cairo_restore(cr);

    // bright rectangle border
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.8);
    cairo_set_line_width(cr, 1.5);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_stroke(cr);

    // label
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 11.0);
    const char *text = _("click to select preview area");
    cairo_text_extents_t ext;
    cairo_text_extents(cr, text, &ext);
    const double pad = 4.0;
    const double bh = ext.height + pad * 2;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.6);
    cairo_rectangle(cr, 0, h - bh, w, bh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, (w - ext.width) / 2.0, h - pad);
    cairo_show_text(cr, text);

    cairo_surface_destroy(thumb_surf);
    return FALSE;
  }

  if(!d->preview_ready || !d->cairo_before || !d->cairo_after)
  {
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    cairo_select_font_face(cr, "sans-serif",
                           CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12.0);
    cairo_text_extents_t ext;
    const char *text = !d->model_available
      ? _("model not available")
      : d->preview_generating
      ? _("generating preview...")
      : !d->preview_requested
      ? _("click to generate preview")
      : d->preview_error == DT_NR_PREVIEW_ERR_UNSUPPORTED
      ? _("image not supported by this task")
      : d->preview_error == DT_NR_PREVIEW_ERR_INIT_FAILED
      ? _("preview initialization failed")
      : _("select an image to preview");
    cairo_text_extents(cr, text, &ext);
    cairo_move_to(cr, (w - ext.width) / 2.0, (h + ext.height) / 2.0);
    cairo_show_text(cr, text);
    return FALSE;
  }

  const int pw = d->preview_w;
  const int ph = d->preview_h;
  if(pw <= 0 || ph <= 0) return FALSE;

  cairo_surface_t *before_surf = cairo_image_surface_create_for_data(
    d->cairo_before, CAIRO_FORMAT_RGB24, pw, ph, d->cairo_stride);
  cairo_surface_t *after_surf = cairo_image_surface_create_for_data(
    d->cairo_after, CAIRO_FORMAT_RGB24, pw, ph, d->cairo_stride);

  // scale preview to fit widget, centered, never below 100% zoom
  const double sx = (double)w / pw;
  const double sy = (double)h / ph;
  const double scale = fmax(1.0, fmin(sx, sy));
  const double img_w = pw * scale;
  const double img_h = ph * scale;
  const double ox = (w - img_w) / 2.0;
  const double oy = (h - img_h) / 2.0;
  const double div_x = ox + d->split_pos * img_w;

  // left side: before
  cairo_save(cr);
  cairo_rectangle(cr, ox, oy, div_x - ox, img_h);
  cairo_clip(cr);
  cairo_translate(cr, ox, oy);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, before_surf, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  // right side: after
  cairo_save(cr);
  cairo_rectangle(cr, div_x, oy, ox + img_w - div_x, img_h);
  cairo_clip(cr);
  cairo_translate(cr, ox, oy);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, after_surf, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
  cairo_paint(cr);
  cairo_restore(cr);

  // divider line
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_set_line_width(cr, 1.5);
  cairo_move_to(cr, div_x, oy);
  cairo_line_to(cr, div_x, oy + img_h);
  cairo_stroke(cr);

  cairo_surface_destroy(before_surf);
  cairo_surface_destroy(after_surf);

  cairo_select_font_face(cr, "sans-serif",
                         CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 11.0);

  // warning overlay at top
  if(d->warning_text[0])
  {
    cairo_text_extents_t ext;
    cairo_text_extents(cr, d->warning_text, &ext);
    const double pad = 4.0;
    const double bh = ext.height + pad * 2;
    cairo_set_source_rgba(cr, 0.8, 0.1, 0.1, 0.85);
    cairo_rectangle(cr, 0, 0, w, bh);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, (w - ext.width) / 2.0, pad + ext.height);
    cairo_show_text(cr, d->warning_text);
  }

  // info overlay at bottom
  if(d->info_text_left[0])
  {
    cairo_text_extents_t ext_l, ext_r;
    cairo_text_extents(cr, d->info_text_left, &ext_l);
    const gboolean with_right = (d->info_text_right[0] != '\0');
    if(with_right)
      cairo_text_extents(cr, d->info_text_right, &ext_r);
    const double pad = 4.0;
    const double arrow_w = ext_l.height * 1.2;
    const double gap = 6.0;
    const double total_w = with_right
      ? ext_l.width + gap + arrow_w + gap + ext_r.width
      : ext_l.width;
    const double bh = ext_l.height + pad * 2;
    const double by = h - bh;
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.3);
    cairo_rectangle(cr, 0, by, w, bh);
    cairo_fill(cr);

    const double tx = (w - total_w) / 2.0;
    const double ty = by + pad + ext_l.height;
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, tx, ty);
    cairo_show_text(cr, d->info_text_left);

    if(with_right)
    {
      // draw arrow between the size texts
      const double ah = ext_l.height * 0.5;
      const double ax = tx + ext_l.width + gap;
      const double ay = ty - ext_l.height * 0.5;
      cairo_set_line_width(cr, 1.5);
      cairo_move_to(cr, ax, ay);
      cairo_line_to(cr, ax + arrow_w, ay);
      cairo_line_to(cr, ax + arrow_w - ah * 0.5, ay - ah * 0.5);
      cairo_move_to(cr, ax + arrow_w, ay);
      cairo_line_to(cr, ax + arrow_w - ah * 0.5, ay + ah * 0.5);
      cairo_stroke(cr);

      cairo_move_to(cr, ax + arrow_w + gap, ty);
      cairo_show_text(cr, d->info_text_right);
    }
  }

  return FALSE;
}

static gboolean _preview_button_press(GtkWidget *widget,
                                      GdkEventButton *event,
                                      dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  guint button = 0;
  gdk_event_get_button((GdkEvent *)event, &button);
  if(button != 1) return FALSE;

  double ex = 0.0, ey = 0.0;
  gdk_event_get_coords((GdkEvent *)event, &ex, &ey);

  // thumbnail picking mode: click to select area and trigger preview
  if(d->picking_thumbnail && d->export_cairo)
  {
    const int w = gtk_widget_get_allocated_width(widget);
    const int h = gtk_widget_get_allocated_height(widget);
    double img_w, img_h, ox, oy;
    _picking_geometry(d, w, h, &img_w, &img_h, &ox, &oy);

    // convert click to normalized image coords.
    //  * RGB denoise / upscale: clamp so the crop rectangle stays
    //    within the image (master behaviour — the export-based preview
    //    needs this because the worker and draw share a single
    //    export_w-based scale).
    //  * raw denoise: no inner-margin clamp — user can pick corners.
    //    the raw worker CLAMPs crop_x/y, and _preview_draw pushes the
    //    rectangle inward to match.
    const float nx = (float)((ex - ox) / img_w);
    const float ny = (float)((ey - oy) / img_h);
    if(nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f)
      return TRUE;

    if(d->task == NEURAL_TASK_RAW_DENOISE)
    {
      d->patch_center[0] = CLAMP(nx, 0.0f, 1.0f);
      d->patch_center[1] = CLAMP(ny, 0.0f, 1.0f);
    }
    else
    {
      const int task_scale = _task_scale(d->task);
      const float half_w = (float)w / task_scale / (2.0f * d->export_w);
      const float half_h = (float)h / task_scale / (2.0f * d->export_h);
      d->patch_center[0] = CLAMP(nx, half_w, 1.0f - half_w);
      d->patch_center[1] = CLAMP(ny, half_h, 1.0f - half_h);
    }

    // patch moved — every cached preview is now stale (different crop)
    _preview_cache_invalidate_all(d);

    // exit picking mode
    d->picking_thumbnail = FALSE;
    gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(d->pick_button), FALSE);

    // trigger preview reusing cached export (skip re-export)
    d->preview_requested = TRUE;
    dt_control_log(_("generating preview..."));
    _trigger_preview(self);
    return TRUE;
  }

  // click to start preview generation
  if(!d->preview_ready && !d->preview_generating)
  {
    d->preview_requested = TRUE;
    _trigger_preview(self);
    return TRUE;
  }

  if(!d->preview_ready) return FALSE;

  const int w = gtk_widget_get_allocated_width(widget);
  const int h = gtk_widget_get_allocated_height(widget);
  const int pw = d->preview_w;
  const int ph = d->preview_h;
  if(pw <= 0 || ph <= 0) return FALSE;
  const double scale = fmax(1.0, fmin((double)w / pw, (double)h / ph));
  const double ox = (w - pw * scale) / 2.0;
  const double div_x = ox + d->split_pos * pw * scale;

  if(fabs(ex - div_x) < 8.0)
  {
    d->dragging_split = TRUE;
    return TRUE;
  }

  return FALSE;
}

static gboolean _preview_button_release(GtkWidget *widget,
                                        GdkEventButton *event,
                                        dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  if(d->dragging_split)
  {
    d->dragging_split = FALSE;
    return TRUE;
  }
  return FALSE;
}

static gboolean _preview_motion(GtkWidget *widget,
                                GdkEventMotion *event,
                                dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  double ex = 0.0, ey = 0.0;
  gdk_event_get_coords((GdkEvent *)event, &ex, &ey);

  // move crop rectangle while hovering in picking mode
  if(d->picking_thumbnail && d->export_cairo)
  {
    const int w = gtk_widget_get_allocated_width(widget);
    const int h = gtk_widget_get_allocated_height(widget);
    double img_w, img_h, ox, oy;
    _picking_geometry(d, w, h, &img_w, &img_h, &ox, &oy);

    // motion follows the cursor, with clamping based on task:
    //  * RGB denoise / upscale: inner-margin clamp (master behaviour)
    //  * raw denoise: free-range in [0, 1] so corners are reachable
    const float rx = (float)((ex - ox) / img_w);
    const float ry = (float)((ey - oy) / img_h);
    if(d->task == NEURAL_TASK_RAW_DENOISE)
    {
      d->patch_center[0] = CLAMP(rx, 0.0f, 1.0f);
      d->patch_center[1] = CLAMP(ry, 0.0f, 1.0f);
    }
    else
    {
      const int task_scale = _task_scale(d->task);
      const float half_w = (float)w / task_scale / (2.0f * d->export_w);
      const float half_h = (float)h / task_scale / (2.0f * d->export_h);
      d->patch_center[0] = CLAMP(rx, half_w, 1.0f - half_w);
      d->patch_center[1] = CLAMP(ry, half_h, 1.0f - half_h);
    }
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  if(d->dragging_split)
  {
    const int w = gtk_widget_get_allocated_width(widget);
    const int pw = d->preview_w;
    const int ph = d->preview_h;
    if(pw <= 0 || ph <= 0) return FALSE;
    const int ah = gtk_widget_get_allocated_height(widget);
    const double scale = fmax(1.0, fmin((double)w / pw,
                                        (double)ah / ph));
    const double ox = (w - pw * scale) / 2.0;
    const double img_w = pw * scale;

    d->split_pos = CLAMP((ex - ox) / img_w, 0.0, 1.0);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  // change cursor near divider
  if(d->preview_ready
     && d->preview_w > 0
     && d->preview_h > 0)
  {
    const int w = gtk_widget_get_allocated_width(widget);
    const int h = gtk_widget_get_allocated_height(widget);
    const double scale = fmax(1.0, fmin((double)w / d->preview_w,
                                        (double)h / d->preview_h));
    const double ox = (w - d->preview_w * scale) / 2.0;
    const double div_x
      = ox + d->split_pos * d->preview_w * scale;

    GdkWindow *win = gtk_widget_get_window(widget);
    if(win)
    {
      const gboolean near = fabs(ex - div_x) < 8.0;
      if(near)
      {
        GdkCursor *cursor = gdk_cursor_new_from_name(
          gdk_display_get_default(), "col-resize");
        gdk_window_set_cursor(win, cursor);
        g_object_unref(cursor);
      }
      else
      {
        gdk_window_set_cursor(win, NULL);
      }
    }
  }

  return FALSE;
}

static void _selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_requested = FALSE;
  _cancel_preview(self);
  _preview_cache_invalidate_all(d);
  _update_info_label(d);
  _update_button_sensitivity(d);
}

static void _image_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_requested = FALSE;
  _cancel_preview(self);
  _preview_cache_invalidate_all(d);
  _update_info_label(d);
  _update_button_sensitivity(d);
}


static void _ai_models_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  if(d->env)
    dt_restore_env_refresh(d->env);

  d->model_available = _check_model_available(d, d->task);
  _update_info_label(d);
  _update_button_sensitivity(d);
}
static void _bpp_combo_changed(GtkWidget *w,
                               dt_lib_module_t *self)
{
  const int idx = dt_bauhaus_combobox_get(w);
  dt_conf_set_int(CONF_BIT_DEPTH, idx);
}

// mirror of export.c: combo index 0 = "image settings", 1..N = profiles
// with out_pos >= 0 ordered by out_pos
static void _profile_combo_changed(GtkWidget *w,
                                   dt_lib_module_t *self)
{
  const int pos = dt_bauhaus_combobox_get(w);
  gboolean done = FALSE;
  if(pos > 0)
  {
    const int out_pos = pos - 1;
    for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
    {
      const dt_colorspaces_color_profile_t *pp = l->data;
      if(pp->out_pos == out_pos)
      {
        dt_conf_set_int(CONF_ICC_TYPE, pp->type);
        dt_conf_set_string(CONF_ICC_FILE,
                           (pp->type == DT_COLORSPACE_FILE) ? pp->filename : "");
        done = TRUE;
        break;
      }
    }
  }
  if(!done)
  {
    dt_conf_set_int(CONF_ICC_TYPE, DT_COLORSPACE_NONE);
    dt_conf_set_string(CONF_ICC_FILE, "");
  }
  _update_info_label((dt_lib_neural_restore_t *)self->data);
}

static void _catalog_toggle_changed(GtkWidget *w,
                                    dt_lib_module_t *self)
{
  const gboolean active
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_conf_set_bool(CONF_ADD_CATALOG, active);
}

static void _preserve_wide_gamut_toggled(GtkWidget *w,
                                         dt_lib_module_t *self)
{
  const gboolean active
    = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_conf_set_bool(CONF_PRESERVE_WIDE_GAMUT, active);
  _update_info_label((dt_lib_neural_restore_t *)self->data);
}

static void _output_dir_changed(GtkEditable *editable,
                                dt_lib_module_t *self)
{
  dt_conf_set_string(CONF_OUTPUT_DIR,
                     gtk_entry_get_text(GTK_ENTRY(editable)));
}

static void _output_dir_browse(GtkWidget *button,
                               dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d
    = (dt_lib_neural_restore_t *)self->data;
  GtkWidget *dialog
    = gtk_file_chooser_dialog_new(_("select output folder"),
                                  GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                  GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                  _("_cancel"), GTK_RESPONSE_CANCEL,
                                  _("_select"), GTK_RESPONSE_ACCEPT,
                                  NULL);

  const char *current
    = gtk_entry_get_text(GTK_ENTRY(d->output_dir_entry));
  if(current && current[0])
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    char *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if(folder)
    {
      gtk_entry_set_text(GTK_ENTRY(d->output_dir_entry), folder);
      dt_conf_set_string(CONF_OUTPUT_DIR, folder);
      g_free(folder);
    }
  }
  gtk_widget_destroy(dialog);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = g_new0(dt_lib_neural_restore_t, 1);
  self->data = d;
  d->env = dt_restore_env_init();
  d->processing_images = g_hash_table_new(g_direct_hash, g_direct_equal);
  dt_pthread_mutex_init(&d->ctx_lock, NULL);
  g_mutex_init(&d->preview_inference_lock);
  d->split_pos = 0.5f;

  // notebook tabs (denoise / upscale)
  static dt_action_def_t notebook_def = {};
  d->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define(DT_ACTION(self), NULL, N_("page"),
                   GTK_WIDGET(d->notebook), &notebook_def);

  // raw denoise sits first: it runs earliest in the denoise workflow
  // (before demosaic-stage processing). bayer / linear variant selection
  // is driven by the active "rawdenoise" model rather than a UI toggle
  d->raw_denoise_page = dt_ui_notebook_page(d->notebook, N_("raw denoise"),
                                            _("AI raw denoising"));
  d->denoise_page     = dt_ui_notebook_page(d->notebook, N_("denoise"),
                                            _("AI denoising"));
  d->upscale_page     = dt_ui_notebook_page(d->notebook, N_("upscale"),
                                            _("AI upscaling"));

  // raw denoise page: strength slider. 100 = full model output,
  // 0 = unchanged source CFA, linear blend in raw ADC space
  const float saved_raw_strength = dt_conf_key_exists(CONF_RAW_STRENGTH)
    ? dt_conf_get_float(CONF_RAW_STRENGTH) : 100.0f;
  d->raw_strength_slider = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                        0.0f, 100.0f, 1.0f,
                                                        saved_raw_strength, 0);
  dt_bauhaus_widget_set_label(d->raw_strength_slider, NULL, N_("strength"));
  dt_bauhaus_slider_set_format(d->raw_strength_slider, "%");
  gtk_widget_set_tooltip_text(d->raw_strength_slider,
                              _("blend between the source CFA (0%) and "
                                "the denoised output (100%)"));
  g_signal_connect(G_OBJECT(d->raw_strength_slider), "value-changed",
                   G_CALLBACK(_raw_strength_slider_changed), self);
  dt_gui_box_add(d->raw_denoise_page, d->raw_strength_slider);

  // denoise page: strength slider. 100 = full denoise, 0 = source-like.
  // dialing below 100 brings DWT-filtered texture back without
  // reintroducing the noise-frequency content.
  const float saved_strength = dt_conf_key_exists(CONF_STRENGTH)
    ? dt_conf_get_float(CONF_STRENGTH) : 100.0f;
  d->recovery_slider = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                    0.0f, 100.0f, 1.0f,
                                                    saved_strength, 0);
  dt_bauhaus_widget_set_label(d->recovery_slider, NULL, N_("strength"));
  dt_bauhaus_slider_set_format(d->recovery_slider, "%");
  gtk_widget_set_tooltip_text(d->recovery_slider,
                              _("100% applies the full AI model output; "
                                "lower values bring back luminance texture "
                                "and grain while keeping color noise suppressed"));
  g_signal_connect(G_OBJECT(d->recovery_slider), "value-changed",
                   G_CALLBACK(_recovery_slider_changed), self);
  dt_gui_box_add(d->denoise_page, d->recovery_slider);

  // upscale page: scale factor selector
  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->scale_combo, self, NULL, N_("scale"),
                                _("upscale factor"),
                                0, _scale_combo_changed, self,
                                N_("2x"), N_("4x"));
  dt_gui_box_add(d->upscale_page, d->scale_combo);

  // restore saved tab
  const int saved_page = dt_conf_get_int(CONF_ACTIVE_PAGE);
  if(saved_page > 0)
    gtk_notebook_set_current_page(d->notebook, saved_page);
  _update_task_from_ui(d);
  d->model_available = _check_model_available(d, d->task);

  // pick area button
  d->patch_center[0] = 0.5f;
  d->patch_center[1] = 0.5f;
  d->picking_thumbnail = FALSE;
  d->pick_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker,
                                          0, NULL);
  gtk_widget_set_tooltip_text(d->pick_button,
                              _("click to pick preview area on the image thumbnail\n"
                                "double-click to reset to center"));
  g_signal_connect(d->pick_button, "toggled",
                   G_CALLBACK(_pick_toggled), self);
  g_signal_connect(d->pick_button, "button-press-event",
                   G_CALLBACK(_pick_double_click), self);

  // preview area (resizable via dt_ui_resize_wrap)
  d->preview_area = GTK_WIDGET(dt_ui_resize_wrap(NULL, 200, CONF_PREVIEW_HEIGHT));
  gtk_widget_add_events(d->preview_area,
                        GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_POINTER_MOTION_MASK);
  g_signal_connect(d->preview_area, "draw",
                   G_CALLBACK(_preview_draw), self);
  g_signal_connect(d->preview_area, "button-press-event",
                   G_CALLBACK(_preview_button_press), self);
  g_signal_connect(d->preview_area, "button-release-event",
                   G_CALLBACK(_preview_button_release), self);
  g_signal_connect(d->preview_area, "motion-notify-event",
                   G_CALLBACK(_preview_motion), self);

  // process button
  d->process_button = dt_action_button_new(self, N_("process"),
                                           _process_clicked, self,
                                           _("process selected images"), 0, 0);

  // process + pick button row
  gtk_widget_set_hexpand(d->process_button, TRUE);
  gtk_widget_set_halign(d->process_button, GTK_ALIGN_FILL);
  GtkWidget *action_row = dt_gui_hbox();
  dt_gui_box_add(action_row, d->process_button);
  dt_gui_box_add(action_row, d->pick_button);

  // main layout: notebook, preview, action row, output
  gtk_widget_set_margin_top(action_row, 4);
  self->widget = dt_gui_vbox(GTK_WIDGET(d->notebook),
                             d->preview_area,
                             action_row);

  // output settings
  dt_gui_new_collapsible_section(&d->cs_output,
                                 CONF_EXPAND_OUTPUT,
                                 _("output parameters"),
                                 GTK_BOX(self->widget),
                                 DT_ACTION(self));

  GtkWidget *cs_box = GTK_WIDGET(d->cs_output.container);

  // bit depth
  const int saved_bpp = dt_conf_key_exists(CONF_BIT_DEPTH)
    ? dt_conf_get_int(CONF_BIT_DEPTH)
    : NEURAL_BPP_16;
  DT_BAUHAUS_COMBOBOX_NEW_FULL(d->bpp_combo, self, NULL, N_("bit depth"),
                               _("output TIFF bit depth"),
                               saved_bpp, _bpp_combo_changed, self,
                               N_("8 bit"), N_("16 bit"), N_("32 bit (float)"));
  dt_gui_box_add(cs_box, d->bpp_combo);

  // output color profile: 0 = image settings (working profile), then the
  // same list the standard export dialog uses; out-of-gamut colors are
  // still clamped by the model, so this only controls the wrapper
  // embedded in the output TIFF
  d->profile_combo = dt_bauhaus_combobox_new_action(DT_ACTION(self));
  dt_bauhaus_widget_set_label(d->profile_combo, NULL, N_("profile"));
  dt_bauhaus_combobox_add(d->profile_combo, _("image settings"));
  for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
  {
    const dt_colorspaces_color_profile_t *pp = l->data;
    if(pp->out_pos > -1)
      dt_bauhaus_combobox_add(d->profile_combo, pp->name);
  }
  // restore saved selection
  int saved_pos = 0;
  if(dt_conf_key_exists(CONF_ICC_TYPE))
  {
    const int saved_type = dt_conf_get_int(CONF_ICC_TYPE);
    if(saved_type != DT_COLORSPACE_NONE)
    {
      gchar *saved_file = dt_conf_get_string(CONF_ICC_FILE);
      for(GList *l = darktable.color_profiles->profiles; l; l = g_list_next(l))
      {
        const dt_colorspaces_color_profile_t *pp = l->data;
        if(pp->out_pos > -1 && pp->type == saved_type
           && (pp->type != DT_COLORSPACE_FILE
               || g_strcmp0(pp->filename, saved_file) == 0))
        {
          saved_pos = pp->out_pos + 1;
          break;
        }
      }
      g_free(saved_file);
    }
  }
  dt_bauhaus_combobox_set(d->profile_combo, saved_pos);
  gtk_widget_set_tooltip_text(d->profile_combo,
                              _("color profile embedded in the output TIFF"));
  g_signal_connect(d->profile_combo, "value-changed",
                   G_CALLBACK(_profile_combo_changed), self);
  dt_gui_box_add(cs_box, d->profile_combo);

  // preserve wide-gamut toggle: when on, out-of-sRGB pixels pass
  // through the model unchanged (preserved but not denoised). when
  // off, every pixel is denoised but wide-gamut colors may be clipped.
  // only affects denoise; upscale always uses the model output.
  d->preserve_wide_gamut_toggle
    = gtk_check_button_new_with_label(_("preserve wide-gamut colors"));
  gtk_toggle_button_set_active(
    GTK_TOGGLE_BUTTON(d->preserve_wide_gamut_toggle),
    dt_conf_key_exists(CONF_PRESERVE_WIDE_GAMUT)
      ? dt_conf_get_bool(CONF_PRESERVE_WIDE_GAMUT)
      : TRUE);
  gtk_widget_set_tooltip_text(d->preserve_wide_gamut_toggle,
    _("when on, pixels outside sRGB gamut pass through without being"
      " denoised; when off, all pixels are denoised but wide-gamut"
      " colors may be clipped; only affects denoise"));
  g_signal_connect(d->preserve_wide_gamut_toggle, "toggled",
                   G_CALLBACK(_preserve_wide_gamut_toggled), self);
  dt_gui_box_add(cs_box, d->preserve_wide_gamut_toggle);

  // add to catalog
  GtkWidget *catalog_box = dt_gui_hbox();
  d->catalog_toggle = gtk_check_button_new_with_label(_("add to the current collection"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->catalog_toggle),
                               dt_conf_key_exists(CONF_ADD_CATALOG)
                                 ? dt_conf_get_bool(CONF_ADD_CATALOG)
                                 : TRUE);
  gtk_widget_set_tooltip_text(d->catalog_toggle,
                              _("automatically import the output image into"
                                " the current collection"));
  g_signal_connect(d->catalog_toggle, "toggled",
                   G_CALLBACK(_catalog_toggle_changed), self);
  dt_gui_box_add(catalog_box, d->catalog_toggle);
  dt_gui_box_add(cs_box, catalog_box);

  // output directory
  GtkWidget *dir_box = dt_gui_hbox();
  d->output_dir_entry = gtk_entry_new();
  char *saved_dir = dt_conf_get_string(CONF_OUTPUT_DIR);
  gtk_entry_set_text(GTK_ENTRY(d->output_dir_entry),
                     (saved_dir && saved_dir[0])
                       ? saved_dir : "$(FILE_FOLDER)");
  g_free(saved_dir);
  gtk_widget_set_tooltip_text(d->output_dir_entry,
                              _("output folder — supports darktable variables\n"
                                "$(FILE_FOLDER) = source image folder"));
  gtk_widget_set_hexpand(d->output_dir_entry, TRUE);
  g_signal_connect(d->output_dir_entry, "changed",
                   G_CALLBACK(_output_dir_changed), self);

  d->output_dir_button = dtgtk_button_new(dtgtk_cairo_paint_directory, 0, NULL);
  gtk_widget_set_tooltip_text(d->output_dir_button, _("select output folder"));
  g_signal_connect(d->output_dir_button, "clicked",
                   G_CALLBACK(_output_dir_browse), self);

  dt_gui_box_add(dir_box, d->output_dir_entry);
  dt_gui_box_add(dir_box, d->output_dir_button);
  dt_gui_box_add(cs_box, dir_box);

  g_signal_connect(d->notebook, "switch-page",
                   G_CALLBACK(_notebook_page_changed), self);

  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(self->widget, TRUE);

  // DT signals
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_SELECTION_CHANGED, _selection_changed_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _image_changed_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_AI_MODELS_CHANGED, _ai_models_changed_callback);

  // sync per-task widget visibility for the initially-active tab.
  // _task_changed does detail-slider + output-knobs visibility and
  // info/button state — safe to call here after all widgets exist.
  _task_changed(d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  DT_CONTROL_SIGNAL_DISCONNECT(_selection_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_image_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_ai_models_changed_callback, self);

  if(d)
  {
    // cancel any pending debounced trigger before tearing down state
    if(d->preview_trigger_timer)
    {
      g_source_remove(d->preview_trigger_timer);
      d->preview_trigger_timer = 0;
    }
    // signal preview thread to exit and wait for it. join blocks
    // here (unlike _cancel_preview during runtime, where we can't
    // afford to freeze the UI) — happens once on shutdown only
    g_atomic_int_inc(&d->preview_sequence);
    if(d->preview_thread)
    {
      g_thread_join(d->preview_thread);
      d->preview_thread = NULL;
    }
    // any worker idle callbacks queued just before the join may still
    // fire after this point. they check sequence and discard, but they
    // dereference `d` to do that — drain the main context once so they
    // run while `d` is still alive
    while(g_main_context_pending(NULL))
      g_main_context_iteration(NULL, FALSE);
    g_mutex_clear(&d->preview_inference_lock);

    g_free(d->preview_before);
    g_free(d->preview_after);
    dt_free_align(d->preview_detail);
    g_free(d->cairo_before);
    g_free(d->cairo_after);
    g_free(d->export_pixels);
    g_free(d->export_cairo);

    // raw denoise preview cache
    if(d->preview_strength_timer)
    {
      g_source_remove(d->preview_strength_timer);
      d->preview_strength_timer = 0;
    }
    g_free(d->preview_full_cfa);
    dt_free_align(d->preview_full_lin);
    g_free(d->preview_raw_src_rgb);
    g_free(d->preview_raw_denoised_rgb);
    _preview_cache_invalidate_all(d);
    if(d->processing_images)
      g_hash_table_destroy(d->processing_images);
    dt_restore_unref(d->cached_ctx);
    dt_pthread_mutex_destroy(&d->ctx_lock);
    if(d->env)
      dt_restore_env_destroy(d->env);
    g_free(d);
  }
  self->data = NULL;
}

void gui_update(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  // re-read model availability in case conf changed
  d->model_available = _check_model_available(d, d->task);
  _update_info_label(d);
  _update_button_sensitivity(d);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  gtk_notebook_set_current_page(d->notebook, 0);
  dt_conf_set_int(CONF_ACTIVE_PAGE, 0);
  dt_bauhaus_combobox_set(d->scale_combo, 0);
  d->task = NEURAL_TASK_DENOISE;
  d->model_available = _check_model_available(d, d->task);
  d->preview_requested = FALSE;
  _cancel_preview(self);
  _update_info_label(d);
  _update_button_sensitivity(d);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
