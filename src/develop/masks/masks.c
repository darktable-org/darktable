/*
    This file is part of darktable,
    copyright (c) 2012 aldric renaudin.

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

#pragma GCC diagnostic ignored "-Wshadow"

// clang-format off
#include "develop/masks/circle.c"
#include "develop/masks/path.c"
#include "develop/masks/brush.c"
#include "develop/masks/gradient.c"
#include "develop/masks/ellipse.c"
#include "develop/masks/group.c"
// clang-format on

typedef struct _masks_undo_data_t
{
  GList *forms;
  dt_masks_form_t *form;
} _masks_undo_data_t;

static void _masks_write_form_db(dt_masks_form_t *form, const int imgid, dt_develop_t *dev);
// write a form into the database

static void _masks_write_forms_db(dt_develop_t *dev, const int imgid, gboolean undo);
// write all masks form into the database. record an undo if undo is true

static dt_masks_form_t *_dup_masks_form(const dt_masks_form_t *form)
{
  if (!form) return NULL;

  dt_masks_form_t *new_form = malloc(sizeof(struct dt_masks_form_t));
  memcpy(new_form, form, sizeof(struct dt_masks_form_t));

  // then duplicate the GList *points

  new_form->points = NULL;

  if (form->points)
  {
    int size_item = 0;

    if (form->type & DT_MASKS_CIRCLE)
      size_item = sizeof(struct dt_masks_point_circle_t);
    else if (form->type & DT_MASKS_ELLIPSE)
      size_item = sizeof(struct dt_masks_point_ellipse_t);
    else if (form->type & DT_MASKS_GRADIENT)
      size_item = sizeof(struct dt_masks_point_gradient_t);
    else if (form->type & DT_MASKS_BRUSH)
      size_item = sizeof(struct dt_masks_point_brush_t);
    else if (form->type & DT_MASKS_GROUP)
      size_item = sizeof(struct dt_masks_point_group_t);
    else if (form->type & DT_MASKS_PATH)
      size_item = sizeof(struct dt_masks_point_path_t);

    if (size_item != 0)
    {
      GList *pt = g_list_first(form->points);
      while (pt)
      {
        void *item = malloc(size_item);
        memcpy(item, pt->data, size_item);
        new_form->points = g_list_append(new_form->points, item);
        pt = g_list_next(pt);
      }
    }
  }

  return new_form;
}

static void *_dup_masks_form_cb(const void *formdata, gpointer user_data)
{
  // duplicate the main form struct
  dt_masks_form_t *form = (dt_masks_form_t *)formdata;
  dt_masks_form_t *uform = (dt_masks_form_t *)user_data;
  const dt_masks_form_t *f = uform == NULL || form->formid != uform->formid ? form : uform;
  return (void *)_dup_masks_form(f);
}

// duplicate the list of forms, replace item in the list with form with the same formid
GList *dt_masks_dup_forms_deep(GList *forms, dt_masks_form_t *form)
{
  return (GList *)g_list_copy_deep(forms, _dup_masks_form_cb, (gpointer)form);
}

static _masks_undo_data_t *_create_snapshot(GList *forms, dt_masks_form_t *form, dt_develop_t *dev)
{
  _masks_undo_data_t *data = malloc(sizeof(struct _masks_undo_data_t));
  data->forms = dt_masks_dup_forms_deep(forms, form);
  data->form  = dev->form_visible ? _dup_masks_form(dev->form_visible) : NULL;
  return data;
}

static void _masks_free_undo(gpointer data)
{
  _masks_undo_data_t *udata = (_masks_undo_data_t *)data;

  g_list_free_full(udata->forms, (void (*)(void *))dt_masks_free_form);
  udata->forms = NULL;
  dt_masks_free_form((dt_masks_form_t *)udata->form);
  free(udata);
}

static void _masks_do_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t *item)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  _masks_undo_data_t *udata = (_masks_undo_data_t *)item;

  dev->forms = dt_masks_dup_forms_deep(udata->forms, NULL);
  dev->form_gui->creation = FALSE;

  dt_masks_clear_form_gui(dev);
  dt_masks_change_form_gui(_dup_masks_form(udata->form));

  _masks_write_forms_db(dev, dev->image_storage.id, FALSE);

  // and we ensure that we are in edit mode
  dt_masks_iop_update(darktable.develop->gui_module);
  dt_dev_masks_list_change(dev);
  dt_masks_set_edit_mode(darktable.develop->gui_module, DT_MASKS_EDIT_FULL);
  dt_masks_update_image(dev);
  dt_control_queue_redraw_center();
}

static void _do_record_undo(dt_develop_t *dev, dt_masks_form_t *form)
{
  dt_undo_record(darktable.undo, dev, DT_UNDO_MASK, (dt_undo_data_t *)_create_snapshot(dev->forms, form, dev),
                 _masks_do_undo, _masks_free_undo);
}

static void _set_hinter_message(dt_masks_form_gui_t *gui, dt_masks_type_t formtype)
{
  char msg[256] = "";

  if(formtype & DT_MASKS_PATH)
  {
    if(gui->creation)
      g_strlcat(msg, _("ctrl+click to add a sharp node"), sizeof(msg));
    else if(gui->point_selected >= 0)
      g_strlcat(msg, _("ctrl+click to switch between smooth/sharp node"), sizeof(msg));
    else if(gui->feather_selected >= 0)
      g_strlcat(msg, _("right-click to reset curvature"), sizeof(msg));
    else if(gui->seg_selected >= 0)
      g_strlcat(msg, _("ctrl+click to add a node"), sizeof(msg));
    else if(gui->form_selected)
      g_strlcat(msg, _("ctrl+scroll to set shape opacity, shift+scroll to set feather size"), sizeof(msg));
  }
  else if(formtype & DT_MASKS_GRADIENT)
  {
    if(gui->form_selected)
      g_strlcat(msg, _("ctrl+scroll to set shape opacity"), sizeof(msg));
    else if(gui->pivot_selected)
      g_strlcat(msg, _("move to rotate shape"), sizeof(msg));
  }
  else if(formtype & DT_MASKS_ELLIPSE)
  {
    if(gui->creation)
      g_strlcat(msg, _("scroll to set size, shift+scroll to set feather size\nctrl+scroll to set shape opacity"), sizeof(msg));
    else if(gui->point_selected >= 0)
      g_strlcat(msg, _("ctrl+click to rotate"), sizeof(msg));
    else if(gui->form_selected)
      g_strlcat(msg, _("shift+click to switch feathering mode, ctrl+scroll to set shape opacity,\nshift+scroll to set feather size, ctrl+click to rotate"), sizeof(msg));
  }
  else if(formtype & DT_MASKS_BRUSH)
  {
    if(gui->creation)
      g_strlcat(msg, _("scroll to set brush size, shift+scroll to set hardness,\nctrl+scroll to set opacity"),
                sizeof(msg));
    else if(gui->border_selected)
      g_strlcat(msg, _("scroll to set brush size"), sizeof(msg));
    else if(gui->form_selected)
      g_strlcat(msg, _("scroll to set hardness, ctrl+scroll to set shape opacity"), sizeof(msg));
  }
  else if(formtype & DT_MASKS_CIRCLE)
  {
    if(gui->creation)
      g_strlcat(msg, _("scroll to set size, shift+scroll to set feather size\nctrl+scroll to set shape opacity"), sizeof(msg));
    else if(gui->form_selected)
      g_strlcat(msg, _("ctrl+scroll to set shape opacity, shift+scroll to set feather size"), sizeof(msg));
  }

  dt_control_hinter_message(darktable.control, msg);
}

void dt_masks_init_form_gui(dt_masks_form_gui_t *gui)
{
  memset(gui, 0, sizeof(dt_masks_form_gui_t));

  gui->posx = gui->posy = -1.0f;
  gui->posx_source = gui->posy_source = -1.0f;
  gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
}

void dt_masks_gui_form_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  if(g_list_length(gui->points) == index)
  {
    dt_masks_form_gui_points_t *gpt2
        = (dt_masks_form_gui_points_t *)calloc(1, sizeof(dt_masks_form_gui_points_t));
    gui->points = g_list_append(gui->points, gpt2);
  }
  else if(g_list_length(gui->points) < index)
    return;

  dt_masks_gui_form_remove(form, gui, index);

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(dt_masks_get_points_border(darktable.develop, form, &gpt->points, &gpt->points_count, &gpt->border,
                                &gpt->border_count, 0))
  {
    if(form->type & DT_MASKS_CLONE)
      dt_masks_get_points_border(darktable.develop, form, &gpt->source, &gpt->source_count, NULL, NULL, 1);
    gui->pipe_hash = darktable.develop->preview_pipe->backbuf_hash;
    gui->formid = form->formid;
  }
}

void dt_masks_form_gui_points_free(gpointer data)
{
  if(!data) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)data;

  free(gpt->points);
  free(gpt->border);
  free(gpt->source);
  free(gpt);
}

void dt_masks_gui_form_remove(dt_masks_form_t *form, dt_masks_form_gui_t *gui, int index)
{
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  gui->pipe_hash = gui->formid = 0;

  if(gpt)
  {
    gpt->points_count = gpt->border_count = gpt->source_count = 0;
    free(gpt->points);
    gpt->points = NULL;
    free(gpt->border);
    gpt->border = NULL;
    free(gpt->source);
    gpt->source = NULL;
  }
}

void dt_masks_gui_form_test_create(dt_masks_form_t *form, dt_masks_form_gui_t *gui)
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
      GList *fpts = g_list_first(form->points);
      int pos = 0;
      while(fpts)
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
        dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
        if (!sel) return;
        dt_masks_gui_form_create(sel, gui, pos);
        fpts = g_list_next(fpts);
        pos++;
      }
    }
    else
      dt_masks_gui_form_create(form, gui, 0);
  }
}

static void _check_id(dt_masks_form_t *form)
{
  GList *forms = g_list_first(darktable.develop->forms);
  int nid = 100;
  while(forms)
  {
    dt_masks_form_t *ff = (dt_masks_form_t *)forms->data;
    if(ff->formid == form->formid)
    {
      form->formid = nid++;
      forms = g_list_first(darktable.develop->forms);
      continue;
    }
    forms = g_list_next(forms);
  }
}

void dt_masks_gui_form_save_creation(dt_develop_t *dev, dt_iop_module_t *module, dt_masks_form_t *form,
                                     dt_masks_form_gui_t *gui)
{
  GList *l;

  // we check if the id is already registered
  _check_id(form);

  if(gui) gui->creation = FALSE;

  // mask nb will be at least the length of the list
  guint nb = 0;

  // count only the same forms to have a clean numbering
  l = dev->forms;
  while(l)
  {
    dt_masks_form_t *f = (dt_masks_form_t *)l->data;
    if(f->type == form->type) nb++;
    l = g_list_next(l);
  }

  gboolean exist = FALSE;

  // check that we do not have duplicate, in case some masks have been
  // removed we can have hole and so nb could already exists.
  do
  {
    exist = FALSE;
    nb++;

    if(form->type & DT_MASKS_CIRCLE)
      snprintf(form->name, sizeof(form->name), _("circle #%d"), nb);
    else if(form->type & DT_MASKS_PATH)
      snprintf(form->name, sizeof(form->name), _("path #%d"), nb);
    else if(form->type & DT_MASKS_GRADIENT)
      snprintf(form->name, sizeof(form->name), _("gradient #%d"), nb);
    else if(form->type & DT_MASKS_ELLIPSE)
      snprintf(form->name, sizeof(form->name), _("ellipse #%d"), nb);
    else if(form->type & DT_MASKS_BRUSH)
      snprintf(form->name, sizeof(form->name), _("brush #%d"), nb);

    l = dev->forms;
    while(l)
    {
      dt_masks_form_t *f = (dt_masks_form_t *)l->data;
      if(!strcmp(f->name, form->name))
      {
        exist = TRUE;
        break;
      }
      l = g_list_next(l);
    }
  } while(exist);

  dev->forms = g_list_append(dev->forms, form);

  dt_masks_write_form(form, dev);

  if(module)
  {
    // is there already a masks group for this module ?
    int grpid = module->blend_params->mask_id;
    dt_masks_form_t *grp = dt_masks_get_from_id(dev, grpid);
    if(!grp)
    {
      // we create a new group
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        grp = dt_masks_create(DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        grp = dt_masks_create(DT_MASKS_GROUP);
      gchar *module_label = dt_history_item_get_name(module);
      snprintf(grp->name, sizeof(grp->name), "grp %s", module_label);
      g_free(module_label);
      _check_id(grp);
      dev->forms = g_list_append(dev->forms, grp);
      module->blend_params->mask_id = grpid = grp->formid;
    }
    // we add the form in this group
    dt_masks_point_group_t *grpt = malloc(sizeof(dt_masks_point_group_t));
    grpt->formid = form->formid;
    grpt->parentid = grpid;
    grpt->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE;
    if(g_list_length(grp->points) > 0) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, grpt);
    // we save the group
    dt_masks_write_form(grp, dev);
    // we update module gui
    if(gui) dt_masks_iop_update(module);
    dt_dev_add_history_item(dev, module, TRUE);
  }
  // show the form if needed
  if(gui) dev->form_gui->formid = form->formid;
  if(gui) dt_dev_masks_list_change(dev);
}

int dt_masks_form_duplicate(dt_develop_t *dev, int formid)
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
  if(fbase->type & DT_MASKS_GROUP)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
      dt_masks_point_group_t *npt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));

      npt->formid = dt_masks_form_duplicate(dev, pt->formid);
      npt->parentid = fdest->formid;
      npt->state = pt->state;
      npt->opacity = pt->opacity;
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }
  else if(fbase->type & DT_MASKS_CIRCLE)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_circle_t *pt = (dt_masks_point_circle_t *)pts->data;
      dt_masks_point_circle_t *npt = (dt_masks_point_circle_t *)malloc(sizeof(dt_masks_point_circle_t));
      memcpy(npt, pt, sizeof(dt_masks_point_circle_t));
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }
  else if(fbase->type & DT_MASKS_PATH)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_path_t *pt = (dt_masks_point_path_t *)pts->data;
      dt_masks_point_path_t *npt = (dt_masks_point_path_t *)malloc(sizeof(dt_masks_point_path_t));
      memcpy(npt, pt, sizeof(dt_masks_point_path_t));
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }
  else if(fbase->type & DT_MASKS_GRADIENT)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_gradient_t *pt = (dt_masks_point_gradient_t *)pts->data;
      dt_masks_point_gradient_t *npt = (dt_masks_point_gradient_t *)malloc(sizeof(dt_masks_point_gradient_t));
      memcpy(npt, pt, sizeof(dt_masks_point_gradient_t));
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }
  else if(fbase->type & DT_MASKS_ELLIPSE)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_ellipse_t *pt = (dt_masks_point_ellipse_t *)pts->data;
      dt_masks_point_ellipse_t *npt = (dt_masks_point_ellipse_t *)malloc(sizeof(dt_masks_point_ellipse_t));
      memcpy(npt, pt, sizeof(dt_masks_point_ellipse_t));
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }
  else if(fbase->type & DT_MASKS_BRUSH)
  {
    GList *pts = g_list_first(fbase->points);
    while(pts)
    {
      dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)pts->data;
      dt_masks_point_brush_t *npt = (dt_masks_point_brush_t *)malloc(sizeof(dt_masks_point_brush_t));
      memcpy(npt, pt, sizeof(dt_masks_point_brush_t));
      fdest->points = g_list_append(fdest->points, npt);
      pts = g_list_next(pts);
    }
  }

  // we save the form
  dt_masks_write_form(fdest, dev);

  // and we return it's id
  return fdest->formid;
}

int dt_masks_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                               float **border, int *border_count, int source)
{
  if(form->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(g_list_first(form->points)->data);
    float x, y;
    if(source)
      x = form->source[0], y = form->source[1];
    else
      x = circle->center[0], y = circle->center[1];
    if(dt_circle_get_points(dev, x, y, circle->radius, points, points_count))
    {
      if(border)
        return dt_circle_get_points(dev, x, y, circle->radius + circle->border, border, border_count);
      else
        return 1;
    }
  }
  else if(form->type & DT_MASKS_PATH)
  {
    return dt_path_get_points_border(dev, form, points, points_count, border, border_count, source);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    return dt_brush_get_points_border(dev, form, points, points_count, border, border_count, source);
  }
  else if(form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);
    if(dt_gradient_get_points(dev, gradient->anchor[0], gradient->anchor[1], gradient->rotation, points,
                              points_count))
    {
      if(border)
        return dt_gradient_get_points_border(dev, gradient->anchor[0], gradient->anchor[1],
                                             gradient->rotation, gradient->compression, border, border_count);
      else
        return 1;
    }
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);
    float x, y, a, b;
    if(source)
      x = form->source[0], y = form->source[1];
    else
      x = ellipse->center[0], y = ellipse->center[1];
    a = ellipse->radius[0], b = ellipse->radius[1];
    if(dt_ellipse_get_points(dev, x, y, a, b, ellipse->rotation, points, points_count))
    {
      if(border)
        return dt_ellipse_get_points(dev, x, y,
                                     (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? a * (1.0f + ellipse->border) : a + ellipse->border),
                                     (ellipse->flags & DT_MASKS_ELLIPSE_PROPORTIONAL ? b * (1.0f + ellipse->border) : b + ellipse->border),
                                     ellipse->rotation, border, border_count);
      else
        return 1;
    }
  }

  return 0;
}

int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      int *width, int *height, int *posx, int *posy)
{
  if(form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_PATH)
  {
    return dt_path_get_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_GRADIENT)
  {
    return dt_gradient_get_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    return dt_ellipse_get_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    return dt_brush_get_area(module, piece, form, width, height, posx, posy);
  }

  return 0;
}

int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy)
{
  if(form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_source_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_PATH)
  {
    return dt_path_get_source_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    return dt_ellipse_get_source_area(module, piece, form, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    return dt_brush_get_source_area(module, piece, form, width, height, posx, posy);
  }
  return 0;
}

int dt_masks_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                      float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_PATH)
  {
    return dt_path_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_GROUP)
  {
    return dt_group_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_GRADIENT)
  {
    return dt_gradient_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    return dt_ellipse_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    return dt_brush_get_mask(module, piece, form, buffer, width, height, posx, posy);
  }
  return 0;
}

int dt_masks_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                          const dt_iop_roi_t *roi, float *buffer)
{
  if(form->type & DT_MASKS_CIRCLE)
  {
    return dt_circle_get_mask_roi(module, piece, form, roi, buffer);
  }
  else if(form->type & DT_MASKS_PATH)
  {
    return dt_path_get_mask_roi(module, piece, form, roi, buffer);
  }
  else if(form->type & DT_MASKS_GROUP)
  {
    return dt_group_get_mask_roi(module, piece, form, roi, buffer);
  }
  else if(form->type & DT_MASKS_GRADIENT)
  {
    return dt_gradient_get_mask_roi(module, piece, form, roi, buffer);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    return dt_ellipse_get_mask_roi(module, piece, form, roi, buffer);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    return dt_brush_get_mask_roi(module, piece, form, roi, buffer);
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

    GList *modules = dev->iop;
    while(modules)
    {
      dt_iop_module_t *find_op = (dt_iop_module_t *)modules->data;
      if(!strcmp(find_op->op, opname))
      {
        module = find_op;
        break;
      }
      modules = g_list_next(modules);
    }

    if(module == NULL) return 1;

    dt_dev_pixelpipe_iop_t piece = { 0 };

    module->init_pipe(module, NULL, &piece);
    module->commit_params(module, module->default_params, NULL, &piece);

    piece.buf_in.width = 1;
    piece.buf_in.height = 1;

    GList *p = g_list_first(m->points);

    if(!p) return 1;

    if(m->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)p->data;
      module->distort_backtransform(module, &piece, circle->center, 1);
    }
    else if(m->type & DT_MASKS_PATH)
    {
      while(p)
      {
        dt_masks_point_path_t *path = (dt_masks_point_path_t *)p->data;
        module->distort_backtransform(module, &piece, path->corner, 1);
        module->distort_backtransform(module, &piece, path->ctrl1, 1);
        module->distort_backtransform(module, &piece, path->ctrl2, 1);

        p = g_list_next(p);
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
      while(p)
      {
        dt_masks_point_brush_t *brush = (dt_masks_point_brush_t *)p->data;
        module->distort_backtransform(module, &piece, brush->corner, 1);
        module->distort_backtransform(module, &piece, brush->ctrl1, 1);
        module->distort_backtransform(module, &piece, brush->ctrl2, 1);

        p = g_list_next(p);
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

static void dt_masks_legacy_params_v2_to_v3_transform(const dt_image_t *img, float *points)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cx = (float)img->crop_x, cy = (float)img->crop_y;

  const float cw = (float)(img->width - img->crop_x - img->crop_width),
              ch = (float)(img->height - img->crop_y - img->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by image original cropped dimensions
   * 2. un-crop them by adding top-left crop coordinates
   * 3. normalize them by the image fully uncropped dimensions
   */
  points[0] = ((points[0] * cw) + cx) / w;
  points[1] = ((points[1] * ch) + cy) / h;
}

