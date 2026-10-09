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

#include "common/colorspaces.h"
#include "common/distance_transform.h"
#include "common/dtdata.h"
#include "common/mipmap_cache.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/pixelpipe_hb.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "views/view.h"

#include <errno.h>
#include <math.h>
#include <string.h>

// --- the stored mask: compiled in every build, AI or not ---

// a committed object's shape is pixels over the whole frame, not points, so
// dt_masks_pixel_get_mask_roi maps the roi back into it, on gradient's grid

// decoded entries, named by the sha1 of their bytes, so none goes stale. the
// cap evicts the least recently used first, where replaced masks end up. it
// holds about 40 masks of an object at the default render size, and five at
// DT_MASKS_PIXEL_MAX_STORED; with more, all are decoded every rerun
#define RASTER_CACHE_BYTES ((size_t)64 << 20)

// how long a lookup trusts the sidecar's presence before asking again, so
// redraws and slow mounts cost at most one stat a second per entry
#define RASTER_CHECK_US G_USEC_PER_SEC

// TRUE or FALSE, or -1 when a removed sidecar cannot be told from an
// unreachable one: a share dropping for a moment, or a drive unmounted with
// its folders, must not cost a working mask
static int _raster_present(const char *path)
{
  GStatBuf st;
  if(!g_stat(path, &st)) return TRUE;
  if(errno != ENOENT && errno != ENOTDIR) return -1;
  gchar *dir = g_path_get_dirname(path);
  const gboolean folder = g_file_test(dir, G_FILE_TEST_IS_DIR);
  g_free(dir);
  return folder ? FALSE : -1;
}

// mask and its size never change, so a reference reads them unlocked; refs
// and evicted take the lock
struct dt_masks_pixel_cache_t
{
  char entry[DT_DTDATA_ENTRY_LEN];
  dt_imgid_t imgid;
  // as stored, 8 bits. NULL for a failed read, not retried while the
  // generation and the sidecar below still stand
  uint8_t *mask;
  int width, height;
  int refs;          // callers still reading mask
  gboolean evicted;  // out of the list, freed by the last release
  // for a failed read, the sidecar generation it failed at, so a mask merged
  // in by a history paste or a local copy is read again. written before the
  // entry is listed, read unlocked after, as are the two below
  guint generation;
  // the sidecar read, or for a failure the one looked in first, and whether
  // it was there. NULL when the image has none. removed or put back, the
  // entry is read again; rewrites keep it there and cost nothing
  gchar *sidecar;
  gboolean existed;
  gint64 checked;    // when its presence was last asked, under the lock
};

// most recently used first
static GList *_cache = NULL;
static size_t _cache_bytes = 0;
// each readable entry's icon place, "imgid:entry" to float[2], kept apart
// from the pixels so that their eviction does not cost every redraw a
// decode: the gui needs no more until an icon is hovered. an entry is named
// by its content, so a place never goes stale; a failed read drops it
static GHashTable *_anchors = NULL;
G_LOCK_DEFINE_STATIC(raster_cache);

// the stored mask's reference, the form's first point. NULL for a new object.
// every pixel form's first point starts with it, whatever follows
const dt_dtdata_ref_t *dt_masks_pixel_ref(const dt_masks_form_t *form)
{
  if(!form->points) return NULL;
  return (const dt_dtdata_ref_t *)form->points->data;
}

static void _raster_free(dt_masks_pixel_cache_t *c)
{
  dt_free_align(c->mask);
  g_free(c->sidecar);
  g_free(c);
}

// a failed read is charged too, or images whose sidecars are missing would
// add entries without end
static size_t _raster_bytes(const dt_masks_pixel_cache_t *c)
{
  return c->mask ? (size_t)c->width * c->height : (size_t)64 << 10;
}

// unlist an entry, freed now or by its last release. called with the lock held
static void _raster_unlist(GList *l)
{
  dt_masks_pixel_cache_t *c = l->data;
  _cache = g_list_delete_link(_cache, l);
  _cache_bytes -= _raster_bytes(c);
  if(c->refs)
    c->evicted = TRUE;
  else
    _raster_free(c);
}

// by address rather than by link. called with the lock and the caller's own
// reference held: released first, the entry could be freed and its address
// reused by a new entry, which the scan would then unlist instead
static void _raster_unlist_entry(const dt_masks_pixel_cache_t *c)
{
  for(GList *l = _cache; l; l = g_list_next(l))
    if(l->data == c)
    {
      _raster_unlist(l);
      return;
    }
}

// least recently used first, down to the cap, never the newest entry
static void _raster_evict(void)
{
  GList *l = g_list_last(_cache);
  while(l && l != _cache && _cache_bytes > RASTER_CACHE_BYTES)
  {
    GList *prev = l->prev;
    _raster_unlist(l);
    l = prev;
  }
}

// called with the lock held
static dt_masks_pixel_cache_t *_raster_lookup(const dt_imgid_t imgid, const char *entry)
{
  for(GList *l = _cache; l; l = g_list_next(l))
  {
    dt_masks_pixel_cache_t *c = l->data;
    if(c->imgid == imgid && !strcmp(c->entry, entry))
    {
      _cache = g_list_remove_link(_cache, l);
      _cache = g_list_concat(l, _cache);
      c->refs++;
      return c;
    }
  }
  return NULL;
}

static void _raster_forget_missing(const dt_imgid_t imgid, const char *entry);

