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

#include "ai/segmentation.h"
#include "common/ai_models.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/mipmap_cache.h"
#include "common/ras2vect.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "develop/pixelpipe_hb.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"

#include <math.h>
#include <string.h>

#define CONF_OBJECT_MODEL_KEY "plugins/darkroom/masks/object/model"
#define CONF_OBJECT_THRESHOLD_KEY "plugins/darkroom/masks/object/threshold"
#define CONF_OBJECT_REFINE_KEY "plugins/darkroom/masks/object/refine_passes"
#define CONF_OBJECT_MORPH_KEY "plugins/darkroom/masks/object/morph_radius"
#define CONF_OBJECT_EDGE_REFINE_KEY "plugins/darkroom/masks/object/edge_refine"
#define CONF_OBJECT_BRUSH_SIZE_KEY "plugins/darkroom/masks/object/brush_size"

// Target resolution for segmentation encoding (longest side in pixels).
// Matches the encoder input size (1024) — rendering higher just to
// downscale in preprocessing wastes pipeline time with no quality gain.
#define SEG_ENCODE_TARGET 1024

// --- Per-session segmentation state (stored in gui->scratchpad) ---

typedef enum _encode_state_t
{
  ENCODE_ERROR = -1,
  ENCODE_IDLE = 0,
  ENCODE_MSG_SHOWN = 1, // busy message queued, waiting for next expose
  ENCODE_READY = 2,     // encoding complete, results available
  ENCODE_RUNNING = 3,   // background thread in progress
} _encode_state_t;

// Minimum drag distance (preview pipe pixels) to distinguish click from brush stroke
#define DRAG_THRESHOLD 5.0f

typedef struct _object_data_t
{
  dt_ai_environment_t *env; // AI environment for model registry
  dt_seg_context_t *seg;    // SAM context (encoder+decoder)
  float *mask;              // current mask buffer (preview pipe size)
  int mask_w, mask_h;       // mask dimensions
  gboolean model_loaded;    // whether the model was loaded
  int encode_state;         // uses _encode_state_t values (atomic access)
  dt_imgid_t encoded_imgid; // image ID that was encoded
  int encode_w, encode_h;   // encoding resolution (for coordinate mapping)
  uint8_t *encode_rgb;      // stored RGB from encoding (uint8, HWC, 3ch)
  int encode_rgb_w, encode_rgb_h;
  guint modifier_poll_id;   // timer to detect shift key changes
  GThread *encode_thread;   // background encoding thread
  gboolean dragging;        // TRUE between press and release during click/brush drag
  float drag_start_x;       // press position (preview pipe pixel space)
  float drag_start_y;
  float drag_end_x;         // current drag position (updated in mouse_moved)
  float drag_end_y;
  // Brush state
  float brush_radius;               // normalized, 0..0.5 (fraction of MIN(iw,ih))
  gboolean brush_painting;          // TRUE during brush drag
  gboolean brush_used;              // TRUE after initial input — switches to +/- refinement mode
  dt_masks_dynbuf_t *brush_points;  // raw brush path (x,y pairs in preview space)
  int brush_points_count;
  // Vectorization preview (auto-updated after each decode)
  GList *preview_forms;             // GList of dt_masks_form_t* (mask-space pixel coords)
  GList *preview_signs;             // parallel GList of sign values ('+' or '-')
  int preview_cleanup;              // current cleanup (potrace turdsize, 0-100)
  float preview_smoothing;          // current smoothing (potrace alphamax, 0.0-1.3)
} _object_data_t;

static _object_data_t *_get_data(dt_masks_form_gui_t *gui)
{
  return (gui && gui->scratchpad) ? (_object_data_t *)gui->scratchpad : NULL;
}

// Free vectorized preview forms (never registered in dev->forms)
static void _free_preview_forms(_object_data_t *d)
{
  if(!d) return;
  for(GList *l = d->preview_forms; l; l = g_list_next(l))
    dt_masks_free_form(l->data);
  g_list_free(d->preview_forms);
  d->preview_forms = NULL;
  g_list_free(d->preview_signs);
  d->preview_signs = NULL;
}

// Free all resources in _object_data_t (must be called after thread has joined)
static void _destroy_data(_object_data_t *d)
{
  if(!d)
    return;
  if(d->modifier_poll_id)
    g_source_remove(d->modifier_poll_id);
  if(d->encode_thread)
    g_thread_join(d->encode_thread);
  if(d->seg)
    dt_seg_free(d->seg);
  if(d->env)
    dt_ai_env_destroy(d->env);
  g_free(d->mask);
  g_free(d->encode_rgb);
  if(d->brush_points)
    dt_masks_dynbuf_free(d->brush_points);
  _free_preview_forms(d);
  g_free(d);
}

// Idle callback for deferred cleanup when background thread was still running
static gboolean _deferred_cleanup(gpointer data)
{
  _object_data_t *d = data;
  const int state = g_atomic_int_get(&d->encode_state);
  if(state == ENCODE_RUNNING)
    return G_SOURCE_CONTINUE;
  _destroy_data(d);
  return G_SOURCE_REMOVE;
}

static void _free_data(dt_masks_form_gui_t *gui)
{
  _object_data_t *d = _get_data(gui);
  if(!d)
    return;
  gui->scratchpad = NULL;

  const int state = g_atomic_int_get(&d->encode_state);
  if(state == ENCODE_RUNNING)
  {
    // Thread still running — defer cleanup so we don't block the UI
    g_timeout_add(200, _deferred_cleanup, d);
    return;
  }
  _destroy_data(d);
}

// Data passed to the background encoding thread
typedef struct _encode_thread_data_t
{
  _object_data_t *d;
  dt_imgid_t imgid; // image to encode (thread renders via export pipe)
} _encode_thread_data_t;

