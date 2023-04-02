/*
    This file is part of darktable,
    Copyright (C) 2013-2023 darktable developers.

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

#include "develop/masks.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/undo.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"

#pragma GCC diagnostic ignored "-Wshadow"

dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *form)
{
  if(!form) return NULL;

  dt_masks_form_t *new_form = malloc(sizeof(struct dt_masks_form_t));
  memcpy(new_form, form, sizeof(struct dt_masks_form_t));

  // then duplicate the GList *points

  GList* newpoints = NULL;

  if(form->points)
  {
    const int size_item = (form->functions) ? form->functions->point_struct_size : 0;

    if(size_item != 0)
    {
      for(GList *pt = form->points; pt; pt = g_list_next(pt))
      {
        void *item = malloc(size_item);
        memcpy(item, pt->data, size_item);
        newpoints = g_list_prepend(newpoints, item);
      }
    }
  }
  // list was built in reverse order, so un-reverse it
  new_form->points = g_list_reverse(newpoints);

  return new_form;
}

static void *_dup_masks_form_cb(const void *formdata, gpointer user_data)
{
  // duplicate the main form struct
  dt_masks_form_t *form = (dt_masks_form_t *)formdata;
  dt_masks_form_t *uform = (dt_masks_form_t *)user_data;
  const dt_masks_form_t *f = uform == NULL || form->formid != uform->formid ? form : uform;
  return (void *)dt_masks_dup_masks_form(f);
}

// duplicate the list of forms, replace item in the list with form with the same formid
GList *dt_masks_dup_forms_deep(GList *forms,
                               dt_masks_form_t *form)
{
  return (GList *)g_list_copy_deep(forms, _dup_masks_form_cb, (gpointer)form);
}

static int _get_opacity(dt_masks_form_gui_t *gui,
                        const dt_masks_form_t *form)
{
  const dt_masks_point_group_t *fpt =
    (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
  const dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!sel) return 0;

  const int formid = sel->formid;

  // look for apacity
  const dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, fpt->parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return 0;

  int opacity = 0;
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    const dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    if(fpt->formid == formid)
    {
      opacity = fpt->opacity * 100;
      break;
    }
  }

  return opacity;
}

static dt_masks_type_t _get_all_types_in_group(dt_masks_form_t *form)
{
  if(form->type & DT_MASKS_GROUP)
  {
    dt_masks_type_t tp = 0;
    for(GList *l = form->points; l; l = g_list_next(l))
    {
      const dt_masks_point_group_t *pt = (dt_masks_point_group_t *)l->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
      tp |= _get_all_types_in_group(f);
    }
    return tp;
  }
  else
  {
    return form->type;
  }
}

GSList *dt_masks_mouse_actions(dt_masks_form_t *form)
{
  dt_masks_type_t formtype = _get_all_types_in_group(form);
  GSList *lm = NULL;

  if(form->functions && form->functions->setup_mouse_actions)
  {
    lm = form->functions->setup_mouse_actions(form);
  }
  // add the common action(s) shared by all shapes
  if(formtype != 0)
  {
    lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_RIGHT, 0,
                                       _("[SHAPE] remove shape"));
  }

  return lm;
}

static void _set_hinter_message(dt_masks_form_gui_t *gui,
                                const dt_masks_form_t *form)
{
  char msg[256] = "";

  const int ftype = form->type;

  int opacity = 100;

  const dt_masks_form_t *sel = form;
  if((ftype & DT_MASKS_GROUP) && (gui->group_edited >= 0))
  {
    // we get the selected form
    const dt_masks_point_group_t *fpt =
      (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return;

    opacity = _get_opacity(gui, form);
  }
  else
  {
    opacity = (int)(dt_conf_get_float("plugins/darkroom/masks/opacity") * 100);
  }

  if(sel->functions && sel->functions->set_hint_message)
  {
    sel->functions->set_hint_message(gui, form, opacity, msg, sizeof(msg));
  }

  dt_control_hinter_message(darktable.control, msg);
}

void dt_masks_init_form_gui(dt_masks_form_gui_t *gui)
{
  memset(gui, 0, sizeof(dt_masks_form_gui_t));

  gui->posx = gui->posy = -1.0f;
  gui->mouse_leaved_center = TRUE;
  gui->posx_source = gui->posy_source = -1.0f;
  gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
}

void dt_masks_gui_form_create(dt_masks_form_t *form,
                              dt_masks_form_gui_t *gui,
                              const int index,
                              dt_iop_module_t *module)
{
  const int npoints = g_list_length(gui->points);

  if(npoints == index)
    gui->points = g_list_append(gui->points, calloc(1, sizeof(dt_masks_form_gui_points_t)));
  else if(npoints > index)
    dt_masks_gui_form_remove(form, gui, index);
  else
    return;

  dt_masks_form_gui_points_t *gpt =
    (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);

  if(dt_masks_get_points_border(darktable.develop, form,
                                &gpt->points, &gpt->points_count,
                                &gpt->border, &gpt->border_count, 0, NULL))
  {
    if(form->type & DT_MASKS_CLONE)
      dt_masks_get_points_border(darktable.develop, form,
                                 &gpt->source, &gpt->source_count, NULL, NULL, 1, module);
    gui->pipe_hash = darktable.develop->preview_pipe->backbuf_hash;
    gui->formid = form->formid;
  }
}

void dt_masks_form_gui_points_free(gpointer data)
{
  if(!data) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)data;

  dt_free_align(gpt->points);
  dt_free_align(gpt->border);
  dt_free_align(gpt->source);
  free(gpt);
}

void dt_masks_gui_form_remove(dt_masks_form_t *form,
                              dt_masks_form_gui_t *gui,
                              const int index)
{
  dt_masks_form_gui_points_t *gpt =
    (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  gui->pipe_hash = gui->formid = 0;

  if(gpt)
  {
    gpt->points_count = gpt->border_count = gpt->source_count = 0;
    dt_free_align(gpt->points);
    gpt->points = NULL;
    dt_free_align(gpt->border);
    gpt->border = NULL;
    dt_free_align(gpt->source);
    gpt->source = NULL;
  }
}

void dt_masks_gui_form_test_create(dt_masks_form_t *form,
                                   dt_masks_form_gui_t *gui,
                                   dt_iop_module_t *module)
{
  // we test if the image has changed
  if(gui->pipe_hash > 0)
  {
    if(gui->pipe_hash != darktable.develop->preview_pipe->backbuf_hash)
    {
      gui->pipe_hash = gui->formid = 0;
      g_list_free_full(gui->points, dt_masks_form_gui_points_free);
      gui->points = NULL;
    }
  }

  // we create the spots if needed
  if(gui->pipe_hash == 0)
  {
    if(form->type & DT_MASKS_GROUP)
    {
      int pos = 0;
      for(GList *fpts = form->points; fpts;  fpts = g_list_next(fpts))
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
        dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
        if(!sel) return;
        dt_masks_gui_form_create(sel, gui, pos, module);
        pos++;
      }
    }
    else
      dt_masks_gui_form_create(form, gui, 0, module);
  }
}

static void _check_id(dt_masks_form_t *form)
{
  int nid = 100;
  for(GList *forms = darktable.develop->forms; forms; )
  {
    dt_masks_form_t *ff = (dt_masks_form_t *)forms->data;
    if(ff->formid == form->formid)
    {
      form->formid = nid++;
      forms = darktable.develop->forms; // jump back to start of list
    }
    else
      forms = g_list_next(forms); // advance to next form
  }
}

static void _set_group_name_from_module(dt_iop_module_t *module,
                                        dt_masks_form_t *grp)
{
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(grp->name, sizeof(grp->name), "grp %s", module_label);
  g_free(module_label);
}

static dt_masks_form_t *_group_create(dt_develop_t *dev,
                                      dt_iop_module_t *module,
                                      const dt_masks_type_t type)
{
  dt_masks_form_t* grp = dt_masks_create(type);
  _set_group_name_from_module(module, grp);
  _check_id(grp);
  dev->forms = g_list_append(dev->forms, grp);
  module->blend_params->mask_id = grp->formid;
  return grp;
}

static dt_masks_form_t *_group_from_module(dt_develop_t *dev,
                                           dt_iop_module_t *module)
{
  return dt_masks_get_from_id(dev, module->blend_params->mask_id);
}

void dt_masks_gui_form_save_creation(dt_develop_t *dev,
                                     dt_iop_module_t *module,
                                     dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui)
{
  // we check if the id is already registered
  _check_id(form);

  if(gui) gui->creation = FALSE;

  // mask nb will be at least the length of the list
  guint nb = 0;

  // count only the same forms to have a clean numbering
  for(GList *l = dev->forms; l; l = g_list_next(l))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)l->data;
    if(f->type == form->type) nb++;
  }

  gboolean exist = FALSE;

  // check that we do not have duplicate, in case some masks have been
  // removed we can have hole and so nb could already exists.
  do
  {
    exist = FALSE;
    nb++;

    if(form->functions && form->functions->set_form_name)
      form->functions->set_form_name(form, nb);

    for(GList *l = dev->forms; l; l = g_list_next(l))
    {
      dt_masks_form_t *f = (dt_masks_form_t *)l->data;
      if(!strcmp(f->name, form->name))
      {
        exist = TRUE;
        break;
      }
    }
  } while(exist);

  dev->forms = g_list_append(dev->forms, form);

  dt_dev_add_masks_history_item(dev, module, TRUE);

  if(module)
  {
    // is there already a masks group for this module ?
    dt_masks_form_t *grp = _group_from_module(dev, module);
    if(!grp)
    {
      // we create a new group
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        grp = _group_create(dev, module, DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        grp = _group_create(dev, module, DT_MASKS_GROUP);
    }
    // we add the form in this group
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grp->formid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(grp->points) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    grp->points = g_list_append(grp->points, grpt);
    // we save the group
    dt_dev_add_masks_history_item(dev, module, TRUE);
    // we update module gui
    if(gui) dt_masks_iop_update(module);
    //dt_dev_add_history_item(dev, module, TRUE);
  }
  // show the form if needed
  if(gui) dev->form_gui->formid = form->formid;
}

int dt_masks_form_duplicate(dt_develop_t *dev, const int formid)
{
  // we create a new empty form
  dt_masks_form_t *fbase = dt_masks_get_from_id(dev, formid);
  if(!fbase) return -1;
  dt_masks_form_t *fdest = dt_masks_create(fbase->type);
  _check_id(fdest);

  // we copy the base values
  fdest->source[0] = fbase->source[0];
  fdest->source[1] = fbase->source[1];
  fdest->version = fbase->version;
  snprintf(fdest->name, sizeof(fdest->name), _("copy of %s"), fbase->name);

  darktable.develop->forms = g_list_append(dev->forms, fdest);

  // we copy all the points
  if(fbase->functions)
    fbase->functions->duplicate_points(dev, fbase, fdest);

  // we save the form
  dt_dev_add_masks_history_item(dev, NULL, TRUE);

  // and we return its id
  return fdest->formid;
}

int dt_masks_get_points_border(dt_develop_t *dev,
                               dt_masks_form_t *form,
                               float **points,
                               int *points_count,
                               float **border,
                               int *border_count,
                               const int source,
                               dt_iop_module_t *module)
{
  if(form->functions && form->functions->get_points_border)
  {
    return form->functions->get_points_border(dev, form, points, points_count,
                                              border, border_count, source,
                                              module);
  }
  return 0;
}

int dt_masks_get_area(dt_iop_module_t *module,
                      dt_dev_pixelpipe_iop_t *piece,
                      dt_masks_form_t *form,
                      int *width,
                      int *height,
                      int *posx,
                      int *posy)
{
  if(form->functions)
    return form->functions->get_area(module, piece, form, width, height, posx, posy);

  return 0;
}

int dt_masks_get_source_area(dt_iop_module_t *module,
                             dt_dev_pixelpipe_iop_t *piece,
                             dt_masks_form_t *form,
                             int *width,
                             int *height,
                             int *posx,
                             int *posy)
{
  *width = *height = *posx = *posy = 0;

  // must be a clone form
  if(form->type & DT_MASKS_CLONE)
  {
    if(form->functions)
      return form->functions->get_source_area(module, piece, form, width, height,
                                              posx, posy);
  }
  return 0;
}

int dt_masks_version(void)
{
  return DEVELOP_MASKS_VERSION;
}

static int dt_masks_legacy_params_v1_to_v2(dt_develop_t *dev, void *params)
{
  /*
   * difference: before v2 images were originally rotated on load, and then
   * maybe in flip iop
   * after v2: images are only rotated in flip iop.
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  const dt_image_orientation_t ori = dt_image_orientation(&dev->image_storage);

  if(ori == ORIENTATION_NONE)
  {
    // image is not rotated, we're fine!
    m->version = 2;
    return 0;
  }
  else
  {
    if(dev->iop == NULL) return 1;

    const char *opname = "flip";
    dt_iop_module_t *module = NULL;

    for(GList *modules = dev->iop; modules; modules = g_list_next(modules))
    {
      dt_iop_module_t *find_op = (dt_iop_module_t *)modules->data;
      if(!strcmp(find_op->op, opname))
      {
        module = find_op;
        break;
      }
    }

    if(module == NULL) return 1;

    dt_dev_pixelpipe_iop_t piece = { 0 };

    module->init_pipe(module, NULL, &piece);
    module->commit_params(module, module->default_params, NULL, &piece);

    piece.buf_in.width = 1;
    piece.buf_in.height = 1;

    GList *p = m->points;

    if(!p) return 1;

    if(m->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)p->data;
      module->distort_backtransform(module, &piece, circle->center, 1);
    }
    else if(m->type & DT_MASKS_PATH)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_point_path_t *path = (dt_masks_point_path_t *)p->data;
        module->distort_backtransform(module, &piece, path->corner, 1);
        module->distort_backtransform(module, &piece, path->ctrl1, 1);
        module->distort_backtransform(module, &piece, path->ctrl2, 1);
      }
    }
    else if(m->type & DT_MASKS_GRADIENT)
    { // TODO: new ones have wrong rotation.
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)p->data;
      module->distort_backtransform(module, &piece, gradient->anchor, 1);

      if(ori == ORIENTATION_ROTATE_180_DEG)
        gradient->rotation -= 180.0f;
      else if(ori == ORIENTATION_ROTATE_CCW_90_DEG)
        gradient->rotation -= 90.0f;
      else if(ori == ORIENTATION_ROTATE_CW_90_DEG)
        gradient->rotation -= -90.0f;
    }
    else if(m->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)p->data;
      module->distort_backtransform(module, &piece, ellipse->center, 1);

      if(ori & ORIENTATION_SWAP_XY)
      {
        const float y = ellipse->radius[0];
        ellipse->radius[0] = ellipse->radius[1];
        ellipse->radius[1] = y;
      }
    }
    else if(m->type & DT_MASKS_BRUSH)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_point_brush_t *brush = (dt_masks_point_brush_t *)p->data;
        module->distort_backtransform(module, &piece, brush->corner, 1);
        module->distort_backtransform(module, &piece, brush->ctrl1, 1);
        module->distort_backtransform(module, &piece, brush->ctrl2, 1);
      }
    }

    if(m->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_PATH
      module->distort_backtransform(module, &piece, m->source, 1);
    }

    m->version = 2;

    return 0;
  }
}

static void dt_masks_legacy_params_v2_to_v3_transform(const dt_image_t *img,
                                                      float *points)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cx = (float)img->crop_x, cy = (float)img->crop_y;

  const float cw = (float)(img->width - img->crop_x - img->crop_right);
  const float ch = (float)(img->height - img->crop_y - img->crop_bottom);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by image original cropped dimensions
   * 2. un-crop them by adding top-left crop coordinates
   * 3. normalize them by the image fully uncropped dimensions
   */
  points[0] = ((points[0] * cw) + cx) / w;
  points[1] = ((points[1] * ch) + cy) / h;
}

