/*
    This file is part of darktable,
    Copyright (C) 2024-2026 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/interpolation.h"
#include "common/math.h"
#include "common/overlay.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_overlay_params_t)

// Compositing precision. Not exposed in the UI: every new edit uses the
// high-precision float path, while edits created before it existed are pinned
// to LEGACY by legacy_params() so their rendering never changes.
typedef enum dt_iop_overlay_compositing_t
{
  DT_OVERLAY_COMPOSITE_HQ = 0,
  DT_OVERLAY_COMPOSITE_LEGACY = 1
} dt_iop_overlay_compositing_t;

typedef enum dt_iop_overlay_base_scale_t
{
  DT_SCALE_MAINMENU_IMAGE            = 0,  // $DESCRIPTION: "image"
  DT_SCALE_MAINMENU_LARGER_BORDER    = 1,  // $DESCRIPTION: "larger border"
  DT_SCALE_MAINMENU_SMALLER_BORDER   = 2,  // $DESCRIPTION: "smaller border"
  DT_SCALE_MAINMENU_MARKERHEIGHT     = 3,  // $DESCRIPTION: "height"
  DT_SCALE_MAINMENU_ADVANCED         = 4   // $DESCRIPTION: "advanced options"
} dt_iop_overlay_base_scale_t;             // this is the first
                                           // drop-down menu, always
                                           // visible

typedef enum dt_iop_overlay_img_scale_t
{
  DT_SCALE_IMG_WIDTH                 = 1,  // $DESCRIPTION: "image width"
  DT_SCALE_IMG_HEIGHT                = 2,  // $DESCRIPTION: "image height"
  DT_SCALE_IMG_LARGER                = 3,  // $DESCRIPTION: "larger image border"
  DT_SCALE_IMG_SMALLER               = 4,  // $DESCRIPTION: "smaller image border"
} dt_iop_overlay_img_scale_t;              // advanced drop-down no. 1

typedef enum dt_iop_overlay_svg_scale_t
{
  DT_SCALE_SVG_WIDTH                 = 0,  // $DESCRIPTION: "marker width"
  DT_SCALE_SVG_HEIGHT                = 1,  // $DESCRIPTION: "marker height"
} dt_iop_overlay_svg_scale_t;              // advanced drop-down no. 1

typedef struct dt_iop_overlay_params_t
{
  /** opacity value of rendering overlay */
  float opacity; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  /** scale value of rendering overlay */
  float scale;   // $MIN: 1.0 $MAX: 500.0 $DEFAULT: 100.0
  /** Pixel independent xoffset, 0 to 1 */
  float xoffset; // $MIN: -1.0 $MAX: 1.0, 0.001 $DEFAULT: 0.0 $DESCRIPTION: "x offset"
  /** Pixel independent yoffset, 0 to 1 */
  float yoffset; // $MIN: -1.0 $MAX: 1.0, 0.001 $DEFAULT: 0.0 $DESCRIPTION: "y offset"
  /** Alignment value 0-8 3x3 */
  int alignment; // $DEFAULT: 4
  /** Rotation **/
  float rotate;  // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0 $DESCRIPTION: "rotation"
  dt_iop_overlay_base_scale_t scale_base; // $DEFAULT: DT_SCALE_MAINMENU_IMAGE $DESCRIPTION: "scale on"
  dt_iop_overlay_img_scale_t scale_img; // $DEFAULT: DT_SCALE_IMG_LARGER $DESCRIPTION: "scale marker to"
  dt_iop_overlay_svg_scale_t scale_svg; // $DEFAULT: DT_SCALE_SVG_WIDTH $DESCRIPTION: "scale marker reference"
  dt_imgid_t imgid; // overlay image id $DESCRIPTION: "image id"
  char filename[1024]; // full overlay's filename
  // compositing precision, not user-settable (see dt_iop_overlay_compositing_t)
  dt_iop_overlay_compositing_t compositing; // $DEFAULT: DT_OVERLAY_COMPOSITE_HQ
  // reserved (kept to allow future fields without a version bump)
  size_t dummy1;
  int64_t dummy2;
} dt_iop_overlay_params_t;

typedef struct dt_iop_overlay_data_t
{
  float opacity;
  float scale;
  float xoffset;
  float yoffset;
  int alignment;
  float rotate;
  dt_iop_overlay_base_scale_t scale_base;
  dt_iop_overlay_svg_scale_t scale_svg;
  dt_iop_overlay_img_scale_t scale_img;
  dt_imgid_t imgid;
  char filename[1024];
  dt_iop_overlay_compositing_t compositing;
} dt_iop_overlay_data_t;

#define MAX_OVERLAY 50

typedef struct dt_iop_overlay_global_data_t
{
  // Cached overlay buffer, one slot per instance (index = multi_priority).
  // The stored format depends on the instance's compositing mode:
  //  - HQ: 4-channel float in the pipe's scene-referred linear working RGB
  //    (colorout filtered out, gamma terminal but passing the float through),
  //    so compositing stays high-precision and colour-matched to the host pipe.
  //  - LEGACY: 8-bit Cairo ARGB32 (the original behaviour), kept for backward
  //    compatibility with edits made before the float path existed.
  // cache_legacy[] records which format the slot currently holds, so a mode
  // change invalidates and re-renders it.
  void *cache[MAX_OVERLAY];
  size_t cwidth[MAX_OVERLAY];
  size_t cheight[MAX_OVERLAY];
  gboolean cache_legacy[MAX_OVERLAY];
  dt_pthread_mutex_t overlay_threadsafe;
  int kernel_overlay_blend;        // float RGBA blend (HQ)
  int kernel_overlay_blend_legacy; // 8-bit Cairo ARGB blend (legacy)
} dt_iop_overlay_global_data_t;

typedef struct dt_iop_overlay_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *align[9];                               // Alignment buttons
  GtkWidget *opacity, *scale, *x_offset, *y_offset;  // opacity, scale, xoffs, yoffs
  GtkWidget *scale_base;                             // "scale on"
  GtkWidget *scale_img;                              // scale reference of image
  GtkWidget *scale_svg;                              // scale reference of marker
  GtkWidget *rotate;
  GtkWidget *imgid;
  gboolean drop_inside;
} dt_iop_overlay_gui_data_t;

