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

#include "common/debug.h"
#include "common/dtdata.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "develop/pixelpipe_hb.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "views/view.h"
#ifdef HAVE_AI
#include "common/ai/segmentation.h"
#include "common/ai_models.h"
#include "common/densecrf.h"
#include "common/distance_transform.h"
#include "common/ras2vect.h"
#endif

#include <limits.h>
#include <math.h>
#include <string.h>

// --- creation: the AI tool, only where AI is compiled in ---

#ifdef HAVE_AI

// a committed object has points and a new one none until the commit. not a
// lookup in dev->forms: a history change mid-edit replaces it with copies
static gboolean _is_edit(const dt_masks_form_t *form)
{
  return form && form->points;
}

// the tool is open, on a new object or a committed one. the vtable also
// serves committed objects outside it, with other actions, and
// setup_mouse_actions, the one slot handed no gui, tells them apart by this
static gboolean _in_tool(void)
{
  const dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  return gui && gui->creation;
}

#define CONF_OBJECT_THRESHOLD_KEY "plugins/darkroom/masks/object/threshold"
#define CONF_OBJECT_REFINE_PASSES_KEY "plugins/darkroom/masks/object/refine_passes"
#define CONF_OBJECT_CLEANUP_KEY "plugins/darkroom/masks/object/cleanup"
#define CONF_OBJECT_SMOOTHING_KEY "plugins/darkroom/masks/object/smoothing"
#define CONF_OBJECT_FEATHER_KEY "plugins/darkroom/masks/object/feather"
#define CONF_OBJECT_PERSIST_KEY "plugins/darkroom/masks/object/persist_model"
#define CONF_OBJECT_VECTORIZE_KEY "plugins/darkroom/masks/object/vectorize"
#define CONF_OBJECT_REFINE_BOUNDARY_KEY "plugins/darkroom/masks/object/refine_boundary"
#define CONF_OBJECT_REFINE_BOUNDARY_ITER_KEY "plugins/darkroom/masks/object/refine_boundary_iterations"
#define CONF_OBJECT_REFINE_BOUNDARY_SIGMA_COLOR_KEY "plugins/darkroom/masks/object/refine_boundary_sigma_color"
#define CONF_OBJECT_REFINE_BOUNDARY_W_BILATERAL_KEY "plugins/darkroom/masks/object/refine_boundary_weight_bilateral"

// default render target (longest side in pixels).
// the SAM encoder internally downscales to 1024 so encoding quality
// is the same, but higher render resolution gives the guided filter
// and vectorizer more detail for edge refinement.
// configurable via plugins/darkroom/masks/object/render_size
#define SEG_RENDER_DEFAULT 1536
#define CONF_OBJECT_RENDER_SIZE_KEY "plugins/darkroom/masks/object/render_size"

// dt_conf_get_int clamps to the schema's bounds (conf.c:183-189), even for a
// hand-edited darktablerc. what is stored is capped apart from this, at
// DT_MASKS_PIXEL_MAX_STORED
static int _render_size(void)
{
  return dt_conf_key_exists(CONF_OBJECT_RENDER_SIZE_KEY)
    ? MAX(dt_conf_get_int(CONF_OBJECT_RENDER_SIZE_KEY), 1024)
    : SEG_RENDER_DEFAULT;
}

// --- per-session segmentation state (stored in gui->scratchpad) ---

typedef enum _encode_state_t
{
  ENCODE_ERROR = -1,
  ENCODE_IDLE = 0,
  ENCODE_MSG_SHOWN = 1, // busy message queued, waiting for next expose
  ENCODE_READY = 2,     // encoding complete, results available
  ENCODE_RUNNING = 3,   // background thread in progress
} _encode_state_t;

// minimum drag distance (preview pipe pixels) to distinguish click from drag
#define DRAG_THRESHOLD 5.0f

// how long input has to settle before the outline is traced again
#define OUTLINE_TRACE_DELAY_MS 150

typedef struct _object_data_t
{
  dt_ai_environment_t *env; // AI environment for model registry
  dt_seg_context_t *seg;    // SAM context (encoder+decoder)
  float *mask;              // the selection at the encoded size, g_free'd
  int mask_w, mask_h;       // mask dimensions
  gboolean model_loaded;    // whether the model was loaded
  int encode_state;         // uses _encode_state_t values (atomic access)
  dt_imgid_t encoded_imgid; // image ID that was encoded
  dt_hash_t encoded_distort_hash; // distort hash at encode time (detects crop/rotate)
  // the render encoded, which the mask spans: its rw x rh is the encoding's
  // size, all zero before one is ready
  dt_masks_pixel_grid_t grid;
  guint modifier_poll_id;   // timer to detect shift key changes
  GThread *encode_thread;   // background encoding thread
  gboolean dragging;        // TRUE between press and release during click drag
  float drag_start_x;       // press position (preview pipe pixel space)
  float drag_start_y;
  gboolean has_selection;   // TRUE after first click, enables refinement mode
  // the prompts, dt_masks_point_object_t input-image normalized,
  // in click order
  GList *prompts;
  // outline of the paths a commit would trace, kept while applying as paths
  GList *outline_forms;             // GList of dt_masks_form_t* (mask-space pixel coords)
  GList *outline_signs;             // parallel GList of sign values ('+' or '-')
  guint outline_trace_id;           // pending trace of the outline, 0 if none
  // editing a committed object: its stored mask and prompts were restored
  gboolean restored;
  // the last restore ran short of memory: the next click retries it, not
  // every redraw, which the modifier poll queues every 100 ms
  gboolean restore_short;
  guint resume_id;          // pending restore of the edit, 0 if none
  // a decode is running, and pumping the main loop from inside it
  gboolean decoding;
  // the selection changed since then, so a commit has something to store
  gboolean changed;
} _object_data_t;

static _object_data_t *_get_data(dt_masks_form_gui_t *gui)
{
  return (gui && gui->scratchpad) ? (_object_data_t *)gui->scratchpad : NULL;
}

static void _on_view_changed(gpointer instance,
                             dt_view_t *old_view,
                             dt_view_t *new_view,
                             gpointer user_data)
{
  (void)instance;
  (void)new_view;
  (void)user_data;

  // free persistent model when leaving darkroom
  if(old_view && old_view->view(old_view) == DT_VIEW_DARKROOM)
  {
    dt_ai_seg_t *seg = &darktable.ai_seg;
    if(seg->ctx)
    {
      dt_print(DT_DEBUG_AI,
               "[object mask] freeing persistent model");
      dt_seg_free(seg->ctx);
      seg->ctx = NULL;
    }
    if(seg->env)
    {
      dt_ai_env_destroy(seg->env);
      seg->env = NULL;
    }
    seg->model_loaded = FALSE;

    DT_CONTROL_SIGNAL_DISCONNECT(_on_view_changed, NULL);
    seg->signal_connected = FALSE;
  }
}

// the outline's forms are never registered in dev->forms
static void _free_outline(_object_data_t *d)
{
  if(!d) return;
  for(GList *l = d->outline_forms; l; l = g_list_next(l))
    dt_masks_free_form(l->data);
  g_list_free(d->outline_forms);
  d->outline_forms = NULL;
  g_list_free(d->outline_signs);
  d->outline_signs = NULL;
}

static void _clear_prompts(_object_data_t *d)
{
  g_list_free_full(d->prompts, free);
  d->prompts = NULL;
}

// a deep copy of a list of prompts, NULL when out of memory
static GList *_copy_prompts(const GList *prompts)
{
  GList *copy = NULL;
  for(const GList *l = prompts; l; l = g_list_next(l))
  {
    dt_masks_point_object_t *pt = malloc(sizeof(dt_masks_point_object_t));
    if(!pt)
    {
      g_list_free_full(copy, free);
      return NULL;
    }
    memcpy(pt, l->data, sizeof(dt_masks_point_object_t));
    copy = g_list_prepend(copy, pt);
  }
  return g_list_reverse(copy);
}

// the "apply as paths" switch. with sidecar files disabled there is nowhere
// to keep pixels, so it is on whatever it says
static gboolean _as_paths(void)
{
  return !dt_dtdata_enabled() || dt_conf_get_bool(CONF_OBJECT_VECTORIZE_KEY);
}

// whether a right-click traces paths. an edit stays pixels: it updates the
// object in place
static gboolean _commits_paths(const dt_masks_form_t *form)
{
  return !_is_edit(form) && _as_paths();
}

// the mask traced into path forms in mask pixels, on the commit's settings
// so the outline matches the paths. FALSE only when out of memory
static gboolean _trace(const _object_data_t *d, GList **forms, GList **signs)
{
  *forms = NULL;
  *signs = NULL;

  // potrace traces dark ink on white, the mask is high inside the object:
  // invert both the mask and the threshold
  const size_t n = (size_t)d->mask_w * d->mask_h;
  float *inv_mask = g_try_malloc(n * sizeof(float));
  if(!inv_mask)
    return FALSE;

  for(size_t i = 0; i < n; i++)
    inv_mask[i] = 1.0f - d->mask[i];

  const int cleanup = dt_conf_get_int(CONF_OBJECT_CLEANUP_KEY);
  const float smoothing = dt_conf_get_float(CONF_OBJECT_SMOOTHING_KEY);
  const float thresh = 1.0f - CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY),
                                    0.3f, 0.9f);
  *forms = ras2forms(inv_mask, d->mask_w, d->mask_h, NULL,
                     thresh, cleanup, (double)smoothing, signs);
  g_free(inv_mask);

  const float feather = dt_conf_get_float(CONF_OBJECT_FEATHER_KEY);
  for(GList *fl = *forms; fl; fl = g_list_next(fl))
  {
    dt_masks_form_t *f = fl->data;
    for(GList *pt = f->points; pt; pt = g_list_next(pt))
    {
      dt_masks_point_path_t *p = pt->data;
      p->border[0] = p->border[1] = feather;
    }
  }
  return TRUE;
}

