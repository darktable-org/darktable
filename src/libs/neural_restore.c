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

/*
   neural restore — lighttable module for AI-based image restoration

   overview
   --------
   provides two operations via a tabbed notebook UI:
     - denoise: run an ONNX denoiser (e.g. NIND UNet) on selected images
     - upscale: run an ONNX super-resolution model (e.g. BSRGAN) at 2x or 4x

   the module lives in the right panel (DT_UI_CONTAINER_PANEL_RIGHT_CENTER)
   and is available in both lighttable and darkroom views. it is only built
   when cmake option USE_AI=ON.

   architecture
   ------------
   the core AI inference, tiling, and detail recovery logic lives in
   src/ai/restore.c (the darktable_ai library). this module handles:

   1. preview (interactive, single-image)
      triggered by clicking the preview widget or switching tabs.
      runs on a background GThread (_preview_thread):
        - exports the selected image at reduced resolution via the
          darktable export pipeline (captures fully-processed pixels)
        - crops a patch matching the widget aspect ratio
        - runs AI inference on the patch via dt_restore_run_patch()
        - delivers before/after buffers to the main thread via g_idle_add
      the preview widget draws a split before/after view with a draggable
      divider. for denoise, DWT-filtered detail is pre-computed so the
      detail recovery slider updates instantly without re-running inference.
      cancellation uses an atomic sequence counter (preview_sequence):
      the thread checks it at key points and discards stale results.

   2. batch processing (multi-image)
      runs as a dt_control_job on the user background queue.
      for each selected image:
        - exports via the darktable pipeline with a custom format module
          that intercepts the pixel buffer in _ai_write_image()
        - for denoise with detail recovery: buffers the full output via
          dt_restore_process_tiled(), applies dt_restore_apply_detail_recovery(),
          then writes TIFF
        - for plain denoise/upscale: streams tiles directly to TIFF via
          _process_tiled_tiff() to avoid buffering the full output
        - output TIFF embeds the darktable working profile and source EXIF
        - imports the result into the darktable library and groups it
          with the source image

   3. output parameters (collapsible section)
      - bit depth: 8/16/32-bit TIFF (default 16-bit)
      - add to catalog: auto-import output into darktable library
      - output directory: supports darktable variables (e.g. $(FILE_FOLDER))

   threading
   ---------
   - preview: background GThread, one at a time. joined before starting
     a new preview and in gui_cleanup. stale results are discarded via
     atomic preview_sequence counter
   - batch: dt_control_job on DT_JOB_QUEUE_USER_BG. supports cancellation
     via dt_control_job_get_state()
   - ai_registry->lock: held briefly to read provider setting
   - all GTK updates go through g_idle_add to stay on the main thread

   key structs
   -----------
   dt_lib_neural_restore_t    — module GUI state and preview data
   dt_neural_job_t            — batch processing job parameters
   dt_neural_format_params_t  — custom format module for export interception
   dt_neural_preview_data_t   — preview thread input parameters
   dt_neural_preview_result_t — preview thread output (delivered via idle)
   dt_neural_preview_capture_t — captures export pixels for preview

   preferences
   -----------
   CONF_DETAIL_RECOVERY — detail recovery slider value
   CONF_ACTIVE_PAGE     — last active notebook tab
   CONF_BIT_DEPTH       — output TIFF bit depth (0=8, 1=16, 2=32)
   CONF_ADD_CATALOG     — auto-import output into library
   CONF_OUTPUT_DIR      — output directory pattern (supports variables)
   CONF_EXPAND_OUTPUT   — output section collapsed/expanded state
*/

#include "common/ai/restore.h"
#include "control/conf.h"
#include "bauhaus/bauhaus.h"
#include "common/act_on.h"
#include "common/collection.h"
#include "common/variables.h"
#include "common/colorspaces.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/grouping.h"
#include "control/jobs/control_jobs.h"
#include "control/signal.h"
#include "develop/develop.h"
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