/* Notes about the implementation.

   The creation of the overlay image use a standard pipe run. This is
   not fast so a cache is used.

   - The cached overlay buffers are stored into the global data.
     One slot is allocated for each instance (index is the multi_priority)
     and holds buffer address and dimensions.

   - To make the internal cache working safely we use a mutex encapsulating cache
     buffer changes making process() re-entry safe for concurrent pixelpipe runs.
 */

const char *name()
{
  return _("composite");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("combine the image with elements from another processed image"),
                                _("corrective and creative"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB"),
                                _("linear, RGB, scene-referred"));
}

const char *aliases()
{
  return _("layer|stack|overlay");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static GList *_get_disabled_modules(const dt_iop_module_t *self,
                                    const dt_imgid_t imgid)
{
  const dt_develop_t *dev = self->dev;
  const int multi_priority = self->multi_priority;

  /* we want a list of all modules that are after the current
     overlay module iop-order to ensure they are not processed via dt_dev_image().
     There are some exceptions:
       - gamma and finalscale are required
       - crop and &ashift make sense
     colorout is *not* an exception: keeping it filtered out (as it always was)
     means the overlay is rendered in the pipe's scene-referred linear working
     RGB. Combined with the want_float passthrough render in dt_dev_image(), the
     overlay reaches process() as linear float instead of a posterized 8-bit
     linear buffer, matching the host pipe at this module's position.
     The list order does not matter
  */

  const dt_iop_module_t *self_module = dt_iop_get_module_by_op_priority
    (dev->iop, "overlay", multi_priority);
  const gboolean is_current = dt_dev_is_current_image(dev, imgid);

  GList *result = NULL;
  gboolean after = FALSE;

  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = l->data;
    if((after
          && !dt_iop_module_is_gamma(mod)
          && !dt_iop_module_is_finalscale(mod)
          && !dt_iop_module_is(mod, "crop")
          && !dt_iop_module_is(mod, "ashift"))
    || (is_current
         && ( dt_iop_module_is(mod, "overlay")
           || dt_iop_module_is(mod, "enlargecanvas"))))
    {
      result = g_list_prepend(result, mod->op);
    }

    // look for ourself, disable all modules after this point
    if(dt_iop_module_is(mod, self_module->op)
         && mod->multi_priority == multi_priority)
      after = TRUE;
  }

  if(darktable.unmuted & (DT_DEBUG_PARAMS | DT_DEBUG_PIPE))
  {
    char *buf = g_malloc0(PATH_MAX);
    for(GList *m = result; m; m = g_list_next(m))
    {
      const char *mod = (char *)(m->data);
      g_strlcat(buf, mod, PATH_MAX);
      g_strlcat(buf, " ", PATH_MAX);
    }
    dt_print_pipe(DT_DEBUG_PARAMS | DT_DEBUG_PIPE, "module_filter_out",
          NULL, self, DT_DEVICE_NONE, NULL, NULL,
          "%s", buf);
    g_free(buf);
  }

  return result;
}

static void _clear_cache_entry(const dt_iop_module_t *self, const int index)
{
  dt_iop_overlay_global_data_t *gd = self->global_data;
  if(!gd) return;

  dt_free_align(gd->cache[index]);
  gd->cache[index] = NULL;
}

static void _module_remove_callback(gpointer instance,
                                    dt_iop_module_t *self,
                                    const gpointer user_data)
{
  if(!self || self != user_data) return;
  const dt_iop_overlay_params_t *p = self->params;

  if(dt_is_valid_imgid(p->imgid))
    dt_overlay_remove(self->dev->image_storage.id, p->imgid);
}

static void _setup_overlay(dt_iop_module_t *self,
                           const dt_dev_pixelpipe_iop_t *piece,
                           const gboolean legacy,
                           void **pbuf,
                           size_t *pwidth,
                           size_t *pheight)
{
  dt_iop_overlay_params_t *p = self->params;
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  const dt_iop_overlay_data_t *data = piece->data;

  dt_imgid_t imgid = data->imgid;

  if(!p || !dt_is_valid_imgid(imgid))
    return;

  dt_develop_t *dev = self->dev;

  gboolean image_exists = dt_image_exists(imgid);

  // The overlay image could have been removed from collection and
  // imported again. Check if we can find
  if(!image_exists)
  {
    const dt_imgid_t new_imgid = dt_image_get_id_full_path(data->filename);
    if(dt_is_valid_imgid(new_imgid))
    {
      image_exists = TRUE;
      p->imgid = new_imgid;
      imgid = new_imgid;
      dt_dev_add_history_item(dev, self, TRUE);
      if(g)
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
    }
    else if(g)
    {
      const gchar *tooltip = g_strdup_printf
        (_("overlay image missing from database\n\n"
           "'%s'" ), p->filename);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), tooltip);
    }
  }

  if(image_exists)
  {
    if(g)
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), "");

    uint8_t *buf;
    size_t bw;
    size_t bh;

    GList *disabled_modules = _get_disabled_modules(self, imgid);

    // HQ (legacy == FALSE): render scene-referred linear float (want_float):
    // colorout is filtered out by _get_disabled_modules() while gamma stays the
    // terminal module and passes the 4-channel float straight through (see
    // gamma.c process()), so the overlay arrives in the working RGB space rather
    // than as an 8-bit ARGB display backbuf.
    // LEGACY (legacy == TRUE): render the 8-bit ARGB display backbuf (want_float
    // FALSE), the original behaviour that the Cairo compositing path expects.
    const gboolean want_float = !legacy;

    // Render at the parent image's storage resolution. The result is
    // scale-stable and cached across host-pipe zoom changes; the float
    // resampler in _get_overlay_rgba_f() handles any final downscaling at
    // composite time. Rendering at a smaller,
    // zoom-dependent target would change the output of scale-dependent
    // modules in the overlay's pipeline (sharpen, denoise, local contrast,
    // lens correction, ...) and break preview/export consistency.
    const size_t width  = dev->image_storage.width;
    const size_t height = dev->image_storage.height;

    dt_dev_image(imgid,
                 width,
                 height,
                 -1,
                 &buf,
                 NULL,
                 &bw,
                 &bh,
                 NULL,
                 -1,
                 disabled_modules,
                 piece->pipe->devid,
                 TRUE,
                 want_float);

    void *old_buf = *pbuf;

    *pwidth = bw;
    *pheight = bh;
    *pbuf = buf;
    dt_free_align(old_buf);
  }
  else
  {
    dt_control_log(_("image %d does not exist"), imgid);
  }
}