// Background thread: loads model, renders image via export pipe, and encodes.
// Does ZERO GLib/GTK calls — only computation + atomic state set.
// The poll timer on the main thread detects completion.
static gpointer _encode_thread_func(gpointer data)
{
  _encode_thread_data_t *td = data;
  _object_data_t *d = td->d;
  const dt_imgid_t imgid = td->imgid;
  g_free(td);

  // Load model if needed
  if(!d->model_loaded)
  {
    if(!d->env)
      d->env = dt_ai_env_init(NULL);

    char *model_id = dt_ai_models_get_active_for_task("mask");
    d->seg = dt_seg_load(d->env, model_id);
    g_free(model_id);

    if(!d->seg)
    {
      g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
      return NULL;
    }
    d->model_loaded = TRUE;
  }

  // Render image at high resolution via temporary export pipeline
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(&buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  if(!buf.buf || !buf.width || !buf.height)
  {
    dt_print(DT_DEBUG_AI, "[object mask] Failed to get image buffer for encoding");
    dt_dev_cleanup(&dev);
    g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
    return NULL;
  }

  const int wd = dev.image_storage.width;
  const int ht = dev.image_storage.height;

  dt_dev_pixelpipe_t pipe;
  if(!dt_dev_pixelpipe_init_export(&pipe, wd, ht, IMAGEIO_RGB | IMAGEIO_INT8, FALSE))
  {
    dt_print(DT_DEBUG_AI, "[object mask] Failed to init export pipe for encoding");
    dt_mipmap_cache_release(&buf);
    dt_dev_cleanup(&dev);
    g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
    return NULL;
  }

  dt_dev_pixelpipe_set_icc(&pipe, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)buf.buf,
                             buf.width, buf.height, buf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, pipe.iwidth, pipe.iheight,
                                  &pipe.processed_width, &pipe.processed_height);

  const double scale = fmin((double)SEG_ENCODE_TARGET / (double)pipe.processed_width,
                            (double)SEG_ENCODE_TARGET / (double)pipe.processed_height);
  const double final_scale = fmin(scale, 1.0); // don't upscale
  const int out_w = (int)(final_scale * pipe.processed_width);
  const int out_h = (int)(final_scale * pipe.processed_height);

  dt_print(DT_DEBUG_AI, "[object mask] Rendering %dx%d (scale=%.3f) for encoding...",
           out_w, out_h, final_scale);

  dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, out_w, out_h, final_scale);

  // backbuf is float RGBA after process_no_gamma — convert to uint8 RGB for SAM
  uint8_t *rgb = NULL;
  if(pipe.backbuf)
  {
    const float *outbuf = (const float *)pipe.backbuf;
    rgb = g_try_malloc((size_t)out_w * out_h * 3);
    if(rgb)
    {
      for(size_t i = 0; i < (size_t)out_w * out_h; i++)
      {
        rgb[i * 3 + 0] = (uint8_t)CLAMP(outbuf[i * 4 + 0] * 255.0f + 0.5f, 0, 255);
        rgb[i * 3 + 1] = (uint8_t)CLAMP(outbuf[i * 4 + 1] * 255.0f + 0.5f, 0, 255);
        rgb[i * 3 + 2] = (uint8_t)CLAMP(outbuf[i * 4 + 2] * 255.0f + 0.5f, 0, 255);
      }
    }
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_mipmap_cache_release(&buf);
  dt_dev_cleanup(&dev);

  if(!rgb)
  {
    dt_print(DT_DEBUG_AI, "[object mask] Failed to render image for encoding");
    g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
    return NULL;
  }

  // Store encoding dimensions for coordinate mapping
  d->encode_w = out_w;
  d->encode_h = out_h;

  // Encode the image
  gboolean ok = dt_seg_encode_image(d->seg, rgb, out_w, out_h);

  // If accelerated encoding failed, fall back to CPU
  if(!ok)
  {
    dt_print(DT_DEBUG_AI, "[object mask] Encoding failed, retrying with CPU provider");
    dt_seg_free(d->seg);
    dt_ai_env_set_provider(d->env, DT_AI_PROVIDER_CPU);
    char *model_id = dt_ai_models_get_active_for_task("mask");
    d->seg = dt_seg_load(d->env, model_id);
    g_free(model_id);

    if(d->seg)
      ok = dt_seg_encode_image(d->seg, rgb, out_w, out_h);
    else
      d->model_loaded = FALSE;
  }

  // Store the RGB image for edge-aware mask refinement
  g_free(d->encode_rgb);
  d->encode_rgb = rgb;
  d->encode_rgb_w = out_w;
  d->encode_rgb_h = out_h;

  // Signal ready immediately so the user can start placing points.
  // The warmup below continues on this background thread — if the user
  // clicks before it finishes, ORT serializes concurrent Run() calls on
  // the same session, so the decode simply waits for the warmup to
  // complete first.  In practice, users need a moment to position their
  // cursor, so the ~1 s warmup usually finishes before the first click.
  g_atomic_int_set(&d->encode_state, ok ? ENCODE_READY : ENCODE_ERROR);

  // Warm up decoder with real encoder embeddings so the first user click
  // doesn't pay ORT's lazy-init + arena-sizing cost on the main thread.
  if(ok)
    dt_seg_warmup_decoder(d->seg);

  return NULL;
}

// Keep only the connected component containing the seed pixel (seed_x, seed_y).
// If the seed is outside any foreground region, keep the largest component instead.
// Operates in-place: non-selected foreground pixels are zeroed.
static void _keep_seed_component(float *mask, int w, int h, float threshold,
                                  int seed_x, int seed_y)
{
  const int npix = w * h;
  int16_t *labels = g_try_malloc0((size_t)npix * sizeof(int16_t));
  if(!labels)
    return;
  int *stack = g_try_malloc((size_t)npix * sizeof(int));
  if(!stack)
  {
    g_free(labels);
    return;
  }

  int16_t n_labels = 0;
  int16_t best_label = 0;
  int best_area = 0;
  int16_t seed_label = 0;

  for(int i = 0; i < npix; i++)
  {
    if(mask[i] <= threshold || labels[i] != 0)
      continue;
    if(n_labels >= INT16_MAX)
      break;

    n_labels++;
    const int16_t label = n_labels;
    int area = 0;
    int sp = 0;
    stack[sp++] = i;
    labels[i] = label;

    while(sp > 0)
    {
      const int p = stack[--sp];
      area++;
      const int px = p % w;
      const int py = p / w;

      if(px == seed_x && py == seed_y)
        seed_label = label;

      // 4-connected neighbors
      if(py > 0 && labels[p - w] == 0 && mask[p - w] > threshold)
      {
        labels[p - w] = label;
        stack[sp++] = p - w;
      }
      if(py < h - 1 && labels[p + w] == 0 && mask[p + w] > threshold)
      {
        labels[p + w] = label;
        stack[sp++] = p + w;
      }
      if(px > 0 && labels[p - 1] == 0 && mask[p - 1] > threshold)
      {
        labels[p - 1] = label;
        stack[sp++] = p - 1;
      }
      if(px < w - 1 && labels[p + 1] == 0 && mask[p + 1] > threshold)
      {
        labels[p + 1] = label;
        stack[sp++] = p + 1;
      }
    }

    if(area > best_area)
    {
      best_area = area;
      best_label = label;
    }
  }

  // Prefer component containing the seed point; fall back to largest
  const int16_t keep = (seed_label > 0) ? seed_label : best_label;

  if(keep > 0)
  {
    for(int i = 0; i < npix; i++)
    {
      if(mask[i] > threshold && labels[i] != keep)
        mask[i] = 0.0f;
    }
  }

  g_free(stack);
  g_free(labels);
}

// Morphological erode: output pixel is 1 only if all pixels in the
// square structuring element of given radius are 1.
static void _morph_erode(const uint8_t *src, uint8_t *dst, int w, int h, int radius)
{
  for(int y = 0; y < h; y++)
  {
    const int y0 = MAX(y - radius, 0);
    const int y1 = MIN(y + radius, h - 1);
    for(int x = 0; x < w; x++)
    {
      const int x0 = MAX(x - radius, 0);
      const int x1 = MIN(x + radius, w - 1);
      uint8_t val = 1;
      for(int ny = y0; ny <= y1 && val; ny++)
        for(int nx = x0; nx <= x1 && val; nx++)
          if(!src[ny * w + nx]) val = 0;
      dst[y * w + x] = val;
    }
  }
}

// Morphological dilate: output pixel is 1 if any pixel in the
// square structuring element of given radius is 1.
static void _morph_dilate(const uint8_t *src, uint8_t *dst, int w, int h, int radius)
{
  for(int y = 0; y < h; y++)
  {
    const int y0 = MAX(y - radius, 0);
    const int y1 = MIN(y + radius, h - 1);
    for(int x = 0; x < w; x++)
    {
      const int x0 = MAX(x - radius, 0);
      const int x1 = MIN(x + radius, w - 1);
      uint8_t val = 0;
      for(int ny = y0; ny <= y1 && !val; ny++)
        for(int nx = x0; nx <= x1 && !val; nx++)
          if(src[ny * w + nx]) val = 1;
      dst[y * w + x] = val;
    }
  }
}

// Morphological open+close on a float mask.
// Open (erode->dilate) removes small protrusions/bridges.
// Close (dilate->erode) fills small holes/gaps.
static void _morph_open_close(float *mask, int w, int h, float threshold, int radius)
{
  if(radius <= 0) return;

  const size_t n = (size_t)w * h;
  uint8_t *bin = g_try_malloc(n);
  uint8_t *tmp = g_try_malloc(n);
  if(!bin || !tmp)
  {
    g_free(bin);
    g_free(tmp);
    return;
  }

  // Binarize
  for(size_t i = 0; i < n; i++)
    bin[i] = mask[i] > threshold ? 1 : 0;

  // Open: erode into tmp, then dilate back into bin
  _morph_erode(bin, tmp, w, h, radius);
  _morph_dilate(tmp, bin, w, h, radius);

  // Close: dilate into tmp, then erode back into bin
  _morph_dilate(bin, tmp, w, h, radius);
  _morph_erode(tmp, bin, w, h, radius);

  // Apply result back to float mask
  for(size_t i = 0; i < n; i++)
  {
    if(bin[i] && mask[i] <= threshold)
      mask[i] = 1.0f;  // filled by close
    else if(!bin[i] && mask[i] > threshold)
      mask[i] = 0.0f;  // removed by open
  }

  g_free(bin);
  g_free(tmp);
}