static void dt_masks_legacy_params_v2_to_v3_transform_only_rescale
  (const dt_image_t *img,
   float *points,
   const size_t points_count)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cw = (float)(img->width - img->crop_x - img->crop_right);
  const float ch = (float)(img->height - img->crop_y - img->crop_bottom);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by minimal of image original cropped dimensions
   * 2. normalize them by the minimal of image fully uncropped dimensions
   */
  for(size_t i = 0; i < points_count; i++)
    points[i] = ((points[i] * MIN(cw, ch))) / MIN(w, h);
}

static int dt_masks_legacy_params_v2_to_v3(dt_develop_t *dev, void *params)
{
  /*
   * difference: before v3 images were originally cropped on load
   * after v3: images are cropped in rawprepare iop.
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  const dt_image_t *img = &(dev->image_storage);

  if(img->crop_x == 0
     && img->crop_y == 0
     && img->crop_right == 0
     && img->crop_bottom == 0)
  {
    // image has no "raw cropping", we're fine!
    m->version = 3;
    return 0;
  }
  else
  {
    GList *p = m->points;

    if(!p) return 1;

    if(m->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)p->data;
      dt_masks_legacy_params_v2_to_v3_transform(img, circle->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &circle->radius, 1);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &circle->border, 1);
    }
    else if(m->type & DT_MASKS_PATH)
    {
      for(; p; p = g_list_next(p))
      {
        dt_masks_point_path_t *path = (dt_masks_point_path_t *)p->data;
        dt_masks_legacy_params_v2_to_v3_transform(img, path->corner);
        dt_masks_legacy_params_v2_to_v3_transform(img, path->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, path->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, path->border, 2);
      }
    }
    else if(m->type & DT_MASKS_GRADIENT)
    {
      dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)p->data;
      dt_masks_legacy_params_v2_to_v3_transform(img, gradient->anchor);
    }
    else if(m->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)p->data;
      dt_masks_legacy_params_v2_to_v3_transform(img, ellipse->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, ellipse->radius, 2);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, &ellipse->border, 1);
    }
    else if(m->type & DT_MASKS_BRUSH)
    {
      for(; p;  p = g_list_next(p))
      {
        dt_masks_point_brush_t *brush = (dt_masks_point_brush_t *)p->data;
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->corner);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, brush->border, 2);
      }
    }

    if(m->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_PATH
      dt_masks_legacy_params_v2_to_v3_transform(img, m->source);
    }

    m->version = 3;

    return 0;
  }
}

static int dt_masks_legacy_params_v3_to_v4(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting ellipse
   * up to v3: only equidistant feathering
   * after v4: choice between equidistant and proportional feathering
   * type of feathering is defined in new flags parameter
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)p->data;
    ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  }

  m->version = 4;

  return 0;
}


static int dt_masks_legacy_params_v4_to_v5(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting gradient
   * up to v4: only linear gradient (relative to input image)
   * after v5: curved gradients
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_GRADIENT)
  {
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)p->data;
    gradient->curvature = 0.0f;
  }

  m->version = 5;

  return 0;
}

static int dt_masks_legacy_params_v5_to_v6(dt_develop_t *dev, void *params)
{
  /*
   * difference affecting gradient
   * up to v5: linear transition
   * after v5: linear or sigmoidal transition
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  GList *p = m->points;

  if(!p) return 1;

  if(m->type & DT_MASKS_GRADIENT)
  {
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)p->data;
    gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;
  }

  m->version = 6;

  return 0;
}


int dt_masks_legacy_params(dt_develop_t *dev,
                           void *params,
                           const int old_version,
                           const int new_version)
{
  int res = 1;
#if 0 // we should not need this any longer
  if(old_version == 1 && new_version == 2)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
  }
#endif

  if(old_version == 1 && new_version == 6)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
    if(!res) res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 2 && new_version == 6)
  {
    res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 3 && new_version == 6)
  {
    res = dt_masks_legacy_params_v3_to_v4(dev, params);
    if(!res) res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 4 && new_version == 6)
  {
    res = dt_masks_legacy_params_v4_to_v5(dev, params);
    if(!res) res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }
  else if(old_version == 5 && new_version == 6)
  {
    res = dt_masks_legacy_params_v5_to_v6(dev, params);
  }

  return res;
}

static int form_id = 0;

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *form = (dt_masks_form_t *)calloc(1, sizeof(dt_masks_form_t));
  if(!form) return NULL;

  form->type = type;
  form->version = dt_masks_version();
  form->formid = time(NULL) + form_id++;

  if(type & DT_MASKS_CIRCLE)
    form->functions = &dt_masks_functions_circle;
  else if(type & DT_MASKS_ELLIPSE)
    form->functions = &dt_masks_functions_ellipse;
  else if(type & DT_MASKS_BRUSH)
    form->functions = &dt_masks_functions_brush;
  else if(type & DT_MASKS_PATH)
    form->functions = &dt_masks_functions_path;
  else if(type & DT_MASKS_GRADIENT)
    form->functions = &dt_masks_functions_gradient;
  else if(type & DT_MASKS_GROUP)
    form->functions = &dt_masks_functions_group;

  if(form->functions && form->functions->sanitize_config)
    form->functions->sanitize_config(type);

  return form;
}

dt_masks_form_t *dt_masks_create_ext(dt_masks_type_t type)
{
  dt_masks_form_t *form = dt_masks_create(type);

  // all forms created here are registered in
  // darktable.develop->allforms for later cleanup
  if(form)
    darktable.develop->allforms = g_list_append(darktable.develop->allforms, form);

  return form;
}

void dt_masks_replace_current_forms(dt_develop_t *dev, GList *forms)
{
  GList *forms_tmp = dt_masks_dup_forms_deep(forms, NULL);

  while(dev->forms)
  {
    darktable.develop->allforms =
      g_list_append(darktable.develop->allforms, dev->forms->data);
    dev->forms = g_list_delete_link(dev->forms, dev->forms);
  }

  dev->forms = forms_tmp;
}

dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, int id)
{
  for(; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->formid == id) return form;
  }
  return NULL;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, const int id)
{
  return dt_masks_get_from_id_ext(dev->forms, id);
}

void dt_masks_read_masks_history(dt_develop_t *dev, const int imgid)
{
  dt_dev_history_item_t *hist_item = NULL;
  dt_dev_history_item_t *hist_item_last = NULL;
  int num_prev = -1;

  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid, formid, form, name, version, points, points_count, source, num "
      "FROM main.masks_history WHERE imgid = ?1 ORDER BY num",
      -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record: 0-img, 1-formid, 2-form_type, 3-name, 4-version,
    // 5-points, 6-points_count, 7-source, 8-num

    // we get the values

    const int formid = sqlite3_column_int(stmt, 1);
    const int num = sqlite3_column_int(stmt, 8);
    const dt_masks_type_t type = sqlite3_column_int(stmt, 2);
    dt_masks_form_t *form = dt_masks_create(type);
    form->formid = formid;
    const char *name = (const char *)sqlite3_column_text(stmt, 3);
    g_strlcpy(form->name, name, sizeof(form->name));
    form->version = sqlite3_column_int(stmt, 4);
    form->points = NULL;
    const int nb_points = sqlite3_column_int(stmt, 6);
    memcpy(form->source, sqlite3_column_blob(stmt, 7), sizeof(float) * 2);

    // and now we "read" the blob
    if(form->functions)
    {
      const char *const ptbuf = (char *)sqlite3_column_blob(stmt, 5);
      const size_t point_size = form->functions->point_struct_size;
      for(int i = 0; i < nb_points; i++)
      {
        char *point = (char *)malloc(point_size);
        memcpy(point, ptbuf + i*point_size, point_size);
        form->points = g_list_append(form->points, point);
      }
    }

    if(form->version != dt_masks_version())
    {
      if(dt_masks_legacy_params(dev, form, form->version, dt_masks_version()))
      {
        const char *fname =
          dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;
        if(fname > dev->image_storage.filename) fname++;

        dt_print(DT_DEBUG_ALWAYS,
                 "[_dev_read_masks_history] %s (imgid `%i'):"
                 " mask version mismatch: history is %d, darktable is %d.\n",
                 fname, imgid, form->version, dt_masks_version());
        dt_control_log(_("%s: mask version mismatch: %d != %d"),
                       fname, dt_masks_version(), form->version);

        continue;
      }
    }

    // if this is a new history entry let's find it
    if(num_prev != num)
    {
      hist_item = NULL;
      for(GList *history = dev->history; history; history = g_list_next(history))
      {
        dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)(history->data);
        if(hitem->num == num)
        {
          hist_item = hitem;
          break;
        }
      }
      num_prev = num;
    }
    // add the form to the history entry
    if(hist_item)
    {
      hist_item->forms = g_list_append(hist_item->forms, form);
    }
    else
      dt_print(DT_DEBUG_ALWAYS,
               "[_dev_read_masks_history] can't find history entry %i"
               " while adding mask %s(%i)\n",
               num, form->name, formid);

    if(num < dev->history_end) hist_item_last = hist_item;
  }
  sqlite3_finalize(stmt);

  // and we update the current forms snapshot
  dt_masks_replace_current_forms(dev, (hist_item_last)?hist_item_last->forms:NULL);
}

void dt_masks_write_masks_history_item(const int imgid,
                                       const int num,
                                       dt_masks_form_t *form)
{
  sqlite3_stmt *stmt;

  // write the form into the database
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "INSERT INTO main.masks_history (imgid, num, formid, form, name, "
     "                                version, points, points_count,source)"
     " VALUES (?1, ?9, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
     -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 9, num);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form->type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, form->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, form->source, 2 * sizeof(float), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, form->version);
  if(form->functions)
  {
    const size_t point_size = form->functions->point_struct_size;
    const guint nb = g_list_length(form->points);
    char *const restrict ptbuf = (char *)malloc(nb * point_size);
    int pos = 0;
    for(GList *points = form->points; points; points = g_list_next(points))
    {
      memcpy(ptbuf + pos, points->data, point_size);
      pos += point_size;
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb * point_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(ptbuf);
  }
}

void dt_masks_free_form(dt_masks_form_t *form)
{
  if(!form) return;
  g_list_free_full(form->points, free);
  form->points = NULL;
  free(form);
}

int dt_masks_events_mouse_leave(struct dt_iop_module_t *module)
{
  if(darktable.develop->form_gui)
  {
    dt_masks_form_gui_t *gui = darktable.develop->form_gui;
    gui->mouse_leaved_center = TRUE;
    dt_control_hinter_message(darktable.control, "");
  }
  return 0;
}

int dt_masks_events_mouse_enter(struct dt_iop_module_t *module)
{
  if(darktable.develop->form_gui)
  {
    dt_masks_form_gui_t *gui = darktable.develop->form_gui;
    gui->mouse_leaved_center = FALSE;
  }
  return 0;
}

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module,
                                const double x,
                                const double y,
                                const double pressure,
                                const int which)
{
  // record mouse position even if there are no masks visible
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  dt_masks_form_t *form = darktable.develop->form_visible;
  float pzx = 0.0f, pzy = 0.0f;

  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(gui)
  {
    // This assume that if this event is generated the mouse is over
    // the center window
    gui->mouse_leaved_center = FALSE;
    gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
  }

  int rep = 0;
  if(form->functions)
    rep = form->functions->mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);

  if(gui) _set_hinter_message(gui, form);

  return rep;
}

int dt_masks_events_button_released(struct dt_iop_module_t *module,
                                    const double x,
                                    const double y,
                                    const int which,
                                    const uint32_t state)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  ++darktable.gui->reset;
  if(dev->mask_form_selected_id)
    dt_dev_masks_selection_change(dev, module, dev->mask_form_selected_id);
  --darktable.gui->reset;

  if(form->functions)
  {
    const int ret =
      form->functions->button_released(module, pzx, pzy, which, state, form, 0, gui, 0);
    form->functions->mouse_moved(module, pzx, pzy, 0, which, form, 0, gui, 0);
    return ret;
  }

  return 0;
}

int dt_masks_events_button_pressed(struct dt_iop_module_t *module,
                                   const double x,
                                   const double y,
                                   const double pressure,
                                   const int which,
                                   const int type,
                                   const uint32_t state)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  // allow to select a shape inside an iop
  if(gui && which == 1)
  {
    dt_masks_form_t *sel = NULL;

    if((gui->form_selected
        || gui->source_selected
        || gui->point_selected
        || gui->seg_selected
        || gui->feather_selected)
       && !gui->creation && gui->group_edited >= 0)
    {
      // we get the selected form
      dt_masks_point_group_t *fpt =
        (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
      if(fpt)
      {
        sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
      }
    }

    dt_masks_select_form(module, sel);
  }

  if(form->functions)
    return form->functions->button_pressed(module, pzx, pzy, pressure,
                                           which, type, state, form, 0, gui, 0)
      || which == 3; // swallow right-clicks even when not handled so
                     // right-drag rotate is disabled when forms
                     // visible
  return 0;
}

int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module,
                                   const double x,
                                   const double y,
                                   const int up,
                                   const uint32_t state)
{
  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  int ret = 0;
  const gboolean incr = dt_mask_scroll_increases(up);

  if(form->functions)
    ret = form->functions->mouse_scrolled(module, pzx, pzy,
                                          incr ? 1 : 0,
                                          state, form, 0, gui, 0);

  if(gui)
  {
    // for brush, the opacity is the density of the masks, do not
    // update opacity here for the brush.
    if(gui->creation && dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      float opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
      const float amount = incr ? 0.05f : -0.05f;

      opacity = CLAMP(opacity + amount, 0.05f, 1.0f);
      dt_conf_set_float("plugins/darkroom/masks/opacity", opacity);
      const int opacitypercent = opacity * 100;
      dt_toast_log(_("opacity: %d%%"), opacitypercent);
      ret = 1;
    }

    _set_hinter_message(gui, form);
  }

  return ret;
}

void dt_masks_events_post_expose(struct dt_iop_module_t *module,
                                 cairo_t *cr,
                                 const int32_t width,
                                 const int32_t height,
                                 const int32_t pointerx,
                                 const int32_t pointery)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(!gui) return;
  if(!form) return;

  const float wd = dev->preview_pipe->backbuf_width;
  const float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_x = dt_control_get_dev_zoom_x();
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  cairo_save(cr);
  cairo_set_source_rgb(cr, .3, .3, .3);

  cairo_translate(cr, width / 2.0, height / 2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // we update the form if needed
  // add preview when creating a circle, ellipse and gradient
  if(!(((form->type & DT_MASKS_CIRCLE)
        || (form->type & DT_MASKS_ELLIPSE)
        || (form->type & DT_MASKS_GRADIENT))
       && gui->creation))
    dt_masks_gui_form_test_create(form, gui, module);

  // draw form
  if(form->type & DT_MASKS_GROUP)
    dt_group_events_post_expose(cr, zoom_scale, form, gui);
  else if(form->functions)
    form->functions->post_expose(cr, zoom_scale, gui, 0, g_list_length(form->points));

  cairo_restore(cr);
}

void dt_masks_clear_form_gui(dt_develop_t *dev)
{
  if(!dev->form_gui) return;
  g_list_free_full(dev->form_gui->points, dt_masks_form_gui_points_free);
  dev->form_gui->points = NULL;
  dt_masks_dynbuf_free(dev->form_gui->guipoints);
  dev->form_gui->guipoints = NULL;
  dt_masks_dynbuf_free(dev->form_gui->guipoints_payload);
  dev->form_gui->guipoints_payload = NULL;
  dev->form_gui->guipoints_count = 0;
  dev->form_gui->pipe_hash = dev->form_gui->formid = 0;
  dev->form_gui->dx = dev->form_gui->dy = 0.0f;
  dev->form_gui->scrollx = dev->form_gui->scrolly = 0.0f;
  dev->form_gui->form_selected = dev->form_gui->border_selected =
    dev->form_gui->form_dragging = dev->form_gui->form_rotating =
    dev->form_gui->border_toggling = dev->form_gui->gradient_toggling = FALSE;
  dev->form_gui->source_selected = dev->form_gui->source_dragging = FALSE;
  dev->form_gui->pivot_selected = FALSE;
  dev->form_gui->point_border_selected = dev->form_gui->seg_selected =
    dev->form_gui->point_selected = dev->form_gui->feather_selected = -1;
  dev->form_gui->point_border_dragging = dev->form_gui->seg_dragging =
    dev->form_gui->feather_dragging = dev->form_gui->point_dragging = -1;
  dev->form_gui->creation_closing_form = dev->form_gui->creation = FALSE;
  dev->form_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  dev->form_gui->creation_module = NULL;
  dev->form_gui->point_edited = -1;

  dev->form_gui->group_edited = -1;
  dev->form_gui->group_selected = -1;
  dev->form_gui->edit_mode = DT_MASKS_EDIT_OFF;
  // allow to select a shape inside an iop
  dt_masks_select_form(NULL, NULL);
}

void dt_masks_change_form_gui(dt_masks_form_t *newform)
{
  const dt_masks_form_t *old = darktable.develop->form_visible;

  dt_masks_clear_form_gui(darktable.develop);
  darktable.develop->form_visible = newform;

  /* update sticky accels window */
  if(newform != old
     && darktable.view_manager->accels_window.window
     && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  if(newform && newform->type != DT_MASKS_GROUP)
    darktable.develop->form_gui->creation = TRUE;

  ++darktable.gui->reset;
  dt_dev_masks_selection_change(darktable.develop, NULL, 0);
  --darktable.gui->reset;
}

void dt_masks_reset_form_gui(void)
{
  dt_masks_change_form_gui(NULL);
  const dt_iop_module_t *m = darktable.develop->gui_module;
  if(m
     && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
     && !(m->flags() & IOP_FLAGS_NO_MASKS)
     && m->blend_data)
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), 0);
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), 0);
  }
}