// an edit stays pixels, so it has no outline. the test is what the
// right-click will do, not whether the edit restored: a failed restore is
// still an edit
static gboolean _wants_outline(const _object_data_t *d)
{
  return d->mask && d->mask_w > 0 && d->mask_h > 0
    && _commits_paths(darktable.develop->form_visible);
}

static gboolean _outline_trace_cb(gpointer data)
{
  _object_data_t *d = data;
  d->outline_trace_id = 0;
  _free_outline(d);
  if(_wants_outline(d))
    _trace(d, &d->outline_forms, &d->outline_signs);
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

// a potrace pass is too slow for every scroll step, so the trace waits for
// input to settle and the last outline stays up meanwhile. with nothing to
// trace, the outline goes at once
static void _schedule_outline(_object_data_t *d)
{
  if(d->outline_trace_id)
  {
    g_source_remove(d->outline_trace_id);
    d->outline_trace_id = 0;
  }
  if(!_wants_outline(d))
  {
    _free_outline(d);
    dt_control_queue_redraw_center();
    return;
  }
  d->outline_trace_id = g_timeout_add(OUTLINE_TRACE_DELAY_MS, _outline_trace_cb, d);
}

// free all resources in _object_data_t (must be called after thread has joined),
// preserves seg+env in persistent statics so the model stays loaded
static void _destroy_data(_object_data_t *d)
{
  if(!d)
    return;
  if(d->modifier_poll_id)
    g_source_remove(d->modifier_poll_id);
  if(d->outline_trace_id)
    g_source_remove(d->outline_trace_id);
  if(d->resume_id)
    g_source_remove(d->resume_id);
  if(d->encode_thread)
    g_thread_join(d->encode_thread);

  // save model to persistent storage - keeps it loaded across
  // mask sessions, disk cache handles embedding persistence.
  // only persist if nobody already claimed the slot (guards
  // against deferred cleanup racing with a new session)
  dt_ai_seg_t *ps = &darktable.ai_seg;
  const gboolean persist = dt_conf_get_bool(CONF_OBJECT_PERSIST_KEY);
  if(persist && !ps->ctx && d->seg)
  {
    dt_seg_reset_encoding(d->seg);
    ps->env = d->env;
    ps->ctx = d->seg;
    ps->model_loaded = d->model_loaded;
    d->env = NULL;
    d->seg = NULL;
  }
  else
  {
    if(d->seg) dt_seg_free(d->seg);
    if(d->env) dt_ai_env_destroy(d->env);
    d->seg = NULL;
    d->env = NULL;
  }

  g_free(d->mask);
  _free_outline(d);
  _clear_prompts(d);
  g_free(d);
}

// idle callback for deferred cleanup when background thread was still running
static gboolean _deferred_cleanup(gpointer data)
{
  _object_data_t *d = data;
  const int state = g_atomic_int_get(&d->encode_state);
  if(state == ENCODE_RUNNING || d->decoding)
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
  if(state == ENCODE_RUNNING || d->decoding)
  {
    // the encode thread holds d, or a decode on this stack pumped the main
    // loop (dt_gui_cursor_set_busy) and writes d on return. at shutdown no
    // loop runs a deferral and the thread would outlive the mipmap cache, so
    // join it; a decode, being on this thread, cannot be in flight then
    if(dt_control_running())
    {
      g_timeout_add(200, _deferred_cleanup, d);
      return;
    }
  }
  _destroy_data(d);
}

// data passed to the background encoding thread
typedef struct _encode_thread_data_t
{
  _object_data_t *d;
  dt_imgid_t imgid;        // image to encode (thread renders via export pipe)
  int32_t history_end;     // darkroom history_end (may be ahead of database)
  dt_hash_t distort_hash;  // hash from live darkroom state (for disk cache key)
} _encode_thread_data_t;

// background thread: loads model, renders image via export pipe, and encodes,
// does ZERO GLib/GTK calls - only computation + atomic state set,
// the poll timer on the main thread detects completion
static gpointer _encode_thread_func(gpointer data)
{
  _encode_thread_data_t *td = data;
  _object_data_t *d = td->d;
  const dt_imgid_t imgid = td->imgid;
  const int32_t td_history_end = td->history_end;
  const dt_hash_t distort_hash = td->distort_hash;
  g_free(td);

  // load model if needed
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

  // use distort hash from darkroom's live state (passed by caller)
  // instead of computing from the thread's dev, which may have
  // stale history (not yet flushed to database). before the render: a hit
  // needs neither the raw nor a pipe
  int fw = 0, fh = 0;
  float fscale = 0.0f;
  if(dt_seg_disk_cache_load(d->seg, imgid, distort_hash, &fw, &fh, &fscale))
  {
    // placed by the render it was made from, which the cache keeps. without
    // that render's pixels there is nothing to place by: encode afresh
    int ew = 0, eh = 0;
    dt_seg_get_encoded_rgb(d->seg, &ew, &eh);
    if(ew > 0 && eh > 0 && fw > 0 && fh > 0 && fscale > 0.0f)
    {
      d->grid = (dt_masks_pixel_grid_t){ fw, fh, ew, eh, fscale };
      g_atomic_int_set(&d->encode_state, ENCODE_READY);
      dt_seg_warmup_decoder(d->seg);
      return NULL;
    }
    dt_seg_reset_encoding(d->seg);
  }

  // render image at high resolution via temporary export pipeline
  int width = 0, height = 0;
  dt_masks_pixel_render_t *render =
    dt_masks_pixel_render_init(imgid, td_history_end, &width, &height);
  if(!render)
  {
    g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
    return NULL;
  }

  const int render_target = _render_size();
  const double scale = fmin((double)render_target / (double)width,
                            (double)render_target / (double)height);
  const double final_scale = fmin(scale, 1.0); // don't upscale
  const int out_w = (int)(final_scale * width);
  const int out_h = (int)(final_scale * height);

  dt_print(DT_DEBUG_AI,
           "[object mask] rendering %dx%d (scale=%.3f) for encoding...",
           out_w, out_h, final_scale);

  uint8_t *rgb = dt_masks_pixel_render(render, out_w, out_h, final_scale);
  dt_masks_pixel_render_cleanup(render);

  if(!rgb)
  {
    dt_print(DT_DEBUG_AI, "[object mask] failed to render image for encoding");
    g_atomic_int_set(&d->encode_state, ENCODE_ERROR);
    return NULL;
  }

  // the render the encoding and every mask decoded from it are placed by
  d->grid = (dt_masks_pixel_grid_t){ width, height, out_w, out_h, final_scale };

  // encode the image
  gboolean ok = dt_seg_encode_image(d->seg, rgb, out_w, out_h);

  // if accelerated encoding failed, fall back to CPU
  if(!ok)
  {
    dt_print(DT_DEBUG_AI,
             "[object mask] encoding failed, retrying with CPU provider");
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

  // dt_seg_encode_image keeps its own copy of rgb for edge refinement
  if(ok)
    dt_seg_disk_cache_save(d->seg, imgid, distort_hash,
                           rgb, out_w, out_h, width, height, final_scale);
  g_free(rgb);

  // signal ready so the user can start placing points; warmup continues
  // on this thread; _run_decoder joins the thread on the first click to
  // avoid a race with warmup on the shared segmentation context
  g_atomic_int_set(&d->encode_state, ok ? ENCODE_READY : ENCODE_ERROR);

  // warm up decoder with real encoder embeddings so the first user click
  // doesn't pay ORT's lazy-init + arena-sizing cost on the main thread
  if(ok)
    dt_seg_warmup_decoder(d->seg);

  return NULL;
}

// how far below the user threshold the kept component may grow: without it
// the cleanup below would cut the stored mask off hard at the threshold
#define MASK_COMPONENT_FLOOR 0.05f

// keep the component holding the seed pixel, or the largest if none does:
// found at the threshold, it grows down to MASK_COMPONENT_FLOOR for its
// soft fringe, and everything else is zeroed
static void _keep_seed_component(float *mask,
                                 const int w,
                                 const int h,
                                 const float threshold,
                                 const int seed_x,
                                 const int seed_y)
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
    {
      // out of labels: the cores left are other components, which the
      // growth below must not take for fringe, and which end up zeroed
      for(int k = i; k < npix; k++)
        if(mask[k] > threshold && labels[k] == 0) labels[k] = -1;
      break;
    }

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

  // prefer component containing the seed point; fall back to largest
  const int16_t keep = (seed_label > 0) ? seed_label : best_label;

  if(keep > 0)
  {
    // hysteresis: grow the kept component down to the floor to take in its
    // soft fringe. a zero label here means "at or below the threshold", so
    // growth can never reach another component's core and merge two objects
    int sp = 0;
    for(int i = 0; i < npix; i++)
    {
      if(labels[i] == keep)
        stack[sp++] = i;
    }

    while(sp > 0)
    {
      const int p = stack[--sp];
      const int px = p % w;
      const int py = p / w;

      if(py > 0 && labels[p - w] == 0 && mask[p - w] > MASK_COMPONENT_FLOOR)
      {
        labels[p - w] = keep;
        stack[sp++] = p - w;
      }
      if(py < h - 1 && labels[p + w] == 0 && mask[p + w] > MASK_COMPONENT_FLOOR)
      {
        labels[p + w] = keep;
        stack[sp++] = p + w;
      }
      if(px > 0 && labels[p - 1] == 0 && mask[p - 1] > MASK_COMPONENT_FLOOR)
      {
        labels[p - 1] = keep;
        stack[sp++] = p - 1;
      }
      if(px < w - 1 && labels[p + 1] == 0 && mask[p + 1] > MASK_COMPONENT_FLOOR)
      {
        labels[p + 1] = keep;
        stack[sp++] = p + 1;
      }
    }

    for(int i = 0; i < npix; i++)
    {
      if(labels[i] != keep)
        mask[i] = 0.0f;
    }
  }

  g_free(stack);
  g_free(labels);
}

static float _mask_iou(const float *const restrict a,
                       const float *const restrict b,
                       const size_t n,
                       const float threshold)
{
  size_t inter = 0, uni = 0;
  DT_OMP_FOR(reduction(+:inter, uni))
  for(size_t i = 0; i < n; i++)
  {
    const int A = a[i] > threshold;
    const int B = b[i] > threshold;
    inter += A & B;
    uni   += A | B;
  }
  return uni > 0 ? (float)inter / (float)uni : 0.0f;
}

// peak of the (exact-Euclidean) distance transform of mask>threshold,
// excluding pixels within min_separation of any positive prompt
static gboolean _find_peak_point(const float *const restrict mask,
                                 const size_t w,
                                 const size_t h,
                                 const float threshold,
                                 const dt_seg_point_t *const exclude,
                                 const int n_exclude,
                                 const float min_separation,
                                 dt_seg_point_t *const out)
{
  float *const restrict dist = dt_alloc_align_float(w * h);
  if(!dist) return FALSE;

  // exact-euclidean DT: dist[i] = distance to nearest pixel where mask<thr
  // require ~4 px interior depth — shallower peaks aren't informative
  const float min_depth = 4.0f;
  const float max_dist
    = dt_image_distance_transform(mask, dist, w, h,
                                  threshold, DT_DISTANCE_TRANSFORM_MASK);
  if(max_dist <= min_depth) { dt_free_align(dist); return FALSE; }

  // zero out pixels too close to existing positive prompts so the
  // subsequent argmax never picks them
  const float min_sep_sq = min_separation * min_separation;
  for(int k = 0; k < n_exclude; k++)
  {
    if(exclude[k].label != 1) continue;
    const float px = exclude[k].x;
    const float py = exclude[k].y;
    const int x0 = MAX(0, (int)(px - min_separation));
    const int x1 = MIN((int)w - 1, (int)(px + min_separation));
    const int y0 = MAX(0, (int)(py - min_separation));
    const int y1 = MIN((int)h - 1, (int)(py + min_separation));
    DT_OMP_FOR(collapse(2))
    for(int y = y0; y <= y1; y++)
      for(int x = x0; x <= x1; x++)
      {
        const float dx = (float)x - px;
        const float dy = (float)y - py;
        if(dx * dx + dy * dy < min_sep_sq) dist[(size_t)y * w + x] = 0.0f;
      }
  }

  // single-threaded combined max+argmax (exclusion may have lowered
  // the peak below max_dist, so we can't reuse that value here)
  size_t best_idx = (size_t)-1;
  float best = min_depth;
  for(size_t i = 0; i < w * h; i++)
    if(dist[i] > best) { best = dist[i]; best_idx = i; }
  dt_free_align(dist);
  if(best_idx == (size_t)-1) return FALSE;

  const size_t py = best_idx / w;
  const size_t px = best_idx % w;
  out->x = (float)px;
  out->y = (float)py;
  out->label = 1;
  return TRUE;
}

// tight bbox around mask>threshold, padded by `padding` (fraction of
// bbox extent); FALSE if mask is empty
static gboolean _compute_bbox(const float *const restrict mask,
                              const int w,
                              const int h,
                              const float threshold,
                              const float padding,
                              dt_seg_point_t *const tl,
                              dt_seg_point_t *const br)
{
  // single-threaded: cheap, and avoids OMP-reduction identity surprises
  int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;
  for(int y = 0; y < h; y++)
  {
    for(int x = 0; x < w; x++)
    {
      if(mask[(size_t)y * w + x] > threshold)
      {
        if(x < min_x) min_x = x;
        if(y < min_y) min_y = y;
        if(x > max_x) max_x = x;
        if(y > max_y) max_y = y;
      }
    }
  }
  if(max_x == INT_MIN) return FALSE;

  const int pad_x = (int)((max_x - min_x) * padding) + 1;
  const int pad_y = (int)((max_y - min_y) * padding) + 1;
  tl->x = (float)CLAMP(min_x - pad_x, 0, w - 1);
  tl->y = (float)CLAMP(min_y - pad_y, 0, h - 1);
  tl->label = 2;
  br->x = (float)CLAMP(max_x + pad_x, 0, w - 1);
  br->y = (float)CLAMP(max_y + pad_y, 0, h - 1);
  br->label = 3;
  return TRUE;
}

// the n prompts forward to preview pixels, the view the encoded image shows
// at its own scale. NULL on error, free with dt_free_align
static float *_prompts_to_preview(const GList *prompts, const int n)
{
  if(n <= 0) return NULL;
  float iwidth, iheight;
  dt_masks_get_image_size(NULL, NULL, &iwidth, &iheight);
  float *pts = dt_alloc_align_float((size_t)2 * n);
  if(!pts) return NULL;

  int k = 0;
  for(const GList *l = prompts; l; l = g_list_next(l), k++)
  {
    const dt_masks_point_object_t *pt = l->data;
    pts[k * 2] = pt->prompt.pos[0] * iwidth;
    pts[k * 2 + 1] = pt->prompt.pos[1] * iheight;
  }
  if(!dt_dev_distort_transform(darktable.develop, pts, n))
  {
    dt_free_align(pts);
    return NULL;
  }
  return pts;
}

// whether a prompt in preview pixels is in view, which a later crop can
// leave it out of. a pixel of slack keeps one on the edge, or rotated just
// past it, and clamps it in
static gboolean _in_view(float *const p, const float wd, const float ht)
{
  if(p[0] < -1.0f || p[1] < -1.0f || p[0] > wd + 1.0f || p[1] > ht + 1.0f)
    return FALSE;
  p[0] = CLAMPF(p[0], 0.0f, wd);
  p[1] = CLAMPF(p[1], 0.0f, ht);
  return TRUE;
}

static inline void _map_point(const float a[2], const float b[2],
                              const float in[2], float out[2])
{
  out[0] = in[0] * a[0] + b[0];
  out[1] = in[1] * a[1] + b[1];
}

// cairo_curve_to through mask coordinates, mapped by a and b
static void _curve_to(cairo_t *cr,
                      const float a[2],
                      const float b[2],
                      const float c1[2],
                      const float c2[2],
                      const float end[2])
{
  float p[3][2];
  _map_point(a, b, c1, p[0]);
  _map_point(a, b, c2, p[1]);
  _map_point(a, b, end, p[2]);
  cairo_curve_to(cr, p[0][0], p[0][1], p[1][0], p[1][1], p[2][0], p[2][1]);
}

// the prompts in view, in encoded pixels, into out: the decoder has nothing
// to relate the others to
static int _encoded_prompts(const _object_data_t *d, dt_seg_point_t *out)
{
  // the inverse of the mapping the mask is drawn and stored by, so a click
  // and the pixel under it agree. as a pixel index, pixel j at j, like the
  // peak and box prompts: the decoder adds the half pixel itself
  float a[2], b[2];
  if(!dt_masks_pixel_grid_to_preview(&d->grid, d->grid.rw, d->grid.rh, a, b)) return 0;
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  float *pts = _prompts_to_preview(d->prompts, g_list_length(d->prompts));
  if(!pts) return 0;

  int count = 0, k = 0;
  for(const GList *l = d->prompts; l; l = g_list_next(l), k++)
  {
    float *const p = pts + 2 * k;
    if(!_in_view(p, wd, ht)) continue;
    const dt_masks_point_object_t *pt = l->data;
    out[count].x = (p[0] - b[0]) / a[0] - 0.5f;
    out[count].y = (p[1] - b[1]) / a[1] - 0.5f;
    out[count].label = (int)pt->prompt.label;
    count++;
  }
  dt_free_align(pts);
  return count;
}

static void _run_decoder(_object_data_t *d)
{
  if(!d || !d->seg || !dt_seg_is_encoded(d->seg) || !d->prompts)
    return;

  // dt_gui_cursor_set_busy below pumps the main loop, so a queued event can
  // reenter here. a second decode would race this one on the shared
  // segmentation context; _free_data defers freeing d while the flag is up
  if(d->decoding)
    return;
  d->decoding = TRUE;

  // wait for encode thread: warmup may still be running after ENCODE_READY
  if(d->encode_thread)
  {
    g_thread_join(d->encode_thread);
    d->encode_thread = NULL;
  }

  // headroom: one peak point per pass + 2 box corners (SAM only)
  const int n_passes = CLAMP(dt_conf_get_int(CONF_OBJECT_REFINE_PASSES_KEY),
                             1, 3);
  dt_seg_point_t *points = g_new(dt_seg_point_t,
                                 g_list_length(d->prompts) + n_passes + 2);
  // every decode sends all the prompts in view at once
  int n_points = _encoded_prompts(d, points);
  if(n_points == 0)
  {
    g_free(points);
    d->decoding = FALSE;
    return;
  }

  dt_gui_cursor_set_busy();

  // the connected component filter keeps what the last foreground prompt is in
  int seed_x = -1, seed_y = -1;
  for(int i = n_points - 1; i >= 0; i--)
  {
    if(points[i].label == 1)
    {
      // pixel j covers [j - 0.5, j + 0.5)
      seed_x = (int)lrintf(points[i].x);
      seed_y = (int)lrintf(points[i].y);
      break;
    }
  }

  const float threshold
    = CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY), 0.3f, 0.9f);
  const gboolean supports_box = dt_seg_supports_box(d->seg);
  int mw = 0, mh = 0;
  float *mask = NULL;
  gboolean box_added = FALSE;

  for(int pass = 0; pass < n_passes; pass++)
  {
    float *new_mask = dt_seg_compute_mask(d->seg, points, n_points, &mw, &mh);
    if(!new_mask) break;

    if(mask && _mask_iou(mask, new_mask, (size_t)mw * mh, threshold) > 0.99f)
    {
      g_free(mask);
      mask = new_mask;
      dt_print(DT_DEBUG_AI,
               "[object mask] converged at pass %d/%d", pass + 1, n_passes);
      break;
    }
    g_free(mask);
    mask = new_mask;

    if(pass + 1 >= n_passes) break;

    gboolean any_added = FALSE;
    dt_seg_point_t peak;
    if(_find_peak_point(mask, mw, mh, threshold,
                        points, n_points, 8.0f, &peak))
    {
      points[n_points++] = peak;
      any_added = TRUE;
    }
    if(supports_box && !box_added)
    {
      dt_seg_point_t tl, br;
      if(_compute_bbox(mask, mw, mh, threshold, 0.05f, &tl, &br))
      {
        points[n_points++] = tl;
        points[n_points++] = br;
        box_added = TRUE;
        any_added = TRUE;
      }
    }
    if(!any_added) break;
  }
  g_free(points);

  if(mask)
  {
    // remove disconnected blobs: keep only the component at the seed point
    seed_x = CLAMP(seed_x, 0, mw - 1);
    seed_y = CLAMP(seed_y, 0, mh - 1);
    _keep_seed_component(mask, mw, mh, threshold, seed_x, seed_y);

    // optional DenseCRF edge refinement using the encoded RGB as guide
    if(dt_conf_get_bool(CONF_OBJECT_REFINE_BOUNDARY_KEY))
    {
      int rgb_w = 0, rgb_h = 0;
      const uint8_t *rgb = dt_seg_get_encoded_rgb(d->seg, &rgb_w, &rgb_h);
      if(rgb && rgb_w == mw && rgb_h == mh)
      {
        const int crf_iter
          = CLAMP(dt_conf_get_int(CONF_OBJECT_REFINE_BOUNDARY_ITER_KEY),
                  1, 10);
        const float crf_sigma_color
          = CLAMP(dt_conf_get_float(CONF_OBJECT_REFINE_BOUNDARY_SIGMA_COLOR_KEY),
                  1.0f, 50.0f);
        const float crf_w_bilateral
          = CLAMP(dt_conf_get_float(CONF_OBJECT_REFINE_BOUNDARY_W_BILATERAL_KEY),
                  0.5f, 30.0f);
        const double t0 = dt_get_wtime();
        dt_dense_crf_binary(mask, rgb, mw, mh,
                            5.0f, crf_sigma_color,
                            3.0f, crf_w_bilateral, crf_iter);
        dt_print(DT_DEBUG_AI,
                 "[object mask] CRF refinement: %dx%d (%.2fs)",
                 mw, mh, dt_get_wtime() - t0);
      }
    }

    g_free(d->mask);
    d->mask = mask;
    d->mask_w = mw;
    d->mask_h = mh;
    _schedule_outline(d);
  }
  d->decoding = FALSE;
  dt_gui_cursor_clear_busy();
}