// Edge-aware threshold refinement: near strong image edges the binarization
// threshold is raised by up to edge_boost, snapping the mask boundary to
// actual object contours.  Uses Scharr gradient of the stored RGB image.
static void _edge_refine_threshold(float *mask, int mw, int mh,
                                    const uint8_t *rgb, int rgb_w, int rgb_h,
                                    float base_threshold, float edge_boost)
{
  if(edge_boost <= 0.0f || !rgb || rgb_w < 3 || rgb_h < 3)
    return;
  if(mw != rgb_w || mh != rgb_h)
    return;

  const size_t npix = (size_t)mw * mh;

  // Step 1: Convert uint8 RGB to float luminance (Rec.601)
  float *lum = g_try_malloc(npix * sizeof(float));
  if(!lum) return;

  for(size_t i = 0; i < npix; i++)
    lum[i] = (0.299f * (float)rgb[i * 3]
            + 0.587f * (float)rgb[i * 3 + 1]
            + 0.114f * (float)rgb[i * 3 + 2]) / 255.0f;

  // Step 2: Compute Scharr gradient magnitude, track max for normalization
  float *grad = g_try_malloc(npix * sizeof(float));
  if(!grad)
  {
    g_free(lum);
    return;
  }

  float grad_max = 0.0f;

  for(int y = 0; y < mh; y++)
  {
    for(int x = 0; x < mw; x++)
    {
      float g = 0.0f;
      if(y >= 1 && y < mh - 1 && x >= 1 && x < mw - 1)
      {
        const float *p = &lum[y * mw + x];
        const float gx = (47.0f / 255.0f) * (p[-mw - 1] - p[-mw + 1]
                                             + p[mw - 1] - p[mw + 1])
                        + (162.0f / 255.0f) * (p[-1] - p[1]);
        const float gy = (47.0f / 255.0f) * (p[-mw - 1] - p[mw - 1]
                                             + p[-mw + 1] - p[mw + 1])
                        + (162.0f / 255.0f) * (p[-mw] - p[mw]);
        g = sqrtf(gx * gx + gy * gy);
      }
      grad[y * mw + x] = g;
      if(g > grad_max) grad_max = g;
    }
  }

  g_free(lum);

  // Step 3: Normalize and apply spatially-varying threshold
  const float inv_max = (grad_max > 1e-6f) ? 1.0f / grad_max : 0.0f;

  for(size_t i = 0; i < npix; i++)
  {
    const float g_norm = grad[i] * inv_max;
    const float effective_thresh = base_threshold + edge_boost * g_norm;
    mask[i] = (mask[i] > effective_thresh) ? 1.0f : 0.0f;
  }

  g_free(grad);
}

// Resample a raw brush path into evenly-spaced foreground points using
// arc-length parameterization and add them to gui->guipoints.
// brush_pts: x,y pairs in preview pipe space, n_pts: number of points.
static void _resample_brush_to_points(dt_masks_form_gui_t *gui,
                                       const float *brush_pts,
                                       const int n_pts)
{
  if(n_pts < 2) return;

  // Compute total arc length
  float total_len = 0.0f;
  for(int i = 1; i < n_pts; i++)
  {
    const float dx = brush_pts[i * 2] - brush_pts[(i - 1) * 2];
    const float dy = brush_pts[i * 2 + 1] - brush_pts[(i - 1) * 2 + 1];
    total_len += sqrtf(dx * dx + dy * dy);
  }

  if(total_len < 1.0f)
  {
    // Degenerate stroke — just add the first point
    dt_masks_dynbuf_reset(gui->guipoints);
    dt_masks_dynbuf_reset(gui->guipoints_payload);
    gui->guipoints_count = 0;
    dt_masks_dynbuf_add_2(gui->guipoints, brush_pts[0], brush_pts[1]);
    dt_masks_dynbuf_add(gui->guipoints_payload, 1.0f);
    gui->guipoints_count++;
    return;
  }

  // Target N points: one per brush diameter, clamped to [3, 32]
  _object_data_t *d = _get_data(gui);
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  const float brush_diam = 2.0f * (d ? d->brush_radius : 0.03f) * MIN(iwidth, iheight);
  const int n_target = CLAMP((int)(total_len / MAX(brush_diam, 1.0f)), 3, 32);

  const float step = total_len / (float)(n_target - 1);
  float accum = 0.0f;

  // Reset guipoints for brush output
  dt_masks_dynbuf_reset(gui->guipoints);
  dt_masks_dynbuf_reset(gui->guipoints_payload);
  gui->guipoints_count = 0;

  // Always emit first point
  dt_masks_dynbuf_add_2(gui->guipoints, brush_pts[0], brush_pts[1]);
  dt_masks_dynbuf_add(gui->guipoints_payload, 1.0f);
  gui->guipoints_count++;

  float next_emit = step;
  accum = 0.0f;

  for(int i = 1; i < n_pts && gui->guipoints_count < n_target - 1; i++)
  {
    const float x0 = brush_pts[(i - 1) * 2];
    const float y0 = brush_pts[(i - 1) * 2 + 1];
    const float x1 = brush_pts[i * 2];
    const float y1 = brush_pts[i * 2 + 1];
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float seg_len = sqrtf(dx * dx + dy * dy);

    if(seg_len < 1e-6f) continue;

    float seg_pos = 0.0f;  // position within this segment

    while(seg_pos < seg_len && gui->guipoints_count < n_target - 1)
    {
      const float remaining = next_emit - accum;
      if(seg_pos + remaining <= seg_len)
      {
        // Emit a point within this segment
        seg_pos += remaining;
        accum += remaining;
        const float t = seg_pos / seg_len;
        const float px = x0 + t * dx;
        const float py = y0 + t * dy;
        dt_masks_dynbuf_add_2(gui->guipoints, px, py);
        dt_masks_dynbuf_add(gui->guipoints_payload, 1.0f);
        gui->guipoints_count++;
        next_emit += step;
      }
      else
      {
        // Rest of segment doesn't reach next emit point
        accum += (seg_len - seg_pos);
        break;
      }
    }
  }

  // Always emit last point
  dt_masks_dynbuf_add_2(gui->guipoints,
                         brush_pts[(n_pts - 1) * 2],
                         brush_pts[(n_pts - 1) * 2 + 1]);
  dt_masks_dynbuf_add(gui->guipoints_payload, 1.0f);
  gui->guipoints_count++;
}