void dt_masks_reset_show_masks_icons(void)
{
  if(darktable.develop->first_load) return;
  for(GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
  {
    const dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if(m
       && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
       && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
      if(!bd) break;  // TODO: this doesn't look right. Why do we
                      // break the while look as soon as one module
                      // has no blend_data?
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
      gtk_widget_queue_draw(bd->masks_edit);
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), 0);
        gtk_widget_queue_draw(bd->masks_shapes[n]);
      }
    }
  }
}

dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module)
{
  return darktable.develop->form_gui
    ? darktable.develop->form_gui->edit_mode
    : DT_MASKS_EDIT_OFF;
}

void dt_masks_set_edit_mode(struct dt_iop_module_t *module,
                            dt_masks_edit_mode_t value)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!bd) return;

  dt_masks_form_t *grp = NULL;
  dt_masks_form_t *form =
    dt_masks_get_from_id(module->dev, module->blend_params->mask_id);

  if(value && form)
  {
    grp = dt_masks_create_ext(DT_MASKS_GROUP);
    grp->formid = 0;
    dt_masks_group_ungroup(grp, form);
  }

  if(bd) bd->masks_shown = value;

  dt_masks_change_form_gui(grp);
  darktable.develop->form_gui->edit_mode = value;

  ++darktable.gui->reset;
  dt_dev_masks_selection_change(darktable.develop, NULL, value && form ? form->formid : 0);
  --darktable.gui->reset;

  if(bd->masks_support)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit),
                                 value == DT_MASKS_EDIT_OFF ? FALSE : TRUE);

  dt_control_queue_redraw_center();
}