// a referenced entry for dt_masks_pixel_release, with width and height, or NULL
// when missing or damaged. the failure is cached until a commit, a
// concurrent read, a sidecar write or the sidecar put back on disk, or each
// pipe run would log it again; a decoded mask goes once its sidecar does.
// *transient, if given, is TRUE when only memory was short: not a lost mask
dt_masks_pixel_cache_t *dt_masks_pixel_get(const dt_imgid_t imgid,
                                           const dt_dtdata_ref_t *ref,
                                           gboolean *transient)
{
  if(transient) *transient = FALSE;
  if(!ref || !ref->entry[0] || !dt_is_valid_imgid(imgid)) return NULL;

  G_LOCK(raster_cache);
  dt_masks_pixel_cache_t *c = _raster_lookup(imgid, ref->entry);
  const gint64 now = g_get_monotonic_time();
  const gboolean check = c && c->sidecar && now - c->checked >= RASTER_CHECK_US;
  if(check) c->checked = now;
  G_UNLOCK(raster_cache);
  // the sidecar removed or put back behind our back: read again rather than
  // apply pixels no file holds, or keep a failure a file may now answer
  const int present = check ? _raster_present(c->sidecar) : -1;
  if(present >= 0 && present != c->existed)
  {
    G_LOCK(raster_cache);
    _raster_unlist_entry(c);
    G_UNLOCK(raster_cache);
    dt_masks_pixel_release(c);
    c = NULL;
  }
  if(c && !c->mask)
  {
    if(c->generation == dt_dtdata_generation())
    {
      dt_masks_pixel_release(c);
      return NULL;
    }
    // a sidecar was written since the failed read: drop the failure and look
    // again. unlisted before our release, as _raster_unlist_entry requires
    G_LOCK(raster_cache);
    _raster_unlist_entry(c);
    G_UNLOCK(raster_cache);
    dt_masks_pixel_release(c);
    c = NULL;
  }
  if(c) return c;

  // read before the decode: a sidecar written during it then leaves a stale
  // generation, and the next caller retries rather than trust our failure
  const guint generation = dt_dtdata_generation();

  // decode unlocked, so a miss does not stall the other pipes. back to the
  // 8 bits dt_masks_pixel_store stored, a quarter of the floats' memory
  int w = 0, h = 0;
  char from[PATH_MAX] = { 0 };
  float *pixels = dt_dtdata_read_gray_from(imgid, ref, &w, &h, from, sizeof(from));
  // a zero dimension is a damaged entry, remembered like an unreadable one
  const gboolean readable = pixels && w > 0 && h > 0;
  const gboolean existed = readable || (from[0] && g_file_test(from, G_FILE_TEST_EXISTS));
  uint8_t *mask = readable ? dt_alloc_aligned((size_t)w * h) : NULL;
  if(mask)
    for(size_t k = 0; k < (size_t)w * h; k++)
      mask[k] = (uint8_t)(CLIP(pixels[k]) * 255.0f + 0.5f);
  // read but not held: at up to 256M pixels this is a real allocation, and
  // caching it as unreadable would leave the object out until a restart.
  // nor decoded, which dt_dtdata_read_gray_from tells by the size it leaves
  const gboolean oom = (readable && !mask) || (!pixels && w > 0 && h > 0);
  dt_free_align(pixels);
  if(oom)
  {
    // not DT_DEBUG_ALWAYS: nothing caches this, and dt_masks_pixel_post_expose
    // would log it every frame for as long as the shortage lasts
    dt_print(DT_DEBUG_MASKS, "[pixel mask] no memory for the %dx%d mask"
             " '%s'", w, h, ref->entry);
    if(transient) *transient = TRUE;
    return NULL;
  }

  G_LOCK(raster_cache);
  // another pipe may have decoded the same entry meanwhile
  c = _raster_lookup(imgid, ref->entry);
  // and may have failed, having looked before the sidecar was in place: our
  // mask wins. released after the lock, which is not recursive
  dt_masks_pixel_cache_t *stale = NULL;
  if(c && !c->mask && mask)
  {
    _raster_unlist_entry(c);
    stale = c;
    c = NULL;
  }
  if(c)
    dt_free_align(mask);
  else
  {
    c = g_malloc0(sizeof(dt_masks_pixel_cache_t));
    g_strlcpy(c->entry, ref->entry, sizeof(c->entry));
    c->imgid = imgid;
    c->mask = mask;
    c->width = w;
    c->height = h;
    c->refs = 1;
    // only read back for a failure, where it says when to try again
    c->generation = generation;
    c->sidecar = from[0] ? g_strdup(from) : NULL;
    c->existed = existed;
    c->checked = now;
    _cache = g_list_prepend(_cache, c);
    _cache_bytes += _raster_bytes(c);
    _raster_evict();
    // lost since it was read: its icon is struck through again
    if(!mask && _anchors)
    {
      gchar *key = g_strdup_printf("%d:%s", imgid, c->entry);
      g_hash_table_remove(_anchors, key);
      g_free(key);
    }
  }
  G_UNLOCK(raster_cache);
  dt_masks_pixel_release(stale);
  if(c && !c->mask)
  {
    dt_masks_pixel_release(c);
    return NULL;
  }
  // read, so a sidecar put back clears what its loss reported
  if(c) _raster_forget_missing(imgid, ref->entry);
  return c;
}

void dt_masks_pixel_release(dt_masks_pixel_cache_t *c)
{
  if(!c) return;
  G_LOCK(raster_cache);
  const gboolean dead = --c->refs == 0 && c->evicted;
  G_UNLOCK(raster_cache);
  if(dead) _raster_free(c);
}

// an icon needs no finer place than this many mask pixels, and the distance
// transform runs on the gui thread
#define RASTER_LOCATE_STEP 4

// the icon's place, the point deepest inside the mask as a fraction of the
// frame, NAN without an interior. the frame edge counts as outside, so the
// anchor of a mask that runs off it, a sky, sits inside the image rather
// than on its border
static void _raster_locate(const dt_masks_pixel_cache_t *c, float anchor[2])
{
  anchor[0] = anchor[1] = NAN;
  // the block maximum, so a part thinner than the step still counts
  const int f = MIN(c->width, c->height) >= 3 * RASTER_LOCATE_STEP
    ? RASTER_LOCATE_STEP : 1;
  const int w = c->width / f, h = c->height / f;
  const size_t n = (size_t)w * h;
  float *m = dt_alloc_align_float(n);
  float *dist = dt_alloc_align_float(n);
  if(m && dist)
  {
    DT_OMP_FOR()
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++)
      {
        uint8_t max = 0;
        for(int j = 0; j < f; j++)
          for(int i = 0; i < f; i++)
            max = MAX(max, c->mask[(size_t)(y * f + j) * c->width + x * f + i]);
        m[(size_t)y * w + x] = max * (1.0f / 255.0f);
      }
    for(int x = 0; x < w; x++) m[x] = m[(size_t)(h - 1) * w + x] = 0.0f;
    for(int y = 0; y < h; y++) m[(size_t)y * w] = m[(size_t)y * w + w - 1] = 0.0f;

    if(dt_image_distance_transform(m, dist, w, h, 0.5f,
                                   DT_DISTANCE_TRANSFORM_MASK) > 0.0f)
    {
      size_t best = 0;
      for(size_t k = 1; k < n; k++)
        if(dist[k] > dist[best]) best = k;
      anchor[0] = ((best % w) + 0.5f) * f / c->width;
      anchor[1] = ((best / w) + 0.5f) * f / c->height;
    }
  }
  dt_free_align(m);
  dt_free_align(dist);
}