static void dt_masks_legacy_params_v2_to_v3_transform_only_rescale(const dt_image_t *img, float *points,
                                                                   size_t points_count)
{
  const float w = (float)img->width, h = (float)img->height;

  const float cw = (float)(img->width - img->crop_x - img->crop_width),
              ch = (float)(img->height - img->crop_y - img->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by minimal of image original cropped dimensions
   * 2. normalize them by the minimal of image fully uncropped dimensions
   */
  for(size_t i = 0; i < points_count; i++) points[i] = ((points[i] * MIN(cw, ch))) / MIN(w, h);
}

static int dt_masks_legacy_params_v2_to_v3(dt_develop_t *dev, void *params)
{
  /*
   * difference: before v3 images were originally cropped on load
   * after v3: images are cropped in rawprepare iop.
   */

  dt_masks_form_t *m = (dt_masks_form_t *)params;

  const dt_image_t *img = &(dev->image_storage);

  if(img->crop_x == 0 && img->crop_y == 0 && img->crop_width == 0 && img->crop_height == 0)
  {
    // image has no "raw cropping", we're fine!
    m->version = 3;
    return 0;
  }
  else
  {
    GList *p = g_list_first(m->points);

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
      while(p)
      {
        dt_masks_point_path_t *path = (dt_masks_point_path_t *)p->data;
        dt_masks_legacy_params_v2_to_v3_transform(img, path->corner);
        dt_masks_legacy_params_v2_to_v3_transform(img, path->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, path->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, path->border, 2);

        p = g_list_next(p);
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
      while(p)
      {
        dt_masks_point_brush_t *brush = (dt_masks_point_brush_t *)p->data;
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->corner);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(img, brush->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(img, brush->border, 2);

        p = g_list_next(p);
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

  GList *p = g_list_first(m->points);

  if(!p) return 1;

  if(m->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)p->data;
    ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  }

  m->version = 4;

  return 0;
}


int dt_masks_legacy_params(dt_develop_t *dev, void *params, const int old_version, const int new_version)
{
  int res = 1;
#if 0 // we should not need this any longer
  if(old_version == 1 && new_version == 2)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
  }
#endif

  if(old_version == 1 && new_version == 4)
  {
    res = dt_masks_legacy_params_v1_to_v2(dev, params);
    if(!res) res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
  }
  else if(old_version == 2 && new_version == 4)
  {
    res = dt_masks_legacy_params_v2_to_v3(dev, params);
    if(!res) res = dt_masks_legacy_params_v3_to_v4(dev, params);
  }
  else if(old_version == 3 && new_version == 4)
  {
    res = dt_masks_legacy_params_v3_to_v4(dev, params);
  }

  return res;
}

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *form = (dt_masks_form_t *)calloc(1, sizeof(dt_masks_form_t));
  if(!form) return NULL;

  form->type = type;
  form->version = dt_masks_version();
  form->formid = time(NULL);

  // all forms created must be registered in darktable.develop->allforms for later cleanup
  darktable.develop->allforms = g_list_append(darktable.develop->allforms, form);

  return form;
}

dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, int id)
{
  while(forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->formid == id) return form;
    forms = g_list_next(forms);
  }
  return NULL;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id)
{
  return dt_masks_get_from_id_ext(dev->forms, id);
}