// Placement geometry shared by the HQ (float) and legacy (Cairo) compositors.
// All of this math is identical between the two paths; only the final
// resampling/blend differs.
typedef struct _overlay_geometry_t
{
  float scale;  // net scale, including roi_out->scale and the user scale
  float tx, ty; // image-space translation (x/y offset folded in), pre roi_out->scale
  float cX, cY; // rotation centre, in output (device) pixels
} _overlay_geometry_t;

static _overlay_geometry_t _overlay_compute_geometry(const dt_iop_overlay_data_t *data,
                                                     const dt_dev_pixelpipe_iop_t *piece,
                                                     const dt_iop_roi_t *const roi_out,
                                                     const size_t bw,
                                                     const size_t bh,
                                                     const float angle)
{
  // overlay source dimensions (named 'dimension' so the placement math below
  // stays verbatim from the original implementation)
  struct
  {
    int width;
    int height;
  } dimension;
  dimension.width = (int)bw ? (int)bw : 1;
  dimension.height = (int)bh ? (int)bh : 1;

  const float iw     = piece->buf_in.width;
  const float ih     = piece->buf_in.height;
  const float uscale = data->scale / 100.0f;

  float wbase, hbase, scale, sbase;
  const float larger = dimension.width > dimension.height
    ? (float)dimension.width : (float)dimension.height;

  switch(data->scale_base)
  {
    case DT_SCALE_MAINMENU_LARGER_BORDER:
      sbase = wbase = hbase = (iw > ih) ? iw : ih;
      scale = sbase / larger;
      break;
    case DT_SCALE_MAINMENU_SMALLER_BORDER:
      sbase = wbase = hbase = (iw < ih) ? iw : ih;
      scale = sbase / larger;
      break;
    case DT_SCALE_MAINMENU_MARKERHEIGHT:
      wbase = iw; sbase = hbase = ih;
      scale = sbase / dimension.height;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      wbase = iw; hbase = ih;
      if(data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        sbase = iw;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else if(data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        sbase = ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else if(data->scale_img == DT_SCALE_IMG_LARGER)
      {
        sbase = (iw > ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else
      {
        sbase = (iw < ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      break;
    case DT_SCALE_MAINMENU_IMAGE:
    default:
      wbase = iw; hbase = ih;
      scale = (dimension.width > dimension.height)
        ? iw / dimension.width : ih / dimension.height;
  }

  scale *= roi_out->scale;
  scale *= uscale;

  float svg_width, svg_height;
  gboolean svg_calc_heightfromwidth;
  float svg_calc_base;

  switch(data->scale_base)
  {
    case DT_SCALE_MAINMENU_LARGER_BORDER:
      svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height);
      break;
    case DT_SCALE_MAINMENU_SMALLER_BORDER:
      svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height);
      break;
    case DT_SCALE_MAINMENU_MARKERHEIGHT:
      svg_calc_base = ih * uscale;
      svg_calc_heightfromwidth = FALSE;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      if(data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        svg_calc_base = iw * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH);
      }
      else if(data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        svg_calc_base = ih * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH);
      }
      else if(data->scale_img == DT_SCALE_IMG_LARGER)
      {
        svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH);
      }
      else
      {
        svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH);
      }
      break;
    case DT_SCALE_MAINMENU_IMAGE:
    default:
      if(dimension.width > dimension.height)
      {
        svg_calc_base = iw * uscale;
        svg_calc_heightfromwidth = TRUE;
      }
      else
      {
        svg_calc_base = ih * uscale;
        svg_calc_heightfromwidth = FALSE;
      }
  }

  if(svg_calc_heightfromwidth)
  {
    svg_width  = svg_calc_base;
    svg_height = dimension.height * (svg_width / dimension.width);
  }
  else
  {
    svg_height = svg_calc_base;
    svg_width  = dimension.width * (svg_height / dimension.height);
  }

  const float bb_width  = fabsf(svg_width * cosf(angle)) + fabsf(svg_height * sinf(angle));
  const float bb_height = fabsf(svg_width * sinf(angle)) + fabsf(svg_height * cosf(angle));
  const float bX = bb_width  / 2.0f - svg_width  / 2.0f;
  const float bY = bb_height / 2.0f - svg_height / 2.0f;

  float ty = 0.f, tx = 0.f;
  if(data->alignment >= 0 && data->alignment < 3)
    ty = bY;
  else if(data->alignment >= 3 && data->alignment < 6)
    ty = (ih / 2.0f) - (svg_height / 2.0f);
  else if(data->alignment >= 6 && data->alignment < 9)
    ty = ih - svg_height - bY;

  if(data->alignment == 0 || data->alignment == 3 || data->alignment == 6)
    tx = bX;
  else if(data->alignment == 1 || data->alignment == 4 || data->alignment == 7)
    tx = (iw / 2.0f) - (svg_width / 2.0f);
  else if(data->alignment == 2 || data->alignment == 5 || data->alignment == 8)
    tx = iw - svg_width - bX;

  tx += data->xoffset * wbase;
  ty += data->yoffset * hbase;

  _overlay_geometry_t geo;
  geo.scale = scale;
  geo.tx = tx;
  geo.ty = ty;
  // Centre of rotation, in output (device) pixels. Matches the Cairo chain:
  //   translate(-roi_in) · translate(t*scale) · translate(c) · rotate · translate(-c) · scale
  geo.cX = svg_width / 2.0f * roi_out->scale;
  geo.cY = svg_height / 2.0f * roi_out->scale;
  return geo;
}

/* Composite the overlay into a straight-alpha float RGBA buffer at roi_out
 * dimensions, in the host pipe's scene-referred linear working RGB.
 *
 * The cached overlay (*pbuf) is rendered once at parent-image storage
 * resolution and reused across zoom levels. Placement / scale / rotation are
 * applied here with a two-stage float resampler: anti-aliased minification
 * (dt_interpolation_resample) followed by per-pixel bicubic sampling of the
 * rotated placement. This is the high-precision counterpart to the 8-bit Cairo
 * path (_get_overlay_argb), which posterizes smooth gradients because the
 * overlay is quantized to a linear 8-bit buffer before compositing; that path
 * is kept selectable (DT_OVERLAY_COMPOSITE_LEGACY) for backward compatibility.
 *
 * Locking: overlay_threadsafe is held for the whole resample so the cached
 * buffer stays stable while it is read.
 *
 * Returns a dt_alloc_align_float'd buffer of 4 * roi_out->width *
 * roi_out->height floats (R, G, B, coverage per pixel) that the caller must
 * dt_free_align(), or NULL on failure.
 */