// transform mask-space forms to input-normalized coords and register them,
// takes ownership of `forms` and `signs` lists (forms are appended to dev->forms)
static dt_masks_form_t *
_register_vectorized_forms(GList *forms,
                           GList *signs,
                           const float a[2],
                           const float b[2])
{
  // darktable mask coordinates are stored in input-image-normalized space:
  //   coord = backtransform(backbuf_pixel) / iwidth
  // this undoes all geometric pipeline transforms (crop, rotation, lens, etc.)
  // so that the mask can be applied at any point in the pipeline
  float iwidth, iheight;
  dt_masks_get_image_size(NULL, NULL, &iwidth, &iheight);

  // vectorized coordinates are in mask space (encoding resolution), mapped to
  // the preview pipe pixel space dt_dev_distort_backtransform expects by a and
  // b from dt_masks_pixel_grid_to_preview. potrace puts pixel i at [i, i + 1),
  // as they do
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    const int npts = g_list_length(f->points);
    if(npts == 0)
      continue;

    // collect all coordinates into a flat array for batch backtransform,
    // each path point has 3 coordinate pairs: corner, ctrl1, ctrl2
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

    for(int j = 0; j < npts * 6; j += 2)
      _map_point(a, b, pts + j, pts + j);

    // fails while the pipe is shorter than the history (develop.c:3155),
    // leaving the points in preview space, which must not reach the xmp
    if(!dt_dev_distort_backtransform(darktable.develop, pts, npts * 3))
    {
      g_free(pts);
      g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
      g_list_free(signs);
      dt_control_log(_("could not store the object, it is discarded"));
      return NULL;
    }

    // write back and normalize by input image dimensions
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

  // always wrap paths in a group; holes use difference mode

  // count existing AI object groups/paths for numbering
  dt_develop_t *dev = darktable.develop;
  const char *group_prefix = _("ai object group");
  const char *path_prefix = _("ai object");

  guint grp_nb = 0;
  guint path_nb = 0;
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    const dt_masks_form_t *f = l->data;
    if(strncmp(f->name, group_prefix, strlen(group_prefix)) == 0)
      grp_nb++;
    if(strncmp(f->name, path_prefix, strlen(path_prefix)) == 0)
      path_nb++;
  }
  grp_nb++;
  path_nb++;
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    snprintf(f->name, sizeof(f->name),
             "%s #%d", path_prefix, (int)path_nb++);
  }

  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  snprintf(grp->name, sizeof(grp->name), "%s #%d", group_prefix, (int)grp_nb);

  // register all path forms so they exist in dev->forms
  for(GList *l = forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = l->data;
    dev->forms = g_list_append(dev->forms, f);
  }

  // add each path to the group; holes get difference mode
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

  // register the group (history item added by caller after blend mask
  // assignment)
  dev->forms = g_list_append(dev->forms, grp);

  g_list_free(forms);
  g_list_free(signs);

  dt_print(DT_DEBUG_MASKS, "[object mask] created %d paths", nbform);
  return grp;
}