// Run the decoder with accumulated points and update the cached mask
static void _run_decoder(dt_masks_form_gui_t *gui)
{
  _object_data_t *d = _get_data(gui);
  if(!d || !d->seg || !dt_seg_is_encoded(d->seg))
    return;
  if(gui->guipoints_count <= 0)
    return;

  dt_gui_cursor_set_busy();

  const float *gp = dt_masks_dynbuf_buffer(gui->guipoints);
  const float *gpp = dt_masks_dynbuf_buffer(gui->guipoints_payload);

  // Points are stored in preview pipe pixel space — scale to encoding space
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  const float sx = (wd > 0) ? (float)d->encode_w / wd : 1.0f;
  const float sy = (ht > 0) ? (float)d->encode_h / ht : 1.0f;

  // Always send all accumulated points.  On the first click reset the
  // previous mask; on subsequent clicks keep it so the decoder gets
  // both all points AND the previous mask as boundary context.
  // After brush decode, prev_mask carries brush context — don't reset it.
  const int n_prompt_points = gui->guipoints_count;
  if(gui->guipoints_count <= 1 && !d->brush_used)
    dt_seg_reset_prev_mask(d->seg);

  dt_seg_point_t *points = g_new(dt_seg_point_t, n_prompt_points);
  for(int i = 0; i < n_prompt_points; i++)
  {
    points[i].x = gp[i * 2 + 0] * sx;
    points[i].y = gp[i * 2 + 1] * sy;
    points[i].label = (int)gpp[i];
  }

  // Find seed point for connected component filter:
  // always search ALL accumulated points (not just prompt points)
  int seed_x = -1, seed_y = -1;
  for(int i = gui->guipoints_count - 1; i >= 0; i--)
  {
    const int label = (int)gpp[i];
    if(label == 1)
    {
      seed_x = (int)(gp[i * 2 + 0] * sx);
      seed_y = (int)(gp[i * 2 + 1] * sy);
      break;
    }
  }

  // Multi-pass iterative refinement: run decoder multiple times,
  // feeding back the low-res mask each time to tighten boundaries.
  const int n_passes = CLAMP(dt_conf_get_int(CONF_OBJECT_REFINE_KEY), 1, 5);
  int mw = 0, mh = 0;
  float *mask = NULL;

  for(int pass = 0; pass < n_passes; pass++)
  {
    float *new_mask = dt_seg_compute_mask(d->seg, points, n_prompt_points, &mw, &mh);
    if(!new_mask)
      break;
    g_free(mask);
    mask = new_mask;
  }
  g_free(points);

  if(mask)
  {
    // Remove disconnected blobs: keep only the component at the seed point
    seed_x = CLAMP(seed_x, 0, mw - 1);
    seed_y = CLAMP(seed_y, 0, mh - 1);
    const float threshold = CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY), 0.3f, 0.9f);

    // Edge-aware threshold refinement: snap mask boundary to image edges
    const float edge_boost = CLAMP(dt_conf_get_float(CONF_OBJECT_EDGE_REFINE_KEY), 0.0f, 0.5f);
    if(edge_boost > 0.0f && d->encode_rgb)
      _edge_refine_threshold(mask, mw, mh,
                             d->encode_rgb, d->encode_rgb_w, d->encode_rgb_h,
                             threshold, edge_boost);

    _keep_seed_component(mask, mw, mh, threshold, seed_x, seed_y);

    // Morphological open+close to remove small protrusions and fill holes
    const int morph_radius = CLAMP(dt_conf_get_int(CONF_OBJECT_MORPH_KEY), 0, 5);
    _morph_open_close(mask, mw, mh, threshold, morph_radius);

    g_free(d->mask);
    d->mask = mask;
    d->mask_w = mw;
    d->mask_h = mh;
  }
  dt_gui_cursor_clear_busy();
}

// Run vectorization with current preview parameters, store result in scratchpad.
// Called automatically after each decode and on scroll parameter changes.
static void _update_preview(_object_data_t *d)
{
  _free_preview_forms(d);
  if(!d->mask || d->mask_w <= 0 || d->mask_h <= 0)
    return;

  const size_t n = (size_t)d->mask_w * d->mask_h;
  float *inv_mask = g_try_malloc(n * sizeof(float));
  if(!inv_mask) return;

  for(size_t i = 0; i < n; i++)
    inv_mask[i] = 1.0f - d->mask[i];

  d->preview_forms = ras2forms(inv_mask, d->mask_w, d->mask_h, NULL,
                               d->preview_cleanup, (double)d->preview_smoothing,
                               &d->preview_signs);
  g_free(inv_mask);
}

// Transform mask-space forms to input-normalized coords and register them.
// Takes ownership of `forms` and `signs` lists (forms are appended to dev->forms).
static dt_masks_form_t *
_register_vectorized_forms(dt_iop_module_t *module,
                           GList *forms, GList *signs,
                           int mask_w, int mask_h)
{
  (void)module;

  // darktable mask coordinates are stored in input-image-normalized space:
  //   coord = backtransform(backbuf_pixel) / iwidth
  // This undoes all geometric pipeline transforms (crop, rotation, lens, etc.)
  // so that the mask can be applied at any point in the pipeline.
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);

  // Vectorized coordinates are in mask space (encoding resolution).
  // dt_dev_distort_backtransform expects preview pipe pixel space.
  const float msx = (mask_w > 0) ? wd / (float)mask_w : 1.0f;
  const float msy = (mask_h > 0) ? ht / (float)mask_h : 1.0f;

  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    const int npts = g_list_length(f->points);
    if(npts == 0)
      continue;

    // Collect all coordinates into a flat array for batch backtransform.
    // Each path point has 3 coordinate pairs: corner, ctrl1, ctrl2.
    float *pts = g_new(float, npts * 6);
    int i = 0;
    for(GList *p = f->points; p; p = g_list_next(p))
    {
      dt_masks_point_path_t *pt = p->data;
      pts[i++] = pt->corner[0];
      pts[i++] = pt->corner[1];
      pts[i++] = pt->ctrl1[0];
      pts[i++] = pt->ctrl1[1];
      pts[i++] = pt->ctrl2[0];
      pts[i++] = pt->ctrl2[1];
    }

    // Scale from mask space (encoding resolution) to preview pipe space
    for(int j = 0; j < npts * 6; j += 2)
    {
      pts[j + 0] *= msx;
      pts[j + 1] *= msy;
    }

    dt_dev_distort_backtransform(darktable.develop, pts, npts * 3);

    // Write back and normalize by input image dimensions
    i = 0;
    for(GList *p = f->points; p; p = g_list_next(p))
    {
      dt_masks_point_path_t *pt = p->data;
      pt->corner[0] = pts[i++] / iwidth;
      pt->corner[1] = pts[i++] / iheight;
      pt->ctrl1[0] = pts[i++] / iwidth;
      pt->ctrl1[1] = pts[i++] / iheight;
      pt->ctrl2[0] = pts[i++] / iwidth;
      pt->ctrl2[1] = pts[i++] / iheight;
    }
    g_free(pts);
  }

  const int nbform = g_list_length(forms);
  if(nbform == 0)
  {
    g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
    g_list_free(signs);
    dt_control_log(_("no mask extracted from AI segmentation"));
    return NULL;
  }

  // Always wrap paths in a group — holes use difference mode

  // Count existing AI object groups/paths for numbering
  dt_develop_t *dev = darktable.develop;
  guint grp_nb = 0;
  guint path_nb = 0;
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(strncmp(f->name, "ai object group", 15) == 0)
      grp_nb++;
    if(strncmp(f->name, "ai object #", 11) == 0)
      path_nb++;
  }
  grp_nb++;
  path_nb++;
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    snprintf(f->name, sizeof(f->name), _("ai object #%d"), (int)path_nb++);
  }

  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  snprintf(grp->name, sizeof(grp->name), _("ai object group #%d"), (int)grp_nb);

  // Register all path forms so they exist in dev->forms
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    dev->forms = g_list_append(dev->forms, f);
  }

  // Add each path to the group; holes get difference mode
  GList *s = signs;
  for(GList *l = forms; l; l = g_list_next(l), s = s ? g_list_next(s) : NULL)
  {
    dt_masks_form_t *f = l->data;
    const int sign = s ? GPOINTER_TO_INT(s->data) : '+';
    dt_masks_point_group_t *grpt = dt_masks_group_add_form(grp, f);
    if(grpt && sign == '-')
    {
      grpt->state = (grpt->state & ~DT_MASKS_STATE_UNION) | DT_MASKS_STATE_DIFFERENCE;
    }
  }

  // Register the group (history item added by caller after blend mask assignment)
  dev->forms = g_list_append(dev->forms, grp);

  g_list_free(forms);
  g_list_free(signs);

  dt_print(DT_DEBUG_AI, "[object mask] created %d paths", nbform);
  return grp;
}

// Finalize using cached preview forms (steals ownership from scratchpad).
static dt_masks_form_t *
_finalize_from_preview(dt_iop_module_t *module, dt_masks_form_gui_t *gui)
{
  _object_data_t *d = _get_data(gui);
  if(!d || !d->preview_forms)
    return NULL;

  GList *forms = d->preview_forms;
  GList *signs = d->preview_signs;
  const int mw = d->mask_w;
  const int mh = d->mask_h;
  d->preview_forms = NULL;
  d->preview_signs = NULL;

  return _register_vectorized_forms(module, forms, signs, mw, mh);
}