void dt_masks_set_edit_mode_single_form(struct dt_iop_module_t *module,
                                        const int formid,
                                        const dt_masks_edit_mode_t value)
{
  if(!module) return;

  dt_masks_form_t *grp = dt_masks_create_ext(DT_MASKS_GROUP);

  const int grid = module->blend_params->mask_id;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(form)
  {
    dt_masks_point_group_t *fpt =
      (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
    fpt->formid = formid;
    fpt->parentid = grid;
    fpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    fpt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, fpt);
  }

  dt_masks_form_t *grp2 = dt_masks_create_ext(DT_MASKS_GROUP);
  grp2->formid = 0;
  dt_masks_group_ungroup(grp2, grp);
  dt_masks_change_form_gui(grp2);
  darktable.develop->form_gui->edit_mode = value;

  ++darktable.gui->reset;
  dt_dev_masks_selection_change(darktable.develop, NULL, value && form ? formid : 0);
  --darktable.gui->reset;

  dt_control_queue_redraw_center();
}

void dt_masks_iop_edit_toggle_callback(GtkToggleButton *togglebutton,
                                       dt_iop_module_t *module)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(module->blend_params->mask_id == 0)
  {
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    return;
  }

  // reset the gui
  dt_masks_set_edit_mode(module,
                         (bd->masks_shown == DT_MASKS_EDIT_OFF
                          ? DT_MASKS_EDIT_FULL
                          : DT_MASKS_EDIT_OFF));
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  // we drop all the forms in the iop
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(grp) dt_masks_form_remove(module, NULL, grp);

  module->blend_params->mask_id = 0;

  // and we update the iop
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  dt_masks_iop_update(module);

  dt_dev_add_history_item(darktable.develop, module, TRUE);
}