// the selection stored in the sidecar: the form's new points, or NULL to
// leave it alone. outside the encoded view an edit keeps old's mask
static GList *_finalize_raster(const _object_data_t *d, const dt_masks_form_t *old)
{
  if(!d || !d->mask || d->mask_w <= 0 || d->mask_h <= 0 || !d->prompts)
  {
    dt_print(DT_DEBUG_MASKS, "[object mask] raster: no mask buffer");
    return NULL;
  }

  // at the encoded view's density, so a crop keeps the detail the encoder
  // saw, up to the shared cap
  int tw = 0, th = 0;
  if(!dt_masks_pixel_store_size(d->mask_w, d->mask_h, DT_MASKS_PIXEL_MAX_STORED,
                                &tw, &th))
  {
    dt_print(DT_DEBUG_MASKS, "[object mask] raster: no view to store against");
    return NULL;
  }

  // the points (the reference, then the prompts in click order) are
  // allocated before the write: failing after it would leave an unreferenced
  // entry, and reclaiming one rewrites the zip, allocating when memory just
  // ran short
  GList *prompts = _copy_prompts(d->prompts);
  dt_masks_point_object_t *head =
    prompts ? calloc(1, sizeof(dt_masks_point_object_t)) : NULL;
  if(!head)
  {
    g_list_free_full(prompts, free);
    return NULL;
  }

  char *model = dt_ai_models_get_active_for_task("mask");
  // the model id and version, as dtdata.h describes the producer
  gchar *producer = model
    ? g_strdup_printf("%s %s", model, dt_ai_model_get_version(model))
    : g_strdup("");
  const gboolean ok =
    dt_masks_pixel_store(d->mask, d->mask_w, d->mask_h, &d->grid, tw, th,
                         old ? dt_masks_pixel_ref(old) : NULL,
                         producer, &head->ref);
  g_free(producer);
  g_free(model);
  if(!ok)
  {
    g_list_free_full(prompts, free);
    free(head);
    return NULL;
  }

  return g_list_prepend(prompts, head);
}

// --- mask event handlers ---

