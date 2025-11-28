/*
    This file is part of darktable,
    Copyright (C) 2013-2025 darktable developers.

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
#include "common/debug.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include "develop/masks.h"

static void _raster_sanitize_config(dt_masks_type_t type)
{
  // Placeholder
}

static GSList *_raster_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL,
                                     GDK_SHIFT_MASK, _("[RASTER] change feather size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL,
                                     GDK_CONTROL_MASK, _("[RASTER] change opacity"));
  return lm;
}

static void _raster_set_form_name(dt_masks_form_t *const form,
                                  const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("raster #%d"), (int)nb);
}

static void _raster_set_hint_message(const dt_masks_form_gui_t *const gui,
                                     const dt_masks_form_t *const form,
                                     const int opacity,
                                     char *const restrict msgbuf,
                                     const size_t msgbuf_len)
{
  // circle has same controls on creation and on edit
  g_snprintf(msgbuf, msgbuf_len,
             _("<b>feather size</b>: shift+scroll\n"
               "<b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _raster_modify_property(dt_masks_form_t *const form,
                                    const dt_masks_property_t prop,
                                    const float old_val,
                                    const float new_val,
                                    float *sum,
                                    int *count,
                                    float *min,
                                    float *max)
{

}

static void _raster_duplicate_points(dt_develop_t *dev,
                                     dt_masks_form_t *const base,
                                     dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_raster_t *pt = pts->data;
    dt_masks_point_raster_t *npt = malloc(sizeof(dt_masks_point_raster_t));
    memcpy(npt, pt, sizeof(dt_masks_point_raster_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _raster_initial_source_pos(const float iwd,
                                       const float iht,
                                       float *x,
                                       float *y)
{
  *x = 0;
  *y = 0;
}

static void _raster_get_distance(const float x,
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
  (void)num_points; // unused arg, keep compiler from complaining
  // initialise returned values
  *inside_source = FALSE;
  *inside = FALSE;
  *inside_border = FALSE;
  *near = -1;
  *dist = FLT_MAX;
}

static int _raster_get_points(dt_develop_t *dev,
                              const float x,
                              const float y,
                              const float radius,
                              const float radius2,
                              const float rotation,
                              float **points,
                              int *points_count)
{
  (void)radius2; // keep compiler from complaining about unused arg
  (void)rotation;
  // float wd, ht;

  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 1;
}

static int _raster_get_points_border(dt_develop_t *dev,
                                     struct dt_masks_form_t *form,
                                     float **points,
                                     int *points_count,
                                     float **border,
                                     int *border_count,
                                     const int source,
                                     const dt_iop_module_t *module)
{
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  return 1;
}

static int _render_raster_mask(const dt_iop_module_t *const restrict module,
                               const dt_dev_pixelpipe_iop_t *const restrict piece,
                               dt_masks_form_t *const restrict form,
                               float *buffer,
                               int width,
                               int height)
{
  const size_t obuffsize = (size_t)width * height;

  // Skip if module is not in focus
  // if(!dt_iop_has_focus((dt_iop_module_t *)module))
  //   return 0;

  // initialize output buffer with zero
  memset(buffer, 0, sizeof(float) * width * height);

  dt_masks_point_raster_t *rasterPoint = form->points->data;
  // if(!rasterPoint)
  //   return 0;

  // Find module with the same ID as the mask ID
  // dt_iop_module_t * source = NULL;
  // GList *source_iter;
  // for(source_iter = piece->pipe->nodes;
  //     source_iter;
  //     source_iter = g_list_next(source_iter))
  // {
  //   dt_dev_pixelpipe_iop_t *candidate = source_iter->data;
  //   if (candidate->module->instance == rasterPoint->sourceInstanceId) {
  //     source = candidate->module;
  //     break;
  //   }
  // }

  // if (!source) {
  //   // TODO: Show error, log, or message
  //   return 0;
  // }

  gboolean free_mask;
  float *raster_mask = dt_dev_get_raster_mask(
    (dt_dev_pixelpipe_iop_t *)piece,
    module->raster_mask.sink.source,
    module->raster_mask.sink.id,
    module,
    &free_mask
  );

  if(!raster_mask)
    return 0;

  // Copy content of mask to prevent modification of the actual mask data
  float *const restrict mask = dt_alloc_align_float(obuffsize);
  DT_OMP_FOR_SIMD(aligned(mask, raster_mask:64))
  for(size_t i = 0; i < obuffsize; i++)
    mask[i] = raster_mask[i] * rasterPoint->opacity;

  // Forward raster mask
  dt_iop_image_scaled_copy(buffer, mask, 1.0f, width, height, 1);

  if(free_mask) dt_free_align(raster_mask);
  dt_free_align(mask); // Don't forget to free our local mask copy

  return 1;
}

static int _raster_get_mask(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            float **buffer,
                            int *width,
                            int *height,
                            int *posx,
                            int *posy)
{
  // dt_iop_gui_blend_data_t *bd = module->blend_data;
  return _render_raster_mask(module, piece, form, *buffer, *width, *height);
}

static int _raster_get_mask_roi(const dt_iop_module_t *const restrict module,
                                const dt_dev_pixelpipe_iop_t *const restrict piece,
                                dt_masks_form_t *const form,
                                const dt_iop_roi_t *const roi,
                                float *const restrict buffer)
{
  const int width = roi->width;
  const int height = roi->height;
  return _render_raster_mask(module, piece, form, buffer, width, height);
}

static int _raster_get_area(const dt_iop_module_t *const restrict module,
                            const dt_dev_pixelpipe_iop_t *const restrict piece,
                            dt_masks_form_t *const restrict form,
                            int *width,
                            int *height,
                            int *posx,
                            int *posy)
{
  *posx = 0;
  *posy = 0;
  *width = piece->pipe->iwidth;
  *height = piece->pipe->iheight;
  return 1;
}

static int _raster_get_source_area(dt_iop_module_t *module,
                                   dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form,
                                   int *width,
                                   int *height,
                                   int *posx,
                                   int *posy)
{
  *posx = 0;
  *posy = 0;
  *width = piece->pipe->iwidth;
  *height = piece->pipe->iheight;
  return 1;
}

static int _raster_events_mouse_moved(dt_iop_module_t *module,
                                      const float pzx,
                                      const float pzy,
                                      const double pressure,
                                      const int which,
                                      const float zoom_scale,
                                      dt_masks_form_t *form,
                                      const dt_mask_id_t parentid,
                                      dt_masks_form_gui_t *gui,
                                      const int index)
{
  if(!gui) return 0;
  return 1;
}

static int _raster_events_mouse_scrolled(dt_iop_module_t *module,
                                         const float pzx,
                                         const float pzy,
                                         const int up,
                                         const uint32_t state,
                                         dt_masks_form_t *form,
                                         const dt_mask_id_t parentid,
                                         dt_masks_form_gui_t *gui,
                                         const int index)
{
  if(!gui) return 0;
  return 1;
}

static int _raster_events_button_pressed(dt_iop_module_t *module,
                                         float pzx, float pzy,
                                         const double pressure,
                                         const int which,
                                         const int type,
                                         const uint32_t state,
                                         dt_masks_form_t *form,
                                         const dt_mask_id_t parentid,
                                         dt_masks_form_gui_t *gui,
                                         const int index)
{
  if(!gui) return 0;
  return 1;
}

static int _raster_events_button_released(dt_iop_module_t *module,
                                          const float pzx,
                                          const float pzy,
                                          const int which,
                                          const uint32_t state,
                                          dt_masks_form_t *form,
                                          const dt_mask_id_t parentid,
                                          dt_masks_form_gui_t *gui,
                                          const int index)
{
  if(!gui) return 0;

  if (gui->creation)
  {
    // Create raster mask
    dt_masks_point_raster_t *raster = malloc(sizeof(dt_masks_point_raster_t));
    raster->opacity = 1.0f;

    dt_iop_module_t *sourceMask = module->raster_mask.sink.source;
    if (!sourceMask) {
      // TODO: Print error
      return 0;
    }
    raster->sourceInstanceId = sourceMask->instance;

    gui->form_dragging = FALSE;

    form->points = g_list_append(form->points, raster);

    dt_iop_module_t *crea_module = gui->creation_module;
    dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

    if(crea_module)
    {
      // we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      // and we switch in edit mode to show all the forms
      dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(crea_module);
    }

    dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid);
    gui->creation_module = NULL;
  }

  return 1;
}

static void _raster_events_post_expose(cairo_t *cr,
                                       const float zoom_scale,
                                       dt_masks_form_gui_t *gui,
                                       const int index,
                                       const int num_points)
{
  // TODO: Implement ME!!!
  return;
}

// The function table for circles.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_raster = {
  .point_struct_size = sizeof(struct dt_masks_point_raster_t),
  .sanitize_config = _raster_sanitize_config,
  .setup_mouse_actions = _raster_setup_mouse_actions,
  .set_form_name = _raster_set_form_name,
  .set_hint_message = _raster_set_hint_message,
  .modify_property = _raster_modify_property,
  .duplicate_points = _raster_duplicate_points,
  .initial_source_pos = _raster_initial_source_pos,
  .get_distance = _raster_get_distance,
  .get_points = _raster_get_points,
  .get_points_border = _raster_get_points_border,
  .get_mask = _raster_get_mask,
  .get_mask_roi = _raster_get_mask_roi,
  .get_area = _raster_get_area,
  .get_source_area = _raster_get_source_area,
  .mouse_moved = _raster_events_mouse_moved,
  .mouse_scrolled = _raster_events_mouse_scrolled,
  .button_pressed = _raster_events_button_pressed,
  .button_released = _raster_events_button_released,
  .post_expose = _raster_events_post_expose
};