void dt_masks_read_forms_ext(dt_develop_t *dev, const int imgid, gboolean no_image)
{
  // first we have to reset the list
  g_list_free(dev->forms);
  dev->forms = NULL;

  if(imgid <= 0) return;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid, formid, form, name, version, points, points_count, source FROM main.mask WHERE imgid = ?1",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-formid, 2-form_type, 3-name, 4-version, 5-points, 6-points_count, 7-source

    // we get the values

    int formid = sqlite3_column_int(stmt, 1);
    dt_masks_type_t type = sqlite3_column_int(stmt, 2);
    dt_masks_form_t *form = dt_masks_create(type);
    form->formid = formid;
    const char *name = (const char *)sqlite3_column_text(stmt, 3);
    snprintf(form->name, sizeof(form->name), "%s", name);
    form->version = sqlite3_column_int(stmt, 4);
    form->points = NULL;
    int nb_points = sqlite3_column_int(stmt, 6);
    memcpy(form->source, sqlite3_column_blob(stmt, 7), 2 * sizeof(float));

    // and now we "read" the blob
    if(form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)malloc(sizeof(dt_masks_point_circle_t));
      memcpy(circle, sqlite3_column_blob(stmt, 5), sizeof(dt_masks_point_circle_t));
      form->points = g_list_append(form->points, circle);
    }
    else if(form->type & DT_MASKS_PATH)
    {
      dt_masks_point_path_t *ptbuf = (dt_masks_point_path_t *)sqlite3_column_blob(stmt, 5);
      for(int i = 0; i < nb_points; i++)
      {
        dt_masks_point_path_t *point = (dt_masks_point_path_t *)malloc(sizeof(dt_masks_point_path_t));
        memcpy(point, ptbuf + i, sizeof(dt_masks_point_path_t));
        form->points = g_list_append(form->points, point);
      }
    }
    else if(form->type & DT_MASKS_GROUP)
    {
      dt_masks_point_group_t *ptbuf = (dt_masks_point_group_t *)sqlite3_column_blob(stmt, 5);
      for(int i = 0; i < nb_points; i++)
      {
        dt_masks_point_group_t *point = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        memcpy(point, ptbuf + i, sizeof(dt_masks_point_group_t));
        form->points = g_list_append(form->points, point);
      }
    }
    else if(form->type & DT_MASKS_GRADIENT)
    {
      dt_masks_point_gradient_t *gradient
          = (dt_masks_point_gradient_t *)malloc(sizeof(dt_masks_point_gradient_t));
      memcpy(gradient, sqlite3_column_blob(stmt, 5), sizeof(dt_masks_point_gradient_t));
      form->points = g_list_append(form->points, gradient);
    }
    else if(form->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_point_ellipse_t *ellipse
          = (dt_masks_point_ellipse_t *)malloc(sizeof(dt_masks_point_ellipse_t));
      memcpy(ellipse, sqlite3_column_blob(stmt, 5), sizeof(dt_masks_point_ellipse_t));
      form->points = g_list_append(form->points, ellipse);
    }
    else if(form->type & DT_MASKS_BRUSH)
    {
      dt_masks_point_brush_t *ptbuf = (dt_masks_point_brush_t *)sqlite3_column_blob(stmt, 5);
      for(int i = 0; i < nb_points; i++)
      {
        dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)malloc(sizeof(dt_masks_point_brush_t));
        memcpy(point, ptbuf + i, sizeof(dt_masks_point_brush_t));
        form->points = g_list_append(form->points, point);
      }
    }

    if(form->version != dt_masks_version())
    {
      if(dt_masks_legacy_params(dev, form, form->version, dt_masks_version()))
      {
        const char *fname = dev->image_storage.filename + strlen(dev->image_storage.filename);
        while(fname > dev->image_storage.filename && *fname != '/') fname--;
        if(fname > dev->image_storage.filename) fname++;

        fprintf(stderr,
                "[dt_masks_read_forms] %s (imgid `%i'): mask version mismatch: history is %d, dt %d.\n",
                fname, imgid, form->version, dt_masks_version());
        dt_control_log(_("%s: mask version mismatch: %d != %d"), fname, dt_masks_version(), form->version);

        continue;
      }
    }

    // and we can add the form to the list
    dev->forms = g_list_append(dev->forms, form);
  }

  sqlite3_finalize(stmt);
  if(!no_image)
  dt_dev_masks_list_change(dev);
}