static int _object_events_mouse_scrolled(dt_iop_module_t *module,
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

  // the trace settings, only once there is a selection to trace as paths
  if(d && d->has_selection && d->mask && _commits_paths(form))
  {
    if(dt_modifier_is(state, 0))
    {
      // plain scroll: adjust smoothing (potrace alphamax)
      const float smoothing =
        CLAMP(dt_conf_get_float(CONF_OBJECT_SMOOTHING_KEY) + (up ? 0.05f : -0.05f),
              0.0f, 1.3f);
      dt_conf_set_float(CONF_OBJECT_SMOOTHING_KEY, smoothing);
      dt_toast_log(_("smoothing: %3.2f"), smoothing);
      dt_dev_masks_list_change(darktable.develop);
      _schedule_outline(d);
      dt_control_queue_redraw_center();
      return 1;
    }
    if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      // shift+scroll: adjust cleanup (potrace turdsize)
      const int cleanup =
        CLAMP(dt_conf_get_int(CONF_OBJECT_CLEANUP_KEY) + (up ? 5 : -5), 0, 100);
      dt_conf_set_int(CONF_OBJECT_CLEANUP_KEY, cleanup);
      dt_toast_log(_("cleanup: %d"), cleanup);
      dt_dev_masks_list_change(darktable.develop);
      _schedule_outline(d);
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  // ctrl+scroll is left to dt_masks_events_mouse_scrolled, as for every
  // shape: the opacity a new object will get, or the edited object's own
  return 0;
}

// clear the selection, its outline, the decoder's refinement state and the
// prompts in view. the others stay, as a commit keeps the stored mask out
// of view
static void _clear_selection(_object_data_t *d)
{
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  float *pts = _prompts_to_preview(d->prompts, g_list_length(d->prompts));
  int k = 0;
  for(GList *l = d->prompts; l; k++)
  {
    GList *next = g_list_next(l);
    // without the transformed positions there is no telling which prompt is
    // in view, so all of them stay: they are the only way to regenerate the
    // object once its pixels are gone
    if(pts && _in_view(pts + 2 * k, wd, ht))
    {
      free(l->data);
      d->prompts = g_list_delete_link(d->prompts, l);
    }
    l = next;
  }
  dt_free_align(pts);

  g_free(d->mask);
  d->mask = NULL;
  d->mask_w = d->mask_h = 0;

  if(d->seg)
    dt_seg_reset_prev_mask(d->seg);

  d->has_selection = FALSE;
  _free_outline(d);

  dt_control_queue_redraw_center();
}

// resume an edit from the stored mask, or decode the prompts if it is lost.
// the decode pumps the main loop, so this runs from a click or an idle, not
// a draw handler; a click passes decode FALSE, as it decodes after adding
// its prompt. FALSE when stored pixels could not be restored yet: retry later
static gboolean _resume_edit(_object_data_t *d,
                             const dt_masks_form_t *form,
                             const gboolean decode)
{
  const dt_dtdata_ref_t *ref = dt_masks_pixel_ref(form);
  // no reference: nothing to come back to or to wait for
  if(!ref) return TRUE;
  // without the encoding the stored mask maps onto, the flag stays clear and
  // the next click or redraw tries again
  if(!d->seg || d->grid.rw <= 0 || d->grid.rh <= 0) return FALSE;

  d->restored = TRUE;
  d->changed = FALSE;

  gboolean transient = FALSE;
  dt_masks_pixel_cache_t *c =
    dt_masks_pixel_get(darktable.develop->image_storage.id, ref, &transient);
  // g_malloc, as every other d->mask
  const int rw = d->grid.rw, rh = d->grid.rh;
  float *mask = c ? g_try_malloc(sizeof(float) * rw * rh) : NULL;
  if(mask && dt_masks_pixel_to_render(c, &d->grid, rw, rh, mask))
  {
    g_free(d->mask);
    d->mask = mask;
    d->mask_w = rw;
    d->mask_h = rh;
    d->has_selection = TRUE;
    dt_seg_set_prev_mask(d->seg, mask, d->mask_w, d->mask_h);
  }
  else
  {
    g_free(mask);
    // the entry was read, so whatever failed here is a shortage of memory or
    // a pipe that is not ready yet, not a lost mask
    transient = transient || c != NULL;
  }
  dt_masks_pixel_release(c);

  d->restore_short = transient;
  if(transient)
  {
    // retry on the next click: decoding the prompts instead would commit an
    // approximation over stored pixels that are still intact
    d->restored = FALSE;
    return FALSE;
  }

  // the stored prompts first: a click can land before the edit is restored
  d->prompts = g_list_concat(_copy_prompts(g_list_next(form->points)), d->prompts);
  if(!d->mask && d->prompts)
  {
    // the context outlives sessions, so no earlier mask may leak in
    dt_seg_reset_prev_mask(d->seg);
    d->has_selection = TRUE;
    if(decode)
    {
      _run_decoder(d);
      d->changed = d->mask != NULL;
    }
  }
  return TRUE;
}

// queued by the redraw after the image is encoded, to restore the stored
// mask outside the draw handler
static gboolean _resume_edit_cb(gpointer data)
{
  _object_data_t *d = data;
  d->resume_id = 0;
  const dt_masks_form_t *form = darktable.develop->form_visible;
  // the tool may have been left, or another form opened, since this was
  // queued, and the scratchpad is then no longer the edit's
  if(_get_data(darktable.develop->form_gui) == d
     && !d->restored
     && g_atomic_int_get(&d->encode_state) == ENCODE_READY
     && _is_edit(form))
  {
    _resume_edit(d, form, TRUE);
    // only when something came back: a redraw after a restore that failed
    // would arm this again from post_expose, and so on at frame rate
    if(d->restored)
      dt_control_queue_redraw_center();
  }
  return G_SOURCE_REMOVE;
}

// leave the tool, committed or not, and select what it leaves behind, which
// also rebuilds the mask manager now rather than on its next lazy redraw
static void _leave_creation(dt_iop_module_t *module,
                            dt_masks_form_gui_t *gui,
                            const dt_mask_id_t select)
{
  gui->creation = FALSE;
  gui->creation_continuous = FALSE;
  gui->creation_continuous_module = NULL;
  gui->creation_module = NULL;

  _free_data(gui);

  dt_control_hinter_message("");

  // dt_masks_set_edit_mode requires a non-NULL module (it returns
  // immediately otherwise), so clear the form directly when module
  // is NULL (standalone mask creation)
  if(module)
  {
    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
  }
  else
  {
    dt_masks_change_form_gui(NULL);
  }
  if(dt_is_valid_maskid(select))
    dt_dev_masks_selection_change(darktable.develop, module, select);
  dt_control_queue_redraw_center();
}

gboolean dt_masks_object_cancel_edit(void)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_gui_t *gui = dev->form_gui;
  const dt_masks_form_t *form = dev->form_visible;
  // a new object has nothing to go back to, so escape leaves it alone
  if(!gui || !gui->creation || !form || !(form->type & DT_MASKS_OBJECT)
     || !_is_edit(form))
    return FALSE;

  // as for right-click: not while the background thread runs. the key is
  // still taken, as letting it through would cancel something else instead
  _object_data_t *d = _get_data(gui);
  if(d && g_atomic_int_get(&d->encode_state) == ENCODE_RUNNING)
    return TRUE;

  _leave_creation(gui->creation_module, gui, form->formid);
  return TRUE;
}

// TRUE when some pixel passes the threshold. below it the model leaves only
// a faint residue, which stored would apply the module weakly over its whole
// extent, or almost everywhere inverted
static gboolean _selection_found(const _object_data_t *d)
{
  if(!d || !d->has_selection || !d->mask) return FALSE;
  const float threshold
    = CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY), 0.3f, 0.9f);
  const size_t n = (size_t)d->mask_w * d->mask_h;
  for(size_t i = 0; i < n; i++)
    if(d->mask[i] > threshold) return TRUE;
  return FALSE;
}

// an edit goes to the form listed under its id now, since a history change
// replaces the edited one. unchanged or cleared, the object stays as it was
static dt_mask_id_t _commit_edit(dt_iop_module_t *module,
                                 const _object_data_t *d,
                                 const dt_masks_form_t *form)
{
  dt_masks_form_t *live = dt_masks_get_from_id(darktable.develop, form->formid);
  if(!live)
  {
    dt_control_log(_("the object was removed, the edit is discarded"));
    return NO_MASKID;
  }
  if(_selection_found(d) && d->changed)
  {
    GList *points = _finalize_raster(d, live);
    if(points)
    {
      g_list_free_full(live->points, free);
      live->points = points;
      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    }
    else
      dt_control_log(_("could not store the object in the sidecar, it is not changed"));
  }
  return live->formid;
}

// a new object traced into paths, grouped into the module's mask group
static dt_mask_id_t _commit_paths(dt_iop_module_t *module, const _object_data_t *d)
{
  GList *forms = NULL, *signs = NULL;
  if(!d || !d->mask || !_trace(d, &forms, &signs)) return NO_MASKID;
  float a[2], b[2];
  if(!dt_masks_pixel_grid_to_preview(&d->grid, d->mask_w, d->mask_h, a, b))
  {
    g_list_free_full(forms, (GDestroyNotify)dt_masks_free_form);
    g_list_free(signs);
    return NO_MASKID;
  }
  dt_masks_form_t *grp = _register_vectorized_forms(forms, signs, a, b);
  if(!grp) return NO_MASKID;

  dt_develop_t *dev = darktable.develop;
  if(module)
  {
    dt_masks_form_t *mod_grp = dt_masks_get_from_id(dev, module->blend_params->mask_id);
    if(!mod_grp)
    {
      mod_grp = dt_masks_create(DT_MASKS_GROUP);
      gchar *module_label = dt_history_item_get_name(module);
      // the string masks.c names a module's group with, so a group made
      // here is not a second msgid saying the same thing
      snprintf(mod_grp->name, sizeof(mod_grp->name),
               _("group `%s'"), module_label);
      g_free(module_label);
      dev->forms = g_list_append(dev->forms, mod_grp);
      module->blend_params->mask_id = mod_grp->formid;
    }
    dt_masks_group_add_form(mod_grp, grp);
  }
  dt_dev_add_masks_history_item(dev, module, TRUE);
  return grp->formid;
}

// a new object as pixels, or as paths when they cannot be stored
static dt_mask_id_t _commit_pixels(dt_iop_module_t *module,
                                   dt_masks_form_gui_t *gui,
                                   const _object_data_t *d,
                                   dt_masks_form_t *form)
{
  GList *points = _finalize_raster(d, NULL);
  if(!points)
  {
    // the fallback can fail too, and then there is no object at all
    const dt_mask_id_t traced = _commit_paths(module, d);
    dt_control_log(dt_is_valid_maskid(traced)
                   ? _("could not store the object in the sidecar, it is saved as paths")
                   : _("could not store the object, it is discarded"));
    return traced;
  }
  form->points = points;
  dt_masks_gui_form_save_creation(darktable.develop, module, form, gui);
  return form->formid;
}

static int _object_events_button_pressed(dt_iop_module_t *module,
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

  _object_data_t *d = _get_data(gui);
  // can be dispatched from the main loop a decode pumps: every branch below
  // would discard or commit a selection the decode is about to replace.
  // encode_state does not cover this, staying ENCODE_READY during a decode
  if(d && d->decoding)
    return 1;

  if(which == 1 && dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK))
  {
    // ctrl+shift+click: clear selection (only after first selection)
    if(d && d->has_selection && d->encode_state == ENCODE_READY)
    {
      _clear_selection(d);
      if(darktable.develop->proxy.masks.module)
        darktable.develop->proxy.masks.list_change(
          darktable.develop->proxy.masks.module);
    }
    return 1;
  }
  else if(which == 1)
  {
    // off the image, a click would be a prompt the decoder never sees
    if(pzx < 0.0f || pzy < 0.0f || pzx >= 1.0f || pzy >= 1.0f)
      return 1;

    // need valid scratchpad and completed encoding
    if(!d || d->encode_state != ENCODE_READY)
      return 1;

    // dismiss the "ready" hint now that the user is interacting
    dt_control_log_ack_all();

    // start drag tracking, resolved as click on button release
    float wd, ht, iwidth, iheight;
    dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);

    d->dragging = TRUE;
    d->drag_start_x = pzx * wd;
    d->drag_start_y = pzy * ht;
    return 1;
  }
  else if(which == 3)
  {
    // don't exit while background threads are running
    if(d && g_atomic_int_get(&d->encode_state) == ENCODE_RUNNING)
      return 1;

    // the module the tool started for, not the focused one. none from the mask
    // manager without a module's row, or for an object not among its masks
    dt_iop_module_t *crea_module = gui->creation_module;
    const gboolean has_mask = _selection_found(d);
    dt_mask_id_t select = NO_MASKID;
    if(_is_edit(form))
      select = _commit_edit(crea_module, d, form);
    else if(has_mask && _commits_paths(form))
      select = _commit_paths(crea_module, d);
    else if(has_mask)
      select = _commit_pixels(crea_module, gui, d, form);
    else if(d && d->has_selection)
      dt_control_log(_("no mask extracted from AI segmentation"));
    _leave_creation(crea_module, gui, select);
    return 1;
  }

  return 0;
}