// TRUE when the entry reads, with its icon place: from _anchors, or found
// once and kept there. FALSE when it is missing or damaged, or, *transient
// then TRUE, when memory ran short to read it
static gboolean _raster_anchor(const dt_imgid_t imgid,
                               const dt_dtdata_ref_t *ref,
                               float anchor[2],
                               gboolean *transient)
{
  if(transient) *transient = FALSE;
  if(!ref || !ref->entry[0]) return FALSE;
  gchar *key = g_strdup_printf("%d:%s", imgid, ref->entry);
  G_LOCK(raster_cache);
  const float *known = _anchors ? g_hash_table_lookup(_anchors, key) : NULL;
  if(known)
  {
    anchor[0] = known[0];
    anchor[1] = known[1];
  }
  G_UNLOCK(raster_cache);
  if(known)
  {
    g_free(key);
    return TRUE;
  }

  dt_masks_pixel_cache_t *c = dt_masks_pixel_get(imgid, ref, transient);
  if(!c)
  {
    g_free(key);
    return FALSE;
  }
  float *place = g_new(float, 2);
  _raster_locate(c, place);
  dt_masks_pixel_release(c);
  anchor[0] = place[0];
  anchor[1] = place[1];
  G_LOCK(raster_cache);
  if(!_anchors)
    _anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  g_hash_table_replace(_anchors, key, place);
  G_UNLOCK(raster_cache);
  return TRUE;
}

gboolean dt_masks_pixel_readable(const dt_imgid_t imgid,
                                 const dt_dtdata_ref_t *ref,
                                 gboolean *transient)
{
  float anchor[2];
  return _raster_anchor(imgid, ref, anchor, transient);
}

// an object whose mask is missing is left out, never silently: an export
// says so for its image, also on stderr for darktable-cli, and the darkroom
// once per mask and session
static GHashTable *_missing_seen = NULL;     // "imgid:entry" already reported
// the object tool's encoder renders, which would report the very object
// being regenerated
static GList *_quiet_pipes = NULL;
G_LOCK_DEFINE_STATIC(raster_missing);

static void _raster_note_missing(const dt_dev_pixelpipe_t *pipe,
                                 const dt_masks_form_t *form,
                                 const dt_dtdata_ref_t *ref)
{
  const dt_imgid_t imgid = pipe->image.id;
  // the darkroom's own pipes can point at the icon. any other full render,
  // an export or dt_dev_image()'s (slideshow, snapshots, an overlay), says
  // what it left out
  const gboolean darkroom = dt_pipe_is_screen(pipe) && !dt_pipe_is_image(pipe);
#ifndef HAVE_AI
  // no icon regenerates it without AI: the darkroom has nothing to offer
  if(darkroom) return;
#endif
  if(!darkroom)
  {
    if(!dt_pipe_is_export(pipe) && !dt_pipe_is_image(pipe)) return;
    G_LOCK(raster_missing);
    const gboolean quiet = g_list_find(_quiet_pipes, pipe) != NULL;
    G_UNLOCK(raster_missing);
    if(quiet) return;
  }

  // once per image and entry for each of the two, so the darkroom's report
  // does not silence an export's: the roi render runs once per referencing
  // module, and a batch export would overrun the log ring with the repeats,
  // pushing out every other message
  gchar *key = g_strdup_printf("%c%d:%s", darkroom ? 'd' : 'e', imgid, ref->entry);
  G_LOCK(raster_missing);
  if(!_missing_seen)
    _missing_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const gboolean first = g_hash_table_add(_missing_seen, key);
  G_UNLOCK(raster_missing);
  if(!first) return;

  if(!darkroom)
  {
    dt_print(DT_DEBUG_ALWAYS, "[pixel mask] %s: the mask of '%s' is missing"
             " and was left out", pipe->image.filename, form->name);
    // worded for any export pipe: neural restore uses them too
    dt_control_log(_("%s: the mask of '%s' is missing and was left out"),
                   pipe->image.filename, form->name);
  }
#ifdef HAVE_AI
  else
    dt_control_log(_("the mask of '%s' is missing, click its icon to regenerate it"),
                   form->name);
#endif
}

// the entry is back, so its next loss is reported again, in both places
static void _raster_forget_missing(const dt_imgid_t imgid, const char *entry)
{
  for(int i = 0; i < 2; i++)
  {
    gchar *key = g_strdup_printf("%c%d:%s", "de"[i], imgid, entry);
    G_LOCK(raster_missing);
    if(_missing_seen) g_hash_table_remove(_missing_seen, key);
    G_UNLOCK(raster_missing);
    g_free(key);
  }
}

// the hovered icon's mask, tinted over the preview. one icon is hovered at a
// time, so one surface, rebuilt when the entry or the preview changes. gui
// thread only
static struct
{
  char entry[DT_DTDATA_ENTRY_LEN];
  dt_hash_t hash;
  cairo_surface_t *surface;
} _hover;

void dt_masks_pixel_cache_cleanup(void)
{
  G_LOCK(raster_missing);
  if(_missing_seen) g_hash_table_destroy(_missing_seen);
  _missing_seen = NULL;
  G_UNLOCK(raster_missing);

  G_LOCK(raster_cache);
  g_list_free_full(_cache, (GDestroyNotify)_raster_free);
  _cache = NULL;
  _cache_bytes = 0;
  if(_anchors) g_hash_table_destroy(_anchors);
  _anchors = NULL;
  G_UNLOCK(raster_cache);

  if(_hover.surface) cairo_surface_destroy(_hover.surface);
  _hover.surface = NULL;
}

// bilinear read of m at pixel coordinates, x in [0, mw - 1], y in [0, mh - 1]
//
// the index is clamped after the cast: dt_masks_pixel_store can pass a NaN
// (ashift.c:1074), (int)NaN is INT_MIN, and -ffast-math may fold away a
// float range check, while clamping the int holds whatever comparisons do
static inline float _bilinear(const float *const restrict m,
                              const int mw,
                              const int mh,
                              const float x,
                              const float y)
{
  const int x0 = CLAMP((int)x, 0, mw - 1), y0 = CLAMP((int)y, 0, mh - 1);
  const int x1 = MIN(x0 + 1, mw - 1), y1 = MIN(y0 + 1, mh - 1);
  const float fx = x - x0, fy = y - y0;
  return (m[(size_t)y0 * mw + x0] * (1.0f - fx) + m[(size_t)y0 * mw + x1] * fx)
             * (1.0f - fy)
         + (m[(size_t)y1 * mw + x0] * (1.0f - fx) + m[(size_t)y1 * mw + x1] * fx)
             * fy;
}

// the stored mask at a fraction of the pipe's input frame, pixel k sitting
// at (k + 0.5) / mw (dt_masks_pixel_store). off the frame by more than the
// margins mu and mv, a NaN included, it reads 0: "keep all pixels" maps
// roi_out corners off the frame, and clamping would stretch an
// edge-touching object across the padding. within them it reads the edge
static inline float _sample(const uint8_t *const restrict m,
                            const int mw,
                            const int mh,
                            const float u,
                            const float v,
                            const float mu,
                            const float mv)
{
  if(!(u >= -mu && u <= 1.0f + mu && v >= -mv && v <= 1.0f + mv)) return 0.0f;
  const float x = CLAMPF(u * mw - 0.5f, 0.0f, mw - 1.0f);
  const float y = CLAMPF(v * mh - 0.5f, 0.0f, mh - 1.0f);
  const int x0 = (int)x, y0 = (int)y;
  const int x1 = MIN(x0 + 1, mw - 1), y1 = MIN(y0 + 1, mh - 1);
  // the four neighbors scaled as the png decoder does, so a value reads
  // back exactly as decoded
  const float n = 1.0f / 255.0f;
  const float q[4] = { m[(size_t)y0 * mw + x0] * n, m[(size_t)y0 * mw + x1] * n,
                       m[(size_t)y1 * mw + x0] * n, m[(size_t)y1 * mw + x1] * n };
  return _bilinear(q, 2, 2, x - x0, y - y0);
}