// Finalize: vectorize the mask and register as a group of path forms.
// Fallback when no preview forms are available.
static dt_masks_form_t *
_finalize_mask(dt_iop_module_t *module, dt_masks_form_t *form, dt_masks_form_gui_t *gui)
{
  (void)form;
  _object_data_t *d = _get_data(gui);
  if(!d || !d->mask)
    return NULL;

  const size_t n = (size_t)d->mask_w * d->mask_h;
  float *inv_mask = g_try_malloc(n * sizeof(float));
  if(!inv_mask)
    return NULL;

  for(size_t i = 0; i < n; i++)
    inv_mask[i] = 1.0f - d->mask[i];

  const int cleanup = dt_conf_get_int("plugins/darkroom/masks/object/cleanup");
  const float smoothing = dt_conf_get_float("plugins/darkroom/masks/object/smoothing");
  GList *signs = NULL;
  GList *forms
    = ras2forms(inv_mask, d->mask_w, d->mask_h, NULL, cleanup, (double)smoothing, &signs);
  g_free(inv_mask);

  return _register_vectorized_forms(module, forms, signs, d->mask_w, d->mask_h);
}

// --- Mask Event Handlers ---

static void _object_get_distance(
  const float x,
  const float y,
  const float as,
  dt_masks_form_gui_t *gui,
  const int index,
  const int num_points,
  gboolean *inside,
  gboolean *inside_border,
  int *near,
  gboolean *inside_source,
  float *dist)
{
  (void)x;
  (void)y;
  (void)as;
  (void)gui;
  (void)index;
  (void)num_points;
  (void)inside;
  (void)inside_border;
  (void)near;
  (void)inside_source;
  (void)dist;
}

static int _object_events_mouse_scrolled(
  dt_iop_module_t *module,
  const float pzx,
  const float pzy,
  const gboolean up,
  const uint32_t state,
  dt_masks_form_t *form,
  const dt_imgid_t parentid,
  dt_masks_form_gui_t *gui,
  const int index)
{
  _object_data_t *d = _get_data(gui);

  // Brush size control (plain scroll, before brush is used)
  if(gui->creation && d && !d->brush_used && dt_modifier_is(state, 0))
  {
    const float val = dt_conf_get_float(CONF_OBJECT_BRUSH_SIZE_KEY);
    const float new_val = dt_masks_change_size(up, val, 0.005f, 0.5f);
    dt_conf_set_float(CONF_OBJECT_BRUSH_SIZE_KEY, new_val);
    d->brush_radius = new_val;
    dt_toast_log(_("size: %3.2f%%"), new_val * 2.0f * 100.0f);
    dt_dev_masks_list_change(darktable.develop);
    dt_control_queue_redraw_center();
    return 1;
  }

  // Vectorization parameter adjustment (after brush is used)
  if(gui->creation && d && d->brush_used && d->mask)
  {
    if(dt_modifier_is(state, 0))
    {
      // Plain scroll: adjust cleanup (potrace turdsize)
      d->preview_cleanup = CLAMP(d->preview_cleanup + (up ? 5 : -5), 0, 100);
      dt_conf_set_int("plugins/darkroom/masks/object/cleanup", d->preview_cleanup);
      _update_preview(d);
      dt_toast_log(_("cleanup: %d"), d->preview_cleanup);
      dt_dev_masks_list_change(darktable.develop);
      dt_control_queue_redraw_center();
      return 1;
    }
    if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      // Shift+scroll: adjust smoothing (potrace alphamax)
      d->preview_smoothing = CLAMP(d->preview_smoothing + (up ? 0.05f : -0.05f), 0.0f, 1.3f);
      dt_conf_set_float("plugins/darkroom/masks/object/smoothing", d->preview_smoothing);
      _update_preview(d);
      dt_toast_log(_("smoothing: %3.2f"), d->preview_smoothing);
      dt_dev_masks_list_change(darktable.develop);
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  // Opacity control (ctrl+scroll)
  if(gui->creation && dt_modifier_is(state, GDK_CONTROL_MASK))
  {
    float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    opacity = CLAMP(opacity + (up ? 0.05f : -0.05f), 0.05f, 1.0f);
    dt_conf_set_float("plugins/darkroom/masks/opacity", opacity);
    dt_toast_log(_("opacity: %d%%"), (int)(opacity * 100.0f));
    dt_dev_masks_list_change(darktable.develop);
    dt_control_queue_redraw_center();
    return 1;
  }
  return 0;
}

// Clear accumulated points, mask preview, and iterative refinement state
static void _clear_selection(dt_masks_form_gui_t *gui)
{
  _object_data_t *d = _get_data(gui);
  if(!d)
    return;

  if(gui->guipoints)
    dt_masks_dynbuf_reset(gui->guipoints);
  if(gui->guipoints_payload)
    dt_masks_dynbuf_reset(gui->guipoints_payload);
  gui->guipoints_count = 0;

  g_free(d->mask);
  d->mask = NULL;
  d->mask_w = d->mask_h = 0;

  if(d->seg)
    dt_seg_reset_prev_mask(d->seg);

  // Reset brush and preview state
  d->brush_used = FALSE;
  d->brush_painting = FALSE;
  d->brush_points_count = 0;
  if(d->brush_points)
    dt_masks_dynbuf_reset(d->brush_points);
  _free_preview_forms(d);

  dt_control_queue_redraw_center();
}

static int _object_events_button_pressed(
  dt_iop_module_t *module,
  float pzx,
  float pzy,
  const double pressure,
  const int which,
  const int type,
  const uint32_t state,
  dt_masks_form_t *form,
  const dt_imgid_t parentid,
  dt_masks_form_gui_t *gui,
  const int index)
{
  (void)pressure;
  (void)parentid;
  (void)index;
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS)
    return 1;
  if(!gui)
    return 0;

  _object_data_t *d = _get_data(gui);

  if(gui->creation && which == 1 && dt_modifier_is(state, GDK_MOD1_MASK))
  {
    // Alt+click: clear selection
    if(d && d->encode_state == ENCODE_READY
       && (gui->guipoints_count > 0 || d->mask || d->brush_used))
      _clear_selection(gui);
    return 1;
  }
  else if(gui->creation && which == 1)
  {
    // need valid scratchpad and completed encoding
    if(!d || d->encode_state != ENCODE_READY)
      return 1;

    // Dismiss the "ready" hint now that the user is interacting
    dt_control_log_ack_all();

    // Start drag tracking — actual point/brush/click is resolved on button release
    float wd, ht, iwidth, iheight;
    dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);

    d->dragging = TRUE;
    d->drag_start_x = pzx * wd;
    d->drag_start_y = pzy * ht;
    d->drag_end_x = d->drag_start_x;
    d->drag_end_y = d->drag_start_y;

    if(!d->brush_used && !dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      d->brush_painting = TRUE;
      if(!d->brush_points)
        d->brush_points = dt_masks_dynbuf_init(200, "object brush_points");
      else
        dt_masks_dynbuf_reset(d->brush_points);
      d->brush_points_count = 0;
      dt_masks_dynbuf_add_2(d->brush_points, d->drag_start_x, d->drag_start_y);
      d->brush_points_count++;
      if(d->brush_radius <= 0.0f)
        d->brush_radius = dt_conf_get_float(CONF_OBJECT_BRUSH_SIZE_KEY);
    }
    return 1;
  }
  else if(gui->creation && which == 3)
  {
    // Don't exit while background threads are running
    if(d && g_atomic_int_get(&d->encode_state) == ENCODE_RUNNING)
      return 1;

    // Right-click: finalize mask (prefer cached preview forms)
    dt_masks_form_t *new_grp = NULL;
    if(d && d->preview_forms)
      new_grp = _finalize_from_preview(module, gui);
    else if(gui->guipoints_count > 0)
      new_grp = _finalize_mask(module, form, gui);

    // Add the new group to the module's blend mask group
    if(new_grp)
    {
      dt_develop_t *dev = darktable.develop;
      if(module)
      {
        dt_masks_form_t *mod_grp
          = dt_masks_get_from_id(dev, module->blend_params->mask_id);
        if(!mod_grp)
        {
          mod_grp = dt_masks_create(DT_MASKS_GROUP);
          gchar *module_label = dt_history_item_get_name(module);
          snprintf(mod_grp->name, sizeof(mod_grp->name), _("group '%s'"), module_label);
          g_free(module_label);
          dev->forms = g_list_append(dev->forms, mod_grp);
          module->blend_params->mask_id = mod_grp->formid;
        }
        dt_masks_point_group_t *grpt = dt_masks_group_add_form(mod_grp, new_grp);
        if(grpt)
          grpt->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
      }
      dt_dev_add_masks_history_item(dev, module, TRUE);
    }

    // Cleanup and exit creation mode
    gui->creation = FALSE;
    gui->creation_continuous = FALSE;
    gui->creation_continuous_module = NULL;

    _free_data(gui);

    dt_masks_dynbuf_free(gui->guipoints);
    dt_masks_dynbuf_free(gui->guipoints_payload);
    gui->guipoints = NULL;
    gui->guipoints_payload = NULL;
    gui->guipoints_count = 0;

    dt_control_hinter_message("");

    // Exit creation mode and select the new group.
    // dt_masks_set_edit_mode requires a non-NULL module (it returns
    // immediately otherwise), so clear the form directly when
    // module is NULL (standalone mask creation).
    if(module)
    {
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);
    }
    else
    {
      dt_masks_change_form_gui(NULL);
    }
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static int _object_events_button_released(
  dt_iop_module_t *module,
  const float pzx,
  const float pzy,
  const int which,
  const uint32_t state,
  dt_masks_form_t *form,
  const dt_imgid_t parentid,
  dt_masks_form_gui_t *gui,
  const int index)
{
  (void)module;
  (void)pzx;
  (void)pzy;
  (void)form;
  (void)parentid;
  (void)index;

  if(!gui || which != 1)
    return 0;

  _object_data_t *d = _get_data(gui);
  if(!d || !d->dragging)
    return 0;

  d->dragging = FALSE;
  const gboolean was_brush_painting = d->brush_painting;
  d->brush_painting = FALSE;

  if(!gui->guipoints)
    gui->guipoints = dt_masks_dynbuf_init(200000, "object guipoints");
  if(!gui->guipoints)
    return 1;
  if(!gui->guipoints_payload)
    gui->guipoints_payload = dt_masks_dynbuf_init(100000, "object guipoints_payload");
  if(!gui->guipoints_payload)
    return 1;

  const float dx = d->drag_end_x - d->drag_start_x;
  const float dy = d->drag_end_y - d->drag_start_y;
  const float dist = sqrtf(dx * dx + dy * dy);

  if(was_brush_painting && dist >= DRAG_THRESHOLD
     && d->brush_points && d->brush_points_count >= 2)
  {
    // Brush stroke: resample path into evenly-spaced foreground points
    _resample_brush_to_points(gui,
                              dt_masks_dynbuf_buffer(d->brush_points),
                              d->brush_points_count);
    d->brush_used = TRUE;
  }
  else
  {
    // Short click: single point (foreground or background)
    const float label = dt_modifier_is(state, GDK_SHIFT_MASK) ? 0.0f : 1.0f;
    dt_masks_dynbuf_add_2(gui->guipoints, d->drag_start_x, d->drag_start_y);
    dt_masks_dynbuf_add(gui->guipoints_payload, label);
    gui->guipoints_count++;
    // A short click in brush mode (no shift) counts as a completed brush stroke
    if(was_brush_painting)
      d->brush_used = TRUE;
  }

  if(d->brush_points)
    dt_masks_dynbuf_reset(d->brush_points);
  d->brush_points_count = 0;

  _run_decoder(gui);

  // Auto-update vectorization preview after each decode
  if(d->mask)
    _update_preview(d);

  dt_control_queue_redraw_center();
  return 1;
}