static float *_get_overlay_rgba_f(dt_iop_module_t *self,
                                  dt_dev_pixelpipe_iop_t *piece,
                                  const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out)
{
  dt_iop_overlay_data_t *data = piece->data;
  dt_iop_overlay_global_data_t *gd = self->global_data;
  const int index = self->multi_priority;
  const float angle = deg2radf(-data->rotate);

  // ── Acquire / refresh the overlay buffer ─────────────────────────────────
  dt_pthread_mutex_lock(&gd->overlay_threadsafe);

  const gboolean use_cache = (self->dev->image_storage.id == darktable.develop->image_storage.id);

  void *cbuf = NULL;
  size_t cwidth = 0;
  size_t cheight = 0;
  void **pbuf = use_cache ? &gd->cache[index] : &cbuf;
  size_t *pwidth = use_cache ? &gd->cwidth[index] : &cwidth;
  size_t *pheight = use_cache ? &gd->cheight[index] : &cheight;

  if(!dt_is_valid_imgid(data->imgid))
    _clear_cache_entry(self, index);

  // drop a cached buffer left over from the other compositing mode
  if(use_cache && gd->cache[index] && gd->cache_legacy[index])
    _clear_cache_entry(self, index);

  if(!*pbuf)
  {
    _setup_overlay(self, piece, FALSE /* legacy */, pbuf, pwidth, pheight);
    if(use_cache)
      gd->cache_legacy[index] = FALSE;
  }

  if(!*pbuf)
  {
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    return NULL;
  }

  const size_t bw = *pwidth;
  const size_t bh = *pheight;

  const _overlay_geometry_t geo = _overlay_compute_geometry(data, piece, roi_out, bw, bh, angle);
  const float scale = geo.scale;
  const float tx = geo.tx;
  const float ty = geo.ty;
  const float cX = geo.cX;
  const float cY = geo.cY;

  // ── Two-stage float resample ─────────────────────────────────────────────
  // Stage 1: anti-aliased minification of the source down to roughly its
  // displayed footprint, so the per-pixel sampling below runs near 1:1 and does
  // not alias on heavy downscales (storage resolution → preview).
  const float *src = *pbuf;
  size_t sw = bw;
  size_t sh = bh;
  float *mip = NULL;
  if(scale < 1.0f)
  {
    const int mw = MAX(1, (int)roundf((float)bw * scale));
    const int mh = MAX(1, (int)roundf((float)bh * scale));
    mip = dt_alloc_align_float((size_t)4 * mw * mh);
    if(mip)
    {
      const dt_iop_roi_t rin = { 0, 0, (int)bw, (int)bh, 1.0f };
      const dt_iop_roi_t rout = { 0, 0, mw, mh, (float)mw / (float)bw };
      const dt_interpolation_t *down = dt_interpolation_new(DT_INTERPOLATION_LANCZOS3);
      dt_interpolation_resample(down, mip, &rout, *pbuf, &rin);
      src = mip;
      sw = mw;
      sh = mh;
    }
  }

  const int ow = roi_out->width;
  const int oh = roi_out->height;
  float *canvas = dt_alloc_align_float((size_t)4 * ow * oh);
  if(!canvas)
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] out of memory %d*%d", ow, oh);
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    dt_free_align(mip);
    if(!use_cache)
      dt_free_align(cbuf);
    return NULL;
  }

  // Stage 2: inverse-map each output pixel back to source coordinates, applying
  // the same translate / rotate / scale chain Cairo used, then sample.
  const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_BICUBIC);
  const float ca = cosf(angle);
  const float sa = sinf(angle);
  const float t2x = tx * roi_out->scale;
  const float t2y = ty * roi_out->scale;
  const float msx = (float)sw / (float)bw; // mip scale (1.0 when no prefilter)
  const float msy = (float)sh / (float)bh;
  // dt_interpolation_compute_pixel4c expects the line stride in floats
  // (elements), not bytes — it indexes a float* as `in + linestride * iy`.
  const int src_stride = (int)(sw * 4);
  const float fsw = (float)sw;
  const float fsh = (float)sh;

  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < oh; y++)
    for(int x = 0; x < ow; x++)
    {
      // device → pre-scale frame (inverse translate/rotate)
      const float dvx = (float)x + (float)roi_in->x - t2x - cX;
      const float dvy = (float)y + (float)roi_in->y - t2y - cY;
      const float rx = ca * dvx + sa * dvy; // inverse rotation (by -angle)
      const float ry = -sa * dvx + ca * dvy;
      // → full-res source coordinates → mip coordinates
      const float u = (rx + cX) / scale;
      const float v = (ry + cY) / scale;
      const float mu = u * msx;
      const float mv = v * msy;

      float *const o = canvas + (size_t)4 * ((size_t)y * ow + x);

      // antialiased rectangle coverage (~1px ramps at the source borders)
      const float covu = CLAMP(mu + 0.5f, 0.0f, 1.0f) - CLAMP(mu - (fsw - 1.5f), 0.0f, 1.0f);
      const float covv = CLAMP(mv + 0.5f, 0.0f, 1.0f) - CLAMP(mv - (fsh - 1.5f), 0.0f, 1.0f);
      const float cov = covu * covv;

      if(cov <= 0.0f)
      {
        o[0] = o[1] = o[2] = o[3] = 0.0f;
        continue;
      }

      float px[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
      dt_interpolation_compute_pixel4c(itor, src, px, mu, mv, (int)sw, (int)sh, src_stride);
      o[0] = px[0];
      o[1] = px[1];
      o[2] = px[2];
      o[3] = cov;
    }

  dt_pthread_mutex_unlock(&gd->overlay_threadsafe);

  dt_free_align(mip);
  // Scratch overlay was local to this call; free it now that the resample is done.
  if(!use_cache) dt_free_align(cbuf);

  return canvas;
}