static int _object_events_button_released(dt_iop_module_t *module,
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
  (void)parentid;
  (void)index;

  if(which != 1)
    return 0;

  _object_data_t *d = _get_data(gui);
  // the press is consumed whichever way we leave: the press handler can
  // return without setting dragging, and a release after that would reuse
  // the previous click's coordinates
  const gboolean dragging = d && d->dragging;
  if(d)
    d->dragging = FALSE;

  // dispatched from the main loop a decode pumps: the decode for this prompt
  // would be refused, leaving the mask and the prompts out of step
  if(d && d->decoding)
    return 1;

  if(!dragging)
    return 0;

  // to input-image coordinates, before anything changes: the backtransform
  // fails while the pipe is shorter than the history (develop.c:3155),
  // leaving the point in preview space, which must not reach the sidecar or
  // the xmp. nothing is touched yet, so the next click starts clean
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  float pt[2] = { d->drag_start_x, d->drag_start_y };
  if(!dt_dev_distort_backtransform(darktable.develop, pt, 1))
  {
    dt_control_log(_("the image is not ready yet, click again"));
    return 1;
  }

  // restore the stored mask and prompts before adding this click: a click is
  // accepted once the encoding is ready, possibly before the queued restore
  // has run. the decoder runs once below, on all the prompts together
  if(!d->restored && _is_edit(form) && !_resume_edit(d, form, FALSE))
  {
    // the stored mask exists but could not be read yet: refining without it
    // and committing would replace the pixels still on disk
    dt_control_log(_("the image is not ready yet, click again"));
    return 1;
  }

  // calloc: the union leaves most of a prompt unused, and the blob is
  // hashed and stored whole
  dt_masks_point_object_t *prompt = calloc(1, sizeof(dt_masks_point_object_t));
  if(!prompt)
    return 1;

  prompt->prompt.pos[0] = pt[0] / iwidth;
  prompt->prompt.pos[1] = pt[1] / iheight;
  // click: foreground point, shift+click: background point (only
  // after first selection)
  prompt->prompt.label = (d->has_selection && dt_modifier_is(state, GDK_SHIFT_MASK))
    ? 0.0f : 1.0f;
  d->prompts = g_list_append(d->prompts, prompt);
  d->has_selection = TRUE;
  d->changed = TRUE;

  _run_decoder(d);

  // refresh mask properties panel so sliders update for
  // the current creation step (size vs cleanup/smoothing)
  if(darktable.develop->proxy.masks.module)
    darktable.develop->proxy.masks.list_change(darktable.develop->proxy.masks.module);

  dt_control_queue_redraw_center();
  return 1;
}

static int _object_events_mouse_moved(dt_iop_module_t *module,
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

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;

  dt_control_queue_redraw_center();

  return 1;
}

// timer callback: periodically redraw center so +/- cursor tracks shift key
static gboolean _modifier_poll(gpointer data)
{
  (void)data;
  dt_control_queue_redraw_center();
  return G_SOURCE_CONTINUE;
}