int dt_masks_pixel_get_mask_roi(const dt_iop_module_t *const module,
                                const dt_dev_pixelpipe_iop_t *const piece,
                                dt_masks_form_t *const form,
                                const dt_iop_roi_t *roi,
                                float *buffer)
{
  const dt_dtdata_ref_t *ref = dt_masks_pixel_ref(form);
  if(!ref) return 0;

  // a missing or damaged entry leaves the object out of its group, which
  // is not the same as an empty mask: an empty mask still takes part, so
  // an inverted object would then apply the module everywhere
  gboolean transient = FALSE;
  dt_masks_pixel_cache_t *c =
    dt_masks_pixel_get(piece->pipe->image.id, ref, &transient);
  if(!c || c->width <= 0 || c->height <= 0)
  {
    dt_masks_pixel_release(c);
    // a memory shortage is not a lost mask, and reporting it as one would
    // use up the once-per-entry message the real loss needs
    if(!transient) _raster_note_missing(piece->pipe, form, ref);
    return 0;
  }
  const uint8_t *const restrict mask = c->mask;
  const int mw = c->width;
  const int mh = c->height;

  const int w = roi->width;
  const int h = roi->height;
  const int px = roi->x;
  const int py = roi->y;
  const float iscale = 1.0f / roi->scale;
  const int grid = CLAMP((10.0f * roi->scale + 2.0f) / 3.0f, 1, 4);
  const int gw = (w + grid - 1) / grid + 1;
  const int gh = (h + grid - 1) / grid + 1;

  float *points = dt_alloc_align_float((size_t)2 * gw * gh);
  if(points == NULL)
  {
    dt_masks_pixel_release(c);
    return 0;
  }

  DT_OMP_FOR(collapse(2))
  for(int j = 0; j < gh; j++)
    for(int i = 0; i < gw; i++)
    {
      const size_t index = (size_t)j * gw + i;
      points[index * 2] = (grid * i + px) * iscale;
      points[index * 2 + 1] = (grid * j + py) * iscale;
    }

  if(!dt_dev_distort_backtransform_plus(module->dev, piece->pipe,
                                        module->iop_order,
                                        DT_DEV_TRANSFORM_DIR_BACK_INCL, points,
                                        (size_t)gw * gh))
  {
    dt_masks_pixel_release(c);
    dt_free_align(points);
    return 0;
  }

  // the stored mask spans the whole frame whatever its own size, so points
  // normalize against the pipe input, not the entry's dimensions
  const float wd = piece->pipe->iwidth;
  const float ht = piece->pipe->iheight;
  // the grid reaches one step past the roi, so past the frame when the roi
  // ends at its edge: read the edge there, or the last cell inside the
  // frame would fade toward 0
  const float mu = grid * iscale / wd;
  const float mv = grid * iscale / ht;

  DT_OMP_FOR()
  for(int k = 0; k < gw * gh; k++)
    points[k * 2] = _sample(mask, mw, mh, points[k * 2] / wd, points[k * 2 + 1] / ht,
                            mu, mv);
  dt_masks_pixel_release(c);

  DT_OMP_FOR()
  for(int j = 0; j < h; j++)
  {
    const int jj = j % grid;
    const int mj = j / grid;
    const int grid_jj = grid - jj;
    for(int i = 0; i < w; i++)
    {
      const int ii = i % grid;
      const int mi = i / grid;
      const int grid_ii = grid - ii;
      const size_t mindex = (size_t)mj * gw + mi;
      buffer[(size_t)j * w + i]
        = (points[mindex * 2] * grid_ii * grid_jj
           + points[(mindex + 1) * 2] * ii * grid_jj
           + points[(mindex + gw) * 2] * grid_ii * jj
           + points[(mindex + gw + 1) * 2] * ii * jj)
          / (grid * grid);
    }
  }

  dt_free_align(points);
  return 1;
}

// each pixel form has its own point struct, so the size comes from the form
void dt_masks_pixel_duplicate_points(dt_develop_t *const dev,
                                     dt_masks_form_t *const base,
                                     dt_masks_form_t *const dest)
{
  const size_t size = base->functions->point_struct_size;
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    const void *pt = pts->data;
    void *npt = malloc(size);
    if(!npt) return;
    memcpy(npt, pt, size);
    dest->points = g_list_append(dest->points, npt);
  }
}


// on the canvas a committed object is an icon at its anchor, its mask tinted
// only while hovered: the soft, scattered masks of skies or subjects have no
// edge worth an outline
#define RASTER_ICON_RADIUS 10.0f // unscaled pixels
// the tint of a mask over the image, premultiplied red at this alpha
#define RASTER_OVERLAY_ALPHA 80.0f
// preview pixels per overlay pixel
#define RASTER_OVERLAY_STEP 2.0f

// the form drawn at index in the visible group: gui->points follows the
// group's direct members in order (dt_masks_gui_form_test_create)
static dt_masks_form_t *_raster_form_at(const int index)
{
  dt_masks_form_t *grp = darktable.develop->form_visible;
  if(!grp) return NULL;
  if(!(grp->type & DT_MASKS_GROUP)) return grp;
  const dt_masks_point_group_t *pt = g_list_nth_data(grp->points, index);
  return pt ? dt_masks_get_from_id(darktable.develop, pt->formid) : NULL;
}

// as is DT_PIXEL_APPLY_DPI(7) / zoom_scale (dt_masks_sensitive_dist), so
// this is the icon's radius at the current zoom
static gboolean _raster_over_icon(const dt_masks_form_gui_points_t *gpt,
                                  const float x,
                                  const float y,
                                  const float as)
{
  const float r = as * RASTER_ICON_RADIUS / 7.0f;
  return sqf(gpt->points[0] - x) + sqf(gpt->points[1] - y) < sqf(r);
}

void dt_masks_pixel_get_distance(const float x,
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
  *inside = FALSE;
  *inside_border = FALSE;
  *inside_source = FALSE;
  *near = -1;
  *dist = FLT_MAX;

  if(!gui) return;
  const dt_masks_form_gui_points_t *gpt = g_list_nth_data(gui->points, index);
  if(!gpt || gpt->points_count < 1) return;

  // only the icon: a sky or a background covers most of the frame and would
  // be selected from almost anywhere, and sampling the mask would cost a
  // read and a backtransform per motion event
  *dist = sqf(gpt->points[0] - x) + sqf(gpt->points[1] - y);
  *inside = _raster_over_icon(gpt, x, y, as);
}