static int _object_events_mouse_moved(
  dt_iop_module_t *module,
  const float pzx,
  const float pzy,
  const double pressure,
  const int which,
  const float zoom_scale,
  dt_masks_form_t *form,
  const dt_imgid_t parentid,
  dt_masks_form_gui_t *gui,
  const int index)
{
  (void)module;
  (void)pressure;
  (void)which;
  (void)zoom_scale;
  (void)form;
  (void)parentid;
  (void)index;

  if(!gui)
    return 0;

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;

  if(gui->creation)
  {
    _object_data_t *d = _get_data(gui);

    // Track drag position and collect brush path points
    if(d && d->dragging)
    {
      float wd, ht, iwidth, iheight;
      dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
      d->drag_end_x = pzx * wd;
      d->drag_end_y = pzy * ht;

      if(d->brush_painting && d->brush_points)
      {
        dt_masks_dynbuf_add_2(d->brush_points, d->drag_end_x, d->drag_end_y);
        d->brush_points_count++;
      }
    }

    dt_control_queue_redraw_center();
  }

  return 1;
}

// Timer callback: periodically redraw center so +/- cursor tracks shift key
static gboolean _modifier_poll(gpointer data)
{
  (void)data;
  dt_control_queue_redraw_center();
  return G_SOURCE_CONTINUE;
}