void dt_masks_read_forms(dt_develop_t *dev)
{
  dt_masks_read_forms_ext(dev, dev->image_storage.id, FALSE);
}

static void _masks_write_forms_db(dt_develop_t *dev, const int imgid, gboolean undo)
{
  // we first erase all masks for the image present in the db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "DELETE FROM main.mask WHERE imgid = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // and now we write each forms
  GList *forms = g_list_first(dev->forms);
  if (undo)
  {
    _do_record_undo(dev, NULL);
  }

  while(forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;

    if (form)
      _masks_write_form_db(form, imgid, dev);

    forms = g_list_next(forms);
  }
}
static void _masks_write_form_db(dt_masks_form_t *form, const int imgid, dt_develop_t *dev)
{
  sqlite3_stmt *stmt;

  // write the form into the database
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "INSERT INTO main.mask (imgid, formid, form, name, "
                                                             "version, points, points_count,source) VALUES "
                                                             "(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, form->type);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, form->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, form->source, 2 * sizeof(float), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, form->version);
  if(form->type & DT_MASKS_CIRCLE)
  {
    GList *points = g_list_first(form->points);
    if(points)
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)(points->data);
      DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, circle, sizeof(dt_masks_point_circle_t), SQLITE_TRANSIENT);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, 1);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
  }
  else if(form->type & DT_MASKS_PATH)
  {
    guint nb = g_list_length(form->points);
    dt_masks_point_path_t *ptbuf = (dt_masks_point_path_t *)calloc(nb, sizeof(dt_masks_point_path_t));
    GList *points = g_list_first(form->points);
    int pos = 0;
    while(points)
    {
      dt_masks_point_path_t *pt = (dt_masks_point_path_t *)points->data;
      ptbuf[pos++] = *pt;
      points = g_list_next(points);
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb * sizeof(dt_masks_point_path_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(ptbuf);
  }
  else if(form->type & DT_MASKS_GROUP)
  {
    guint nb = g_list_length(form->points);
    dt_masks_point_group_t *ptbuf = (dt_masks_point_group_t *)calloc(nb, sizeof(dt_masks_point_group_t));
    GList *points = g_list_first(form->points);
    int pos = 0;
    while(points)
    {
      dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
      ptbuf[pos++] = *pt;
      points = g_list_next(points);
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb * sizeof(dt_masks_point_group_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(ptbuf);
  }
  else if(form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_point_gradient_t *gradient = (dt_masks_point_gradient_t *)(g_list_first(form->points)->data);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, gradient, sizeof(dt_masks_point_gradient_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, 1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else if(form->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_point_ellipse_t *ellipse = (dt_masks_point_ellipse_t *)(g_list_first(form->points)->data);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ellipse, sizeof(dt_masks_point_ellipse_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, 1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else if(form->type & DT_MASKS_BRUSH)
  {
    guint nb = g_list_length(form->points);
    dt_masks_point_brush_t *ptbuf = (dt_masks_point_brush_t *)calloc(nb, sizeof(dt_masks_point_brush_t));
    GList *points = g_list_first(form->points);
    int pos = 0;
    while(points)
    {
      dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)points->data;
      ptbuf[pos++] = *pt;
      points = g_list_next(points);
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, ptbuf, nb * sizeof(dt_masks_point_brush_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, nb);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(ptbuf);
  }
}

void dt_masks_write_form(dt_masks_form_t *form, dt_develop_t *dev)
{
  _do_record_undo(dev, form);

  // we first erase all masks for the image present in the db
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.mask WHERE imgid = ?1 AND formid = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, form->formid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  _masks_write_form_db(form, dev->image_storage.id, dev);
}

void dt_masks_write_forms_ext(dt_develop_t *dev, const int imgid, gboolean undo)
{
  _masks_write_forms_db(dev, imgid, undo);
}

void dt_masks_write_forms(dt_develop_t *dev)
{
  _masks_write_forms_db(dev, dev->image_storage.id, TRUE);
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
  // reset mouse position for masks
  if(darktable.develop->form_gui)
  {
    dt_masks_form_gui_t *gui = darktable.develop->form_gui;

    // if masks are being created or edited don't reset the position
    if(gui->creation || gui->form_dragging || gui->source_dragging || gui->point_dragging >= 0
       || gui->feather_dragging >= 0 || gui->seg_dragging >= 0 || gui->point_border_dragging >= 0)
      return 0;

    gui->posx = gui->posy = -1.f;
  }
  return 0;
}

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure, int which)
{
  // record mouse position even if there are no masks visible
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  dt_masks_form_t *form = darktable.develop->form_visible;
  float pzx, pzy;

  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(gui)
  {
    gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
  }

  // do not preocess if no forms visible
  if(!form) return 0;

  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int rep = 0;
  if(form->type & DT_MASKS_CIRCLE)
    rep = dt_circle_events_mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);
  else if(form->type & DT_MASKS_PATH)
    rep = dt_path_events_mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);
  else if(form->type & DT_MASKS_GROUP)
    rep = dt_group_events_mouse_moved(module, pzx, pzy, pressure, which, form, gui);
  else if(form->type & DT_MASKS_GRADIENT)
    rep = dt_gradient_events_mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);
  else if(form->type & DT_MASKS_ELLIPSE)
    rep = dt_ellipse_events_mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);
  else if(form->type & DT_MASKS_BRUSH)
    rep = dt_brush_events_mouse_moved(module, pzx, pzy, pressure, which, form, 0, gui, 0);

  if(gui)
  {
    int ftype = form->type;
    if(ftype & DT_MASKS_GROUP)
    {
      if(gui->group_edited >= 0)
      {
        // we get the slected form
        dt_masks_point_group_t *fpt
            = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
        dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
        if(!sel) return 0;
        ftype = sel->type;
      }
    }
    _set_hinter_message(gui, ftype);
  }

  return rep;
}

int dt_masks_events_button_released(struct dt_iop_module_t *module, double x, double y, int which,
                                    uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(form->type & DT_MASKS_CIRCLE)
    return dt_circle_events_button_released(module, pzx, pzy, which, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_PATH)
    return dt_path_events_button_released(module, pzx, pzy, which, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_GROUP)
    return dt_group_events_button_released(module, pzx, pzy, which, state, form, gui);
  else if(form->type & DT_MASKS_GRADIENT)
    return dt_gradient_events_button_released(module, pzx, pzy, which, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_ELLIPSE)
    return dt_ellipse_events_button_released(module, pzx, pzy, which, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_BRUSH)
    return dt_brush_events_button_released(module, pzx, pzy, which, state, form, 0, gui, 0);

  return 0;
}

int dt_masks_events_button_pressed(struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int which, int type, uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  // allow to select a shape inside an iop
  if(gui && which == 1)
  {
    dt_masks_form_t *sel = NULL;

    if((gui->form_selected || gui->source_selected || gui->point_selected || gui->seg_selected
        || gui->feather_selected)
       && !gui->creation && gui->group_edited >= 0)
    {
      // we get the slected form
      dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
      if(fpt)
      {
        sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
      }
    }

    dt_masks_select_form(module, sel);
  }

  if(form->type & DT_MASKS_CIRCLE)
    return dt_circle_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_PATH)
    return dt_path_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_GROUP)
    return dt_group_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, gui);
  else if(form->type & DT_MASKS_GRADIENT)
    return dt_gradient_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_ELLIPSE)
    return dt_ellipse_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_BRUSH)
    return dt_brush_events_button_pressed(module, pzx, pzy, pressure, which, type, state, form, 0, gui, 0);

  return 0;
}