#define CONF_DETAIL_RECOVERY "plugins/lighttable/neural_restore/detail_recovery"
#define CONF_ACTIVE_PAGE "plugins/lighttable/neural_restore/active_page"
#define CONF_BIT_DEPTH "plugins/lighttable/neural_restore/bit_depth"
#define CONF_ADD_CATALOG "plugins/lighttable/neural_restore/add_to_catalog"
#define CONF_OUTPUT_DIR "plugins/lighttable/neural_restore/output_directory"
#define CONF_ICC_TYPE "plugins/lighttable/neural_restore/icc_type"
#define CONF_ICC_FILE "plugins/lighttable/neural_restore/icc_filename"
#define CONF_EXPAND_OUTPUT "plugins/lighttable/neural_restore/expand_output"
#define CONF_PREVIEW_HEIGHT "plugins/lighttable/neural_restore/preview_height"

typedef enum dt_neural_task_t
{
  NEURAL_TASK_DENOISE = 0,
  NEURAL_TASK_UPSCALE_2X,
  NEURAL_TASK_UPSCALE_4X,
} dt_neural_task_t;

typedef enum dt_neural_bpp_t
{
  NEURAL_BPP_8 = 0,
  NEURAL_BPP_16 = 1,
  NEURAL_BPP_32 = 2,
} dt_neural_bpp_t;

typedef struct dt_lib_neural_restore_t
{
  GtkNotebook *notebook;
  GtkWidget *denoise_page;
  GtkWidget *upscale_page;
  GtkWidget *scale_combo;
  GtkWidget *preview_area;
  GtkWidget *process_button;
  char info_text_left[64];
  char info_text_right[128];
  char warning_text[128];
  GtkWidget *recovery_slider;
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

  // output settings (collapsible)
  dt_gui_collapsible_section_t cs_output;
  GtkWidget *bpp_combo;
  GtkWidget *profile_combo;
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
  float detail_recovery;
  dt_lib_module_t *self;
  dt_neural_bpp_t bpp;
  gboolean add_to_catalog;
  char *output_dir;  // NULL = same as source
  // output color profile. DT_COLORSPACE_NONE means "use image's working profile"
  dt_colorspaces_color_profile_type_t icc_type;
  char *icc_filename;  // only used when icc_type == DT_COLORSPACE_FILE
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
    case NEURAL_TASK_UPSCALE_2X:
      return dt_restore_load_upscale_x2(env);
    case NEURAL_TASK_UPSCALE_4X:
      return dt_restore_load_upscale_x4(env);
    default:
      return NULL;
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

  const float recovery_alpha = job->detail_recovery / 100.0f;
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
      dt_grouping_add_to_group(source_imgid, newid);
    dt_collection_update_query(darktable.collection,
                               DT_COLLECTION_CHANGE_RELOAD,
                               DT_COLLECTION_PROP_UNDEF,
                               NULL);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, newid);
  }
}