static void _object_events_post_expose(
  cairo_t *cr,
  const float zoom_scale,
  dt_masks_form_gui_t *gui,
  const int index,
  const int num_points)
{
  (void)index;
  (void)num_points;
  if(!gui)
    return;
  if(!gui->creation)
    return;

  // Ensure scratchpad exists
  _object_data_t *d = _get_data(gui);
  if(!d)
  {
    d = g_new0(_object_data_t, 1);
    d->brush_radius = dt_conf_get_float(CONF_OBJECT_BRUSH_SIZE_KEY);
    d->preview_cleanup = dt_conf_get_int("plugins/darkroom/masks/object/cleanup");
    d->preview_smoothing = dt_conf_get_float("plugins/darkroom/masks/object/smoothing");
    gui->scratchpad = d;
  }

  // Detect image change: reset encoding if we switched to a different image
  const dt_imgid_t cur_imgid = darktable.develop->image_storage.id;
  const int cur_state = g_atomic_int_get(&d->encode_state);
  if((cur_state == ENCODE_READY || cur_state == ENCODE_ERROR)
     && d->encoded_imgid != cur_imgid)
  {
    if(d->encode_thread)
    {
      g_thread_join(d->encode_thread);
      d->encode_thread = NULL;
    }
    if(d->seg)
      dt_seg_reset_encoding(d->seg);
    g_free(d->mask);
    d->mask = NULL;
    d->mask_w = d->mask_h = 0;
    d->encode_w = d->encode_h = 0;
    g_free(d->encode_rgb);
    d->encode_rgb = NULL;
    d->encode_rgb_w = d->encode_rgb_h = 0;
    d->encode_state = ENCODE_IDLE;
    // Reset brush, preview, and point state so the new image starts fresh
    d->brush_used = FALSE;
    d->brush_painting = FALSE;
    d->brush_points_count = 0;
    if(d->brush_points)
      dt_masks_dynbuf_reset(d->brush_points);
    _free_preview_forms(d);
    if(gui->guipoints)
      dt_masks_dynbuf_reset(gui->guipoints);
    if(gui->guipoints_payload)
      dt_masks_dynbuf_reset(gui->guipoints_payload);
    gui->guipoints_count = 0;
  }

  // Eager encoding: load model and encode image as soon as tool opens
  if(d->encode_state == ENCODE_IDLE)
  {
    dt_control_log(_("object mask: analyzing image..."));
    d->encode_state = ENCODE_MSG_SHOWN;
    dt_control_queue_redraw_center();
    return;
  }

  if(d->encode_state == ENCODE_MSG_SHOWN)
  {
    // Frame 2: launch background thread to render and encode the image.
    // The thread creates a temporary export pipe at high resolution
    // instead of using the low-res preview backbuf.
    _encode_thread_data_t *td = g_new(_encode_thread_data_t, 1);
    td->d = d;
    td->imgid = cur_imgid;

    d->encoded_imgid = cur_imgid;
    d->encode_state = ENCODE_RUNNING;
    // Start poll timer BEFORE the thread — it will detect completion
    // and also tracks modifier keys once encoding is ready
    if(!d->modifier_poll_id)
      d->modifier_poll_id = g_timeout_add(100, _modifier_poll, NULL);
    d->encode_thread = g_thread_new("ai-mask-encode", _encode_thread_func, td);
    return;
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_RUNNING)
  {
    // Keep the message visible while the thread is working
    dt_control_log(_("object mask: analyzing image..."));
    return;
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_READY && d->encode_thread)
  {
    // Thread finished (detected by poll timer redraw) — join it
    g_thread_join(d->encode_thread);
    d->encode_thread = NULL;
    dt_control_log_ack_all();
    dt_control_log(_("brush over object to create mask"));
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_ERROR)
  {
    if(d->encode_thread)
    {
      g_thread_join(d->encode_thread);
      d->encode_thread = NULL;
      // Log only once when the thread is first joined
      dt_control_log(_("object mask preparation failed"));
    }
    return;
  }

  if(d->encode_state != ENCODE_READY)
    return;

  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);

  // --- Draw red overlay of current mask ---
  if(d->mask && d->mask_w > 0 && d->mask_h > 0)
  {
    const int mw = d->mask_w;
    const int mh = d->mask_h;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, mw);
    unsigned char *buf = g_try_malloc0((size_t)stride * mh);
    if(buf)
    {
      const float mask_thresh = CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY), 0.3f, 0.9f);
      for(int y = 0; y < mh; y++)
      {
        unsigned char *row = buf + y * stride;
        for(int x = 0; x < mw; x++)
        {
          const float val = d->mask[y * mw + x];
          if(val > mask_thresh)
          {
            const unsigned char alpha = 80;
            row[x * 4 + 0] = 0;     // B
            row[x * 4 + 1] = 0;     // G
            row[x * 4 + 2] = alpha; // R (premultiplied)
            row[x * 4 + 3] = alpha; // A
          }
        }
      }

      cairo_surface_t *surface
        = cairo_image_surface_create_for_data(buf, CAIRO_FORMAT_ARGB32, mw, mh, stride);

      if(surface)
      {
        cairo_save(cr);
        cairo_scale(cr, wd / mw, ht / mh);
        cairo_set_source_surface(cr, surface, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(surface);
      }
      g_free(buf);
    }
  }

  // --- Draw vectorization preview (real path style with anchor dots) ---
  if(d->preview_forms)
  {
    const float msx = (d->mask_w > 0) ? wd / (float)d->mask_w : 1.0f;
    const float msy = (d->mask_h > 0) ? ht / (float)d->mask_h : 1.0f;

    for(GList *fl = d->preview_forms; fl; fl = g_list_next(fl))
    {
      dt_masks_form_t *f = fl->data;
      GList *pts = f->points;
      if(!pts) continue;

      dt_masks_point_path_t *first_pt = pts->data;
      cairo_move_to(cr,
                    first_pt->corner[0] * msx,
                    first_pt->corner[1] * msy);

      for(GList *p = g_list_next(pts); p; p = g_list_next(p))
      {
        dt_masks_point_path_t *pt = p->data;
        cairo_curve_to(cr,
                       pt->ctrl1[0] * msx, pt->ctrl1[1] * msy,
                       pt->ctrl2[0] * msx, pt->ctrl2[1] * msy,
                       pt->corner[0] * msx, pt->corner[1] * msy);
      }

      // Close path back to first point
      cairo_curve_to(cr,
                     first_pt->ctrl1[0] * msx, first_pt->ctrl1[1] * msy,
                     first_pt->ctrl2[0] * msx, first_pt->ctrl2[1] * msy,
                     first_pt->corner[0] * msx, first_pt->corner[1] * msy);

      dt_masks_line_stroke(cr, FALSE, FALSE, FALSE, zoom_scale);

      for(GList *p = pts; p; p = g_list_next(p))
      {
        dt_masks_point_path_t *pt = p->data;
        dt_masks_draw_anchor(cr, FALSE, zoom_scale,
                             pt->corner[0] * msx, pt->corner[1] * msy);
      }
    }
  }

  // Query pointer position and modifier state directly from GDK so the
  // cursor/brush is drawn at the correct location even before the first
  // mouse_moved event fires.
  GtkWidget *cw = dt_ui_center(darktable.gui->ui);
  GdkWindow *win = gtk_widget_get_window(cw);
  GdkDevice *pointer
    = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  GdkModifierType mod = 0;
  int dev_x = 0, dev_y = 0;
  if(win && pointer)
    gdk_window_get_device_position(win, pointer, &dev_x, &dev_y, &mod);
  const gboolean shift_held = (mod & GDK_SHIFT_MASK) != 0;

  // Convert device coordinates to preview pipe pixel space
  {
    float pzx, pzy, zs;
    dt_dev_get_pointer_zoom_pos(&darktable.develop->full,
                                (float)dev_x, (float)dev_y,
                                &pzx, &pzy, &zs);
    gui->posx = pzx * wd;
    gui->posy = pzy * ht;
  }

  if(d->brush_painting && d->brush_points && d->brush_points_count >= 2)
  {
    // During brush painting: draw stroke path and circle at current position
    const float min_dim = MIN(iwidth, iheight);
    const float radius = d->brush_radius * min_dim;
    const float opacity = 0.5f;

    // Draw brush stroke path (DT_GUI_COLOR_BRUSH_TRACE matches brush mask style)
    const float *bp = dt_masks_dynbuf_buffer(d->brush_points);
    cairo_save(cr);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width(cr, 2.0f * radius);
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_TRACE, opacity);

    cairo_move_to(cr, bp[0], bp[1]);
    for(int i = 1; i < d->brush_points_count; i++)
      cairo_line_to(cr, bp[i * 2], bp[i * 2 + 1]);
    cairo_stroke(cr);

    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, opacity);
    cairo_set_line_width(cr, 3.0 / zoom_scale);
    cairo_arc(cr, gui->posx, gui->posy, radius, 0, 2.0 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.8);
    cairo_stroke(cr);
    cairo_restore(cr);
  }
  else if(!d->brush_used)
  {
    // Before brush completed: draw brush circle cursor (same style as brush mask)
    const float min_dim = MIN(iwidth, iheight);
    const float radius = d->brush_radius * min_dim;
    const float opacity = 0.5f;

    cairo_save(cr);
    dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, opacity);
    cairo_set_line_width(cr, 3.0 / zoom_scale);
    cairo_arc(cr, gui->posx, gui->posy, radius, 0, 2.0 * M_PI);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0.8, 0.8, 0.8, 0.8);
    cairo_stroke(cr);
    cairo_restore(cr);
  }
  else
  {
    // After brush used: draw +/- cursor indicator for point refinement
    const float r = DT_PIXEL_APPLY_DPI(8.0f) / zoom_scale;
    const float lw = DT_PIXEL_APPLY_DPI(2.0f) / zoom_scale;
    cairo_set_line_width(cr, lw);
    cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);

    // Horizontal line (common to both + and -)
    cairo_move_to(cr, gui->posx - r, gui->posy);
    cairo_line_to(cr, gui->posx + r, gui->posy);
    cairo_stroke(cr);

    if(!shift_held)
    {
      // Add mode: vertical line to form "+"
      cairo_move_to(cr, gui->posx, gui->posy - r);
      cairo_line_to(cr, gui->posx, gui->posy + r);
      cairo_stroke(cr);
    }
  }

}

// --- Stub functions (object is transient — result is path masks) ---

static int _object_get_points(
  dt_develop_t *dev,
  const float x,
  const float y,
  const float radius,
  const float radius2,
  const float rotation,
  float **points,
  int *points_count)
{
  (void)dev;
  (void)x;
  (void)y;
  (void)radius;
  (void)radius2;
  (void)rotation;
  *points = NULL;
  *points_count = 0;
  return 0;
}