/* Legacy (8-bit) compositor: the original behaviour, kept so edits made before
 * the float path existed render identically. The overlay is rendered to an
 * 8-bit Cairo ARGB32 surface (display-encoded sRGB, BGRA), scaled/rotated by
 * Cairo, and returned as a Cairo ARGB32 buffer (row pitch = *out_stride bytes)
 * that the caller must g_free(), or NULL on failure. Placement geometry is the
 * same _overlay_compute_geometry() used by the float path.
 */
static guint8 *_get_overlay_argb(dt_iop_module_t *self,
                                 dt_dev_pixelpipe_iop_t *piece,
                                 const dt_iop_roi_t *const roi_in,
                                 const dt_iop_roi_t *const roi_out,
                                 int *out_stride)
{
  dt_iop_overlay_data_t *data = piece->data;
  dt_iop_overlay_global_data_t *gd = self->global_data;
  const int index = self->multi_priority;
  const float angle = deg2radf(-data->rotate);

  // ── Acquire / refresh the overlay buffer ─────────────────────────────────
  dt_pthread_mutex_lock(&gd->overlay_threadsafe);

  const gboolean use_cache = (self->dev->image_storage.id == darktable.develop->image_storage.id);

  void *cbuf = NULL;
  size_t cwidth = 0;
  size_t cheight = 0;
  void **pbuf = use_cache ? &gd->cache[index] : &cbuf;
  size_t *pwidth = use_cache ? &gd->cwidth[index] : &cwidth;
  size_t *pheight = use_cache ? &gd->cheight[index] : &cheight;

  if(!dt_is_valid_imgid(data->imgid))
    _clear_cache_entry(self, index);

  // drop a cached buffer left over from the HQ float mode
  if(use_cache && gd->cache[index] && !gd->cache_legacy[index])
    _clear_cache_entry(self, index);

  if(!*pbuf)
  {
    _setup_overlay(self, piece, TRUE /* legacy */, pbuf, pwidth, pheight);
    if(use_cache)
      gd->cache_legacy[index] = TRUE;
  }

  if(!*pbuf)
  {
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    return NULL;
  }

  // ── Allocate the Cairo output canvas ─────────────────────────────────────
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, roi_out->width);
  if(stride < 0)
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] cairo stride error");
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    return NULL;
  }

  guint8 *image = (guint8 *)g_try_malloc0_n(roi_out->height, stride);
  if(!image)
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] out of memory %d*%d", roi_out->height, stride);
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    return NULL;
  }

  cairo_surface_t *surface = cairo_image_surface_create_for_data(
    image, CAIRO_FORMAT_ARGB32, roi_out->width, roi_out->height, stride);

  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[overlay] cairo surface error: %s",
             cairo_status_to_string(cairo_surface_status(surface)));
    cairo_surface_destroy(surface);
    g_free(image);
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    return NULL;
  }

  // ── Cairo rendering ───────────────────────────────────────────────────────
  // plugin_threadsafe guards Cairo/rsvg.  Ordering: overlay_threadsafe first,
  // then plugin_threadsafe — consistent everywhere, no deadlock risk.
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  const size_t bw = *pwidth;
  const size_t bh = *pheight;

  // Wrap *pbuf directly — no memcpy of the (potentially large) buffer.
  cairo_surface_t *surface_two = dt_view_create_surface(*pbuf, bw, bh);

  if(cairo_surface_status(surface_two) != CAIRO_STATUS_SUCCESS)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[overlay] cairo overlay surface error: %s",
             cairo_status_to_string(cairo_surface_status(surface_two)));
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    dt_pthread_mutex_unlock(&gd->overlay_threadsafe);
    cairo_surface_destroy(surface_two);
    cairo_surface_destroy(surface);
    if(!use_cache)
      dt_free_align(cbuf);
    g_free(image);
    return NULL;
  }

  const _overlay_geometry_t geo = _overlay_compute_geometry(data, piece, roi_out, bw, bh, angle);

  cairo_t *cr = cairo_create(surface);

  cairo_translate(cr, -roi_in->x, -roi_in->y);
  cairo_translate(cr, geo.tx * roi_out->scale, geo.ty * roi_out->scale);

  cairo_translate(cr, geo.cX, geo.cY);
  cairo_rotate(cr, angle);
  cairo_translate(cr, -geo.cX, -geo.cY);

  cairo_scale(cr, geo.scale, geo.scale);
  cairo_surface_flush(surface_two);
  cairo_set_source_surface(cr, surface_two, 0.0, 0.0);
  cairo_paint(cr);

  // cairo_paint() is synchronous for CPU surfaces: *pbuf is no longer read.
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  dt_pthread_mutex_unlock(&gd->overlay_threadsafe);

  cairo_destroy(cr);
  cairo_surface_flush(surface);
  cairo_surface_destroy(surface);
  cairo_surface_destroy(surface_two); // drops reference to *pbuf, not ownership

  // Scratch overlay was local to this call; free it now that Cairo is done.
  if(!use_cache)
    dt_free_align(cbuf);

  *out_stride = stride;
  return image;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_overlay_data_t *data = piece->data;
  const int ch = piece->colors;
  const float *const in  = (const float *)ivoid;
  float *const       out = (float *)ovoid;
  const float opacity = data->opacity / 100.0f;

  if(data->compositing == DT_OVERLAY_COMPOSITE_LEGACY)
  {
    // legacy 8-bit Cairo ARGB32 overlay (display-encoded sRGB, BGRA byte order)
    int stride = 0;
    guint8 *image = _get_overlay_argb(self, piece, roi_in, roi_out, &stride);

    if(!image)
    {
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
      return;
    }

    DT_OMP_FOR(collapse(2))
    for(int y = 0; y < roi_out->height; y++)
      for(int x = 0; x < roi_out->width; x++)
      {
        const int j = y * roi_out->width + x;
        const float *i = in + ch * j;
        float *o = out + ch * j;
        // Cairo ARGB32 (little-endian): byte order is [B, G, R, A]
        const guint8 *s = image + (size_t)y * stride + (size_t)x * 4;

        const float alpha = (s[3] / 255.0f) * opacity;
        o[0] = (1.0f - alpha) * i[0] + opacity * s[2] / 255.0f;
        o[1] = (1.0f - alpha) * i[1] + opacity * s[1] / 255.0f;
        o[2] = (1.0f - alpha) * i[2] + opacity * s[0] / 255.0f;
        o[3] = i[3];
      }

    g_free(image);
    return;
  }

  // HQ: straight-alpha float RGBA overlay (R,G,B linear working RGB, A=coverage)
  float *const image = _get_overlay_rgba_f(self, piece, roi_in, roi_out);

  if(!image)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  DT_OMP_FOR(collapse(2))
  for(int y = 0; y < roi_out->height; y++)
    for(int x = 0; x < roi_out->width; x++)
    {
      const int    j = y * roi_out->width + x;
      const float *i = in  + ch * j;
      float       *o = out + ch * j;
      const float *s = image + (size_t)4 * j;

      const float alpha = s[3] * opacity;
      o[0] = (1.0f - alpha) * i[0] + alpha * s[0];
      o[1] = (1.0f - alpha) * i[1] + alpha * s[1];
      o[2] = (1.0f - alpha) * i[2] + alpha * s[2];
      o[3] = i[3];
    }

  dt_free_align(image);
}