// the anchor in image space, carried forward to the preview
int dt_masks_pixel_get_points_border(const dt_masks_pixel_type_t *type,
                                     dt_develop_t *dev,
                                     dt_masks_form_t *form,
                                     float **points,
                                     int *points_count,
                                     float **border,
                                     int *border_count,
                                     const int source,
                                     const dt_iop_module_t *const module)
{
  if(border) *border = NULL;
  if(border_count) *border_count = 0;
  const dt_dtdata_ref_t *ref = dt_masks_pixel_ref(form);
  if(source || !ref) return 0;

  float anchor[2] = { NAN, NAN };
  _raster_anchor(dev->image_storage.id, ref, anchor, NULL);
  float ax = anchor[0], ay = anchor[1];
  // no mask, or none of it 0.5 or above off the frame edge: the icon is
  // still the way to edit or regenerate it, so it goes where the form says.
  // dt_isnan holds under -ffast-math, unlike a comparison
  float fallback[2];
  if(dt_isnan(ax) && type->fallback_anchor(form, fallback))
  {
    ax = fallback[0];
    ay = fallback[1];
  }
  if(dt_isnan(ax)) return 0;

  float iwidth, iheight;
  dt_masks_get_image_size(NULL, NULL, &iwidth, &iheight);
  float *pts = dt_alloc_align_float(2);
  if(!pts) return 0;
  pts[0] = ax * iwidth;
  pts[1] = ay * iheight;
  if(!dt_dev_distort_transform(dev, pts, 1))
  {
    dt_free_align(pts);
    return 0;
  }
  *points = pts;
  *points_count = 1;
  return 1;
}

// ctrl+scroll changes the opacity, as for every shape. the group dispatches
// a scroll only to the form get_distance hit, here the icon. no
// gui->scrollx bookkeeping: it freezes reselection while a scroll resizes
// a shape under the pointer, and nothing moves here
int dt_masks_pixel_mouse_scrolled(dt_iop_module_t *module,
                                  const float pzx,
                                  const float pzy,
                                  const gboolean up,
                                  const uint32_t state,
                                  dt_masks_form_t *form,
                                  const dt_imgid_t parentid,
                                  dt_masks_form_gui_t *gui,
                                  const int index)
{
  if(gui && gui->form_selected && dt_modifier_is(state, GDK_CONTROL_MASK))
  {
    dt_masks_form_change_opacity(form, parentid, up ? 0.05f : -0.05f);
    return 1;
  }
  return 0;
}

// a right-click on the icon removes the form from its group, as for every
// shape (_circle_events_button_released)
int dt_masks_pixel_button_released(dt_iop_module_t *module,
                                   const int which,
                                   dt_masks_form_t *form,
                                   const dt_mask_id_t parentid,
                                   dt_masks_form_gui_t *gui)
{
  if(!(gui && which == GDK_BUTTON_SECONDARY && gui->form_selected
       && dt_is_valid_maskid(parentid) && gui->edit_mode == DT_MASKS_EDIT_FULL))
    return 0;

  dt_develop_t *dev = darktable.develop;
  // we hide the form
  if(!(dev->form_visible->type & DT_MASKS_GROUP)
     || g_list_shorter_than(dev->form_visible->points, 2))
    dt_masks_change_form_gui(NULL);
  else
  {
    dt_masks_clear_form_gui(dev);
    for(GList *forms = dev->form_visible->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *gpt = forms->data;
      if(gpt->formid == form->formid)
      {
        dev->form_visible->points = g_list_remove(dev->form_visible->points, gpt);
        free(gpt);
        break;
      }
    }
    gui->edit_mode = DT_MASKS_EDIT_FULL;
  }

  // we remove the shape
  dt_masks_form_remove(module, dt_masks_get_from_id(dev, parentid), form);
  return 1;
}

int dt_masks_pixel_mouse_moved(dt_iop_module_t *module,
                               float pzx,
                               float pzy,
                               const double pressure,
                               const int which,
                               const float zoom_scale,
                               dt_masks_form_t *form,
                               const dt_imgid_t parentid,
                               dt_masks_form_gui_t *gui,
                               const int index)
{
  if(gui->creation) return 0;

  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  gboolean in, inb, ins;
  int near;
  float dist;
  dt_masks_pixel_get_distance(pzx * wd, pzy * ht, dt_masks_sensitive_dist(zoom_scale),
                              gui, index, 0, &in, &inb, &near, &ins, &dist);
  gui->form_selected = in;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  // the icon is the object's one handle, what a click on it opens the edit from
  gui->point_selected = in ? 0 : -1;
  gui->point_border_selected = -1;

  // no redraw: dt_masks_events_mouse_moved queues one when a field set here
  // changes (_gui_hover_state_equal in masks.c), and one per motion event
  // would cost a stored-mask read and a backtransform per committed object
  return in;
}

// the stored mask resampled into out on a w x h grid whose pixel (x, y) sits
// at preview ((x + 0.5) * a[0] + b[0], (y + 0.5) * a[1] + b[1]), as
// dt_masks_pixel_get_mask_roi reads it for the pipe
static gboolean _resample(const dt_masks_pixel_cache_t *c,
                          const int w,
                          const int h,
                          const float a[2],
                          const float b[2],
                          float *const out)
{
  float iwidth, iheight;
  dt_masks_get_image_size(NULL, NULL, &iwidth, &iheight);
  const size_t n = (size_t)w * h;
  float *pts = dt_alloc_align_float(2 * n);
  if(!pts) return FALSE;

  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const size_t k = (size_t)y * w + x;
      pts[k * 2] = (x + 0.5f) * a[0] + b[0];
      pts[k * 2 + 1] = (y + 0.5f) * a[1] + b[1];
    }
  if(!dt_dev_distort_backtransform(darktable.develop, pts, n))
  {
    dt_free_align(pts);
    return FALSE;
  }

  const uint8_t *const mask = c->mask;
  const int mw = c->width, mh = c->height;
  // no off-frame or NaN test here: _sample reads 0 for both
  DT_OMP_FOR()
  for(size_t k = 0; k < n; k++)
    out[k] = _sample(mask, mw, mh, pts[k * 2] / iwidth, pts[k * 2 + 1] / iheight,
                     0.0f, 0.0f);
  dt_free_align(pts);
  return TRUE;
}

// the stored mask resampled into out over the whole preview frame on a
// w x h grid, for the hover overlay. not for a mask over a render, which
// the pipe placed apart from this: see dt_masks_pixel_to_render
static gboolean _to_preview(const dt_masks_pixel_cache_t *c,
                            const int w,
                            const int h,
                            float *const out)
{
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  const float a[2] = { wd / w, ht / h };
  const float b[2] = { 0.0f, 0.0f };
  return _resample(c, w, h, a, b, out);
}