int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *form = darktable.develop->form_visible;
  dt_masks_form_gui_t *gui = darktable.develop->form_gui;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(form->type & DT_MASKS_CIRCLE)
    return dt_circle_events_mouse_scrolled(module, pzx, pzy, up, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_PATH)
    return dt_path_events_mouse_scrolled(module, pzx, pzy, up, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_GROUP)
    return dt_group_events_mouse_scrolled(module, pzx, pzy, up, state, form, gui);
  else if(form->type & DT_MASKS_GRADIENT)
    return dt_gradient_events_mouse_scrolled(module, pzx, pzy, up, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_ELLIPSE)
    return dt_ellipse_events_mouse_scrolled(module, pzx, pzy, up, state, form, 0, gui, 0);
  else if(form->type & DT_MASKS_BRUSH)
    return dt_brush_events_mouse_scrolled(module, pzx, pzy, up, state, form, 0, gui, 0);

  return 0;
}
void dt_masks_events_post_expose(struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(!gui) return;
  if(!form) return;
  // if it's a spot in creation, nothing to draw
  // add preview when creating a circle or ellipse
  if((form->type & DT_MASKS_GRADIENT) && gui->creation)
    return;

  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  if(wd < 1.0 || ht < 1.0) return;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);

  cairo_save(cr);
  cairo_set_source_rgb(cr, .3, .3, .3);

  cairo_translate(cr, width / 2.0, height / 2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // we update the form if needed
  // add preview when creating a circle or ellipse
  if(!(((form->type & DT_MASKS_CIRCLE) || (form->type & DT_MASKS_ELLIPSE)) && gui->creation))
    dt_masks_gui_form_test_create(form, gui);

  // draw form
  if(form->type & DT_MASKS_CIRCLE)
    dt_circle_events_post_expose(cr, zoom_scale, gui, 0);
  else if(form->type & DT_MASKS_PATH)
    dt_path_events_post_expose(cr, zoom_scale, gui, 0, g_list_length(form->points));
  else if(form->type & DT_MASKS_GROUP)
    dt_group_events_post_expose(cr, zoom_scale, form, gui);
  else if(form->type & DT_MASKS_GRADIENT)
    dt_gradient_events_post_expose(cr, zoom_scale, gui, 0);
  else if(form->type & DT_MASKS_ELLIPSE)
    dt_ellipse_events_post_expose(cr, zoom_scale, gui, 0);
  else if(form->type & DT_MASKS_BRUSH)
    dt_brush_events_post_expose(cr, zoom_scale, gui, 0, g_list_length(form->points));

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
  dev->form_gui->form_selected = dev->form_gui->border_selected = dev->form_gui->form_dragging
      = dev->form_gui->form_rotating = dev->form_gui->border_toggling = FALSE;
  dev->form_gui->source_selected = dev->form_gui->source_dragging = FALSE;
  dev->form_gui->pivot_selected = FALSE;
  dev->form_gui->point_border_selected = dev->form_gui->seg_selected = dev->form_gui->point_selected
      = dev->form_gui->feather_selected = -1;
  dev->form_gui->point_border_dragging = dev->form_gui->seg_dragging = dev->form_gui->feather_dragging
      = dev->form_gui->point_dragging = -1;
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
  dt_masks_clear_form_gui(darktable.develop);
  darktable.develop->form_visible = newform;
}

void dt_masks_reset_form_gui(void)
{
  dt_masks_change_form_gui(NULL);
  dt_iop_module_t *m = darktable.develop->gui_module;
  if(m && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
  {
    dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
    bd->masks_shown = DT_MASKS_EDIT_OFF;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), 0);
  }
}

void dt_masks_reset_show_masks_icons(void)
{
  if(darktable.develop->first_load) return;
  GList *modules = g_list_first(darktable.develop->iop);
  while(modules)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if((m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)m->blend_data;
      if(!bd) break;
      bd->masks_shown = DT_MASKS_EDIT_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
      gtk_widget_queue_draw(bd->masks_edit);
    }
    modules = g_list_next(modules);
  }
}

void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  dt_masks_form_t *grp = NULL;
  dt_masks_form_t *form = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
  if(value && form)
  {
    grp = dt_masks_create(DT_MASKS_GROUP);
    grp->formid = 0;
    dt_masks_group_ungroup(grp, form);
  }

  if (bd) bd->masks_shown = value;

  dt_masks_change_form_gui(grp);
  darktable.develop->form_gui->edit_mode = value;
  if(value && form)
    dt_dev_masks_selection_change(darktable.develop, form->formid, FALSE);
  else
    dt_dev_masks_selection_change(darktable.develop, 0, FALSE);

  dt_control_queue_redraw_center();
}