static int _object_get_points_border(
  dt_develop_t *dev,
  struct dt_masks_form_t *form,
  float **points,
  int *points_count,
  float **border,
  int *border_count,
  const int source,
  const dt_iop_module_t *module)
{
  (void)dev;
  (void)form;
  (void)points;
  (void)points_count;
  (void)border;
  (void)border_count;
  (void)source;
  (void)module;
  return 0;
}

static int _object_get_source_area(
  dt_iop_module_t *module,
  dt_dev_pixelpipe_iop_t *piece,
  dt_masks_form_t *form,
  int *width,
  int *height,
  int *posx,
  int *posy)
{
  (void)module;
  (void)piece;
  (void)form;
  (void)width;
  (void)height;
  (void)posx;
  (void)posy;
  return 1;
}

static int _object_get_area(
  const dt_iop_module_t *const restrict module,
  const dt_dev_pixelpipe_iop_t *const restrict piece,
  dt_masks_form_t *const restrict form,
  int *width,
  int *height,
  int *posx,
  int *posy)
{
  (void)module;
  (void)piece;
  (void)form;
  (void)width;
  (void)height;
  (void)posx;
  (void)posy;
  return 1;
}

static int _object_get_mask(
  const dt_iop_module_t *const restrict module,
  const dt_dev_pixelpipe_iop_t *const restrict piece,
  dt_masks_form_t *const restrict form,
  float **buffer,
  int *width,
  int *height,
  int *posx,
  int *posy)
{
  (void)module;
  (void)piece;
  (void)form;
  (void)buffer;
  (void)width;
  (void)height;
  (void)posx;
  (void)posy;
  return 1;
}

static int _object_get_mask_roi(
  const dt_iop_module_t *const restrict module,
  const dt_dev_pixelpipe_iop_t *const restrict piece,
  dt_masks_form_t *const form,
  const dt_iop_roi_t *const roi,
  float *const restrict buffer)
{
  (void)module;
  (void)piece;
  (void)form;
  (void)roi;
  (void)buffer;
  return 1;
}

static GSList *_object_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  (void)form;
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT_DRAG,
    0,
    _("[OBJECT] brush over object"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    0,
    _("[OBJECT] add foreground point"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    GDK_SHIFT_MASK,
    _("[OBJECT] add background point"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_RIGHT,
    0,
    _("[OBJECT] apply mask"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_SCROLL,
    0,
    _("[OBJECT] change brush size / cleanup"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_SCROLL,
    GDK_SHIFT_MASK,
    _("[OBJECT] change smoothing"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_SCROLL,
    GDK_CONTROL_MASK,
    _("[OBJECT] change opacity"));
  return lm;
}

static void _object_sanitize_config(dt_masks_type_t type) { (void)type; }

static void _object_set_form_name(dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("object #%d"), (int)nb);
}

static void _object_set_hint_message(
  const dt_masks_form_gui_t *const gui,
  const dt_masks_form_t *const form,
  const int opacity,
  char *const restrict msgbuf,
  const size_t msgbuf_len)
{
  (void)form;
  if(gui->creation)
  {
    const _object_data_t *d = _get_data((dt_masks_form_gui_t *)gui);
    if(!d || d->encode_state != ENCODE_READY)
      return;  // no hints while encoding
    if(d->brush_used)
      g_snprintf(
        msgbuf,
        msgbuf_len,
        _("<b>add</b>: click, <b>subtract</b>: shift+click, "
          "<b>clear</b>: alt+click, <b>apply</b>: right-click\n"
          "<b>cleanup</b>: scroll (%d), <b>smoothing</b>: shift+scroll (%3.2f), "
          "<b>opacity</b>: ctrl+scroll (%d%%)"),
        d->preview_cleanup, d->preview_smoothing, opacity);
    else
      g_snprintf(
        msgbuf,
        msgbuf_len,
        _("<b>brush</b>: drag, <b>size</b>: scroll, "
          "<b>opacity</b>: ctrl+scroll (%d%%)"),
        opacity);
  }
}

static void _object_duplicate_points(
  dt_develop_t *dev,
  dt_masks_form_t *const base,
  dt_masks_form_t *const dest)
{
  (void)dev;
  (void)base;
  (void)dest;
}

static void _object_modify_property(
  dt_masks_form_t *const form,
  const dt_masks_property_t prop,
  const float old_val,
  const float new_val,
  float *sum,
  int *count,
  float *min,
  float *max)
{
  (void)form;

  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  _object_data_t *d = gui ? _get_data(gui) : NULL;

  if(!gui || !gui->creation) return;

  switch(prop)
  {
    case DT_MASKS_PROPERTY_SIZE:;
      const float ratio = (!old_val || !new_val) ? 1.0f : new_val / old_val;
      float brush_size = dt_conf_get_float(CONF_OBJECT_BRUSH_SIZE_KEY);
      // Only allow resizing before the first brush stroke
      if(!d || !d->brush_used)
      {
        brush_size = CLAMP(brush_size * ratio, 0.005f, 0.5f);
        dt_conf_set_float(CONF_OBJECT_BRUSH_SIZE_KEY, brush_size);
        if(d) d->brush_radius = brush_size;
      }

      *sum += 2.0f * brush_size;
      *max = fminf(*max, 0.5f / brush_size);
      *min = fmaxf(*min, 0.005f / brush_size);
      ++*count;
      break;
    case DT_MASKS_PROPERTY_CLEANUP:;
      int cleanup = dt_conf_get_int("plugins/darkroom/masks/object/cleanup");
      if(d && d->brush_used)
      {
        cleanup = CLAMP(cleanup + (int)(new_val - old_val), 0, 100);
        dt_conf_set_int("plugins/darkroom/masks/object/cleanup", cleanup);
        d->preview_cleanup = cleanup;
        _update_preview(d);
      }
      *sum += cleanup;
      ++*count;
      break;
    case DT_MASKS_PROPERTY_SMOOTHING:;
      float smoothing = dt_conf_get_float("plugins/darkroom/masks/object/smoothing");
      if(d && d->brush_used)
      {
        smoothing = CLAMP(smoothing + (new_val - old_val), 0.0f, 1.3f);
        dt_conf_set_float("plugins/darkroom/masks/object/smoothing", smoothing);
        d->preview_smoothing = smoothing;
        _update_preview(d);
      }
      *sum += smoothing;
      ++*count;
      break;
    default:;
  }
}

static void
_object_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  (void)iwd;
  (void)iht;
  (void)x;
  (void)y;
}

// The function table for object masks
const dt_masks_functions_t dt_masks_functions_object = {
  .point_struct_size = sizeof(struct dt_masks_point_object_t),
  .sanitize_config = _object_sanitize_config,
  .setup_mouse_actions = _object_setup_mouse_actions,
  .set_form_name = _object_set_form_name,
  .set_hint_message = _object_set_hint_message,
  .modify_property = _object_modify_property,
  .duplicate_points = _object_duplicate_points,
  .initial_source_pos = _object_initial_source_pos,
  .get_distance = _object_get_distance,
  .get_points = _object_get_points,
  .get_points_border = _object_get_points_border,
  .get_mask = _object_get_mask,
  .get_mask_roi = _object_get_mask_roi,
  .get_area = _object_get_area,
  .get_source_area = _object_get_source_area,
  .mouse_moved = _object_events_mouse_moved,
  .mouse_scrolled = _object_events_mouse_scrolled,
  .button_pressed = _object_events_button_pressed,
  .button_released = _object_events_button_released,
  .post_expose = _object_events_post_expose
};

gboolean dt_masks_object_available(void)
{
  if(!darktable.ai_registry || !darktable.ai_registry->ai_enabled)
    return FALSE;
  char *model_id = dt_ai_models_get_active_for_task("mask");
  dt_ai_model_t *model = dt_ai_models_get_by_id(darktable.ai_registry, model_id);
  g_free(model_id);
  const gboolean available = model && model->status == DT_AI_MODEL_DOWNLOADED;
  dt_ai_model_free(model);
  return available;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