#ifdef HAVE_OPENCL
// GPU alpha-blend path. The overlay resample runs on CPU (_get_overlay_rgba_f),
// but the pixel-blending of the full output image runs on the GPU.
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_overlay_data_t *data = piece->data;
  const dt_iop_overlay_global_data_t *gd = self->global_data;
  const int devid  = piece->pipe->devid;
  const int width  = roi_out->width;
  const int height = roi_out->height;
  const float opacity = data->opacity / 100.0f;

  if(data->compositing == DT_OVERLAY_COMPOSITE_LEGACY)
  {
    // legacy 8-bit Cairo ARGB32 overlay, blended by the legacy kernel (stride)
    int stride = 0;
    guint8 *limage = _get_overlay_argb(self, piece, roi_in, roi_out, &stride);

    if(!limage)
      return dt_opencl_enqueue_copy_image(
        devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN, (size_t[2]){ width, height });

    cl_int lerr = DT_OPENCL_SYSMEM_ALLOCATION;
    const size_t lsize = (size_t)height * stride;
    cl_mem dev_lov = dt_opencl_alloc_device_buffer(devid, lsize);
    if(!dev_lov)
      goto legacy_cleanup;

    lerr = dt_opencl_write_buffer_to_device(devid, limage, dev_lov, 0, lsize, TRUE);
    if(lerr != CL_SUCCESS)
      goto legacy_cleanup;

    lerr = dt_opencl_enqueue_kernel_2d_args(devid,
                                            gd->kernel_overlay_blend_legacy,
                                            width,
                                            height,
                                            CLARG(dev_in),
                                            CLARG(dev_lov),
                                            CLARG(dev_out),
                                            CLARG(width),
                                            CLARG(height),
                                            CLARG(opacity),
                                            CLARG(stride));

  legacy_cleanup:
    dt_opencl_release_mem_object(dev_lov);
    g_free(limage);
    return lerr;
  }

  // HQ: straight-alpha float RGBA overlay (R,G,B linear working RGB, A=coverage)
  float *image = _get_overlay_rgba_f(self, piece, roi_in, roi_out);

  if(!image)
  {
    // No overlay: copy input to output on GPU
    return dt_opencl_enqueue_copy_image(
      devid, dev_in, dev_out, CLIMG_ORIGIN, CLIMG_ORIGIN, (size_t[2]){ width, height });
  }

  cl_int err = DT_OPENCL_SYSMEM_ALLOCATION;

  // Upload the float RGBA overlay (4 floats/pixel) to GPU as a plain buffer
  const size_t overlay_size = (size_t)width * height * 4 * sizeof(float);
  cl_mem dev_overlay = dt_opencl_alloc_device_buffer(devid, overlay_size);
  if(!dev_overlay) goto cleanup;

  err = dt_opencl_write_buffer_to_device(devid, image, dev_overlay, 0, overlay_size, TRUE);
  if(err != CL_SUCCESS) goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(devid,
                                         gd->kernel_overlay_blend,
                                         width,
                                         height,
                                         CLARG(dev_in),
                                         CLARG(dev_overlay),
                                         CLARG(dev_out),
                                         CLARG(width),
                                         CLARG(height),
                                         CLARG(opacity));

cleanup:
  dt_opencl_release_mem_object(dev_overlay);
  dt_free_align(image);
  return err;
}
#endif

static gboolean _draw_thumb(GtkWidget *area,
                            cairo_t *crf,
                            const dt_iop_module_t *self)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  const dt_iop_overlay_params_t *p = self->params;

  GtkAllocation allocation;
  gtk_widget_get_allocation(area, &allocation);
  const int width = allocation.width;
  const int height = allocation.height;

  if(dt_is_valid_imgid(p->imgid))
  {
    cairo_surface_t *surf = NULL;
    const dt_view_surface_value_t res =
      dt_view_image_get_surface(p->imgid, width, height, &surf, TRUE);

    if(res == DT_VIEW_SURFACE_OK)
    {
      // compute dx/dy to center thumb on the area
      const int img_width = cairo_image_surface_get_width(surf);
      const int img_height = cairo_image_surface_get_height(surf);

      int dx = 0;
      int dy = 0;

      if(img_width > img_height)
        dy = (height - img_height) / 2;
      else
        dx = (width - img_width) / 2;

      dt_gui_gtk_set_source_rgb(crf, DT_GUI_COLOR_THUMBNAIL_BG);
      cairo_paint(crf);
      cairo_set_source_surface(crf, surf, dx, dy);
      cairo_paint(crf);
    }
  }
  else
  {
    dt_gui_gtk_set_source_rgb(crf, DT_GUI_COLOR_BG);
    cairo_set_line_width(crf, 3.0);
    cairo_rectangle(crf, 0.0, 0.0, width, height);
    if(g->drop_inside)
    {
      cairo_fill(crf);
    }
    cairo_move_to(crf, 0.0, 0.0);
    cairo_line_to(crf, width, height);
    cairo_move_to(crf, 0.0, height);
    cairo_line_to(crf, width, 0.0);
    cairo_stroke(crf);

    PangoFontDescription *desc =
      pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, DT_PIXEL_APPLY_DPI(12) * PANGO_SCALE);
    PangoLayout *layout = pango_cairo_create_layout(crf);
    pango_layout_set_font_description(layout, desc);
    // TRANSLATORS: This text must be very narrow, check in the GUI that it is not truncated
    pango_layout_set_text(layout, _("drop\nimage\nfrom filmstrip\nhere"), -1);

    PangoRectangle ink;
    pango_layout_get_pixel_extents(layout, &ink, NULL);

    dt_gui_gtk_set_source_rgb(crf, DT_GUI_COLOR_LIGHTTABLE_FONT);
    cairo_move_to(crf,
                  (width - ink.width) / 2.0,
                  (height - ink.height) / 2.0);
    pango_cairo_show_layout(crf, layout);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  return FALSE;
}

