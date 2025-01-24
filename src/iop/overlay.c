/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

DT_MODULE_INTROSPECTION(1, dt_iop_overlay_params_t)

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
  dt_imgid_t imgid; // overlay image id
  char filename[1024]; // full overlay's filename
  // keep parameter struct to avoid a version bump
  size_t dummy0;
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
} dt_iop_overlay_data_t;

#define MAX_OVERLAY 50

typedef struct dt_iop_overlay_global_data_t
{
  uint8_t *cache[MAX_OVERLAY];
  size_t cwidth[MAX_OVERLAY];
  size_t cheight[MAX_OVERLAY];
  dt_pthread_mutex_t overlay_threadsafe;
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
  return dt_iop_set_description
    (self,
     _("combine with elements from a processed image"),
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
          && !dt_iop_module_is(mod->so, "gamma")
          && !dt_iop_module_is(mod->so, "finalscale")
          && !dt_iop_module_is(mod->so, "crop")
          && !dt_iop_module_is(mod->so, "ashift"))
    || (is_current
         && ( dt_iop_module_is(mod->so, "overlay")
           || dt_iop_module_is(mod->so, "enlargecanvas"))))
    {
      result = g_list_prepend(result, mod->op);
    }

    // look for ourself, disable all modules after this point
    if(dt_iop_module_is(mod->so, self_module->op)
         && mod->multi_priority == multi_priority)
      after = TRUE;
  }

  if(darktable.unmuted & (DT_DEBUG_PARAMS | DT_DEBUG_PIPE))
  {
    char *buf = g_malloc0(PATH_MAX);
    for(GList *m = result; m; m = g_list_next(m))
    {
      char *mod = (char *)(m->data);
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

static void _clear_cache_entry(dt_iop_module_t *self, const int index)
{
  dt_iop_overlay_global_data_t *gd = self->global_data;
  if(!gd) return;

  dt_free_align(gd->cache[index]);
  gd->cache[index] = NULL;
}

static void _module_remove_callback(gpointer instance,
                                    dt_iop_module_t *self,
                                    gpointer user_data)
{
  if(!self || self != user_data) return;
  dt_iop_overlay_params_t *p = self->params;

  if(dt_is_valid_imgid(p->imgid))
    dt_overlay_remove(self->dev->image_storage.id, p->imgid);
}

static void _setup_overlay(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           uint8_t **pbuf,
                           size_t *pwidth,
                           size_t *pheight)
{
  dt_iop_overlay_params_t *p = self->params;
  dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_data_t *data = piece->data;

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
      gchar *tooltip = g_strdup_printf
        (_("overlay image missing from database\n\n"
           "'%s'" ), p->filename);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), tooltip);
    }
  }

  if(image_exists)
  {
    const size_t width  = dev->image_storage.width;
    const size_t height = dev->image_storage.width;

    if(g)
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), "");

    uint8_t *buf;
    size_t bw;
    size_t bh;

    GList *disabled_modules = _get_disabled_modules(self, imgid);

    dt_dev_image(imgid, width, height,
                 -1,
                 &buf, NULL, &bw, &bh,
                 NULL, NULL,
                 -1, disabled_modules, piece->pipe->devid, TRUE);

    uint8_t *old_buf = *pbuf;

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

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_overlay_data_t *data = piece->data;
  dt_iop_overlay_global_data_t *gd = self->global_data;

  /* We have several pixelpipes that might want to save the processed overlay in
     the internal cache (both previews and full).
     By using a mutex here we ensure
     a) safe data pointer and dimension
     b) only the first darkroom pipe being here has the hard work via _setup_overlay().
  */
  dt_pthread_mutex_lock(&gd->overlay_threadsafe);

  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const float angle = (M_PI / 180) * (-data->rotate);
  const int index   = self->multi_priority;

  if(!dt_is_valid_imgid(data->imgid))
    _clear_cache_entry(self, index);

  // scratch buffer data and dimension
  uint8_t *cbuf = NULL;
  size_t cwidth = 0;
  size_t cheight = 0;

  uint8_t **pbuf;
  size_t *pwidth;
  size_t *pheight;

  // if called from darkroom (the edited image is the one in
  // darktable->develop) we use the cache, otherwise we just use a
  // scratch buffer local to process for rendering.
  if(self->dev->image_storage.id == darktable.develop->image_storage.id)
  {
    pbuf = &gd->cache[index];
    pwidth = &gd->cwidth[index];
    pheight = &gd->cheight[index];
  }
  else
  {
    pbuf = &cbuf;
    pwidth = &cwidth;
    pheight = &cheight;
  }

  if(!*pbuf)
  {
    // need the overlay - either because we use the scratch buffer or the cacheline
    // is still empty - create the buffer now and leave address dimension
    _setup_overlay(self, piece, pbuf, pwidth, pheight);
  }

  dt_pthread_mutex_unlock(&gd->overlay_threadsafe);

  /*
     From here on we check every processing step for success, if there is a problem
     we return after plain copy input -> output and possible leave a log note.
  */

  if(!*pbuf)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  /* setup stride for performance */
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, roi_out->width);
  if(stride == -1)
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] cairo stride error");
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  /* create a cairo memory surface that is later used for reading
   * overlay overlay data */
  guint8 *image = (guint8 *)g_try_malloc0_n(roi_out->height, stride);
  if(!image)
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] out of memory - could not allocate %d*%d",
             roi_out->height, stride);
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }
  cairo_surface_t *surface =
    cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32,
                                        roi_out->width,
                                        roi_out->height, stride);

  if((cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) || (image == NULL))
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] cairo surface error: %s",
             cairo_status_to_string(cairo_surface_status(surface)));
    g_free(image);
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  // rsvg (or some part of cairo which is used underneath) isn't
  // thread safe, for example when handling fonts

  // we use a second surface
  cairo_surface_t *surface_two = NULL;

  /* get the dimension of svg or png */
  RsvgDimensionData dimension;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  const size_t bw = *pwidth;
  const size_t bh = *pheight;

  const size_t size_buf = bw * bh * sizeof(uint32_t);
  uint8_t *buf = (uint8_t *)dt_alloc_aligned(size_buf);
  memcpy(buf, *pbuf, size_buf);

  // load overlay image into surface 2
  surface_two = dt_view_create_surface(buf, bw, bh);

  if((cairo_surface_status(surface_two) != CAIRO_STATUS_SUCCESS))
  {
    dt_print(DT_DEBUG_ALWAYS, "[overlay] cairo png surface 2 error: %s",
             cairo_status_to_string(cairo_surface_status(surface_two)));
    cairo_surface_destroy(surface);
    g_free(image);
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    return;
  }

  dimension.width = cairo_image_surface_get_width(surface_two);
  dimension.height = cairo_image_surface_get_height(surface_two);

  // if no text is given dimensions are null
  if(!dimension.width)
    dimension.width = 1;
  if(!dimension.height)
    dimension.height = 1;

  //  width/height of current (possibly cropped) image
  const float iw = piece->buf_in.width;
  const float ih = piece->buf_in.height;
  const float uscale = data->scale / 100.0f; // user scale, from GUI in percent

  // wbase, hbase are the base width and height, this is the
  // multiplicator used for the offset computing scale is the scale of
  // the overlay itself and is used only to render it.
  // sbase is used for scale calculation in the larger/smaller modes
  float wbase, hbase, scale, sbase;

  // in larger/smaller (legacy) side mode, set wbase and hbase to the largest
  // or smallest side of the image
  const float larger = dimension.width > dimension.height
    ? (float)dimension.width
    : (float)dimension.height;

  // set the base width and height to either large or smaller
  // border of current image and calculate scale using either
  // marker (SVG object) width or height
  switch (data->scale_base)
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
      wbase = iw;
      sbase = hbase = ih;
      scale = sbase / dimension.height;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      wbase = iw;
      hbase = ih;
      if (data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        sbase = iw;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else if (data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        sbase = ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else if (data->scale_img == DT_SCALE_IMG_LARGER)
      {
        sbase = (iw > ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      else // data->scale_img == DT_SCALE_IMG_SMALLER
      {
        sbase = (iw < ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH)
          ? sbase / dimension.width : sbase / dimension.height;
      }
      break;

    // default to "image" mode
    case DT_SCALE_MAINMENU_IMAGE:
    default:
      // in image mode, the wbase and hbase are just the image width and height
      wbase = iw;
      hbase = ih;
      if(dimension.width > dimension.height)
        scale = iw / dimension.width;
      else
        scale = ih / dimension.height;
  }

  scale *= roi_out->scale;
  scale *= uscale;

  // compute the width and height of the SVG object in image
  // dimension. This is only used to properly layout the overlay
  // based on the alignment.

  float svg_width, svg_height;

  // help to reduce the number of if clauses
  gboolean svg_calc_heightfromwidth;   // calculate svg_height from svg_width if TRUE
                                       // calculate svg_width from svg_height if FALSE
  float svg_calc_base;                 // this value is used as svg_width or svg_height,
                                       // depending on svg_calc_heightfromwidth

  switch (data->scale_base)
  {
    case DT_SCALE_MAINMENU_LARGER_BORDER:
      svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height) ? TRUE : FALSE;
      break;
    case DT_SCALE_MAINMENU_SMALLER_BORDER:
      svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height) ? TRUE : FALSE;
      break;
    case DT_SCALE_MAINMENU_MARKERHEIGHT:
      svg_calc_base = ih * uscale;
      svg_calc_heightfromwidth = FALSE;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      if (data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        svg_calc_base = iw * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else if (data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        svg_calc_base = ih * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else if (data->scale_img == DT_SCALE_IMG_LARGER)
      {
        svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else // data->scale_img == DT_SCALE_IMG_SMALLER
      {
        svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      break;

    // default to "image" mode
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
    // calculate svg_height from svg_width
    svg_width = svg_calc_base;
    svg_height = dimension.height * (svg_width / dimension.width);
  }
  else
  {
    // calculate svg_width from svg_height
    svg_height = svg_calc_base;
    svg_width = dimension.width * (svg_height / dimension.height);
  }

  /* For the rotation we need an extra cairo image as rotations are
     buggy via rsvg_handle_render_cairo.  distortions and blurred
     images are obvious but you also can easily have crashes.
  */

  float svg_offset_x = 0;
  float svg_offset_y = 0;

  /* create cairo context and setup transformation/scale */
  cairo_t *cr = cairo_create(surface);

  /* create cairo context for the scaled overlay */
  cairo_t *cr_two = cairo_create(surface_two);

  // compute bounding box of rotated overlay
  const float bb_width = fabsf(svg_width * cosf(angle)) + fabsf(svg_height * sinf(angle));
  const float bb_height = fabsf(svg_width * sinf(angle)) + fabsf(svg_height * cosf(angle));
  const float bX = bb_width / 2.0f - svg_width / 2.0f;
  const float bY = bb_height / 2.0f - svg_height / 2.0f;

  // compute translation for the given alignment in image dimension

  float ty = 0, tx = 0;
  if(data->alignment >= 0 && data->alignment < 3) // Align to verttop
    ty = bY;
  else if(data->alignment >= 3 && data->alignment < 6) // Align to vertcenter
    ty = (ih / 2.0f) - (svg_height / 2.0f);
  else if(data->alignment >= 6 && data->alignment < 9) // Align to vertbottom
    ty = ih - svg_height - bY;

  if(data->alignment == 0 || data->alignment == 3 || data->alignment == 6)
    tx = bX;
  else if(data->alignment == 1 || data->alignment == 4 || data->alignment == 7)
    tx = (iw / 2.0f) - (svg_width / 2.0f);
  else if(data->alignment == 2 || data->alignment == 5 || data->alignment == 8)
    tx = iw - svg_width - bX;

  // translate to position
  cairo_translate(cr, -roi_in->x, -roi_in->y);

  // add translation for the given value in GUI (xoffset,yoffset)
  tx += data->xoffset * wbase;
  ty += data->yoffset * hbase;

  cairo_translate(cr, tx * roi_out->scale, ty * roi_out->scale);

  // compute the center of the svg to rotate from the center
  const float cX = svg_width / 2.0f * roi_out->scale;
  const float cY = svg_height / 2.0f * roi_out->scale;

  cairo_translate(cr, cX, cY);
  cairo_rotate(cr, angle);
  cairo_translate(cr, -cX, -cY);

  // now set proper scale and translationfor the overlay itself
  cairo_translate(cr_two, svg_offset_x, svg_offset_y);

  cairo_scale(cr, scale, scale);
  cairo_surface_flush(surface_two);

  // paint the overlay
  cairo_set_source_surface(cr, surface_two, -svg_offset_x, -svg_offset_y);
  cairo_paint(cr);

  // no more non-thread safe rsvg usage
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  cairo_destroy(cr);
  cairo_destroy(cr_two);

  /* ensure that all operations on surface finishing up */
  cairo_surface_flush(surface);

  /* render surface on output */
  const float opacity = data->opacity / 100.0f;

  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height * roi_out->width; j++)
  {
    float *const i = in + ch*j;
    float *const o = out + ch*j;
    guint8 *const s = image + 4*j;

    const float alpha = (s[3] / 255.0f) * opacity;

    o[0] = (1.0f - alpha) * i[0] + (opacity * s[2] / 255.0f);
    o[1] = (1.0f - alpha) * i[1] + (opacity * s[1] / 255.0f);
    o[2] = (1.0f - alpha) * i[2] + (opacity * s[0] / 255.0f);
    o[3] = in[3];
  }

  /* clean up */
  cairo_surface_destroy(surface);
  cairo_surface_destroy(surface_two);
  g_free(image);
  dt_free_align(buf);
}

static void _draw_thumb(GtkWidget *area,
                        cairo_t *crf,
                        dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;

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
}

static void _alignment_callback(GtkWidget *tb, dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;
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

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_overlay_params_t *p = (dt_iop_overlay_params_t *)p1;
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
  dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;

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
  dt_iop_overlay_gui_data_t *g = self->gui_data;
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
  dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;

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
  self->data = gd;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_overlay_global_data_t *gd = self->data;

  for(int k=0; k<MAX_OVERLAY; k++)
    dt_free_align(gd->cache[k]);

  dt_pthread_mutex_destroy(&gd->overlay_threadsafe);
  free(gd);
  self->data = NULL;
}

static void _signal_image_changed(gpointer instance, dt_iop_module_t *self)
{
  if(!self) return;

  for(int k=0; k<MAX_OVERLAY; k++)
    _clear_cache_entry(self, k);
}

static void _drag_and_drop_received(GtkWidget *widget,
                                    GdkDragContext *context,
                                    gint x,
                                    gint y,
                                    GtkSelectionData *selection_data,
                                    guint target_type,
                                    guint time,
                                    dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;
  dt_iop_overlay_params_t *p = self->params;

  gboolean success = FALSE;
  if(selection_data != NULL && target_type == DND_TARGET_IMGID)
  {
    const int imgs_nb = gtk_selection_data_get_length(selection_data) / sizeof(dt_imgid_t);
    if(imgs_nb)
    {
      const int index  = self->multi_priority;
      dt_imgid_t *imgs = (dt_imgid_t *)gtk_selection_data_get_data(selection_data);

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
                                dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;

  g->drop_inside = TRUE;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void _on_drag_leave(GtkWidget *widget,
                           GdkDragContext *dc,
                           guint time,
                           dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = self->gui_data;

  g->drop_inside = FALSE;
  gtk_widget_queue_draw(widget);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_overlay_gui_data_t *g = IOP_GUI_ALLOC(overlay);
  dt_iop_overlay_params_t *p = self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

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

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), TRUE, TRUE, 0);

  // Add opacity/scale sliders to table
  g->opacity = dt_bauhaus_slider_from_params(self, N_("opacity"));
  dt_bauhaus_slider_set_format(g->opacity, "%");

  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_section_label_new(C_("section", "placement")), TRUE, TRUE, 0);

  // rotate
  g->rotate = dt_bauhaus_slider_from_params(self, "rotate");
  dt_bauhaus_slider_set_format(g->rotate, "°");

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

  gtk_box_pack_start(GTK_BOX(self->widget), bat, FALSE, FALSE, 0);

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
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