static void _menu_add_shape(struct dt_iop_module_t *module,
                            const dt_masks_type_t type)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(type);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_exist(dt_iop_module_t *module,
                            const int formid)
{
  if(!module) return;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(!form) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(!grp)
  {
    grp = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we add the form in this group
  dt_masks_group_add_form(grp, form);
  // we save the group
  // and we ensure that we are in edit mode
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_iop_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

void dt_masks_group_update_name(dt_iop_module_t *module)
{
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(!grp)
    return;

  _set_group_name_from_module(module, grp);
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
  dt_masks_iop_update(module);
}

void dt_masks_iop_use_same_as(dt_iop_module_t *module,
                              dt_iop_module_t *src)
{
  if(!module || !src) return;

  // we get the source group
  int srcid = src->blend_params->mask_id;
  dt_masks_form_t *src_grp = dt_masks_get_from_id(darktable.develop, srcid);
  if(!src_grp || src_grp->type != DT_MASKS_GROUP) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
  if(!grp)
  {
    grp = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we copy the src group in this group
  for(GList *points = src_grp->points; points; points = g_list_next(points))
  {
    const dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, pt->formid);
    if(form)
    {
      dt_masks_point_group_t *grpt = dt_masks_group_add_form(grp, form);
      if(grpt)
      {
        grpt->state = pt->state;
        grpt->opacity = pt->opacity;
      }
    }
  }

  // we save the group
  dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
}

void dt_masks_iop_combo_populate(GtkWidget *w, struct dt_iop_module_t **m)
{
  // we ensure that the module has focus
  dt_iop_module_t *module = *m;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  // we determine a higher approx of the entry number
  const guint nbe = 5
    + g_list_length(darktable.develop->forms)
    + g_list_length(darktable.develop->iop);

  free(bd->masks_combo_ids);
  bd->masks_combo_ids = malloc(sizeof(int) * nbe);

  int *cids = bd->masks_combo_ids;
  GtkWidget *combo = bd->masks_combo;

  // we remove all the combo entries except the first one
  while(dt_bauhaus_combobox_length(combo) > 1)
  {
    dt_bauhaus_combobox_remove_at(combo, 1);
  }

  int pos = 0;
  cids[pos++] = 0; // nothing to do for the first entry (already here)


  // add existing shapes
  int nb = 0;
  for(GList *forms = darktable.develop->forms;
      forms;
      forms = g_list_next(forms))
  {
    const dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
       || form->formid == module->blend_params->mask_id)
    {
      continue;
    }

    // we search were this form is used in the current module
    int used = 0;
    dt_masks_form_t *grp = _group_from_module(darktable.develop, module);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      for(GList *pts = grp->points; pts; pts = g_list_next(pts))
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
        if(pt->formid == form->formid)
        {
          used = 1;
          break;
        }
      }
    }
    if(!used)
    {
      if(nb == 0)
      {
        dt_bauhaus_combobox_add_section(combo, _("add existing shape"));
        cids[pos++] = 0; // nothing to do
      }
      dt_bauhaus_combobox_add(combo, form->name);
      cids[pos++] = form->formid;
      nb++;
    }
  }

  // masks from other iops
  nb = 0;
  int pos2 = 1;
  for(GList *modules = darktable.develop->iop;
      modules;
      modules = g_list_next(modules))
  {
    dt_iop_module_t *other_mod = (dt_iop_module_t *)modules->data;

    if((other_mod != module)
       && (other_mod->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
       && !(other_mod->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *grp = _group_from_module(darktable.develop, other_mod);
      if(grp)
      {
        if(nb == 0)
        {
          dt_bauhaus_combobox_add_section(combo, _("use same shapes as"));
          cids[pos++] = 0; // nothing to do
        }
        gchar *module_label = dt_history_item_get_name(other_mod);
        dt_bauhaus_combobox_add(combo, module_label);
        g_free(module_label);
        cids[pos++] = -1 * pos2;
        nb++;
      }
    }
    pos2++;
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget,
                                         struct dt_iop_module_t *module)
{
  // we get the corresponding value
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  const int sel = dt_bauhaus_combobox_get(bd->masks_combo);
  if(sel == 0) return;
  if(sel == 1)
  {
    ++darktable.gui->reset;
    dt_bauhaus_combobox_set(bd->masks_combo, 0);
    --darktable.gui->reset;
    return;
  }
  if(sel > 0)
  {
    int val = bd->masks_combo_ids[sel];
    if(val == -1000000)
    {
      // delete all masks
      _menu_no_masks(module);
    }
    else if(val == -2000001)
    {
      // add a circle shape
      _menu_add_shape(module, DT_MASKS_CIRCLE);
    }
    else if(val == -2000002)
    {
      // add a path shape
      _menu_add_shape(module, DT_MASKS_PATH);
    }
    else if(val == -2000016)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_GRADIENT);
    }
    else if(val == -2000032)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_ELLIPSE);
    }
    else if(val == -2000064)
    {
      // add a brush shape
      _menu_add_shape(module, DT_MASKS_BRUSH);
    }
    else if(val < 0)
    {
      // use same shapes as another iop
      val = -1 * val - 1;
      if(val < g_list_length(module->dev->iop))
      {
        dt_iop_module_t *m = (dt_iop_module_t *)g_list_nth_data(module->dev->iop, val);
        dt_masks_iop_use_same_as(module, m);
        // and we ensure that we are in edit mode
        //dt_dev_add_history_item(darktable.develop, module, TRUE);
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
        dt_masks_iop_update(module);
        dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      }
    }
    else if(val > 0)
    {
      // add an existing shape
      _menu_add_exist(module, val);
    }
    else
      return;
  }
  // we update the combo line
  dt_masks_iop_update(module);
}