// a mask as a red tint, premultiplied: in proportion to the mask, or at
// full strength above threshold. NULL on error
cairo_surface_t *dt_masks_pixel_tint(const float *const mask,
                                     const int w,
                                     const int h,
                                     const gboolean proportional,
                                     const float threshold)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_surface_flush(surface);
  unsigned char *data = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);

  DT_OMP_FOR()
  for(int y = 0; y < h; y++)
  {
    unsigned char *row = data + (size_t)y * stride;
    for(int x = 0; x < w; x++)
    {
      const float v = mask[(size_t)y * w + x];
      const float a = proportional
        ? CLAMPF(v, 0.0f, 1.0f) * RASTER_OVERLAY_ALPHA
        : (v > threshold ? RASTER_OVERLAY_ALPHA : 0.0f);
      row[x * 4 + 0] = 0;                  // B
      row[x * 4 + 1] = 0;                  // G
      row[x * 4 + 2] = (unsigned char)a;   // R, premultiplied
      row[x * 4 + 3] = (unsigned char)a;   // A
    }
  }
  cairo_surface_mark_dirty(surface);
  return surface;
}

// the pixels are read only to build the surface, not to reuse it
static cairo_surface_t *_raster_overlay(const dt_imgid_t imgid,
                                        const dt_dtdata_ref_t *ref)
{
  const dt_hash_t hash = darktable.develop->preview_pipe->backbuf_hash;
  if(_hover.surface && _hover.hash == hash && !strcmp(_hover.entry, ref->entry))
    return _hover.surface;

  dt_masks_pixel_cache_t *c = dt_masks_pixel_get(imgid, ref, NULL);
  if(!c) return NULL;
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  const int ow = MAX(1, (int)ceilf(wd / RASTER_OVERLAY_STEP));
  const int oh = MAX(1, (int)ceilf(ht / RASTER_OVERLAY_STEP));
  float *val = dt_alloc_align_float((size_t)ow * oh);
  cairo_surface_t *surface =
    val && _to_preview(c, ow, oh, val)
    ? dt_masks_pixel_tint(val, ow, oh, TRUE, 0.0f) : NULL;
  dt_free_align(val);
  dt_masks_pixel_release(c);
  if(!surface) return NULL;

  if(_hover.surface) cairo_surface_destroy(_hover.surface);
  _hover.surface = surface;
  _hover.hash = hash;
  g_strlcpy(_hover.entry, ref->entry, sizeof(_hover.entry));
  return surface;
}

void dt_masks_pixel_post_expose(const dt_masks_pixel_type_t *type,
                                cairo_t *cr,
                                const float zoom_scale,
                                dt_masks_form_gui_t *gui,
                                const int index,
                                const int num_points)
{
  if(!gui) return;
  const dt_masks_form_gui_points_t *gpt = g_list_nth_data(gui->points, index);
  if(!gpt || gpt->points_count < 1) return;

  const dt_masks_form_t *form = _raster_form_at(index);
  // over the icon, the only part that selects (dt_masks_pixel_get_distance)
  const gboolean hovered = gui->group_selected == index && gui->form_selected;
  const dt_dtdata_ref_t *ref = form ? dt_masks_pixel_ref(form) : NULL;
  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const gboolean missing = ref && !dt_masks_pixel_readable(imgid, ref, NULL);
  const gboolean active =
    hovered || (form && form->formid == darktable.develop->mask_form_selected_id);

  // hovering the icon tints the pixels; a selection elsewhere only lights it
  if(hovered && ref && !missing)
  {
    cairo_surface_t *overlay = _raster_overlay(imgid, ref);
    if(overlay)
    {
      float wd, ht;
      dt_masks_get_image_size(&wd, &ht, NULL, NULL);
      cairo_save(cr);
      cairo_scale(cr, wd / cairo_image_surface_get_width(overlay),
                  ht / cairo_image_surface_get_height(overlay));
      cairo_set_source_surface(cr, overlay, 0, 0);
      cairo_paint(cr);
      cairo_restore(cr);
    }
  }

  const float x = gpt->points[0], y = gpt->points[1];
  const float r = DT_PIXEL_APPLY_DPI(RASTER_ICON_RADIUS) / zoom_scale;
  cairo_save(cr);
  cairo_set_dash(cr, NULL, 0, 0);
  cairo_arc(cr, x, y, r, 0.0, 2.0 * M_PI);
  dt_draw_set_color_overlay(cr, FALSE, active ? 0.8 : 0.5);
  cairo_fill(cr);
  // the paint function takes integer coordinates, which a zoomed-in canvas
  // would round away: draw it at a fixed size and scale that instead
  const float s = 1.4f * r;
  cairo_translate(cr, x - 0.5f * s, y - 0.5f * s);
  cairo_scale(cr, s / 100.0f, s / 100.0f);
  dt_draw_set_color_overlay(cr, TRUE, active ? 1.0 : 0.7);
  type->icon(cr, 0, 0, 100, 100, 0, NULL);
  if(missing)
  {
    // struck through: the object is left out until it is regenerated
    cairo_set_line_width(cr, 12.0);
    cairo_move_to(cr, 15.0, 85.0);
    cairo_line_to(cr, 85.0, 15.0);
    cairo_stroke(cr);
  }
  cairo_restore(cr);
}


// what a click and a scroll on the icon do, not the tool's actions, which a
// committed object does not implement. dt_masks_mouse_actions adds the
// right-click that removes it, as for every shape
GSList *dt_masks_pixel_setup_mouse_actions(const dt_masks_pixel_type_t *type)
{
  GSList *lm = NULL;
#ifdef HAVE_AI
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_LEFT,
    0,
    _(type->edit_action));
#endif
  lm = dt_mouse_action_create_simple(
    lm,
    DT_MOUSE_ACTION_SCROLL,
    GDK_CONTROL_MASK,
    _(type->opacity_action));
  return lm;
}

// only reached over the icon: the group hands the hint to the form its
// get_distance reported a hit for, and that is the icon alone
void dt_masks_pixel_set_hint_message(const int opacity,
                                     char *const restrict msgbuf,
                                     const size_t msgbuf_len)
{
#ifdef HAVE_AI
  g_snprintf(msgbuf,
             msgbuf_len,
             _("<b>edit</b>: click, <b>opacity</b>: ctrl+scroll (%d%%)"),
             opacity);
#else
  // without AI a stored object still renders and takes an opacity, but
  // there is no tool to reopen it with
  g_snprintf(msgbuf,
             msgbuf_len,
             _("<b>opacity</b>: ctrl+scroll (%d%%)"),
             opacity);
#endif
}


// --- making a stored mask: only where AI is compiled in ---