static const char *_task_suffix(dt_neural_task_t task)
{
  switch(task)
  {
    case NEURAL_TASK_DENOISE:    return "_denoise";
    case NEURAL_TASK_UPSCALE_2X: return "_upscale-2x";
    case NEURAL_TASK_UPSCALE_4X: return "_upscale-4x";
    default:                     return "_restore";
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

static int32_t _process_job_run(dt_job_t *job)
{
  dt_neural_job_t *j = dt_control_job_get_params(job);

  const char *task_name = (j->task == NEURAL_TASK_DENOISE)
    ? _("denoise") : _("upscale");
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

    // find unique filename: base.tif, base_1.tif, ...
    char filename[PATH_MAX];
    snprintf(filename, sizeof(filename), "%s.tif", base);

    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      gboolean found = FALSE;
      for(int s = 1; s < 10000; s++)
      {
        snprintf(filename, sizeof(filename), "%s_%d.tif", base, s);
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
             (j->task == NEURAL_TASK_DENOISE) ? _("denoising image %d/%d...")
             : (j->task == NEURAL_TASK_UPSCALE_2X) ? _("upscaling 2x image %d/%d...")
             : _("upscaling 4x image %d/%d..."),
             count + 1, total);
    dt_control_job_set_progress_message(job, msg);

    const int export_err
      = dt_imageio_export_with_flags(imgid,
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

    if(export_err)
    {
      dt_print(DT_DEBUG_AI,
               "[neural_restore] export failed for imgid %d", imgid);
      dt_control_log(_("neural restore: export failed"));
      dt_control_job_set_progress(job, (double)++count / total);
      continue;
    }

    if(j->add_to_catalog)
      _import_image(filename, imgid);
    dt_control_job_set_progress(job, (double)++count / total);
  }

  dt_restore_unref(j->ctx);
  j->ctx = NULL;

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

// TRUE if the given profile type has primaries wider than sRGB;
// exhaustive switch with no default: when dt_colorspaces_color_profile_type_t
// gains a new value, the compiler will warn and force this list to be
// updated; DT_COLORSPACE_NONE and DT_COLORSPACE_FILE require additional
// context and are handled by the caller
static gboolean _profile_type_is_wide_gamut(dt_colorspaces_color_profile_type_t t)
{
  switch(t)
  {
    // wider than sRGB
    case DT_COLORSPACE_ADOBERGB:
    case DT_COLORSPACE_PROPHOTO_RGB:
    case DT_COLORSPACE_LIN_REC2020:
    case DT_COLORSPACE_PQ_REC2020:
    case DT_COLORSPACE_HLG_REC2020:
    case DT_COLORSPACE_PQ_P3:
    case DT_COLORSPACE_HLG_P3:
    case DT_COLORSPACE_DISPLAY_P3:
      return TRUE;

    // sRGB primaries (gamma may differ but gamut is the same)
    case DT_COLORSPACE_SRGB:
    case DT_COLORSPACE_REC709:
    case DT_COLORSPACE_LIN_REC709:
      return FALSE;

    // non-RGB / internal / pseudo profiles — treat as not wide-gamut
    case DT_COLORSPACE_NONE:
    case DT_COLORSPACE_FILE:
    case DT_COLORSPACE_XYZ:
    case DT_COLORSPACE_LAB:
    case DT_COLORSPACE_INFRARED:
    case DT_COLORSPACE_DISPLAY:
    case DT_COLORSPACE_DISPLAY2:
    case DT_COLORSPACE_EMBEDDED_ICC:
    case DT_COLORSPACE_EMBEDDED_MATRIX:
    case DT_COLORSPACE_STANDARD_MATRIX:
    case DT_COLORSPACE_ENHANCED_MATRIX:
    case DT_COLORSPACE_VENDOR_MATRIX:
    case DT_COLORSPACE_ALTERNATE_MATRIX:
    case DT_COLORSPACE_BRG:
    case DT_COLORSPACE_EXPORT:
    case DT_COLORSPACE_SOFTPROOF:
    case DT_COLORSPACE_WORK:
    case DT_COLORSPACE_LAST:
      return FALSE;
  }
  return FALSE;
}

// TRUE if the configured output profile is wider than sRGB (colors
// outside sRGB gamut will be clipped by the AI model which operates
// in sRGB internally); for "image settings" (NONE), resolves to the
// image's actual working profile when imgid is valid
static gboolean _output_profile_is_wide_gamut(dt_imgid_t imgid)
{
  const int icc_type = dt_conf_key_exists(CONF_ICC_TYPE)
    ? dt_conf_get_int(CONF_ICC_TYPE)
    : DT_COLORSPACE_NONE;

  if(icc_type == DT_COLORSPACE_NONE)
  {
    // fall back to image's working profile
    if(!dt_is_valid_imgid(imgid)) return FALSE;
    const dt_colorspaces_color_profile_t *work
      = dt_colorspaces_get_work_profile(imgid);
    return work ? _profile_type_is_wide_gamut(work->type) : FALSE;
  }

  if(icc_type == DT_COLORSPACE_FILE)
    return TRUE;  // unknown custom — be conservative

  return _profile_type_is_wide_gamut(icc_type);
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

  // gamut note (informational, not a warning): reuse the same info
  // line as the upscale size display. for denoise, shows standalone
  // in info_text_left; for upscale, appended to the size info
  if(_output_profile_is_wide_gamut(imgid))
  {
    const char *msg = (scale == 1)
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

  g_list_free(imgs);
  gtk_widget_queue_draw(d->preview_area);
}

static void _trigger_preview(dt_lib_module_t *self);
static void _cancel_preview(dt_lib_module_t *self);

static void _task_changed(dt_lib_neural_restore_t *d)
{
  d->model_available = _check_model_available(d, d->task);
  if(!d->model_available)
  {
    d->preview_ready = FALSE;
    d->preview_generating = FALSE;
    gtk_widget_queue_draw(d->preview_area);
  }

  // restore detail recovery slider from conf when switching to denoise,
  // reset to 0 when switching away (upscale has no detail recovery).
  // use _recovery_changing flag to avoid redundant conf writes from
  // the slider's value-changed callback
  d->recovery_changing = TRUE;
  if(d->task == NEURAL_TASK_DENOISE)
  {
    const float saved = dt_conf_get_float(CONF_DETAIL_RECOVERY);
    dt_bauhaus_slider_set(d->recovery_slider, saved);
  }
  else
  {
    dt_bauhaus_slider_set(d->recovery_slider, 0.0f);
  }
  d->recovery_changing = FALSE;

  _update_info_label(d);
  _update_button_sensitivity(d);
}

// rebuild the "after" cairo surface from cached float buffers, applying
// DWT-filtered detail recovery so that slider changes don't re-run inference
static void _rebuild_cairo_after(dt_lib_neural_restore_t *d)
{
  if(!d->preview_after || !d->cairo_after) return;

  const int w = d->preview_w;
  const int h = d->preview_h;
  const int stride = d->cairo_stride;
  const float alpha = dt_conf_get_float(CONF_DETAIL_RECOVERY) / 100.0f;
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
  g_idle_add(_preview_result_idle, result);

cleanup:
  g_free(pd);
  return NULL;
}

static void _cancel_preview(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_ready = FALSE;
  d->preview_generating = FALSE;
  g_atomic_int_inc(&d->preview_sequence);
  if(d->preview_thread)
  {
    g_thread_join(d->preview_thread);
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

static void _trigger_preview(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  if(!d->model_available || !d->preview_requested)
    return;

  // invalidate current preview and bump sequence so running thread exits early
  d->preview_ready = FALSE;
  g_atomic_int_inc(&d->preview_sequence);
  gtk_widget_queue_draw(d->preview_area);

  GList *imgs = dt_act_on_get_images(TRUE, FALSE, FALSE);
  if(!imgs) return;
  dt_imgid_t imgid = GPOINTER_TO_INT(imgs->data);
  g_list_free(imgs);

  if(!dt_is_valid_imgid(imgid)) return;

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
  // _trigger_preview joins the previous thread before starting,
  // and _cancel_preview joins before freeing export_pixels
  if(d->export_pixels)
  {
    pd->reuse_pixels = d->export_pixels;
    pd->reuse_w = d->export_w;
    pd->reuse_h = d->export_h;
  }
  // join previous preview thread before starting a new one
  if(d->preview_thread)
  {
    g_thread_join(d->preview_thread);
    d->preview_thread = NULL;
  }
  d->preview_thread = g_thread_new("neural_preview",
                                   _preview_thread, pd);
}

static void _update_task_from_ui(dt_lib_neural_restore_t *d)
{
  const int page = gtk_notebook_get_current_page(d->notebook);
  if(page == 0)
    d->task = NEURAL_TASK_DENOISE;
  else
  {
    const int scale_pos = dt_bauhaus_combobox_get(d->scale_combo);
    d->task = (scale_pos == 1) ? NEURAL_TASK_UPSCALE_4X : NEURAL_TASK_UPSCALE_2X;
  }
}

static void _notebook_page_changed(GtkNotebook *notebook,
                                   GtkWidget *page,
                                   guint page_num,
                                   dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  // switch-page fires before the page changes, so use page_num
  if(page_num == 0)
    d->task = NEURAL_TASK_DENOISE;
  else
  {
    const int scale_pos = dt_bauhaus_combobox_get(d->scale_combo);
    d->task = (scale_pos == 1) ? NEURAL_TASK_UPSCALE_4X : NEURAL_TASK_UPSCALE_2X;
  }

  dt_conf_set_int(CONF_ACTIVE_PAGE, page_num);
  _task_changed(d);
  if(d->preview_requested)
    _trigger_preview(self);
}

static void _scale_combo_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  _update_task_from_ui(d);
  _task_changed(d);
  if(d->preview_requested)
    _trigger_preview(self);
}

static void _recovery_slider_changed(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  if(d->recovery_changing) return;
  dt_conf_set_float(CONF_DETAIL_RECOVERY, dt_bauhaus_slider_get(d->recovery_slider));
  if(d->preview_ready)
  {
    _rebuild_cairo_after(d);
    gtk_widget_queue_draw(d->preview_area);
  }
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
  job_data->detail_recovery = dt_conf_get_float(CONF_DETAIL_RECOVERY);
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

    // draw crop rectangle at current patch_center
    const int task_scale = _task_scale(d->task);
    const int crop_w = w / task_scale;
    const int crop_h = h / task_scale;
    const double rw = (double)crop_w / d->export_w * img_w;
    const double rh = (double)crop_h / d->export_h * img_h;
    const double rx = ox + d->patch_center[0] * img_w - rw / 2.0;
    const double ry = oy + d->patch_center[1] * img_h - rh / 2.0;

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

    // convert click to normalized image coords, clamped so
    // the crop rectangle stays within the image
    const int task_scale = _task_scale(d->task);
    const float half_w = (float)w / task_scale / (2.0f * d->export_w);
    const float half_h = (float)h / task_scale / (2.0f * d->export_h);
    const float nx = (float)((ex - ox) / img_w);
    const float ny = (float)((ey - oy) / img_h);
    if(nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f)
      return TRUE;

    d->patch_center[0] = CLAMP(nx, half_w, 1.0f - half_w);
    d->patch_center[1] = CLAMP(ny, half_h, 1.0f - half_h);

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

    // clamp so the crop rectangle stays within the image
    const int task_scale = _task_scale(d->task);
    const float half_w = (float)w / task_scale / (2.0f * d->export_w);
    const float half_h = (float)h / task_scale / (2.0f * d->export_h);
    const float nx = CLAMP((float)((ex - ox) / img_w),
                           half_w, 1.0f - half_w);
    const float ny = CLAMP((float)((ey - oy) / img_h),
                           half_h, 1.0f - half_h);
    d->patch_center[0] = nx;
    d->patch_center[1] = ny;
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
  _update_info_label(d);
  _update_button_sensitivity(d);
}

static void _image_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;
  d->preview_requested = FALSE;
  _cancel_preview(self);
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
  d->split_pos = 0.5f;

  // notebook tabs (denoise / upscale)
  static dt_action_def_t notebook_def = {};
  d->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define(DT_ACTION(self), NULL, N_("page"),
                   GTK_WIDGET(d->notebook), &notebook_def);

  d->denoise_page = dt_ui_notebook_page(d->notebook, N_("denoise"),
                                        _("AI denoising"));
  d->upscale_page = dt_ui_notebook_page(d->notebook, N_("upscale"),
                                        _("AI upscaling"));

  // denoise page: detail recovery slider
  const float saved_recovery = dt_conf_get_float(CONF_DETAIL_RECOVERY);
  d->recovery_slider = dt_bauhaus_slider_new_action(DT_ACTION(self),
                                                    0.0f, 100.0f, 1.0f,
                                                    saved_recovery, 0);
  dt_bauhaus_widget_set_label(d->recovery_slider, NULL, N_("detail recovery"));
  dt_bauhaus_slider_set_format(d->recovery_slider, "%");
  gtk_widget_set_tooltip_text(d->recovery_slider,
                              _("recover fine texture lost during denoising "
                                "while suppressing noise"));
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

  // add to catalog
  GtkWidget *catalog_box = dt_gui_hbox();
  d->catalog_toggle = gtk_check_button_new_with_label(_("add to catalog"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->catalog_toggle),
                               dt_conf_key_exists(CONF_ADD_CATALOG)
                                 ? dt_conf_get_bool(CONF_ADD_CATALOG)
                                 : TRUE);
  gtk_widget_set_tooltip_text(d->catalog_toggle,
                              _("automatically import output image into the"
                                " darktable library"));
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

  _update_info_label(d);
  _update_button_sensitivity(d);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_neural_restore_t *d = (dt_lib_neural_restore_t *)self->data;

  DT_CONTROL_SIGNAL_DISCONNECT(_selection_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_image_changed_callback, self);
  DT_CONTROL_SIGNAL_DISCONNECT(_ai_models_changed_callback, self);

  if(d)
  {
    // signal preview thread to exit and wait for it
    g_atomic_int_inc(&d->preview_sequence);
    if(d->preview_thread)
    {
      g_thread_join(d->preview_thread);
      d->preview_thread = NULL;
    }

    g_free(d->preview_before);
    g_free(d->preview_after);
    dt_free_align(d->preview_detail);
    g_free(d->cairo_before);
    g_free(d->cairo_after);
    g_free(d->export_pixels);
    g_free(d->export_cairo);
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