void dt_masks_iop_update(struct dt_iop_module_t *module)
{
  if(!module) return;

  dt_iop_gui_update(module);
  dt_iop_gui_update_masks(module);
}

void dt_masks_form_remove(struct dt_iop_module_t *module,
                          dt_masks_form_t *grp,
                          dt_masks_form_t *form)
{
  if(!form) return;
  const int id = form->formid;
  if(grp && !(grp->type & DT_MASKS_GROUP)) return;

  if(!(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
     && grp)
  {
    // we try to remove the form from the masks group
    gboolean ok = FALSE;
    for(GList *forms = grp->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt->formid == id)
      {
        ok = TRUE;
        grp->points = g_list_remove(grp->points, grpt);
        free(grpt);
        break;
      }
    }
    if(ok) dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    if(ok && module)
    {
      dt_masks_iop_update(module);
      dt_masks_update_image(darktable.develop);
    }
    if(ok && grp->points == NULL) dt_masks_form_remove(module, NULL, grp);
    return;
  }

  if(form->type & DT_MASKS_GROUP && form->type & DT_MASKS_CLONE)
  {
    // when removing a cloning group the children have to be removed,
    // too, as they won't be shown in the mask manager and are thus
    // not accessible afterwards.
    while(form->points)
    {
      dt_masks_point_group_t *group_child = (dt_masks_point_group_t *)form->points->data;
      dt_masks_form_t *child =
        dt_masks_get_from_id(darktable.develop, group_child->formid);
      dt_masks_form_remove(module, form, child);
      // no need to do anything to form->points, the recursive call
      // will have removed child from the list
    }
  }

  // if we are here that mean we have to permanently delete this form
  // we drop the form from all modules
  int form_removed = 0;
  for(GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
    if(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      // is the form the base group of the iop ?
      if(id == m->blend_params->mask_id)
      {
        m->blend_params->mask_id = 0;
        dt_masks_iop_update(m);
        dt_dev_add_history_item(darktable.develop, m, TRUE);
      }
      else
      {
        dt_masks_form_t *iopgrp = _group_from_module(darktable.develop, m);
        if(iopgrp && (iopgrp->type & DT_MASKS_GROUP))
        {
          gboolean ok = FALSE;
          GList *forms = iopgrp->points;
          while(forms)
          {
            dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
            if(grpt->formid == id)
            {
              ok = TRUE;
              iopgrp->points = g_list_remove(iopgrp->points, grpt);
              free(grpt);
              forms = iopgrp->points; // jump back to start of list
              continue;
            }
            forms = g_list_next(forms); // advance to next form
          }
          if(ok)
          {
            form_removed = 1;
            dt_masks_iop_update(m);
            dt_masks_update_image(darktable.develop);
            if(iopgrp->points == NULL) dt_masks_form_remove(m, NULL, iopgrp);
          }
        }
      }
    }
  }
  // we drop the form from the general list
  for(GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *f = (dt_masks_form_t *)forms->data;
    if(f->formid == id)
    {
      darktable.develop->forms = g_list_remove(darktable.develop->forms, f);
      form_removed = 1;
      break;
    }
  }
  if(form_removed) dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
}

float dt_masks_form_change_opacity(dt_masks_form_t *form,
                                   const int parentid,
                                   const float amount)
{
  if(!form) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return 0;

  // we first need to test if the opacity can be set to the form
  if(form->type & DT_MASKS_GROUP) return 0;
  const int id = form->formid;

  // so we change the value inside the group
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    if(fpt->formid == id)
    {
      const float opacity = CLAMP(fpt->opacity + amount, 0.05f, 1.0f);
      if(opacity != fpt->opacity)
      {
        fpt->opacity = opacity;
        const int opacitypercent = opacity * 100;
        dt_toast_log(_("opacity: %d%%"), opacitypercent);
        dt_dev_add_masks_history_item(darktable.develop, NULL, TRUE);
        dt_masks_update_image(darktable.develop);
      }
      return opacity;
    }
  }
  return 0;
}

void dt_masks_form_move(dt_masks_form_t *grp,
                        const int formid,
                        const int up)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  // we search the form in the group
  dt_masks_point_group_t *grpt = NULL;
  guint pos = 0;
  for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    if(fpt->formid == formid)
    {
      grpt = fpt;
      break;
    }
    pos++;
  }

  // we remove the form and read it
  if(grpt)
  {
    if(!up && pos == 0) return;
    if(up && pos == g_list_length(grp->points) - 1) return;

    grp->points = g_list_remove(grp->points, grpt);
    if(!up)
      pos -= 1;
    else
      pos += 1;
    grp->points = g_list_insert(grp->points, grpt, pos);
  }
}

static int _find_in_group(dt_masks_form_t *grp,
                          const int formid)
{
  if(!(grp->type & DT_MASKS_GROUP)) return 0;
  if(grp->formid == formid) return 1;

  int nb = 0;
  for(GList *forms = grp->points; forms; forms = g_list_next(forms))
  {
    const dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
    if(form)
    {
      if(form->type & DT_MASKS_GROUP) nb += _find_in_group(form, formid);
    }
  }
  return nb;
}

dt_masks_point_group_t *dt_masks_group_add_form(dt_masks_form_t *grp,
                                                dt_masks_form_t *form)
{
  // add a form to group and check for self inclusion

  if(!(grp->type & DT_MASKS_GROUP)) return NULL;
  // either the form to add is not a group, so no risk
  // or we go through all points of form to see if we find a ref to grp->formid
  if(!(form->type & DT_MASKS_GROUP) || _find_in_group(form, grp->formid) == 0)
  {
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grp->formid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(grp->points) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    grp->points = g_list_append(grp->points, grpt);
    return grpt;
  }

  dt_control_log(_("masks can not contain themselves"));
  return NULL;
}