static void _object_events_post_expose(cairo_t *cr,
                                       const float zoom_scale,
                                       dt_masks_form_gui_t *gui,
                                       const int index,
                                       const int num_points)
{
  (void)index;
  (void)num_points;

  // ensure scratchpad exists
  _object_data_t *d = _get_data(gui);
  if(!d)
  {
    d = g_new0(_object_data_t, 1);

    // restore persistent model (stays loaded across mask sessions)
    // if the active model changed in preferences, discard the old one
    {
      dt_ai_seg_t *ps = &darktable.ai_seg;
      char *active = dt_ai_models_get_active_for_task("mask");
      const char *persistent_id = dt_seg_get_model_id(ps->ctx);
      if(ps->ctx && active
         && g_strcmp0(active, persistent_id) != 0)
      {
        dt_print(DT_DEBUG_AI,
                 "[object mask] model changed (%s -> %s), "
                 "discarding persistent model",
                 persistent_id, active);
        dt_seg_free(ps->ctx);
        ps->ctx = NULL;
        dt_ai_env_destroy(ps->env);
        ps->env = NULL;
        ps->model_loaded = FALSE;
      }
      g_free(active);
      d->env = ps->env;
      d->seg = ps->ctx;
      d->model_loaded = ps->model_loaded;
      ps->env = NULL;
      ps->ctx = NULL;
      ps->model_loaded = FALSE;

      // connect view-change signal once to free model on darkroom exit
      if(!ps->signal_connected)
      {
        DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                                  _on_view_changed, NULL);
        ps->signal_connected = TRUE;
      }
    }

    gui->scratchpad = d;
    gui->scratchpad_cleanup = _free_data;
  }

  // detect distortion changes (crop/rotate on same image):
  // reset encoding so the image is re-analyzed
  const dt_imgid_t cur_imgid = darktable.develop->image_storage.id;
  const int cur_state = g_atomic_int_get(&d->encode_state);
  // not during a decode: this draw can run from the loop _run_decoder pumps,
  // and the reset would free what the decode works from. the rest of the
  // draw only reads d
  if(!d->decoding
     && (cur_state == ENCODE_READY || cur_state == ENCODE_ERROR)
     && (d->encoded_imgid != cur_imgid
         || d->encoded_distort_hash != dt_masks_pixel_distort_hash(darktable.develop)))
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
    d->grid = (dt_masks_pixel_grid_t){ 0 };
    d->encode_state = ENCODE_IDLE;
    // reset selection, outline, and prompts so the new image starts fresh
    d->has_selection = FALSE;
    d->restored = FALSE;
    d->restore_short = FALSE;
    d->changed = FALSE;
    _clear_prompts(d);
    _free_outline(d);
  }

  // eager encoding: load model and encode image as soon as tool opens
  if(d->encode_state == ENCODE_IDLE)
  {
    dt_control_log(_("object mask: analyzing image..."));
    d->encode_state = ENCODE_MSG_SHOWN;
    dt_control_queue_redraw_center();
    return;
  }

  if(d->encode_state == ENCODE_MSG_SHOWN)
  {
    // frame 2: launch background thread to render and encode the image.
    // the thread creates a temporary export pipe at high resolution
    // instead of using the low-res preview backbuf.
    // flush history to database so the encode thread's dt_dev_load_image
    // sees the current edits (crop/rotate may not be flushed yet)
    dt_dev_write_history(darktable.develop);

    const dt_hash_t cur_hash = dt_masks_pixel_distort_hash(darktable.develop);

    _encode_thread_data_t *td = g_new(_encode_thread_data_t, 1);
    td->d = d;
    td->imgid = cur_imgid;
    td->history_end = darktable.develop->history_end;
    td->distort_hash = cur_hash;

    d->encoded_imgid = cur_imgid;
    d->encoded_distort_hash = cur_hash;
    d->encode_state = ENCODE_RUNNING;
    // start poll timer BEFORE the thread, it will detect completion
    // and also tracks modifier keys once encoding is ready
    if(!d->modifier_poll_id)
      d->modifier_poll_id = g_timeout_add(100, _modifier_poll, NULL);
    d->encode_thread = g_thread_new("ai-mask-encode", _encode_thread_func, td);
    return;
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_RUNNING)
  {
    // keep the message visible while the thread is working
    dt_control_log(_("object mask: analyzing image..."));
    return;
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_READY && d->encode_thread)
  {
    // thread finished (detected by poll timer redraw), join it
    g_thread_join(d->encode_thread);
    d->encode_thread = NULL;
    dt_control_log_ack_all();
    if(!_is_edit(darktable.develop->form_visible))
      dt_control_log(_("click on object to create mask"));
  }

  if(g_atomic_int_get(&d->encode_state) == ENCODE_ERROR)
  {
    if(d->encode_thread)
    {
      g_thread_join(d->encode_thread);
      d->encode_thread = NULL;
      // log only once when the thread is first joined
      dt_control_log(_("object mask preparation failed"));
    }
    return;
  }

  if(d->encode_state != ENCODE_READY)
    return;

  // the restore may decode, which pumps the main loop: not from inside
  // cairo's draw, where a dispatched event could commit the edit and free d
  if(!d->restored && !d->restore_short && !d->resume_id
     && _is_edit(darktable.develop->form_visible))
    d->resume_id = g_idle_add(_resume_edit_cb, d);

  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);

  // the selection and its outline as they will be stored
  float a[2], b[2];
  const gboolean placed =
    d->mask && dt_masks_pixel_grid_to_preview(&d->grid, d->mask_w, d->mask_h, a, b);

  // --- Draw red overlay of current mask ---
  if(placed)
  {
    // the overlay has to predict the commit: thresholded where potrace will
    // cut the contour, in proportion where the pixels are stored as they are
    const gboolean traced = _commits_paths(darktable.develop->form_visible);
    const float mask_thresh =
      CLAMP(dt_conf_get_float(CONF_OBJECT_THRESHOLD_KEY), 0.3f, 0.9f);
    cairo_surface_t *surface
      = dt_masks_pixel_tint(d->mask, d->mask_w, d->mask_h, !traced, mask_thresh);
    if(surface)
    {
      cairo_save(cr);
      cairo_translate(cr, b[0], b[1]);
      cairo_scale(cr, a[0], a[1]);
      cairo_set_source_surface(cr, surface, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
      cairo_surface_destroy(surface);
    }
  }

  // draw the outline (real path style with anchor dots)
  if(placed && d->outline_forms)
  {
    for(GList *fl = d->outline_forms; fl; fl = g_list_next(fl))
    {
      dt_masks_form_t *f = fl->data;
      GList *pts = f->points;
      if(!pts) continue;

      dt_masks_point_path_t *first_pt = pts->data;
      float c[2];
      _map_point(a, b, first_pt->corner, c);
      cairo_move_to(cr, c[0], c[1]);

      // cairo_curve_to(c1, c2, end) expects:
      //   c1 = outgoing handle of previous point (prev.ctrl2)
      //   c2 = incoming handle of this point (this.ctrl1)
      dt_masks_point_path_t *prev_pt = first_pt;
      for(GList *p = g_list_next(pts); p; p = g_list_next(p))
      {
        dt_masks_point_path_t *pt = p->data;
        _curve_to(cr, a, b, prev_pt->ctrl2, pt->ctrl1, pt->corner);
        prev_pt = pt;
      }

      // close path back to first point
      _curve_to(cr, a, b, prev_pt->ctrl2, first_pt->ctrl1, first_pt->corner);

      dt_masks_line_stroke(cr, FALSE, FALSE, FALSE, zoom_scale);

      for(GList *p = pts; p; p = g_list_next(p))
      {
        dt_masks_point_path_t *pt = p->data;
        _map_point(a, b, pt->corner, c);
        dt_masks_draw_anchor(cr, FALSE, zoom_scale, c[0], c[1]);
      }
    }
  }

  // query pointer position and modifier state directly from GDK so the
  // cursor is drawn at the correct location even before the first
  // mouse_moved event fires.
  GtkWidget *cw = dt_ui_center(darktable.gui->ui);
  GdkWindow *win = gtk_widget_get_window(cw);
  GdkDevice *pointer = gdk_seat_get_pointer
    (gdk_display_get_default_seat(gdk_display_get_default()));
  GdkModifierType mod = 0;
  int dev_x = 0, dev_y = 0;
  if(win && pointer)
    gdk_window_get_device_position(win, pointer, &dev_x, &dev_y, &mod);

  // skip indicator when pointer is over a window above us (e.g. prefs)
  if(pointer
     && gdk_device_get_window_at_position(pointer, NULL, NULL) != win)
    return;
  const gboolean has_sel = d && d->has_selection;
  const gboolean ctrl_shift_held
    = has_sel
      && (mod & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
           == (GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  const gboolean shift_held
    = has_sel && !ctrl_shift_held
      && (mod & GDK_SHIFT_MASK) != 0;

  // convert device coordinates to preview pipe pixel space
  {
    float pzx, pzy, zs;
    dt_dev_get_pointer_zoom_pos(&darktable.develop->full,
                                (float)dev_x, (float)dev_y,
                                &pzx, &pzy, &zs);
    gui->posx = pzx * wd;
    gui->posy = pzy * ht;
  }

  // draw cursor indicator for click interaction
  if(gui->posx >= 0.0f && gui->posx <= wd
     && gui->posy >= 0.0f && gui->posy <= ht)
  {
    const float r = DT_PIXEL_APPLY_DPI(8.0f) / zoom_scale;
    const float lw = DT_PIXEL_APPLY_DPI(2.0f) / zoom_scale;
    cairo_set_line_width(cr, lw);

    if(ctrl_shift_held)
    {
      // clear mode: draw undo/revert arrow above cursor
      cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
      const float s = r * 0.7f; // icon size
      const float cx = gui->posx;
      const float cy = gui->posy - s * 1.8f; // above cursor

      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

      // arrowhead pointing left
      const float ax = cx - s * 0.8f;
      const float ay = cy - s;
      cairo_move_to(cr, ax + s * 0.65f, ay - s * 0.6f);
      cairo_line_to(cr, ax, ay);
      cairo_line_to(cr, ax + s * 0.65f, ay + s * 0.6f);
      cairo_stroke(cr);

      // horizontal line from arrow tip to half-circle top
      cairo_move_to(cr, ax, ay);
      cairo_line_to(cr, cx, ay);

      // half-circle curving right and down
      cairo_arc(cr, cx, cy, s, -G_PI * 0.5f, G_PI * 0.5f);

      // small horizontal tail at bottom going left
      cairo_line_to(cr, cx - s * 0.5f, cy + s);
      cairo_stroke(cr);
    }
    else
    {
      cairo_set_source_rgba(cr, 0.9, 0.9, 0.9, 0.9);
      // horizontal line (common to both + and -)
      cairo_move_to(cr, gui->posx - r, gui->posy);
      cairo_line_to(cr, gui->posx + r, gui->posy);
      cairo_stroke(cr);
      if(!shift_held)
      {
        // add mode: vertical line to form "+"
        cairo_move_to(cr, gui->posx, gui->posy - r);
        cairo_line_to(cr, gui->posx, gui->posy + r);
        cairo_stroke(cr);
      }
    }
  }

}

static GSList *_object_events_setup_mouse_actions
  (const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    0,
    _("[OBJECT] select / add foreground point"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    GDK_SHIFT_MASK,
    _("[OBJECT] add background point"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    GDK_CONTROL_MASK | GDK_SHIFT_MASK,
    _("[OBJECT] clear selection"));
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_RIGHT,
    0,
    _("[OBJECT] apply mask"));
  // the trace settings, as _object_events_mouse_scrolled takes them
  if(_commits_paths(form))
  {
    lm = dt_mouse_action_create_simple(
      lm,
      DT_MOUSE_ACTION_SCROLL,
      0,
      _("[OBJECT] change smoothing"));
    lm = dt_mouse_action_create_simple(
      lm,
      DT_MOUSE_ACTION_SCROLL,
      GDK_SHIFT_MASK,
      _("[OBJECT] change cleanup"));
  }
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_SCROLL,
    GDK_CONTROL_MASK,
    _("[OBJECT] change opacity"));
  return lm;
}

static void _object_events_set_hint_message(const dt_masks_form_gui_t *const gui,
                                            const dt_masks_form_t *const form,
                                            const int opacity,
                                            char *const restrict msgbuf,
                                            const size_t msgbuf_len)
{
  if(gui->creation)
  {
    const _object_data_t *d = _get_data((dt_masks_form_gui_t *)gui);
    if(!d || d->encode_state != ENCODE_READY)
      return;  // no hints while encoding
    // the right-click line names what a commit produces, and the trace
    // settings are listed only while they apply
    const gboolean editing = _is_edit(form);
    if(d->has_selection && _commits_paths(form))
      g_snprintf(msgbuf,
                 msgbuf_len,
                 _("<b>add</b>: click, <b>subtract</b>: shift+click, "
                   "<b>clear</b>: ctrl+shift+click, "
                   "<b>apply as paths</b>: right-click\n"
                   "<b>smoothing</b>: scroll (%3.2f), "
                   "<b>cleanup</b>: shift+scroll (%d), "
                   "<b>opacity</b>: ctrl+scroll (%d%%)"),
                 dt_conf_get_float(CONF_OBJECT_SMOOTHING_KEY),
                 dt_conf_get_int(CONF_OBJECT_CLEANUP_KEY), opacity);
    else if(editing)
      g_snprintf(msgbuf,
                 msgbuf_len,
                 _("<b>add</b>: click, <b>subtract</b>: shift+click, "
                   "<b>clear</b>: ctrl+shift+click, "
                   "<b>apply</b>: right-click, <b>cancel</b>: esc\n"
                   "<b>opacity</b>: ctrl+scroll (%d%%)"),
                 opacity);
    else if(d->has_selection)
      g_snprintf(msgbuf,
                 msgbuf_len,
                 _("<b>add</b>: click, <b>subtract</b>: shift+click, "
                   "<b>clear</b>: ctrl+shift+click, "
                   "<b>apply</b>: right-click\n"
                   "<b>opacity</b>: ctrl+scroll (%d%%)"),
                 opacity);
    else
      g_snprintf(msgbuf,
                 msgbuf_len,
                 _("<b>select</b>: click on object, "
                   "<b>opacity</b>: ctrl+scroll (%d%%)"),
                 opacity);
  }
}

static gboolean _refresh_properties(gpointer data)
{
  (void)data;
  dt_dev_masks_list_change(darktable.develop);
  return G_SOURCE_REMOVE;
}

static void _object_modify_property(dt_masks_form_t *const form,
                                    const dt_masks_property_t prop,
                                    const float old_val,
                                    const float new_val,
                                    float *sum,
                                    int *count,
                                    float *min,
                                    float *max)
{
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  _object_data_t *d = gui ? _get_data(gui) : NULL;

  if(!gui || !gui->creation) return;

  // an edit gets neither the switch nor the trace settings. *count left at
  // 0 hides a property's widget (libs/masks.c)
  const gboolean editing = _is_edit(form);
  const gboolean traced = _commits_paths(form);

  switch(prop)
  {
    case DT_MASKS_PROPERTY_SIZE:
      break; // no size slider for click-based interaction
    case DT_MASKS_PROPERTY_CLEANUP:
    {
      if(!traced) break;
      const int old = dt_conf_get_int(CONF_OBJECT_CLEANUP_KEY);
      const int cleanup = CLAMP(old + (int)(new_val - old_val), 0, 100);
      dt_conf_set_int(CONF_OBJECT_CLEANUP_KEY, cleanup);
      if(d && cleanup != old) _schedule_outline(d);
      *sum += cleanup;
      ++*count;
      break;
    }
    case DT_MASKS_PROPERTY_SMOOTHING:
    {
      if(!traced) break;
      const float old = dt_conf_get_float(CONF_OBJECT_SMOOTHING_KEY);
      const float smoothing = CLAMP(old + (new_val - old_val), 0.0f, 1.3f);
      dt_conf_set_float(CONF_OBJECT_SMOOTHING_KEY, smoothing);
      if(d && smoothing != old) _schedule_outline(d);
      *sum += smoothing;
      ++*count;
      break;
    }
    case DT_MASKS_PROPERTY_FEATHER:
    {
      if(!traced) break;
      const float ratio = (!old_val || !new_val) ? 1.0f : new_val / old_val;
      float feather = dt_conf_get_float(CONF_OBJECT_FEATHER_KEY);
      if(feather < 0.0005f && ratio > 1.0f)
        feather = 0.001f; // bootstrap from zero on increase
      feather = CLAMP(feather * ratio, 0.0005f, 1.0f);
      dt_conf_set_float(CONF_OBJECT_FEATHER_KEY, feather);
      *sum += feather + feather; // both borders (same as path)
      *max = fminf(*max, 1.0f / feather);
      *min = fmaxf(*min, 0.0005f / feather);
      *count += 2; // both borders (same as path)
      break;
    }
    case DT_MASKS_PROPERTY_REFINE:
    {
      // toggle applies on the next decoder run, not immediately
      if(new_val != old_val)
        dt_conf_set_bool(CONF_OBJECT_REFINE_BOUNDARY_KEY, new_val > 0.5f);
      const gboolean enabled
        = dt_conf_get_bool(CONF_OBJECT_REFINE_BOUNDARY_KEY);
      *sum += enabled ? 1.0f : 0.0f;
      ++*count;
      break;
    }
    case DT_MASKS_PROPERTY_VECTORIZE:
    {
      if(editing) break;
      // locked on without sidecar files: the collapsed range has the mask
      // manager gray the switch out (libs/masks.c)
      const gboolean locked = !dt_dtdata_enabled();
      if(locked) *max = *min;
      if(new_val != old_val && !locked)
      {
        dt_conf_set_bool(CONF_OBJECT_VECTORIZE_KEY, new_val > 0.5f);
        if(d) _schedule_outline(d);
        // the trace settings show or hide with the switch. not from here:
        // this runs inside the mask manager's update of the switch itself
        g_idle_add(_refresh_properties, NULL);
      }
      *sum += _as_paths() ? 1.0f : 0.0f;
      ++*count;
      break;
    }
    default:;
  }
}

// a click on a committed object's icon reopens it in the tool, which resumes
// from its stored mask once the image is encoded (_resume_edit)
static int _start_edit(dt_iop_module_t *module, dt_masks_form_t *form)
{
  if(!dt_masks_object_available())
  {
    dt_control_log(_("AI model is not available. Check preferences > AI"));
    return 1;
  }
  // an edit rewrites the stored pixels, so it needs the sidecar; the stored
  // mask still renders without it
  if(!dt_dtdata_enabled())
  {
    dt_control_log(_("editing a stored object needs XMP files:"
                     " see preferences > storage > create XMP files"));
    return 1;
  }
  dt_masks_change_form_gui(form);
  // module is the focused one: the commit files history under it and enables
  // it, so it takes the edit only if the object is one of its masks
  darktable.develop->form_gui->creation_module
    = dt_masks_is_in_module(form->formid, module) ? module : NULL;
  dt_control_queue_redraw_center();
  return 1;
}

#endif // HAVE_AI

// --- one table for both lives of the form ---
// the AI tool while it is being created, the stored mask once committed.
// without AI only the second exists: a committed object still renders,
// shows its icon and selects, but none can be made

// an icon the mask gives no place goes to the first foreground click.
// dt_isnan holds under -ffast-math, unlike a comparison
static gboolean _object_fallback_anchor(const dt_masks_form_t *form,
                                        float anchor[2])
{
  float ax = NAN, ay = NAN;
  for(const GList *l = g_list_next(form->points); l && dt_isnan(ax); l = g_list_next(l))
  {
    const dt_masks_point_object_t *pt = l->data;
    if(pt->prompt.label > 0.5f)
    {
      ax = pt->prompt.pos[0];
      ay = pt->prompt.pos[1];
    }
  }
  if(dt_isnan(ax)) return FALSE;
  anchor[0] = ax;
  anchor[1] = ay;
  return TRUE;
}

// the stored mask's side, shared with the other pixel forms (pixel_mask.c)
static const dt_masks_pixel_type_t _object_pixel_type = {
  .icon = dtgtk_cairo_paint_masks_object,
  .fallback_anchor = _object_fallback_anchor,
  .edit_action = N_("[OBJECT] edit shape"),
  .opacity_action = N_("[OBJECT] change opacity"),
};

static void _object_set_form_name(dt_masks_form_t *const form,
                                  const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("ai object #%d"), (int)nb);
}