void dt_masks_set_edit_mode_single_form(struct dt_iop_module_t *module, const int formid,
                                        dt_masks_edit_mode_t value)
{
  if(!module) return;

  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);

  const int grid = module->blend_params->mask_id;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(form)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
    fpt->formid = formid;
    fpt->parentid = grid;
    fpt->state = DT_MASKS_STATE_USE;
    fpt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, fpt);
  }

  dt_masks_form_t *grp2 = dt_masks_create(DT_MASKS_GROUP);
  grp2->formid = 0;
  dt_masks_group_ungroup(grp2, grp);
  dt_masks_change_form_gui(grp2);
  darktable.develop->form_gui->edit_mode = value;

  if(value && form)
    dt_dev_masks_selection_change(darktable.develop, formid, FALSE);
  else
    dt_dev_masks_selection_change(darktable.develop, 0, FALSE);

  dt_control_queue_redraw_center();
}

void dt_masks_iop_edit_toggle_callback(GtkToggleButton *togglebutton, dt_iop_module_t *module)
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
                         (bd->masks_shown == DT_MASKS_EDIT_OFF ? DT_MASKS_EDIT_FULL : DT_MASKS_EDIT_OFF));
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  // we drop all the forms in the iop
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
  if(grp) dt_masks_form_remove(module, NULL, grp);
  module->blend_params->mask_id = 0;

  // and we update the iop
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  dt_masks_iop_update(module);

  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_dev_masks_list_change(darktable.develop);
}

static void _menu_add_circle(struct dt_iop_module_t *module)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *spot = dt_masks_create(DT_MASKS_CIRCLE);
  dt_masks_change_form_gui(spot);

  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_path(struct dt_iop_module_t *module)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_PATH);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_gradient(struct dt_iop_module_t *module)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *spot = dt_masks_create(DT_MASKS_GRADIENT);
  dt_masks_change_form_gui(spot);

  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_ellipse(struct dt_iop_module_t *module)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *spot = dt_masks_create(DT_MASKS_ELLIPSE);
  dt_masks_change_form_gui(spot);

  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_brush(struct dt_iop_module_t *module)
{
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);
  // we create the new form
  dt_masks_form_t *form = dt_masks_create(DT_MASKS_BRUSH);
  dt_masks_change_form_gui(form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;
  dt_control_queue_redraw_center();
}

static void _menu_add_exist(dt_iop_module_t *module, int formid)
{
  if(!module) return;
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, formid);
  if(!form) return;

  // is there already a masks group for this module ?
  int grpid = module->blend_params->mask_id;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
  if(!grp)
  {
    // we create a new group
    grp = dt_masks_create(DT_MASKS_GROUP);
    gchar *module_label = dt_history_item_get_name(module);
    snprintf(grp->name, sizeof(grp->name), "grp %s", module_label);
    g_free(module_label);
    _check_id(grp);
    darktable.develop->forms = g_list_append(darktable.develop->forms, grp);
    module->blend_params->mask_id = grpid = grp->formid;
  }
  // we add the form in this group
  dt_masks_group_add_form(grp, form);
  // we save the group
  dt_masks_write_form(grp, darktable.develop);

  // and we ensure that we are in edit mode
  dt_dev_add_history_item(darktable.develop, module, TRUE);
  dt_masks_iop_update(module);
  dt_dev_masks_list_change(darktable.develop);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

void dt_masks_iop_use_same_as(dt_iop_module_t *module, dt_iop_module_t *src)
{
  if(!module || !src) return;

  // we get the source group
  int srcid = src->blend_params->mask_id;
  dt_masks_form_t *src_grp = dt_masks_get_from_id(darktable.develop, srcid);
  if(!src_grp || src_grp->type != DT_MASKS_GROUP) return;

  // is there already a masks group for this module ?
  int grpid = module->blend_params->mask_id;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
  if(!grp)
  {
    // we create a new group
    grp = dt_masks_create(DT_MASKS_GROUP);
    gchar *module_label = dt_history_item_get_name(module);
    snprintf(grp->name, sizeof(grp->name), "grp %s", module_label);
    g_free(module_label);
    _check_id(grp);
    darktable.develop->forms = g_list_append(darktable.develop->forms, grp);
    module->blend_params->mask_id = grpid = grp->formid;
  }
  // we copy the src group in this group
  GList *points = g_list_first(src_grp->points);
  while(points)
  {
    dt_masks_point_group_t *pt = (dt_masks_point_group_t *)points->data;
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
    points = g_list_next(points);
  }

  // we save the group
  dt_masks_write_form(grp, darktable.develop);
}

void dt_masks_iop_combo_populate(GtkWidget *w, struct dt_iop_module_t **m)
{
  // we ensure that the module has focus
  dt_iop_module_t *module = *m;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  // we determine a higher approx of the entry number
  guint nbe = 5 + g_list_length(darktable.develop->forms) + g_list_length(darktable.develop->iop);
  free(bd->masks_combo_ids);
  bd->masks_combo_ids = malloc(nbe * sizeof(int));

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
  GList *forms = g_list_first(darktable.develop->forms);
  int nb = 0;
  while(forms)
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) || form->formid == module->blend_params->mask_id)
    {
      forms = g_list_next(forms);
      continue;
    }

    // we search were this form is used in the current module
    int used = 0;
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, module->blend_params->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP))
    {
      GList *pts = g_list_first(grp->points);
      while(pts)
      {
        dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
        if(pt->formid == form->formid)
        {
          used = 1;
          break;
        }
        pts = g_list_next(pts);
      }
    }
    if(!used)
    {
      if(nb == 0)
      {
        dt_bauhaus_combobox_add_aligned(combo, _("add existing shape"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
        cids[pos++] = 0; // nothing to do
      }
      dt_bauhaus_combobox_add(combo, form->name);
      cids[pos++] = form->formid;
      nb++;
    }

    forms = g_list_next(forms);
  }

  // masks from other iops
  GList *modules = g_list_first(darktable.develop->iop);
  nb = 0;
  int pos2 = 1;
  while(modules)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
    if((m != module) && (m->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(m->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, m->blend_params->mask_id);
      if(grp)
      {
        if(nb == 0)
        {
          dt_bauhaus_combobox_add_aligned(combo, _("use same shapes as"), DT_BAUHAUS_COMBOBOX_ALIGN_LEFT);
          cids[pos++] = 0; // nothing to do
        }
        gchar *module_label = dt_history_item_get_name(m);
        dt_bauhaus_combobox_add(combo, module_label);
        g_free(module_label);
        cids[pos++] = -1 * pos2;
        nb++;
      }
    }
    pos2++;
    modules = g_list_next(modules);
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  // we get the corresponding value
  dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;

  int sel = dt_bauhaus_combobox_get(bd->masks_combo);
  if(sel == 0) return;
  if(sel == 1)
  {
    darktable.gui->reset = 1;
    dt_bauhaus_combobox_set(bd->masks_combo, 0);
    darktable.gui->reset = 0;
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
      _menu_add_circle(module);
    }
    else if(val == -2000002)
    {
      // add a path shape
      _menu_add_path(module);
    }
    else if(val == -2000016)
    {
      // add a gradient shape
      _menu_add_gradient(module);
    }
    else if(val == -2000032)
    {
      // add a gradient shape
      _menu_add_ellipse(module);
    }
    else if(val == -2000064)
    {
      // add a brush shape
      _menu_add_brush(module);
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
        dt_dev_add_history_item(darktable.develop, module, TRUE);
        dt_masks_iop_update(module);
        dt_dev_masks_list_change(darktable.develop);
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

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form)
{
  if(!form) return;
  int id = form->formid;
  if(grp && !(grp->type & DT_MASKS_GROUP)) return;

  if(!(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) && grp)
  {
    // we try to remove the form from the masks group
    int ok = 0;
    GList *forms = g_list_first(grp->points);
    while(forms)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      if(grpt->formid == id)
      {
        ok = 1;
        grp->points = g_list_remove(grp->points, grpt);
        free(grpt);
        break;
      }
      forms = g_list_next(forms);
    }
    if(ok) dt_masks_write_form(grp, darktable.develop);
    if(ok && module)
    {
      dt_masks_iop_update(module);
      dt_masks_update_image(darktable.develop);
    }
    if(ok && g_list_length(grp->points) == 0) dt_masks_form_remove(module, NULL, grp);
    return;
  }

  if(form->type & DT_MASKS_GROUP && form->type & DT_MASKS_CLONE)
  {
    // when removing a cloning group the children have to be removed, too, as they won't be shown in the mask manager
    // and are thus not accessible afterwards.
    while(form->points)
    {
      dt_masks_point_group_t *group_child = (dt_masks_point_group_t *)form->points->data;
      dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, group_child->formid);
      dt_masks_form_remove(module, form, child);
      // no need to do anything to form->points, the recursive call will have removed child from the list
    }
  }

  // if we are here that mean we have to permanently delete this form
  // we drop the form from all modules
  GList *iops = g_list_first(darktable.develop->iop);
  while(iops)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
    if(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      int ok = 0;
      // is the form the base group of the iop ?
      if(id == m->blend_params->mask_id)
      {
        m->blend_params->mask_id = 0;
        dt_masks_iop_update(m);
        dt_dev_add_history_item(darktable.develop, m, TRUE);
      }
      else
      {
        dt_masks_form_t *iopgrp = dt_masks_get_from_id(darktable.develop, m->blend_params->mask_id);
        if(iopgrp && (iopgrp->type & DT_MASKS_GROUP))
        {
          GList *forms = g_list_first(iopgrp->points);
          while(forms)
          {
            dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
            if(grpt->formid == id)
            {
              ok = 1;
              iopgrp->points = g_list_remove(iopgrp->points, grpt);
              free(grpt);
              forms = g_list_first(iopgrp->points);
              continue;
            }
            forms = g_list_next(forms);
          }
          if(ok)
          {
            dt_masks_write_form(iopgrp, darktable.develop);
            dt_masks_iop_update(m);
            dt_masks_update_image(darktable.develop);
            if(g_list_length(iopgrp->points) == 0) dt_masks_form_remove(m, NULL, iopgrp);
          }
        }
      }
    }
    iops = g_list_next(iops);
  }
  // we drop the form from the general list
  GList *forms = g_list_first(darktable.develop->forms);
  while(forms)
  {
    dt_masks_form_t *f = (dt_masks_form_t *)forms->data;
    if(f->formid == id)
    {
      darktable.develop->forms = g_list_remove(darktable.develop->forms, f);
      dt_masks_write_forms(darktable.develop);
      break;
    }
    forms = g_list_next(forms);
  }
}