#ifdef HAVE_AI

// changes with crop, rotate, perspective or lens, not with exposure, color
// or masks: a mask made from the view before such a change no longer fits
// it. the modules as the history leaves them, not its items: an item that
// switches crop off holds no crop params, and a module applied by default
// has no item at all
dt_hash_t dt_masks_pixel_distort_hash(dt_develop_t *dev)
{
  dt_hash_t hash = DT_INITHASH;
  for(const GList *l = dev->iop; l; l = g_list_next(l))
  {
    const dt_iop_module_t *m = l->data;
    if(m->enabled && (m->operation_tags() & IOP_TAG_DISTORT))
    {
      hash = dt_hash(hash, &m->iop_order, sizeof(m->iop_order));
      hash = dt_hash(hash, m->params, m->params_size);
    }
  }
  return hash;
}

struct dt_masks_pixel_render_t
{
  dt_develop_t dev;
  dt_mipmap_buffer_t buf;
  dt_dev_pixelpipe_t pipe;
};

dt_masks_pixel_render_t *dt_masks_pixel_render_init(const dt_imgid_t imgid,
                                                    const int32_t history_end,
                                                    int *width,
                                                    int *height)
{
  dt_masks_pixel_render_t *r = g_new0(dt_masks_pixel_render_t, 1);
  dt_dev_init(&r->dev, FALSE);
  dt_dev_load_image(&r->dev, imgid);

  // the database's history_end may lag behind the darkroom's
  // in-memory state (crop/rotate not flushed yet), override
  // so synch_all applies all current edits
  if(history_end > 0 && history_end > r->dev.history_end)
    r->dev.history_end = history_end;

  dt_mipmap_cache_get(&r->buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  if(!r->buf.buf || !r->buf.width || !r->buf.height)
  {
    dt_print(DT_DEBUG_AI,
             "[pixel mask] failed to get image buffer for encoding");
    dt_mipmap_cache_release(&r->buf);
    dt_dev_cleanup(&r->dev);
    g_free(r);
    return NULL;
  }

  const int wd = r->dev.image_storage.width;
  const int ht = r->dev.image_storage.height;

  if(!dt_dev_pixelpipe_init_export(&r->pipe, wd, ht, IMAGEIO_RGB | IMAGEIO_INT8,
                                   FALSE))
  {
    dt_print(DT_DEBUG_AI,
             "[pixel mask] failed to init export pipe for encoding");
    dt_mipmap_cache_release(&r->buf);
    dt_dev_cleanup(&r->dev);
    g_free(r);
    return NULL;
  }

  dt_dev_pixelpipe_set_icc(&r->pipe, DT_COLORSPACE_SRGB, NULL,
                           DT_INTENT_PERCEPTUAL);
  dt_dev_pixelpipe_set_input(&r->pipe, &r->dev, (float *)r->buf.buf,
                             r->buf.width, r->buf.height, r->buf.iscale);
  dt_dev_pixelpipe_create_nodes(&r->pipe, &r->dev);
  dt_dev_pixelpipe_synch_all(&r->pipe, &r->dev);

  dt_dev_pixelpipe_get_dimensions(&r->pipe, &r->dev, r->pipe.iwidth, r->pipe.iheight,
                                  &r->pipe.processed_width,
                                  &r->pipe.processed_height);
  *width = r->pipe.processed_width;
  *height = r->pipe.processed_height;
  return r;
}

uint8_t *dt_masks_pixel_render(dt_masks_pixel_render_t *r,
                               const int width,
                               const int height,
                               const float scale)
{
  G_LOCK(raster_missing);
  _quiet_pipes = g_list_prepend(_quiet_pipes, &r->pipe);
  G_UNLOCK(raster_missing);
  dt_dev_pixelpipe_process_no_gamma(&r->pipe, &r->dev, 0, 0, width, height, scale);
  G_LOCK(raster_missing);
  _quiet_pipes = g_list_remove(_quiet_pipes, &r->pipe);
  G_UNLOCK(raster_missing);

  // backbuf is float RGBA after process_no_gamma, convert to uint8 RGB
  uint8_t *rgb = NULL;
  if(r->pipe.backbuf)
  {
    const float *outbuf = (const float *)r->pipe.backbuf;
    rgb = g_try_malloc((size_t)width * height * 3);
    if(rgb)
    {
      for(size_t i = 0; i < (size_t)width * height; i++)
      {
        rgb[i * 3 + 0] = (uint8_t)CLAMP(outbuf[i * 4 + 0] * 255.0f + 0.5f, 0, 255);
        rgb[i * 3 + 1] = (uint8_t)CLAMP(outbuf[i * 4 + 1] * 255.0f + 0.5f, 0, 255);
        rgb[i * 3 + 2] = (uint8_t)CLAMP(outbuf[i * 4 + 2] * 255.0f + 0.5f, 0, 255);
      }
    }
  }
  return rgb;
}

void dt_masks_pixel_render_cleanup(dt_masks_pixel_render_t *r)
{
  if(!r) return;
  dt_dev_pixelpipe_cleanup(&r->pipe);
  dt_mipmap_cache_release(&r->buf);
  dt_dev_cleanup(&r->dev);
  g_free(r);
}

// a stored mask into the cache as the decoder will read it back, with the
// encoder's 8-bit rounding (dtdata.c): the pipes and the overlay then do
// not decode what was just encoded. a decoded entry already there stays; a
// failed read, a regenerated mask that got the lost one's name, goes
static void _raster_seed(const dt_imgid_t imgid,
                         const char *entry,
                         const float *const mask,
                         const int w,
                         const int h)
{
  uint8_t *pixels = dt_alloc_aligned((size_t)w * h);
  if(!pixels) return;
  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)w * h; k++)
    pixels[k] = (uint8_t)(CLIP(mask[k]) * 255.0f + 0.5f);
  // where dt_dtdata_write_gray() just put it
  char sidecar[PATH_MAX] = { 0 };
  dt_dtdata_path(imgid, sidecar, sizeof(sidecar));

  G_LOCK(raster_cache);
  dt_masks_pixel_cache_t *c = _raster_lookup(imgid, entry);
  dt_masks_pixel_cache_t *failed = c && !c->mask ? c : NULL;
  if(failed)
  {
    _raster_unlist_entry(failed);
    c = NULL;
  }
  if(!c)
  {
    dt_masks_pixel_cache_t *n = g_malloc0(sizeof(dt_masks_pixel_cache_t));
    g_strlcpy(n->entry, entry, sizeof(n->entry));
    n->imgid = imgid;
    n->mask = pixels;
    n->width = w;
    n->height = h;
    n->sidecar = sidecar[0] ? g_strdup(sidecar) : NULL;
    n->existed = TRUE;
    n->checked = g_get_monotonic_time();
    pixels = NULL;
    _cache = g_list_prepend(_cache, n);
    _cache_bytes += _raster_bytes(n);
    _raster_evict();
  }
  G_UNLOCK(raster_cache);
  // released after the lock, which is not recursive
  dt_masks_pixel_release(failed);
  dt_masks_pixel_release(c);
  dt_free_align(pixels);
}