static GSList *_object_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
#ifdef HAVE_AI
  if(_in_tool())
    return _object_events_setup_mouse_actions(form);
#endif
  return dt_masks_pixel_setup_mouse_actions(&_object_pixel_type);
}

static void _object_set_hint_message(const dt_masks_form_gui_t *const gui,
                                     const dt_masks_form_t *const form,
                                     const int opacity,
                                     char *const restrict msgbuf,
                                     const size_t msgbuf_len)
{
#ifdef HAVE_AI
  if(gui->creation)
  {
    _object_events_set_hint_message(gui, form, opacity, msgbuf, msgbuf_len);
    return;
  }
#endif
  dt_masks_pixel_set_hint_message(opacity, msgbuf, msgbuf_len);
}

static int _object_mouse_moved(dt_iop_module_t *module,
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
  if(!gui) return 0;
#ifdef HAVE_AI
  if(gui->creation)
    return _object_events_mouse_moved(module, pzx, pzy, pressure, which, zoom_scale,
                                      form, parentid, gui, index);
#endif
  return dt_masks_pixel_mouse_moved(module, pzx, pzy, pressure, which, zoom_scale,
                                    form, parentid, gui, index);
}

static int _object_mouse_scrolled(dt_iop_module_t *module,
                                  const float pzx,
                                  const float pzy,
                                  const gboolean up,
                                  const uint32_t state,
                                  dt_masks_form_t *form,
                                  const dt_imgid_t parentid,
                                  dt_masks_form_gui_t *gui,
                                  const int index)
{
#ifdef HAVE_AI
  if(gui && gui->creation)
    return _object_events_mouse_scrolled(module, pzx, pzy, up, state,
                                         form, parentid, gui, index);
#endif
  return dt_masks_pixel_mouse_scrolled(module, pzx, pzy, up, state,
                                       form, parentid, gui, index);
}

static int _object_button_pressed(dt_iop_module_t *module,
                                  const float pzx,
                                  const float pzy,
                                  const double pressure,
                                  const int which,
                                  const int type,
                                  const uint32_t state,
                                  dt_masks_form_t *form,
                                  const dt_imgid_t parentid,
                                  dt_masks_form_gui_t *gui,
                                  const int index)
{
#ifdef HAVE_AI
  if(gui && gui->creation)
    return _object_events_button_pressed(module, pzx, pzy, pressure, which, type, state,
                                         form, parentid, gui, index);
  if(gui && which == 1 && type == GDK_BUTTON_PRESS && gui->form_selected
     && gui->point_selected == 0 && dt_masks_pixel_ref(form))
    return _start_edit(module, form);
#endif
  return 0;
}

static int _object_button_released(dt_iop_module_t *module,
                                   const float pzx,
                                   const float pzy,
                                   const int which,
                                   const uint32_t state,
                                   dt_masks_form_t *form,
                                   const dt_imgid_t parentid,
                                   dt_masks_form_gui_t *gui,
                                   const int index)
{
#ifdef HAVE_AI
  if(gui && gui->creation)
    return _object_events_button_released(module, pzx, pzy, which, state,
                                          form, parentid, gui, index);
#endif
  return dt_masks_pixel_button_released(module, which, form, parentid, gui);
}

static void _object_post_expose(cairo_t *cr,
                                const float zoom_scale,
                                dt_masks_form_gui_t *gui,
                                const int index,
                                const int num_points)
{
  if(!gui) return;
#ifdef HAVE_AI
  if(gui->creation)
  {
    _object_events_post_expose(cr, zoom_scale, gui, index, num_points);
    return;
  }
#endif
  dt_masks_pixel_post_expose(&_object_pixel_type, cr, zoom_scale, gui, index,
                             num_points);
}

static int _object_get_points_border(dt_develop_t *dev,
                                     dt_masks_form_t *form,
                                     float **points,
                                     int *points_count,
                                     float **border,
                                     int *border_count,
                                     const int source,
                                     const dt_iop_module_t *const module)
{
  return dt_masks_pixel_get_points_border(&_object_pixel_type, dev, form,
                                          points, points_count, border,
                                          border_count, source, module);
}

const dt_masks_functions_t dt_masks_functions_object = {
  .point_struct_size = sizeof(struct dt_masks_point_object_t),
  .sanitize_config = NULL,
  .setup_mouse_actions = _object_setup_mouse_actions,
  .set_hint_message = _object_set_hint_message,
#ifdef HAVE_AI
  .modify_property = _object_modify_property,
#else
  .modify_property = NULL,
#endif
  .set_form_name = _object_set_form_name,
  .duplicate_points = dt_masks_pixel_duplicate_points,
  .initial_source_pos = NULL,
  .get_distance = dt_masks_pixel_get_distance,
  .get_points = NULL,
  .get_points_border = _object_get_points_border,
  // the object never joins a clone group, the only user of these
  .get_mask = NULL,
  .get_mask_roi = dt_masks_pixel_get_mask_roi,
  .get_area = NULL,
  .get_source_area = NULL,
  .mouse_moved = _object_mouse_moved,
  .mouse_scrolled = _object_mouse_scrolled,
  .button_pressed = _object_button_pressed,
  .button_released = _object_button_released,
  .post_expose = _object_post_expose
};

#ifdef HAVE_AI
gboolean dt_masks_object_available(void)
{
  if(!dt_ai_registry_is_enabled())
    return FALSE;
  char *model_id = dt_ai_models_get_active_for_task("mask");
  dt_ai_model_t *model = dt_ai_models_get_by_id(model_id);
  g_free(model_id);
  const gboolean available = model && model->status == DT_AI_MODEL_DOWNLOADED;
  dt_ai_model_free(model);
  return available;
}
#endif // HAVE_AI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