void dt_masks_form_change_opacity(dt_masks_form_t *form, int parentid, int up)
{
  if(!form) return;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, parentid);
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  // we first need to test if the opacity can be set to the form
  if(form->type & DT_MASKS_GROUP) return;
  int id = form->formid;
  float amount = 0.05f;
  if(!up) amount = -amount;

  // so we change the value inside the group
  GList *fpts = g_list_first(grp->points);
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    if(fpt->formid == id)
    {
      float nv = fpt->opacity + amount;
      if(nv <= 1.0f && nv >= 0.0f)
      {
        fpt->opacity = nv;
        dt_masks_write_form(grp, darktable.develop);
        dt_masks_update_image(darktable.develop);
        dt_dev_masks_list_update(darktable.develop);
      }
      break;
    }
    fpts = g_list_next(fpts);
  }
}

void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

  // we search the form in the group
  dt_masks_point_group_t *grpt = NULL;
  guint pos = 0;
  GList *fpts = g_list_first(grp->points);
  while(fpts)
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    if(fpt->formid == formid)
    {
      grpt = fpt;
      break;
    }
    pos++;
    fpts = g_list_next(fpts);
  }

  // we remove the form and readd it
  if(grpt)
  {
    if(up && pos == 0) return;
    if(!up && pos == g_list_length(grp->points) - 1) return;

    grp->points = g_list_remove(grp->points, grpt);
    if(up)
      pos -= 1;
    else
      pos += 1;
    grp->points = g_list_insert(grp->points, grpt, pos);
    dt_masks_write_form(grp, darktable.develop);
  }
}

static int _find_in_group(dt_masks_form_t *grp, int formid)
{
  if(!(grp->type & DT_MASKS_GROUP)) return 0;
  if(grp->formid == formid) return 1;
  GList *forms = g_list_first(grp->points);
  int nb = 0;
  while(forms)
  {
    dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
    dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, grpt->formid);
    if(form)
    {
      if(form->type & DT_MASKS_GROUP) nb += _find_in_group(form, formid);
    }
    forms = g_list_next(forms);
  }
  return nb;
}

dt_masks_point_group_t *dt_masks_group_add_form(dt_masks_form_t *grp, dt_masks_form_t *form)
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
    if(g_list_length(grp->points) > 0) grpt->state |= DT_MASKS_STATE_UNION;
    grpt->opacity = 1.0f;
    grp->points = g_list_append(grp->points, grpt);
    return grpt;
  }

  dt_control_log(_("masks can not contain themselves"));
  return NULL;
}

void dt_masks_group_ungroup(dt_masks_form_t *dest_grp, dt_masks_form_t *grp)
{
  if(!grp || !dest_grp) return;
  if(!(grp->type & DT_MASKS_GROUP) || !(dest_grp->type & DT_MASKS_GROUP)) return;

  GList *forms = g_list_first(grp->points);
  while(forms)
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
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = grpt->formid;
        fpt->parentid = grpt->parentid;
        fpt->state = grpt->state;
        fpt->opacity = grpt->opacity;
        dest_grp->points = g_list_append(dest_grp->points, fpt);
      }
    }
    forms = g_list_next(forms);
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

  GList *forms = g_list_first(form->points);
  while(forms)
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
    else if(form->type & DT_MASKS_CIRCLE)
    {
      pos += sizeof(dt_masks_point_circle_t);
    }
    else if(form->type & DT_MASKS_PATH)
    {
      pos += sizeof(dt_masks_point_path_t);
    }
    else if(form->type & DT_MASKS_GRADIENT)
    {
      pos += sizeof(dt_masks_point_gradient_t);
    }
    else if(form->type & DT_MASKS_ELLIPSE)
    {
      pos += sizeof(dt_masks_point_ellipse_t);
    }
    else if(form->type & DT_MASKS_BRUSH)
    {
      pos += sizeof(dt_masks_point_brush_t);
    }

    forms = g_list_next(forms);
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
  memcpy(str + pos, &form->source, 2 * sizeof(float));
  pos += 2 * sizeof(float);

  GList *forms = g_list_first(form->points);
  while(forms)
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
    else if(form->type & DT_MASKS_CIRCLE)
    {
      memcpy(str + pos, forms->data, sizeof(dt_masks_point_circle_t));
      pos += sizeof(dt_masks_point_circle_t);
    }
    else if(form->type & DT_MASKS_PATH)
    {
      memcpy(str + pos, forms->data, sizeof(dt_masks_point_path_t));
      pos += sizeof(dt_masks_point_path_t);
    }
    else if(form->type & DT_MASKS_GRADIENT)
    {
      memcpy(str + pos, forms->data, sizeof(dt_masks_point_gradient_t));
      pos += sizeof(dt_masks_point_gradient_t);
    }
    else if(form->type & DT_MASKS_ELLIPSE)
    {
      memcpy(str + pos, forms->data, sizeof(dt_masks_point_ellipse_t));
      pos += sizeof(dt_masks_point_ellipse_t);
    }
    else if(form->type & DT_MASKS_BRUSH)
    {
      memcpy(str + pos, forms->data, sizeof(dt_masks_point_brush_t));
      pos += sizeof(dt_masks_point_brush_t);
    }
    forms = g_list_next(forms);
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
  dt_dev_invalidate_all(dev);
}

static void _cleanup_unused_recurs(dt_develop_t *dev, int formid, int *used, int nb)
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
  dt_masks_form_t *form = dt_masks_get_from_id(dev, formid);
  if(form && (form->type & DT_MASKS_GROUP))
  {
    GList *grpts = g_list_first(form->points);
    while(grpts)
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)grpts->data;
      _cleanup_unused_recurs(dev, grpt->formid, used, nb);
      grpts = g_list_next(grpts);
    }
  }
}