static void _alignment_callback(const GtkWidget *tb, dt_iop_module_t *self)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;

  DT_GUARD_GUI_UPDATE();
  dt_iop_overlay_params_t *p = self->params;

  int index = -1;

  for(int i = 0; i < 9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func(g->align[i], _alignment_callback, self);

    if(GTK_WIDGET(g->align[i]) == tb)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), TRUE);
      index = i;
    }
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func(g->align[i], _alignment_callback, self);
  }
  p->alignment = index;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    // v1 had no compositing option: it always used the 8-bit Cairo path.
    // Migrate to LEGACY so existing edits keep their original appearance.
    typedef struct dt_iop_overlay_params_v1_t
    {
      float opacity;
      float scale;
      float xoffset;
      float yoffset;
      int alignment;
      float rotate;
      dt_iop_overlay_base_scale_t scale_base;
      dt_iop_overlay_img_scale_t scale_img;
      dt_iop_overlay_svg_scale_t scale_svg;
      dt_imgid_t imgid;
      char filename[1024];
      size_t dummy0;
      size_t dummy1;
      int64_t dummy2;
    } dt_iop_overlay_params_v1_t;

    const dt_iop_overlay_params_v1_t *o = old_params;
    dt_iop_overlay_params_t *n = malloc(sizeof(dt_iop_overlay_params_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = o->rotate;
    n->scale_base = o->scale_base;
    n->scale_img = o->scale_img;
    n->scale_svg = o->scale_svg;
    n->imgid = o->imgid;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    n->compositing = DT_OVERLAY_COMPOSITE_LEGACY;
    n->dummy1 = 0;
    n->dummy2 = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_overlay_params_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_overlay_params_t *p = (dt_iop_overlay_params_t *)p1;
  dt_iop_overlay_data_t *d = piece->data;

  d->opacity    = p->opacity;
  d->scale      = p->scale;
  d->rotate     = p->rotate;
  d->xoffset    = p->xoffset;
  d->yoffset    = p->yoffset;
  d->alignment  = p->alignment;
  d->scale_base = p->scale_base;
  d->scale_img  = p->scale_img;
  d->scale_svg  = p->scale_svg;
  d->imgid      = p->imgid;
  d->compositing = p->compositing;
  g_strlcpy(d->filename, p->filename, sizeof(p->filename));
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_overlay_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  const dt_iop_overlay_params_t *p = self->params;

  for(int i = 0; i < 9; i++)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[p->alignment]), TRUE);

  if(p->scale_base == DT_SCALE_MAINMENU_ADVANCED)
  {
    gtk_widget_set_visible(GTK_WIDGET(g->scale_img), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), TRUE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(g->scale_img), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), FALSE);
  }

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_overlay_params_t *p = self->params;

  if(dt_is_valid_imgid(p->imgid))
    dt_overlay_remove(self->dev->image_storage.id, p->imgid);

  p->imgid = NO_IMGID;
}

void gui_reset(dt_iop_module_t *self)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;
  if(dt_is_valid_imgid(p->imgid))
    dt_overlay_remove(self->dev->image_storage.id, p->imgid);

  p->imgid = NO_IMGID;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  const dt_iop_overlay_params_t *p = self->params;

  if(w == g->scale_base)
  {
    if(p->scale_base == DT_SCALE_MAINMENU_ADVANCED)
    {
      gtk_widget_set_visible(GTK_WIDGET(g->scale_img), TRUE);
      gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), TRUE);
    }
    else
    {
      gtk_widget_set_visible(GTK_WIDGET(g->scale_img), FALSE);
      gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), FALSE);
    }
  }

  gtk_widget_queue_draw(GTK_WIDGET(g->area));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void init_global(dt_iop_module_so_t *self)
{
  dt_iop_overlay_global_data_t *gd = calloc(1, sizeof(dt_iop_overlay_global_data_t));

  pthread_mutexattr_t recursive_locking;
  pthread_mutexattr_init(&recursive_locking);
  pthread_mutexattr_settype(&recursive_locking, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&gd->overlay_threadsafe, &recursive_locking);

#ifdef HAVE_OPENCL
  const int program = 41; // overlay.cl
  gd->kernel_overlay_blend = dt_opencl_create_kernel(program, "overlay_blend");
  gd->kernel_overlay_blend_legacy = dt_opencl_create_kernel(program, "overlay_blend_legacy");
#else
  gd->kernel_overlay_blend = -1;
  gd->kernel_overlay_blend_legacy = -1;
#endif

  self->data = gd;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_overlay_global_data_t *gd = self->data;

  for(int k=0; k<MAX_OVERLAY; k++)
    dt_free_align(gd->cache[k]);

  dt_pthread_mutex_destroy(&gd->overlay_threadsafe);

#ifdef HAVE_OPENCL
  dt_opencl_free_kernel(gd->kernel_overlay_blend);
  dt_opencl_free_kernel(gd->kernel_overlay_blend_legacy);
#endif

  free(gd);
  self->data = NULL;
}

static void _signal_image_changed(gpointer instance, dt_iop_module_t *self)
{
  if(!self) return;

  for(int k=0; k<MAX_OVERLAY; k++)
    _clear_cache_entry(self, k);
}

static void _signal_module_moved(gpointer instance, dt_iop_module_t *self)
{
  if(!self) return;

  _clear_cache_entry(self, self->multi_priority);
  dt_dev_reprocess_all(self->dev);
}