void dt_masks_group_ungroup(dt_masks_form_t *dest_grp,
                            dt_masks_form_t *grp)
{
  if(!grp || !dest_grp) return;
  if(!(grp->type & DT_MASKS_GROUP)
     || !(dest_grp->type & DT_MASKS_GROUP))
    return;

  for(GList *forms = grp->points; forms; forms = g_list_next(forms))
  {
    dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
    if(form)
    {
      if(form->type & DT_MASKS_GROUP)
      {
        dt_masks_group_ungroup(dest_grp, form);
      }
      else
      {
        dt_masks_point_group_t *fpt =
          (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = grpt->formid;
        fpt->parentid = grpt->parentid;
        fpt->state = grpt->state;
        fpt->opacity = grpt->opacity;
        dest_grp->points = g_list_append(dest_grp->points, fpt);
      }
    }
  }
}

int dt_masks_group_get_hash_buffer_length(dt_masks_form_t *form)
{
  if(!form) return 0;
  int pos = 0;
  // basic infos
  pos += sizeof(dt_masks_type_t);
  pos += sizeof(int);
  pos += sizeof(int);
  pos += 2 * sizeof(float);

  for(GList *forms = form->points;
      forms;
      forms = g_list_next(forms))
  {
    if(form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
      {
        // state & opacity
        pos += sizeof(int);
        pos += sizeof(float);
        // the form itself
        pos += dt_masks_group_get_hash_buffer_length(f);
      }
    }
    else if(form->functions)
    {
      pos += form->functions->point_struct_size;
    }
  }
  return pos;
}

char *dt_masks_group_get_hash_buffer(dt_masks_form_t *form, char *str)
{
  if(!form) return str;
  int pos = 0;
  // basic infos
  memcpy(str + pos, &form->type, sizeof(dt_masks_type_t));
  pos += sizeof(dt_masks_type_t);
  memcpy(str + pos, &form->formid, sizeof(int));
  pos += sizeof(int);
  memcpy(str + pos, &form->version, sizeof(int));
  pos += sizeof(int);
  memcpy(str + pos, &form->source, sizeof(float) * 2);
  pos += 2 * sizeof(float);

  for(const GList *forms = form->points; forms; forms = g_list_next(forms))
  {
    if(form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
      {
        // state & opacity
        memcpy(str + pos, &grpt->state, sizeof(int));
        pos += sizeof(int);
        memcpy(str + pos, &grpt->opacity, sizeof(float));
        pos += sizeof(float);
        // the form itself
        str = dt_masks_group_get_hash_buffer(f, str + pos) - pos;
      }
    }
    else if(form->functions)
    {
      memcpy(str + pos,forms->data, form->functions->point_struct_size);
      pos += form->functions->point_struct_size;
    }
  }
  return str + pos;
}

void dt_masks_update_image(dt_develop_t *dev)
{
  /* invalidate image data*/
  // dt_similarity_image_dirty(dev->image_storage.id);

  // invalidate buffers and force redraw of darkroom
  dev->pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dev->preview2_pipe->changed |= DT_DEV_PIPE_SYNCH;
  dt_dev_invalidate_all(dev);
}

// adds formid to used array
// if formid is a group it adds all the forms that belongs to that group
static void _cleanup_unused_recurs(GList *forms,
                                   const int formid,
                                   int *used,
                                   const int nb)
{
  // first, we search for the formid in used table
  for(int i = 0; i < nb; i++)
  {
    if(used[i] == 0)
    {
      // we store the formid
      used[i] = formid;
      break;
    }
    if(used[i] == formid) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *form = dt_masks_get_from_id_ext(forms, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    for(GList *grpts = form->points; grpts; grpts = g_list_next(grpts))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)grpts->data;
      _cleanup_unused_recurs(forms, grpt->formid, used, nb);
    }
  }
}

// removes from _forms all forms that are not used in history_list up to history_end
static int _masks_cleanup_unused(GList **_forms,
                                 GList *history_list,
                                 const int history_end)
{
  int masks_removed = 0;
  GList *forms = *_forms;

  // we create a table to store the ids of used forms
  const guint nbf = g_list_length(forms);
  int *used = calloc(nbf, sizeof(int));

  // check in history if the module has drawn masks and add it to used
  // array
  int num = 0;
  for(GList *history = history_list;
      history && num < history_end;
      history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    dt_develop_blend_params_t *blend_params = hist->blend_params;
    if(blend_params)
    {
      if(blend_params->mask_id > 0)
        _cleanup_unused_recurs(forms, blend_params->mask_id, used, nbf);
    }
    num++;
  }

  // and we delete all unused forms
  GList *shapes = forms;
  while(shapes)
  {
    dt_masks_form_t *f = (dt_masks_form_t *)shapes->data;
    gboolean found = FALSE;
    for(int i = 0; i < nbf; i++)
    {
      if(used[i] == f->formid)
      {
        found = TRUE;
        break;
      }
      if(used[i] == 0) break;
    }

    shapes = g_list_next(shapes); // need to get 'next' now, because
                                  // we may be removing the current
                                  // node

    if(found == FALSE)
    {
      forms = g_list_remove(forms, f);
      // and add it to allforms for cleanup
      darktable.develop->allforms = g_list_append(darktable.develop->allforms, f);
      masks_removed = 1;
    }
  }

  free(used);

  *_forms = forms;

  return masks_removed;
}

// removes all unused form from history if there are multiple
// hist->forms entries in history it may leave some unused forms we do
// it like this so the user can go back in history for a more accurate
// cleanup the user should compress history
void dt_masks_cleanup_unused_from_list(GList *history_list)
{
  // a mask is used in a given hist->forms entry if it is used up to
  // the next hist->forms so we are going to remove for each
  // hist->forms from the top
  int num = g_list_length(history_list);
  int history_end = num;

  for(const GList *history = g_list_last(history_list);
      history;
      history = g_list_previous(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;
    if(hist->forms
       && strcmp(hist->op_name, "mask_manager") == 0)
    {
      _masks_cleanup_unused(&hist->forms, history_list, history_end);
      history_end = num - 1;
    }
    num--;
  }
}

void dt_masks_cleanup_unused(dt_develop_t *dev)
{
  dt_masks_change_form_gui(NULL);

  // we remove the forms from history
  dt_masks_cleanup_unused_from_list(dev->history);

  // and we save all that
  GList *forms = NULL;
  dt_iop_module_t *module = NULL;
  int num = 0;
  for(const GList *history = dev->history;
      history && num < dev->history_end;
      history = g_list_next(history))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)history->data;

    if(hist->forms) forms = hist->forms;
    if(hist->module
       && strcmp(hist->op_name, "mask_manager") != 0)
      module = hist->module;

    num++;
  }

  dt_masks_replace_current_forms(dev, forms);

  if(module)
    dt_dev_add_history_item(dev, module, module->enabled);
  else
    dt_dev_add_masks_history_item(dev, NULL, TRUE);
}

int dt_masks_point_in_form_exact(const float x,
                                 const float y,
                                 float *points,
                                 const int points_start,
                                 const int points_count)
{
  // we use ray casting algorithm to avoid most problems with
  // horizontal segments, y should be rounded as int so that there's
  // very little chance than y==points...

  if(points_count > 2 + points_start)
  {
    const int start = isnan(points[points_start * 2])
      && !isnan(points[points_start * 2 + 1])
         ? points[points_start * 2 + 1]
         : points_start;

    const float yf = (float)y;
    int nb = 0;

    for(int i = start, next = start + 1; i < points_count;)
    {
      const float y1 = points[i * 2 + 1];
      const float y2 = points[next * 2 + 1];
      //if we need to skip points (in case of deleted point, because
      //of self-intersection)
      if(isnan(points[next * 2]))
      {
        next = isnan(y2) ? start : (int)y2;
        continue;
      }
      if(((yf <= y2 && yf > y1)
          || (yf >= y2 && yf < y1))
         && (points[i * 2] > x)) nb++;

      if(next == start) break;
      i = next++;
      if(next >= points_count) next = start;
    }
    return (nb & 1);
  }
  return 0;
}

int dt_masks_point_in_form_near(const float x,
                                const float y,
                                float *points,
                                const int points_start,
                                const int points_count,
                                const float distance,
                                int *near)
{
  // we use ray casting algorithm to avoid most problems with
  // horizontal segments, y should be rounded as int so that there's
  // very little chance than y==points...

  // TODO : distance is only evaluated in x, not y...

  if(points_count > 2 + points_start)
  {
    const int start =
      isnan(points[points_start * 2]) && !isnan(points[points_start * 2 + 1])
      ? points[points_start * 2 + 1]
      : points_start;

    const float yf = (float)y;
    int nb = 0;
    for(int i = start, next = start + 1; i < points_count;)
    {
      const float y1 = points[i * 2 + 1];
      const float y2 = points[next * 2 + 1];
      //if we need to jump to skip points (in case of deleted point,
      //because of self-intersection)
      if(isnan(points[next * 2]))
      {
        next = isnan(y2) ? start : (int)y2;
        continue;
      }
      if((yf <= y2 && yf > y1) || (yf >= y2 && yf < y1))
      {
        if(points[i * 2] > x) nb++;
        if(points[i * 2] - x < distance && points[i * 2] - x > -distance) *near = 1;
      }

      if(next == start) break;
      i = next++;
      if(next >= points_count) next = start;
    }
    return (nb & 1);
  }
  return 0;
}

