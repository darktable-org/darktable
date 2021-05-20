/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

float get_mask_opacity(dt_masks_form_gui_t *gui, const dt_masks_form_t *form)
{
  float opacity = -1.f;

  if(gui && form && ((int)form->type & DT_MASKS_GROUP) && (gui->group_edited >= 0))
  {
    // we have a form
    const dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    const dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return -1.f;
    const int formid = sel->formid;

    // look for opacity
    const dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, fpt->parentid);
    if(!grp || !(grp->type & DT_MASKS_GROUP)) return 0;

    for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
    {
      const dt_masks_point_group_t *fptt = (dt_masks_point_group_t *)fpts->data;
      if(fptt->formid == formid)
      {
        opacity = fptt->opacity;
        break;
      }
    }
  }
  else
  {
    // we have nothing, fetch global pref
    opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
  }

  return opacity;
}

void set_mask_opacity(dt_masks_form_gui_t *gui, dt_masks_form_t *form, const float opacity)
{
  if(gui && form && ((int)form->type & DT_MASKS_GROUP) && (gui->group_edited >= 0))
  {
    // we have a form
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
    const dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel) return;
    const int formid = sel->formid;

    // look for opacity
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, fpt->parentid);
    if(!grp || !(grp->type & DT_MASKS_GROUP)) return;

    for(GList *fpts = grp->points; fpts; fpts = g_list_next(fpts))
    {
      dt_masks_point_group_t *fptt = (dt_masks_point_group_t *)fpts->data;
      if(fptt->formid == formid)
      {
        fptt->opacity = opacity;
        break;
      }
    }
  }

  // save in global pref for later
  dt_conf_set_float("plugins/darkroom/masks/opacity", opacity);
}


float get_mask_hardness(dt_masks_form_gui_t *gui, dt_masks_form_t *form)
{
  dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
  const dt_masks_form_t *selected = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!selected) return -1.f;

  if(selected->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((selected->points)->data);
    return circle->border / circle->radius;
  }
  if(selected->type & DT_MASKS_BRUSH)
  {
    size_t num_points = 0;
    float avg_hardness = 0.f;
    for(GList *l = selected->points; l; l = g_list_next(l))
    {
      dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)l->data;
      avg_hardness += point->hardness;
      num_points++;
    }
    return avg_hardness / num_points;
  }
  else
    return -1.f;
}

void set_mask_hardness(dt_masks_form_gui_t *gui, dt_masks_form_t *form, const float hardness)
{
  dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)g_list_nth_data(form->points, gui->group_edited);
  const dt_masks_form_t *selected = dt_masks_get_from_id(darktable.develop, fpt->formid);
  if(!selected) return;

  if(selected->type & DT_MASKS_CIRCLE)
  {
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *)((selected->points)->data);
    circle->border = hardness * circle->radius;
  }
  if(selected->type & DT_MASKS_BRUSH)
  {
    size_t num_points = 0;
    float avg_hardness = 0.f;
    for(GList *l = selected->points; l; l = g_list_next(l))
    {
      dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)l->data;
      avg_hardness += point->hardness;
      num_points++;
    }
    avg_hardness /= num_points;

    for(GList *l = selected->points; l; l = g_list_next(l))
    {
      dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)l->data;
      point->hardness = point->hardness / avg_hardness * hardness;
    }
  }
}