static void _drag_and_drop_received(GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    const GtkSelectionData *selection_data,
                                    const guint target_type,
                                    const guint time,
                                    dt_iop_module_t *self)
{
  const dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;

  gboolean success = FALSE;
  if(selection_data != NULL && target_type == DND_TARGET_IMGID)
  {
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(dt_imgid_t);
    if(imgs_nb)
    {
      const int index  = self->multi_priority;
      const dt_imgid_t *imgs = (dt_imgid_t *)gtk_selection_data_get_data(selection_data);

      const dt_imgid_t imgid_intended_overlay = imgs[0];
      const dt_imgid_t imgid_target_image = self->dev->image_storage.id;

      // check for cross-references, that is this imgid_intended_overlay should not be using
      // the current image as overlay.

      if(dt_overlay_used_by(imgid_intended_overlay, imgid_target_image))
      {
        dt_control_log
          (_("cannot use image %d as an overlay"
             " as it is using the current image as an overlay, directly or indirectly"),
           imgid_intended_overlay);
      }
      else
      {
        // remove previous overlay if valid
        if(dt_is_valid_imgid(p->imgid))
          dt_overlay_remove(imgid_target_image, p->imgid);

        // and record the new one
        p->imgid         = imgid_intended_overlay;
        _clear_cache_entry(self, index);

        dt_overlay_record(imgid_target_image, imgid_intended_overlay);

        // zero-fill so trailing bytes of any previous image path do not bleed
        // into piece->hash (params are hashed by full buffer length)
        memset(p->filename, 0, sizeof(p->filename));
        dt_image_full_path(imgid_intended_overlay, p->filename, sizeof(p->filename), NULL);

        dt_dev_add_history_item(darktable.develop, self, TRUE);

        dt_control_queue_redraw_center();

        gtk_widget_queue_draw(GTK_WIDGET(g->area));

        success = TRUE;
      }
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
}

static gboolean _on_drag_motion(GtkWidget *widget,
                                GdkDragContext *dc,
                                gint x,
                                gint y,
                                guint time,
                                const dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;

  g->drop_inside = TRUE;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void _on_drag_leave(GtkWidget *widget,
                           GdkDragContext *dc,
                           guint time,
                           const dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;

  g->drop_inside = FALSE;
  gtk_widget_queue_draw(widget);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = IOP_GUI_ALLOC(overlay);
  dt_iop_overlay_params_t *p = self->params;

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(grid, DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(10));
  int line = 0;

  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_height(0));
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_draw_thumb), self);
  gtk_widget_set_size_request(GTK_WIDGET(g->area), 150, 150);
  gtk_grid_attach(grid, GTK_WIDGET(g->area), 0, line++, 1, 2);

  gtk_widget_grab_focus(GTK_WIDGET(g->area));

  gtk_drag_dest_set
    (GTK_WIDGET(g->area),  /* widget that will accept a drop */
     GTK_DEST_DEFAULT_ALL, /* default actions for dest on DnD */
     target_list_all,      /* lists of target to support */
     n_targets_all,        /* size of list */
     GDK_ACTION_MOVE       /* what to do with data after dropped */
     );

  g_signal_connect(GTK_WIDGET(g->area),
                   "drag-data-received", G_CALLBACK(_drag_and_drop_received), self);
  g_signal_connect(GTK_WIDGET(g->area),
                   "drag-motion", G_CALLBACK(_on_drag_motion), self);
  g_signal_connect(GTK_WIDGET(g->area),
                   "drag-leave", G_CALLBACK(_on_drag_leave), self);

  self->widget = dt_gui_vbox(grid);

  // Add opacity/scale sliders to table
  g->opacity = dt_bauhaus_slider_from_params(self, N_("opacity"));
  dt_bauhaus_slider_set_format(g->opacity, "%");

  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "placement")));

  // rotate
  g->rotate = dt_bauhaus_slider_from_params(self, "rotate");
  dt_bauhaus_slider_set_format(g->rotate, "°");
  dt_bauhaus_slider_set_factor(g->rotate, -1.f);

  // scale
  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_soft_max(g->scale, 100.0);
  dt_bauhaus_slider_set_format(g->scale, "%");

  // legacy scale on drop-down
  g->scale_base = dt_bauhaus_combobox_from_params(self, "scale_base");
  gtk_widget_set_tooltip_text
    (g->scale_base,
     _("choose how to scale the overlay\n"
       "• image: scale overlay relative to whole image\n"
       "• larger border: scale larger overlay border relative to larger image border\n"
       "• smaller border: scale larger overlay border relative to smaller image border\n"
       "• height: scale overlay height to image height\n"
       "• advanced options: choose overlay and image dimensions independently"));

  // scale image reference
  g->scale_img = dt_bauhaus_combobox_from_params(self, "scale_img");
  gtk_widget_set_tooltip_text
    (g->scale_img,
     _("reference image dimension against which to scale the overlay"));

  // scale marker reference
  g->scale_svg = dt_bauhaus_combobox_from_params(self, "scale_svg");
  gtk_widget_set_tooltip_text(g->scale_svg, _("overlay dimension to scale"));

  // Create the 3x3 gtk table toggle button table...
  GtkWidget *bat = gtk_grid_new();
  GtkWidget *label = dtgtk_reset_label_new(_("alignment"),
                                           self, &p->alignment, sizeof(p->alignment));
  gtk_grid_attach(GTK_GRID(bat), label, 0, 0, 1, 3);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_grid_set_row_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  for(int i = 0; i < 9; i++)
  {
    g->align[i] = dtgtk_togglebutton_new(dtgtk_cairo_paint_alignment,
                                         (CPF_SPECIAL_FLAG << i), NULL);
    gtk_grid_attach(GTK_GRID(bat), GTK_WIDGET(g->align[i]), 1 + i%3, i/3, 1, 1);
    g_signal_connect(G_OBJECT(g->align[i]), "toggled",
                     G_CALLBACK(_alignment_callback), self);
  }

  dt_gui_box_add(self->widget, bat);

  // x/y offset
  g->x_offset = dt_bauhaus_slider_from_params(self, "xoffset");
  dt_bauhaus_slider_set_digits(g->x_offset, 3);
  g->y_offset = dt_bauhaus_slider_from_params(self, "yoffset");
  dt_bauhaus_slider_set_digits(g->y_offset, 3);

  // Let's add some tooltips and hook up some signals...
  gtk_widget_set_tooltip_text(g->opacity, _("the opacity of the overlay"));
  gtk_widget_set_tooltip_text(g->scale, _("the scale of the overlay"));
  gtk_widget_set_tooltip_text(g->rotate, _("the rotation of the overlay"));

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_MODULE_REMOVE, _module_remove_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _signal_image_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_MODULE_MOVED, _signal_module_moved);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