void dt_masks_cleanup_unused(dt_develop_t *dev)
{
  // we create a table to store the ids of used forms
  guint nbf = g_list_length(dev->forms);
  int *used = calloc(nbf, sizeof(int));

  // now we iterate through all iop to find used forms
  GList *iops = g_list_first(dev->iop);
  while(iops)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iops->data;
    if(m->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      if(m->blend_params->mask_id > 0) _cleanup_unused_recurs(dev, m->blend_params->mask_id, used, nbf);
    }
    iops = g_list_next(iops);
  }

  // and we delete all unused forms
  GList *shapes = g_list_first(dev->forms);
  while(shapes)
  {
    dt_masks_form_t *f = (dt_masks_form_t *)shapes->data;
    int u = 0;
    for(int i = 0; i < nbf; i++)
    {
      if(used[i] == f->formid)
      {
        u = 1;
        break;
      }
      if(used[i] == 0) break;
    }

    shapes = g_list_next(shapes);

    if(u == 0)
    {
      dev->forms = g_list_remove(dev->forms, f);
    }
  }

  // and we save all that
  dt_masks_write_forms(dev);
  free(used);
}

int dt_masks_point_in_form_exact(float x, float y, float *points, int points_start, int points_count)
{
  // we use ray casting algorithm
  // to avoid most problems with horizontal segments, y should be rounded as int
  // so that there's very little chance than y==points...

  if(points_count > 2 + points_start)
  {
    float last = isnan(points[points_count * 2 - 1]) ? -INFINITY : points[points_count * 2 - 1];
    float yf = (float)y;
    int nb = 0;
    for(int i = points_start; i < points_count; i++)
    {
      float yy = points[i * 2 + 1];
      //if we need to skip points (in case of deleted point, because of self-intersection)
      if(isnan(points[i * 2]))
      {
        if(isnan(yy)) break;
        i = (int)yy - 1;
        continue;
      }
      if (((yf<=yy && yf>last) || (yf>=yy && yf<last)) && (points[i * 2] > x)) nb++;
      last = yy;
    }
    return (nb & 1);
  }
  return 0;
}

int dt_masks_point_in_form_near(float x, float y, float *points, int points_start, int points_count, float distance, int *near)
{
  // we use ray casting algorithm
  // to avoid most problems with horizontal segments, y should be rounded as int
  // so that there's very little chance than y==points...

  // TODO : distance is only evaluated in x, not y...

  if(points_count > 2 + points_start)
  {
    float last = isnan(points[points_count * 2 - 1]) ? -INFINITY : points[points_count * 2 - 1];
    float yf = (float)y;
    int nb = 0;
    for(int i = points_start; i < points_count; i++)
    {
      float yy = points[i * 2 + 1];
      //if we need to jump to skip points (in case of deleted point, because of self-intersection)
      if(isnan(points[i * 2]))
      {
        if(isnan(yy)) break;
        i = (int)yy - 1;
        continue;
      }
      if ((yf<=yy && yf>last) || (yf>=yy && yf<last))
      {
        if(points[i * 2] > x) nb++;
        if(points[i * 2] - x < distance && points[i * 2] - x > -distance) *near = 1;
      }
      last = yy;
    }
    return (nb & 1);
  }
  return 0;
}

// allow to select a shape inside an iop
void dt_masks_select_form(struct dt_iop_module_t *module, dt_masks_form_t *sel)
{
  int selection_changed = 0;

  if(sel)
  {
    if(sel->formid != darktable.develop->mask_form_selected_id)
    {
      darktable.develop->mask_form_selected_id = sel->formid;
      selection_changed = 1;
    }
  }
  else
  {
    if(darktable.develop->mask_form_selected_id != 0)
    {
      darktable.develop->mask_form_selected_id = 0;
      selection_changed = 1;
    }
  }
  if(selection_changed)
  {
    if(!module && darktable.develop->mask_form_selected_id == 0) module = darktable.develop->gui_module;
    if(module)
    {
      if(module->masks_selection_changed)
        module->masks_selection_changed(module, darktable.develop->mask_form_selected_id);
    }
  }
}

// draw a cross where the source position of a clone mask will be created
void dt_masks_draw_clone_source_pos(cairo_t *cr, const float zoom_scale, const float x, const float y)
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

// sets if the initial source position for a clone mask will be absolute or relative,
// based on mouse position and key state
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *gui, const uint32_t state, const float pzx,
                                           const float pzy)
{
  if((state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)) == (GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    gui->source_pos_type = DT_MASKS_SOURCE_POS_ABSOLUTE;
  else if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_state] unknown state for setting masks position type\n");

  // both source types record an absolute position,
  // for the relative type, the first time is used the position is recorded,
  // the second time a relative position is calculated based on that one
  gui->posx_source = pzx * darktable.develop->preview_pipe->backbuf_width;
  gui->posy_source = pzy * darktable.develop->preview_pipe->backbuf_height;
}

// set the initial source position value for a clone mask
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *gui, const int mask_type, dt_masks_form_t *form,
                                                   const float pzx, const float pzy)
{
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  const float iwd = darktable.develop->preview_pipe->iwidth;
  const float iht = darktable.develop->preview_pipe->iheight;

  // if this is the first time the relative pos is used
  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if is has not been defined by the user, set some default
    if(gui->posx_source == -1.f && gui->posy_source == -1.f)
    {
      if(mask_type & DT_MASKS_CIRCLE)
      {
        const float radius = MIN(0.5f, dt_conf_get_float("plugins/darkroom/spots/circle_size"));

        gui->posx_source = (radius * iwd);
        gui->posy_source = -(radius * iht);
      }
      else if(mask_type & DT_MASKS_ELLIPSE)
      {
        const float radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
        const float radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");

        gui->posx_source = (radius_a * iwd);
        gui->posy_source = -(radius_b * iht);
      }
      else if(mask_type & DT_MASKS_PATH)
      {
        gui->posx_source = (0.02f * iwd);
        gui->posy_source = (0.02f * iht);
      }
      else if(mask_type & DT_MASKS_BRUSH)
      {
        gui->posx_source = 0.01f * iwd;
        gui->posy_source = 0.01f * iht;
      }
      else
        fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unsuported masks type when calculating source position initial value\n");

      float pts[2] = { pzx * wd + gui->posx_source, pzy * ht + gui->posy_source };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      form->source[0] = pts[0] / iwd;
      form->source[1] = pts[1] / iht;
    }
    else
    {
      // if a position was defined by the user, use the absolute value the first time
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
    // original pos was already defined and relative value calculated, just use it
    float pts[2] = { pzx * wd + gui->posx_source, pzy * ht + gui->posy_source };
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
    fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unknown source position type\n");
}

// calculates the source position value for preview drawing, on cairo coordinates
void dt_masks_calculate_source_pos_value(dt_masks_form_gui_t *gui, const int mask_type, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos, float *px,
                                         float *py, const int adding)
{
  float x = 0.f, y = 0.f;

  const float iwd = darktable.develop->preview_pipe->iwidth;
  const float iht = darktable.develop->preview_pipe->iheight;

  if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    x = xpos + gui->posx_source;
    y = ypos + gui->posy_source;
  }
  else if(gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    if(gui->posx_source == -1.f && gui->posy_source == -1.f)
    {
      if(mask_type & DT_MASKS_CIRCLE)
      {
        const float radius = MIN(0.5f, dt_conf_get_float("plugins/darkroom/spots/circle_size"));
        x = xpos + radius * iwd;
        y = ypos - radius * iht;
      }
      else if(mask_type & DT_MASKS_ELLIPSE)
      {
        const float radius_a = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_a");
        const float radius_b = dt_conf_get_float("plugins/darkroom/spots/ellipse_radius_b");
        x = xpos + radius_a * iwd;
        y = ypos - radius_b * iht;
      }
      else if(mask_type & DT_MASKS_PATH)
      {
        x = xpos + 0.02f * iwd;
        y = ypos + 0.02f * iht;
      }
      else if(mask_type & DT_MASKS_BRUSH)
      {
        x = xpos + 0.01f * iwd;
        y = ypos + 0.01f * iht;
      }
      else
        fprintf(stderr, "[dt_masks_calculate_source_pos_value] unsuported masks type when calculating source position value\n");
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
    fprintf(stderr, "[dt_masks_calculate_source_pos_value] unknown source position type for setting source position value\n");

  *px = x;
  *py = y;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