gboolean dt_masks_pixel_grid_to_preview(const dt_masks_pixel_grid_t *grid,
                                        const int w,
                                        const int h,
                                        float a[2],
                                        float b[2])
{
  float wd, ht;
  dt_masks_get_image_size(&wd, &ht, NULL, NULL);
  if(wd <= 0.0f || ht <= 0.0f || w <= 0 || h <= 0
     || grid->width <= 0 || grid->height <= 0 || grid->rw <= 0 || grid->rh <= 0
     || !(grid->scale > 0.0f))
    return FALSE;
  // render pixels per preview pixel: the preview frame is the render's frame
  const float k[2] = { grid->scale * grid->width / wd, grid->scale * grid->height / ht };
  // c is at render pixel c * rw / w - 0.5, the mask covering the render edge
  // to edge, and render pixel j at preview j / k
  a[0] = grid->rw / (w * k[0]);
  a[1] = grid->rh / (h * k[1]);
  b[0] = -0.5f / k[0];
  b[1] = -0.5f / k[1];
  return TRUE;
}

gboolean dt_masks_pixel_to_render(const dt_masks_pixel_cache_t *c,
                                  const dt_masks_pixel_grid_t *grid,
                                  const int w,
                                  const int h,
                                  float *const out)
{
  float a[2], b[2];
  return dt_masks_pixel_grid_to_preview(grid, w, h, a, b)
    && _resample(c, w, h, a, b, out);
}

gboolean dt_masks_pixel_store_size(const int mask_w,
                                   const int mask_h,
                                   const int max_side,
                                   int *tw,
                                   int *th)
{
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  if(mask_w <= 0 || mask_h <= 0
     || wd <= 0.0f || ht <= 0.0f || iwidth <= 0.0f || iheight <= 0.0f)
    return FALSE;
  // the view's axes need not be the input's, which a flip or a rotation in
  // the pipe swaps, so the density is the larger of the two
  const float d = fmaxf(mask_w / wd, mask_h / ht);
  const float w = ceilf(d * iwidth);
  const float h = ceilf(d * iheight);
  // image_storage, not iwidth/iheight: those are the preview pipe's, capped
  // at 1440x900 by DT_MIPMAP_F (mipmap_cache.c:744)
  const float fw = darktable.develop->image_storage.width;
  const float fh = darktable.develop->image_storage.height;
  float cap = max_side / fmaxf(w, h);
  if(fw > 0.0f && fh > 0.0f) cap = fminf(cap, fminf(fw / w, fh / h));
  cap = fminf(cap, 1.0f);
  *tw = MAX(1, (int)(w * cap));
  *th = MAX(1, (int)(h * cap));
  return TRUE;
}

gboolean dt_masks_pixel_store(const float *mask,
                              const int mask_w,
                              const int mask_h,
                              const dt_masks_pixel_grid_t *grid,
                              const int tw,
                              const int th,
                              const dt_dtdata_ref_t *old,
                              const char *producer,
                              dt_dtdata_ref_t *ref)
{
  // the mask lives after crop, rotate and lens, but is stored before them,
  // like every form's points, so it survives a later crop
  float wd, ht, iwidth, iheight;
  dt_masks_get_image_size(&wd, &ht, &iwidth, &iheight);
  float a[2], b[2];
  if(!mask || mask_w <= 0 || mask_h <= 0 || tw <= 0 || th <= 0
     || wd <= 0.0f || ht <= 0.0f || iwidth <= 0.0f || iheight <= 0.0f
     || !grid || !dt_masks_pixel_grid_to_preview(grid, mask_w, mask_h, a, b))
    return FALSE;

  float *pts = dt_alloc_align_float((size_t)2 * tw * th);
  float *out = dt_alloc_align_float((size_t)tw * th);
  if(!pts || !out)
  {
    dt_free_align(pts);
    dt_free_align(out);
    return FALSE;
  }

  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < th; y++)
    for(int x = 0; x < tw; x++)
    {
      const size_t k = (size_t)y * tw + x;
      pts[k * 2] = (x + 0.5f) * iwidth / tw;
      pts[k * 2 + 1] = (y + 0.5f) * iheight / th;
    }

  // input pixels forward to where the encode pipe saw them, every one and in
  // one call: liquify maps a point by the extent of all it is given, so
  // bands or a coarser grid would move a mask by up to tens of pixels
  if(!dt_dev_distort_transform(darktable.develop, pts, (size_t)tw * th))
  {
    dt_print(DT_DEBUG_MASKS, "[pixel mask] raster: distort transform failed");
    dt_free_align(pts);
    dt_free_align(out);
    return FALSE;
  }

  dt_masks_pixel_cache_t *oc =
    dt_masks_pixel_get(darktable.develop->image_storage.id, old, NULL);
  const uint8_t *const om = oc ? oc->mask : NULL;
  const int ow = oc ? oc->width : 0, oh = oc ? oc->height : 0;

  DT_OMP_FOR()
  for(int k = 0; k < tw * th; k++)
  {
    const float px = pts[k * 2];
    const float py = pts[k * 2 + 1];
    // dt_isnan first: a NaN from the transform (ashift.c:1074) passes every
    // range test
    if(dt_isnan(px) || dt_isnan(py) || px < 0.0f || py < 0.0f || px > wd || py > ht)
      out[k] = om
        ? _sample(om, ow, oh, (k % tw + 0.5f) / tw, (k / tw + 0.5f) / th, 0.0f, 0.0f)
        : 0.0f;
    else
      // the inverse of dt_masks_pixel_grid_to_preview, less half a pixel as
      // _bilinear takes an index. clamped as _sample reads it back, so the
      // outer half pixel, and the strip a rounded-down render missed, read
      // the edge
      out[k] = _bilinear(mask, mask_w, mask_h,
                         CLAMPF((px - b[0]) / a[0] - 0.5f, 0.0f, mask_w - 1.0f),
                         CLAMPF((py - b[1]) / a[1] - 0.5f, 0.0f, mask_h - 1.0f));
  }
  dt_masks_pixel_release(oc);
  dt_free_align(pts);

  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const gboolean ok =
    dt_dtdata_write_gray(imgid, DT_DTDATA_KIND_MASK, DT_DTDATA_ORIGIN_REGENERABLE,
                         producer, out, tw, th, 8, ref);
  if(ok)
  {
    _raster_forget_missing(imgid, ref->entry);
    _raster_seed(imgid, ref->entry, out, tw, th);
  }
  dt_free_align(out);
  return ok;
}

#endif // HAVE_AI

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