float dt_masks_drag_factor(dt_masks_form_gui_t *gui,
                           const int index,
                           const int k,
                           const gboolean border)
{
  // we need the reference points
  dt_masks_form_gui_points_t *gpt = g_list_nth_data(gui->points, index);

  if(!gpt) return 0;

  float *boundary = border ? gpt->border : gpt->points;
  const float xref = gpt->points[0];
  const float yref = gpt->points[1];
  const float rx = boundary[k * 2] - xref;
  const float ry = boundary[k * 2 + 1] - yref;
  const float deltax = gui->posx + gui->dx - xref;
  const float deltay = gui->posy + gui->dy - yref;

  // we remap dx, dy to the right values, as it will be used in next
  // movements
  gui->dx = xref - gui->posx;
  gui->dy = yref - gui->posy;

  const float r = sqrtf(rx * rx + ry * ry);
  const float d = (rx * deltax + ry * deltay) / r;
  const float s = fmaxf(r > 0.0f ? (r + d) / r : 0.0f, 0.0f);

  return s;
}

// allow to select a shape inside an iop
void dt_masks_select_form(struct dt_iop_module_t *module,
                          dt_masks_form_t *sel)
{
  gboolean selection_changed = FALSE;

  if(sel)
  {
    if(sel->formid != darktable.develop->mask_form_selected_id)
    {
      darktable.develop->mask_form_selected_id = sel->formid;
      selection_changed = TRUE;
    }
  }
  else
  {
    if(darktable.develop->mask_form_selected_id != 0)
    {
      darktable.develop->mask_form_selected_id = 0;
      selection_changed = TRUE;
    }
  }
  if(selection_changed)
  {
    if(!module && darktable.develop->mask_form_selected_id == 0)
      module = darktable.develop->gui_module;
    if(module)
    {
      if(module->masks_selection_changed)
        module->masks_selection_changed(module, darktable.develop->mask_form_selected_id);
    }
  }
}

// draw a cross where the source position of a clone mask will be created
void dt_masks_draw_clone_source_pos(cairo_t *cr,
                                    const float zoom_scale,
                                    const float x,
                                    const float y)
{
  const float dx = 3.5f / zoom_scale;
  const float dy = 3.5f / zoom_scale;

  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;

  cairo_set_dash(cr, dashed, 0, 0);
  cairo_set_line_width(cr, 3.0 / zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);

  cairo_move_to(cr, x + dx, y);
  cairo_line_to(cr, x - dx, y);
  cairo_move_to(cr, x, y + dy);
  cairo_line_to(cr, x, y - dy);
  cairo_stroke_preserve(cr);

  cairo_set_line_width(cr, 1.0 / zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  cairo_stroke(cr);
}

// sets if the initial source position for a clone mask will be
// absolute or relative, based on mouse position and key state
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui,
                                           const uint32_t state,
                                           const float pzx,
                                           const float pzy)
{
  if(dt_modifier_is(state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    gui->source_pos_type = DT_MASKS_SOURCE_POS_ABSOLUTE;
  else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  else
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_masks_set_source_pos_initial_state] unknown state for setting masks position type\n");

  // both source types record an absolute position, for the relative
  // type, the first time is used the position is recorded, the second
  // time a relative position is calculated based on that one
  gui->posx_source = pzx * darktable.develop->preview_pipe->backbuf_width;
  gui->posy_source = pzy * darktable.develop->preview_pipe->backbuf_height;
}

// set the initial source position value for a clone mask
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui,
                                           const int mask_type,
                                           dt_masks_form_t *form,
                                           const float pzx,
                                           const float pzy)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const float iwd = darktable.develop->preview_pipe->iwidth;
  const float iht = darktable.develop->preview_pipe->iheight;

  // if this is the first time the relative pos is used
  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if it has not been defined by the user, set some default
    if(gui->posx_source == -1.0f && gui->posy_source == -1.0f)
    {
      if(form->functions && form->functions->initial_source_pos)
      {
        form->functions->initial_source_pos(iwd, iht, &gui->posx_source, &gui->posy_source);
      }
      else
        dt_print(DT_DEBUG_ALWAYS, "[dt_masks_set_source_pos_initial_value]"
                 " unsupported masks type when calculating source position initial value\n");

      float pts[2] = { pzx * wd + gui->posx_source, pzy * ht + gui->posy_source };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / iwd;
      form->source[1] = pts[1] / iht;
    }
    else
    {
      // if a position was defined by the user, use the absolute value
      // the first time
      float pts[2] = { gui->posx_source, gui->posy_source };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / iwd;
      form->source[1] = pts[1] / iht;

      gui->posx_source = gui->posx_source - pzx * wd;
      gui->posy_source = gui->posy_source - pzy * ht;
    }

    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    // original pos was already defined and relative value calculated,
    // just use it
    float pts[2] = { pzx * wd + gui->posx_source,
                     pzy * ht + gui->posy_source };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    form->source[0] = pts[0] / iwd;
    form->source[1] = pts[1] / iht;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // an absolute position was defined by the user
    float pts_src[2] = { gui->posx_source, gui->posy_source };
    dt_dev_distort_backtransform(darktable.develop, pts_src, 1);

    form->source[0] = pts_src[0] / iwd;
    form->source[1] = pts_src[1] / iht;
  }
  else
    dt_print(DT_DEBUG_ALWAYS, "[dt_masks_set_source_pos_initial_value]"
             " unknown source position type\n");
}

// calculates the source position value for preview drawing, on cairo coordinates
void dt_masks_calculate_source_pos_value(dt_masks_form_gui_t *gui,
                                         const int mask_type,
                                         const float initial_xpos,
                                         const float initial_ypos,
                                         const float xpos,
                                         const float ypos,
                                         float *px,
                                         float *py,
                                         const int adding)
{
  float x = 0.0f, y = 0.0f;
  const float pr_d = darktable.develop->preview_downsampling;
  const float iwd = pr_d * darktable.develop->preview_pipe->iwidth;
  const float iht = pr_d * darktable.develop->preview_pipe->iheight;

  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    x = xpos + gui->posx_source;
    y = ypos + gui->posy_source;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    if(gui->posx_source == -1.0f && gui->posy_source == -1.0f)
    {
#if 0 //TODO: replace individual cases with this generic one (will
      //require passing 'form' through multiple layers...)
      if(form->functions && form->functions->initial_source_pos)
      {
        form->functions->initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
#else
      if(mask_type & DT_MASKS_CIRCLE)
      {
        dt_masks_functions_circle.initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
      else if(mask_type & DT_MASKS_ELLIPSE)
      {
        dt_masks_functions_ellipse.initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
      else if(mask_type & DT_MASKS_PATH)
      {
        dt_masks_functions_path.initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
      else if(mask_type & DT_MASKS_BRUSH)
      {
        dt_masks_functions_brush.initial_source_pos(iwd, iht, &x, &y);
        x += xpos;
        y += ypos;
      }
#endif
      else
        dt_print(DT_DEBUG_ALWAYS, "[dt_masks_calculate_source_pos_value]"
                 " unsupported masks type when calculating source position value\n");
    }
    else
    {
      x = gui->posx_source;
      y = gui->posy_source;
    }
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // if the user is actually adding the mask follow the cursor
    if(adding)
    {
      x = xpos + gui->posx_source - initial_xpos;
      y = ypos + gui->posy_source - initial_ypos;
    }
    else
    {
      // if not added yet set the start position
      x = gui->posx_source;
      y = gui->posy_source;
    }
  }
  else
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_masks_calculate_source_pos_value]"
             " unknown source position type for setting source position value\n");

  *px = x;
  *py = y;
}

void dt_masks_draw_anchor(cairo_t *cr,
                          const gboolean selected,
                          const float zoom_scale,
                          const float x,
                          const float y)
{
  const float anchor_size = DT_PIXEL_APPLY_DPI(selected ? 8.0f : 5.0f) / zoom_scale;

  cairo_set_dash(cr, NULL, 0, 0);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_rectangle(cr,
                  x - (anchor_size * 0.5),
                  y - (anchor_size * 0.5),
                  anchor_size,
                  anchor_size);
  cairo_fill_preserve(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(selected ? 2.0 : 1.0) / zoom_scale);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);
  cairo_stroke(cr);
}

#include "detail.c"

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
