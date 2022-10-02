/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#pragma GCC diagnostic ignored "-Wshadow"

static void _lib_masks_recreate_list(dt_lib_module_t *self);
static void _lib_masks_update_list(dt_lib_module_t *self);

typedef struct dt_lib_masks_t
{
  /* vbox with managed history items */
  GtkWidget *hbox;
  GtkWidget *bt_circle, *bt_path, *bt_gradient, *bt_ellipse, *bt_brush;
  GtkWidget *treeview;
  dt_gui_collapsible_section_t cs;
  GtkWidget *property[DT_MASKS_PROPERTY_LAST];
  float last_value[DT_MASKS_PROPERTY_LAST];
  GtkWidget *none_label;

  GdkPixbuf *ic_inverse, *ic_union, *ic_intersection, *ic_difference, *ic_exclusion, *ic_used;
  int gui_reset;
} dt_lib_masks_t;


const char *name(dt_lib_module_t *self)
{
  return _("mask manager");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 10;
}

typedef enum dt_masks_tree_cols_t
{
  TREE_TEXT = 0,
  TREE_MODULE,
  TREE_GROUPID,
  TREE_FORMID,
  TREE_EDITABLE,
  TREE_IC_OP,
  TREE_IC_OP_VISIBLE,
  TREE_IC_INVERSE,
  TREE_IC_INVERSE_VISIBLE,
  TREE_IC_USED,
  TREE_IC_USED_VISIBLE,
  TREE_USED_TEXT,
  TREE_COUNT
} dt_masks_tree_cols_t;

const struct
{
  gchar *name;
  gchar *format;
  float min, max;
  gboolean relative;
} _masks_properties[DT_MASKS_PROPERTY_LAST]
  = { [ DT_MASKS_PROPERTY_OPACITY] = {N_("opacity"), "%", 0, 1, FALSE },
      [ DT_MASKS_PROPERTY_SIZE] = { N_("size"), "%", 0.0001, 1, TRUE },
      [ DT_MASKS_PROPERTY_HARDNESS] = { N_("hardness"), "%", 0.0001, 1, TRUE },
      [ DT_MASKS_PROPERTY_FEATHER] = { N_("feather"), "%", 0.0001, 1, TRUE },
      [ DT_MASKS_PROPERTY_ROTATION] = { N_("rotation"), "°", 0, 360, FALSE },
      [ DT_MASKS_PROPERTY_CURVATURE] = { N_("curvature"), "%", 0.0001, 1, FALSE },
      [ DT_MASKS_PROPERTY_COMPRESSION] = { N_("compression"), "%", 0.0001, 1, TRUE },
};

gboolean _timeout_show_all_feathers(gpointer userdata)
{
  dt_masks_form_gui_t *gui = userdata;
  gui->show_all_feathers = 0;
  dt_control_queue_redraw_center();
  return G_SOURCE_REMOVE;
}

static void _property_changed(GtkWidget *widget, dt_masks_property_t prop)
{
  if(darktable.gui->reset) return;

  dt_lib_module_t *self = darktable.develop->proxy.masks.module;
  dt_lib_masks_t *d = self->data;

  float value = dt_bauhaus_slider_get(widget);

  dt_develop_t *dev = darktable.develop;
  dt_masks_form_t *form = dev->form_visible;
  dt_masks_form_gui_t *gui = dev->form_gui;
  if(!gui) return;
  if(!form) return;

  int count = 0, pos = 0;
  float sum = 0, min = _masks_properties[prop].min, max = _masks_properties[prop].max;
  if(_masks_properties[prop].relative)
  {
    max /= min;
    min /= _masks_properties[prop].max;
  }
  else
  {
    max -= min;
    min -= _masks_properties[prop].max;
  }

  if(gui->creation && form->functions && form->functions->modify_property)
    form->functions->modify_property(form, prop, d->last_value[prop], value, &sum, &count, &min, &max);
  else
  for(GList *fpts = form->points; fpts; fpts = g_list_next(fpts))
  {
    dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)fpts->data;
    dt_masks_form_t *sel = dt_masks_get_from_id(darktable.develop, fpt->formid);
    if(!sel || (dev->mask_form_selected_id && dev->mask_form_selected_id != sel->formid)) continue;;

    if(prop == DT_MASKS_PROPERTY_OPACITY && fpt->parentid)
    {
      float new_opacity = dt_masks_form_change_opacity(sel, fpt->parentid, value - d->last_value[prop]);
      sum += new_opacity;
      max = fminf(max, 1.0f - new_opacity);
      min = fmaxf(min, .05f - new_opacity);
      ++count;
    }
    else
    {
      int saved_count = count;

      if(sel->functions && sel->functions->modify_property)
        sel->functions->modify_property(sel, prop, d->last_value[prop], value, &sum, &count, &min, &max);

      if(value != d->last_value[prop] && count != saved_count && value != d->last_value[prop])
      {
        // we recreate the form points
        dt_masks_gui_form_remove(sel, gui, pos);
        dt_masks_gui_form_create(sel, gui, pos, dev->gui_module);
      }
    }
    pos++;
  }

  if(count)
  {
    if(value != d->last_value[prop] && sum / count != d->last_value[prop] && prop != DT_MASKS_PROPERTY_OPACITY)
    {
      if(gui->show_all_feathers) g_source_remove(gui->show_all_feathers);
      gui->show_all_feathers = g_timeout_add_seconds(2, _timeout_show_all_feathers, gui);

      // we save the new parameters
      dt_dev_add_masks_history_item(darktable.develop, dev->gui_module, TRUE);
      dt_masks_update_image(darktable.develop);
    }

    ++darktable.gui->reset;
    if(_masks_properties[prop].relative)
    {
      max *= sum / count;
      min *= sum / count;
    }
    else
    {
      max += sum / count;
      min += sum / count;
    }
    dt_bauhaus_slider_set_soft_range(widget, min, max);

    dt_bauhaus_slider_set(widget, sum / count);
    d->last_value[prop] = dt_bauhaus_slider_get(widget);
    --darktable.gui->reset;

    gtk_widget_show(widget);
    gtk_widget_hide(d->none_label);
  }
  else
    gtk_widget_hide(widget);
}

static void _update_all_properties(dt_lib_masks_t *self)
{
  gtk_widget_show(self->none_label);

  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
    _property_changed(self->property[i], i);
}

static void _lib_masks_get_values(GtkTreeModel *model, GtkTreeIter *iter,
                                  dt_iop_module_t **module, int *groupid, int *formid)
{
  // returns module & groupid & formid if requested
  if(module ) gtk_tree_model_get(model, iter, TREE_MODULE, module, -1);
  if(groupid) gtk_tree_model_get(model, iter, TREE_GROUPID, groupid, -1);
  if(formid ) gtk_tree_model_get(model, iter, TREE_FORMID, formid, -1);
}

static void _lib_masks_inactivate_icons(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we set the add shape icons inactive
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_circle), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_ellipse), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_path), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_gradient), FALSE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(lm->bt_brush), FALSE);
}

static void _tree_add_shape(GtkButton *button, gpointer shape)
{
  dt_iop_module_t *module = NULL;

  dt_lib_masks_t *lm = darktable.develop->proxy.masks.module->data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GtkTreeModel *model = NULL;
  GList *selected = gtk_tree_selection_get_selected_rows(selection, &model);
  if(selected)
  {
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, selected->data))
      _lib_masks_get_values(model, &iter, &module, NULL, NULL);
    g_list_free_full(selected, (GDestroyNotify) gtk_tree_path_free);
  }

  // we create the new form
  dt_masks_form_t *spot = dt_masks_create(GPOINTER_TO_INT(shape));
  dt_masks_change_form_gui(spot);
  darktable.develop->form_gui->creation_module = module;
  darktable.develop->form_gui->group_selected = 0;
  dt_control_queue_redraw_center();
}

static void _bt_add_shape(GtkWidget *widget, GdkEventButton *event, gpointer shape)
{
  if(darktable.gui->reset) return;

  if(event->button == 1)
  {
    _tree_add_shape(NULL, shape);
    if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
      darktable.develop->form_gui->creation_continuous = TRUE;
      darktable.develop->form_gui->creation_continuous_module =
        darktable.develop->form_gui->creation_module;
    }

    _lib_masks_inactivate_icons(darktable.develop->proxy.masks.module);
  }
}

static void _tree_add_exist(GtkButton *button, dt_masks_form_t *grp)
{
  if(!grp || !(grp->type & DT_MASKS_GROUP)) return;
  // we get the new formid
  const int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "formid"));
  dt_iop_module_t *module = g_object_get_data(G_OBJECT(button), "module");

  // we add the form in this group
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(form && dt_masks_group_add_form(grp, form))
  {
    // we save the group
    dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);

    // and we apply the change
    dt_masks_update_image(darktable.develop);
    dt_masks_iop_update(module);
    dt_dev_masks_selection_change(darktable.develop, NULL, grp->formid, TRUE);
  }
}

static void _tree_group(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // we create the new group
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  snprintf(grp->name, sizeof(grp->name), _("group #%d"), g_list_length(darktable.develop->forms));

  // we add all selected forms to this group
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  int pos = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, NULL, &id);

      if(id > 0)
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = id;
        fpt->parentid = grp->formid;
        fpt->opacity = 1.0f;
        fpt->state = DT_MASKS_STATE_USE;
        if(pos > 0) fpt->state |= DT_MASKS_STATE_UNION;
        grp->points = g_list_append(grp->points, fpt);
        pos++;
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  // we add this group to the general list
  darktable.develop->forms = g_list_append(darktable.develop->forms, grp);

  // add we save
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
  _lib_masks_recreate_list(self);
  // dt_masks_change_form_gui(grp);
}

static void _set_iter_name(dt_lib_masks_t *lm, dt_masks_form_t *form, int state, float opacity,
                           GtkTreeModel *model, GtkTreeIter *iter)
{
  if(!form) return;

  char str[256] = "";
  g_strlcat(str, form->name, sizeof(str));

  if(opacity != 1.0f)
  {
    char str2[256] = "";
    g_strlcpy(str2, str, sizeof(str2));
    snprintf(str, sizeof(str), "%s %d%%", str2, (int)(opacity * 100));
  }

  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;
  if(state & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(state & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(state & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(state & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;
  if(state & DT_MASKS_STATE_INVERSE) icinv = lm->ic_inverse;

  gtk_tree_store_set(GTK_TREE_STORE(model), iter, TREE_TEXT, str, TREE_IC_OP, icop, TREE_IC_OP_VISIBLE,
                     (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE, (icinv != NULL), -1);
}

static void _tree_cleanup(GtkButton *button, dt_lib_module_t *self)
{
  dt_masks_cleanup_unused(darktable.develop);
  _lib_masks_recreate_list(self);
}

static void _add_masks_history_item(dt_lib_masks_t *lm)
{
  const int reset = lm->gui_reset;
  lm->gui_reset = 1;
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
  lm->gui_reset = reset;
}


static void _tree_inverse(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
          if(pt->formid == id)
          {
            if(pt->state & DT_MASKS_STATE_INVERSE)
              pt->state &= ~DT_MASKS_STATE_INVERSE;
            else
              pt->state |= DT_MASKS_STATE_INVERSE;
            _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                           &iter);
            change = 1;
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static void _tree_intersection(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
          if(pt->formid == id)
          {
            if(!(pt->state & DT_MASKS_STATE_INTERSECTION))
            {
              if(pt->state & DT_MASKS_STATE_DIFFERENCE)
                pt->state &= ~DT_MASKS_STATE_DIFFERENCE;
              else if(pt->state & DT_MASKS_STATE_UNION)
                pt->state &= ~DT_MASKS_STATE_UNION;
              else if(pt->state & DT_MASKS_STATE_EXCLUSION)
                pt->state &= ~DT_MASKS_STATE_EXCLUSION;
              pt->state |= DT_MASKS_STATE_INTERSECTION;
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter);
              change = 1;
            }
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static void _tree_difference(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
          if(pt->formid == id)
          {
            if(!(pt->state & DT_MASKS_STATE_DIFFERENCE))
            {
              if(pt->state & DT_MASKS_STATE_UNION)
                pt->state &= ~DT_MASKS_STATE_UNION;
              else if(pt->state & DT_MASKS_STATE_INTERSECTION)
                pt->state &= ~DT_MASKS_STATE_INTERSECTION;
              else if(pt->state & DT_MASKS_STATE_EXCLUSION)
                pt->state &= ~DT_MASKS_STATE_EXCLUSION;
              pt->state |= DT_MASKS_STATE_DIFFERENCE;
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter);
              change = 1;
            }
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static void _tree_exclusion(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
          if(pt->formid == id)
          {
            if(!(pt->state & DT_MASKS_STATE_EXCLUSION))
            {
              if(pt->state & DT_MASKS_STATE_DIFFERENCE)
                pt->state &= ~DT_MASKS_STATE_DIFFERENCE;
              else if(pt->state & DT_MASKS_STATE_INTERSECTION)
                pt->state &= ~DT_MASKS_STATE_INTERSECTION;
              else if(pt->state & DT_MASKS_STATE_UNION)
                pt->state &= ~DT_MASKS_STATE_UNION;
              pt->state |= DT_MASKS_STATE_EXCLUSION;
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter);
              change = 1;
            }
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static void _tree_union(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  int change = 0;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);
      if(grp && (grp->type & DT_MASKS_GROUP))
      {
        // we search the entry to inverse
        for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
          if(pt->formid == id)
          {
            if(!(pt->state & DT_MASKS_STATE_UNION))
            {
              if(pt->state & DT_MASKS_STATE_DIFFERENCE)
                pt->state &= ~DT_MASKS_STATE_DIFFERENCE;
              else if(pt->state & DT_MASKS_STATE_INTERSECTION)
                pt->state &= ~DT_MASKS_STATE_INTERSECTION;
              else if(pt->state & DT_MASKS_STATE_EXCLUSION)
                pt->state &= ~DT_MASKS_STATE_EXCLUSION;
              pt->state |= DT_MASKS_STATE_UNION;
              _set_iter_name(lm, dt_masks_get_from_id(darktable.develop, id), pt->state, pt->opacity, model,
                             &iter);
              change = 1;
            }
            break;
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  if(change)
  {
    _add_masks_history_item(lm);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
  }
}

static void _tree_moveup(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 0);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);
  dt_masks_update_image(darktable.develop);
}

static void _tree_movedown(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_move(dt_masks_get_from_id(darktable.develop, grid), id, 1);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);
  dt_masks_update_image(darktable.develop);
}

static void _tree_delete_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we first discard all visible shapes
  dt_masks_change_form_gui(NULL);

  // now we go through all selected nodes
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  dt_iop_module_t *module = NULL;
  lm->gui_reset = 1;
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, &module, &grid, &id);

      dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, grid),
                           dt_masks_get_from_id(darktable.develop, id));
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  lm->gui_reset = 0;
  _lib_masks_recreate_list(self);
}

static void _tree_duplicate_shape(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  // we get the selected node
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  if(!items) return;
  GtkTreePath *item = (GtkTreePath *)items->data;
  GtkTreeIter iter;
  if(gtk_tree_model_get_iter(model, &iter, item))
  {
    int id = -1;
    _lib_masks_get_values(model, &iter, NULL, NULL, &id);

    const int nid = dt_masks_form_duplicate(darktable.develop, id);
    if(nid > 0)
    {
      dt_dev_masks_selection_change(darktable.develop, NULL, nid, TRUE);
      //_lib_masks_recreate_list(self);
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
}

static void _tree_cell_edited(GtkCellRendererText *cell, gchar *path_string, gchar *new_text,
                              dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(model, &iter, path_string)) return;

  int id = -1;
  _lib_masks_get_values(model, &iter, NULL, NULL, &id);
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return;

  // we want to make sure that the new name is not an empty string. else this would convert
  // in the xmp file into "<rdf:li/>" which produces problems. we use a single whitespace
  // as the pure minimum text.
  gchar *text = strlen(new_text) == 0 ? " " : new_text;

  // first, we need to update the mask name

  g_strlcpy(form->name, text, sizeof(form->name));
  dt_dev_add_masks_history_item(darktable.develop, NULL, FALSE);
}

static void _tree_selection_change(GtkTreeSelection *selection, dt_lib_masks_t *self)
{
  if(self->gui_reset) return;
  // we reset all "show mask" icon of iops
  dt_masks_reset_show_masks_icons();

  // if selection empty, we hide all
  const int nb = gtk_tree_selection_count_selected_rows(selection);
  if(nb == 0)
  {
    dt_masks_change_form_gui(NULL);
    dt_control_queue_redraw_center();
    return;
  }

  // else, we create a new form group with the selection and display it
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(self->treeview));
  dt_masks_form_t *grp = dt_masks_create(DT_MASKS_GROUP);
  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  for(const GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int grid = -1;
      int id = -1;
      _lib_masks_get_values(model, &iter, NULL, &grid, &id);

      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
      if(form)
      {
        dt_masks_point_group_t *fpt = (dt_masks_point_group_t *)malloc(sizeof(dt_masks_point_group_t));
        fpt->formid = id;
        fpt->parentid = grid;
        fpt->state = DT_MASKS_STATE_USE;
        fpt->opacity = 1.0f;
        grp->points = g_list_append(grp->points, fpt);
        // we eventually set the "show masks" icon of iops
        if(nb == 1 && (form->type & DT_MASKS_GROUP))
        {
          dt_iop_module_t *module = NULL;
          _lib_masks_get_values(model, &iter, &module, NULL, NULL);

          if(module && module->blend_data
             && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
             && !(module->flags() & IOP_FLAGS_NO_MASKS))
          {
            dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)module->blend_data;
            bd->masks_shown = 1;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), TRUE);
            gtk_widget_queue_draw(bd->masks_edit);
          }
        }
      }
    }
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);

  dt_masks_form_t *grp2 = dt_masks_create(DT_MASKS_GROUP);
  grp2->formid = 0;
  dt_masks_group_ungroup(grp2, grp);

  // don't call dt_masks_change_form_gui because it triggers a selection change again
  dt_masks_clear_form_gui(darktable.develop);
  darktable.develop->form_visible = grp2;

  // update sticky accels window
  if(darktable.view_manager->accels_window.window && darktable.view_manager->accels_window.sticky)
    dt_view_accels_refresh(darktable.view_manager);

  darktable.develop->form_gui->edit_mode = DT_MASKS_EDIT_FULL;
  dt_control_queue_redraw_center();

  _update_all_properties(self);
}

static int _tree_button_pressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  // we first need to adjust selection
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));

  GtkTreePath *mouse_path = NULL;
  GtkTreeIter iter;
  dt_iop_module_t *module = NULL;
  int on_row = 0;
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &mouse_path, NULL,
                                   NULL, NULL))
  {
    on_row = 1;
    // we retrieve the iter and module from path
    if(gtk_tree_model_get_iter(model, &iter, mouse_path))
    {
      _lib_masks_get_values(model, &iter, &module, NULL, NULL);
    }
  }
  /* single click with the right mouse button? */
  if(event->type == GDK_BUTTON_PRESS && event->button == 1)
  {
    // if click on a blank space, then deselect all
    if(!on_row)
    {
      gtk_tree_selection_unselect_all(selection);
    }
  }
  else if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    // if we are already inside the selection, no change
    if(on_row && !gtk_tree_selection_path_is_selected(selection, mouse_path))
    {
      if(!dt_modifier_is(event->state, GDK_CONTROL_MASK)) gtk_tree_selection_unselect_all(selection);
      gtk_tree_selection_select_path(selection, mouse_path);
      gtk_tree_path_free(mouse_path);
    }

    // and we display the context-menu
    GtkMenuShell *menu = GTK_MENU_SHELL(gtk_menu_new());
    GtkWidget *item;

    // we get all infos from selection
    const int nb = gtk_tree_selection_count_selected_rows(selection);
    int from_group = 0;

    int grpid = 0;
    int depth = 0;
    dt_masks_form_t *grp = NULL;

    if(nb > 0)
    {
      GList *selected = gtk_tree_selection_get_selected_rows(selection, NULL);
      GtkTreePath *it0 = (GtkTreePath *)selected->data;
      depth = gtk_tree_path_get_depth(it0);
      if(nb == 1)
      {
        // before freeing the list of selected rows, we check if the form is a group or not
        if(gtk_tree_model_get_iter(model, &iter, it0))
        {
          _lib_masks_get_values(model, &iter, NULL, NULL, &grpid);
          grp = dt_masks_get_from_id(darktable.develop, grpid);
        }
      }
      g_list_free_full(selected, (GDestroyNotify)gtk_tree_path_free);
    }
    if(depth > 1) from_group = 1;

    if(nb == 0 || (grp && grp->type & DT_MASKS_GROUP))
    {
      item = gtk_menu_item_new_with_label(_("add brush"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_shape, GINT_TO_POINTER(DT_MASKS_BRUSH));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(_("add circle"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_shape, GINT_TO_POINTER(DT_MASKS_CIRCLE));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(_("add ellipse"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_shape, GINT_TO_POINTER(DT_MASKS_ELLIPSE));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(_("add path"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_shape, GINT_TO_POINTER(DT_MASKS_PATH));
      gtk_menu_shell_append(menu, item);

      item = gtk_menu_item_new_with_label(_("add gradient"));
      g_signal_connect(item, "activate", (GCallback)_tree_add_shape, GINT_TO_POINTER(DT_MASKS_GRADIENT));
      gtk_menu_shell_append(menu, item);
    }

    if(grp && grp->type & DT_MASKS_GROUP)
    {
      // existing forms
      gboolean has_unused_shapes = FALSE;
      GtkWidget *menu0 = gtk_menu_new();
      for(GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
      {
        dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
        if((form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) || form->formid == grpid)
        {
          continue;
        }
        char str[10000] = "";
        g_strlcat(str, form->name, sizeof(str));
        int nbuse = 0;

        // we search were this form is used
        for(const GList *modules = darktable.develop->iop; modules; modules = g_list_next(modules))
        {
          dt_iop_module_t *m = (dt_iop_module_t *)modules->data;
          dt_masks_form_t *grp = dt_masks_get_from_id(m->dev, m->blend_params->mask_id);
          if(grp && (grp->type & DT_MASKS_GROUP))
          {
            for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
            {
              dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
              if(pt->formid == form->formid)
              {
                if(m == module)
                {
                  nbuse = -1;
                  break;
                }
                if(nbuse == 0) g_strlcat(str, " (", sizeof(str));
                g_strlcat(str, " ", sizeof(str));
                gchar *module_label = dt_history_item_get_name(m);
                g_strlcat(str, module_label, sizeof(str));
                g_free(module_label);
                nbuse++;
              }
            }
          }
        }
        if(nbuse != -1)
        {
          if(nbuse > 0) g_strlcat(str, " )", sizeof(str));

          // we add the menu entry
          item = gtk_menu_item_new_with_label(str);
          g_object_set_data(G_OBJECT(item), "formid", GUINT_TO_POINTER(form->formid));
          g_object_set_data(G_OBJECT(item), "module", module);
          g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(_tree_add_exist), grp);
          gtk_menu_shell_append(GTK_MENU_SHELL(menu0), item);
          has_unused_shapes = TRUE;
        }
      }

      if(has_unused_shapes)
      {
        item = gtk_menu_item_new_with_label(_("add existing shape"));
        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), menu0);
        gtk_menu_shell_append(menu, item);
      }
    }

    if(!from_group && nb > 0)
    {
      dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grpid);
      if(!(grp && (grp->type & DT_MASKS_GROUP)))
      {
        if(nb == 1)
        {
          item = gtk_menu_item_new_with_label(_("duplicate this shape"));
          g_signal_connect(item, "activate", (GCallback)_tree_duplicate_shape, self);
          gtk_menu_shell_append(menu, item);
        }
        item = gtk_menu_item_new_with_label(_("delete this shape"));
        g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
        gtk_menu_shell_append(menu, item);
      }
      else
      {
        item = gtk_menu_item_new_with_label(_("delete group"));
        g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
        gtk_menu_shell_append(menu, item);
      }
    }
    else if(nb > 0 && depth < 3)
    {
      item = gtk_menu_item_new_with_label(_("remove from group"));
      g_signal_connect(item, "activate", (GCallback)_tree_delete_shape, self);
      gtk_menu_shell_append(menu, item);
    }

    if(nb > 1 && !from_group)
    {
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("group the forms"));
      g_signal_connect(item, "activate", (GCallback)_tree_group, self);
      gtk_menu_shell_append(menu, item);
    }

    if(from_group && depth < 3)
    {
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("use inverted shape"));
      g_signal_connect(item, "activate", (GCallback)_tree_inverse, self);
      gtk_menu_shell_append(menu, item);
      if(nb == 1)
      {
        gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
        item = gtk_menu_item_new_with_label(_("mode: union"));
        g_signal_connect(item, "activate", (GCallback)_tree_union, self);
        gtk_menu_shell_append(menu, item);
        item = gtk_menu_item_new_with_label(_("mode: intersection"));
        g_signal_connect(item, "activate", (GCallback)_tree_intersection, self);
        gtk_menu_shell_append(menu, item);
        item = gtk_menu_item_new_with_label(_("mode: difference"));
        g_signal_connect(item, "activate", (GCallback)_tree_difference, self);
        gtk_menu_shell_append(menu, item);
        item = gtk_menu_item_new_with_label(_("mode: exclusion"));
        g_signal_connect(item, "activate", (GCallback)_tree_exclusion, self);
        gtk_menu_shell_append(menu, item);
      }
      gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
      item = gtk_menu_item_new_with_label(_("move up"));
      g_signal_connect(item, "activate", (GCallback)_tree_moveup, self);
      gtk_menu_shell_append(menu, item);
      item = gtk_menu_item_new_with_label(_("move down"));
      g_signal_connect(item, "activate", (GCallback)_tree_movedown, self);
      gtk_menu_shell_append(menu, item);
    }

    gtk_menu_shell_append(menu, gtk_separator_menu_item_new());
    item = gtk_menu_item_new_with_label(_("cleanup unused shapes"));
    g_signal_connect(item, "activate", (GCallback)_tree_cleanup, self);
    gtk_menu_shell_append(menu, item);

    gtk_widget_show_all(GTK_WIDGET(menu));

    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

    return 1;
  }

  return 0;
}

static gboolean _tree_restrict_select(GtkTreeSelection *selection, GtkTreeModel *model, GtkTreePath *path,
                                      gboolean path_currently_selected, gpointer data)
{
  dt_lib_masks_t *self = (dt_lib_masks_t *)data;
  if(self->gui_reset) return TRUE;

  // if the change is SELECT->UNSELECT no pb
  if(path_currently_selected) return TRUE;

  // if selection is empty, no pb
  if(gtk_tree_selection_count_selected_rows(selection) == 0) return TRUE;

  // now we unselect all members of selection with not the same parent node
  // idem for all those with a different depth
  int *indices = gtk_tree_path_get_indices(path);
  int depth = gtk_tree_path_get_depth(path);

  GList *items = gtk_tree_selection_get_selected_rows(selection, NULL);
  GList *items_iter = items;
  while(items_iter)
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    int dd = gtk_tree_path_get_depth(item);
    int *ii = gtk_tree_path_get_indices(item);
    int ok = 1;
    if(dd != depth)
      ok = 0;
    else if(dd == 1)
      ok = 1;
    else if(ii[dd - 2] != indices[dd - 2])
      ok = 0;
    if(!ok)
    {
      gtk_tree_selection_unselect_path(selection, item);
      g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
      items_iter = items = gtk_tree_selection_get_selected_rows(selection, NULL);
      continue;
    }
    items_iter = g_list_next(items_iter);
  }
  g_list_free_full(items, (GDestroyNotify)gtk_tree_path_free);
  return TRUE;
}

static gboolean _tree_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_tip,
                                    GtkTooltip *tooltip, gpointer data)
{
  GtkTreeIter iter;
  GtkTreeView *tree_view = GTK_TREE_VIEW(widget);
  GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
  GtkTreePath *path = NULL;
  gchar *tmp = NULL;
  gboolean show = FALSE;

  if(!gtk_tree_view_get_tooltip_context(tree_view, &x, &y, keyboard_tip, &model, &path, &iter)) return FALSE;

  gtk_tree_model_get(model, &iter, TREE_IC_USED_VISIBLE, &show, TREE_USED_TEXT, &tmp, -1);
  if(show)
  {
    gtk_tooltip_set_markup(tooltip, tmp);
    gtk_tree_view_set_tooltip_row(tree_view, tooltip, path);
  }

  gtk_tree_path_free(path);
  g_free(tmp);

  return show;
}

static void _is_form_used(int formid, dt_masks_form_t *grp, char *text, size_t text_length, int *nb)
{
  if(!grp)
  {
    for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
    {
      dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
      if(form->type & DT_MASKS_GROUP) _is_form_used(formid, form, text, text_length, nb);
    }
  }
  else if(grp->type & DT_MASKS_GROUP)
  {
    for(const GList *points = grp->points; points; points = g_list_next(points))
    {
      dt_masks_point_group_t *point = (dt_masks_point_group_t *)points->data;
      dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, point->formid);
      if(form)
      {
        if(point->formid == formid)
        {
          (*nb)++;
          if(*nb > 1) g_strlcat(text, "\n", text_length);
          g_strlcat(text, grp->name, text_length);
        }
        if(form->type & DT_MASKS_GROUP) _is_form_used(formid, form, text, text_length, nb);
      }
    }
  }
}

static void _lib_masks_list_recurs(GtkTreeStore *treestore, GtkTreeIter *toplevel, dt_masks_form_t *form,
                                   int grp_id, dt_iop_module_t *module, int gstate, float opacity,
                                   dt_lib_masks_t *lm)
{
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE)) return;
  // we create the text entry
  char str[256] = "";
  g_strlcat(str, form->name, sizeof(str));
  // we get the right pixbufs
  GdkPixbuf *icop = NULL;
  GdkPixbuf *icinv = NULL;
  GdkPixbuf *icuse = NULL;
  if(gstate & DT_MASKS_STATE_UNION)
    icop = lm->ic_union;
  else if(gstate & DT_MASKS_STATE_INTERSECTION)
    icop = lm->ic_intersection;
  else if(gstate & DT_MASKS_STATE_DIFFERENCE)
    icop = lm->ic_difference;
  else if(gstate & DT_MASKS_STATE_EXCLUSION)
    icop = lm->ic_exclusion;
  if(gstate & DT_MASKS_STATE_INVERSE) icinv = lm->ic_inverse;
  char str2[1000] = "";
  int nbuse = 0;
  if(grp_id == 0)
  {
    _is_form_used(form->formid, NULL, str2, sizeof(str2), &nbuse);
    if(nbuse > 0) icuse = lm->ic_used;
  }

  if(!(form->type & DT_MASKS_GROUP))
  {
    // we just add it to the tree
    GtkTreeIter child;
    gtk_tree_store_append(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0), TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (icinv != NULL), TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);
  }
  else
  {
    // we first check if it's a "module" group or not
    if(grp_id == 0 && !module)
    {
      for(const GList *iops = darktable.develop->iop; iops; iops = g_list_next(iops))
      {
        dt_iop_module_t *iop = (dt_iop_module_t *)iops->data;
        if((iop->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(iop->flags() & IOP_FLAGS_NO_MASKS)
           && iop->blend_params->mask_id == form->formid)
        {
          module = iop;
          break;
        }
      }
    }

    // we add the group node to the tree
    GtkTreeIter child;
    gtk_tree_store_append(treestore, &child, toplevel);
    gtk_tree_store_set(treestore, &child, TREE_TEXT, str, TREE_MODULE, module, TREE_GROUPID, grp_id,
                       TREE_FORMID, form->formid, TREE_EDITABLE, (grp_id == 0), TREE_IC_OP, icop,
                       TREE_IC_OP_VISIBLE, (icop != NULL), TREE_IC_INVERSE, icinv, TREE_IC_INVERSE_VISIBLE,
                       (icinv != NULL), TREE_IC_USED, icuse, TREE_IC_USED_VISIBLE, (nbuse > 0),
                       TREE_USED_TEXT, str2, -1);
    _set_iter_name(lm, form, gstate, opacity, GTK_TREE_MODEL(treestore), &child);

    // we add all nodes to the tree
    for(const GList *forms = form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_point_group_t *grpt = (dt_masks_point_group_t *)forms->data;
      dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, grpt->formid);
      if(f)
        _lib_masks_list_recurs(treestore, &child, f, form->formid, module, grpt->state, grpt->opacity, lm);
    }
  }
}

gboolean _find_mask_iter_by_values(GtkTreeModel *model, GtkTreeIter *iter,
                                   const dt_iop_module_t *module, const int formid, const int level)
{
  gboolean found = FALSE;
  do
  {
    int fid = -1;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, iter, &mod, NULL, &fid);
    found = (fid == formid)
      && ((level == 1)
          || (module == NULL || (mod && (!g_strcmp0(module->op, mod->op)))));
    if(found) return found;
    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_mask_iter_by_values(model, &child, module, formid, level + 1);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

GList *_lib_masks_get_selected(dt_lib_module_t *self)
{
  GList *res = NULL;
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));

  GList *items = gtk_tree_selection_get_selected_rows(selection, &model);

  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    GtkTreePath *item = (GtkTreePath *)items_iter->data;
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, item))
    {
      int fid = -1;
      int gid = -1;
      dt_iop_module_t *mod;
      _lib_masks_get_values(model, &iter, &mod, &gid, &fid);
      res = g_list_prepend(res, GINT_TO_POINTER(fid));
      res = g_list_prepend(res, GINT_TO_POINTER(gid));
      res = g_list_prepend(res, (void *)(mod));
    }
  }

  g_list_foreach(items, (GFunc)gtk_tree_path_free, NULL);
  g_list_free(items);

  return res;
}

static void _lib_masks_recreate_list(dt_lib_module_t *self)
{
  /* first destroy all buttons in list */
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(!lm) return;
  if(lm->gui_reset) return;

  const int gui_reset = lm->gui_reset;
  lm->gui_reset = 1;
  // if(lm->treeview) gtk_widget_destroy(lm->treeview);

  // if a treeview is already present, let's get the currently selected items
  // as we are going to recreate the tree.
  GList *selectids = NULL;

  if(lm->treeview)
  {
    selectids = _lib_masks_get_selected(self);
  }

  _lib_masks_inactivate_icons(self);

  GtkTreeStore *treestore;
  // we store : text ; *module ; groupid ; formid
  treestore = gtk_tree_store_new(TREE_COUNT, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_INT,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF,
                                 G_TYPE_BOOLEAN, GDK_TYPE_PIXBUF, G_TYPE_BOOLEAN, G_TYPE_STRING);

  // we first add all groups
  for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(form->type & DT_MASKS_GROUP) _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
  }

  // and we add all forms
  for(const GList *forms = darktable.develop->forms; forms; forms = g_list_next(forms))
  {
    dt_masks_form_t *form = (dt_masks_form_t *)forms->data;
    if(!(form->type & DT_MASKS_GROUP)) _lib_masks_list_recurs(treestore, NULL, form, 0, NULL, 0, 1.0, lm);
  }

  gtk_tree_view_set_model(GTK_TREE_VIEW(lm->treeview), GTK_TREE_MODEL(treestore));

  // select the images as selected in the previous tree
  if(selectids)
  {
    GList *ids = selectids;
    while(ids)
    {
      GtkTreeModel *model = GTK_TREE_MODEL(treestore);
      dt_iop_module_t *mod = (dt_iop_module_t *)ids->data;
      ids = g_list_next(ids);
      // const int gid = GPOINTER_TO_INT(ids->data); // not needed, skip it
      ids = g_list_next(ids);
      const int fid = GPOINTER_TO_INT(ids->data);
      ids = g_list_next(ids);

      GtkTreeIter iter;
      gtk_tree_model_get_iter_first(model, &iter);
      // get formid in group for the given module
      const gboolean found = _find_mask_iter_by_values(model, &iter, mod, fid, 1);

      if(found)
      {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(lm->treeview), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(lm->treeview), path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_path_free(path);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
        gtk_tree_selection_select_iter(selection, &iter);
      }
    }
    g_list_free(selectids);
  }

  g_object_unref(treestore);

  _update_all_properties(lm);

  lm->gui_reset = gui_reset;
}

static gboolean _update_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  if(!iter) return 0;

  // we retrieve the ids
  int grid = -1;
  int id = -1;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  // we retrieve the forms
  dt_masks_form_t *form = dt_masks_get_from_id(darktable.develop, id);
  if(!form) return 0;
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, grid);

  // and the values
  int state = 0;
  float opacity = 1.0f;

  if(grp && (grp->type & DT_MASKS_GROUP))
  {
    for(const GList *pts = grp->points; pts; pts = g_list_next(pts))
    {
      dt_masks_point_group_t *pt = (dt_masks_point_group_t *)pts->data;
      if(pt->formid == id)
      {
        state = pt->state;
        opacity = pt->opacity;
        break;
      }
    }
  }

  _set_iter_name(data, form, state, opacity, model, iter);
  return 0;
}

static void _lib_masks_update_list(dt_lib_module_t *self)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  gtk_tree_model_foreach(model, _update_foreach, lm);
}

static gboolean _remove_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  if(!iter) return 0;
  GList **rl = (GList **)data;
  const int refid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "formid"));
  const int refgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(model), "groupid"));

  int grid = -1;
  int id = -1;
  _lib_masks_get_values(model, iter, NULL, &grid, &id);

  if(grid == refgid && id == refid)
  {
    GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
    *rl = g_list_append(*rl, rowref);
  }
  return 0;
}

static void _lib_masks_remove_item(dt_lib_module_t *self, int formid, int parentid)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  // for each node , we refresh the string
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  GList *rl = NULL;
  g_object_set_data(G_OBJECT(model), "formid", GUINT_TO_POINTER(formid));
  g_object_set_data(G_OBJECT(model), "groupid", GUINT_TO_POINTER(parentid));
  gtk_tree_model_foreach(model, _remove_foreach, &rl);

  for(const GList *rlt = rl; rlt; rlt = g_list_next(rlt))
  {
    GtkTreeRowReference *rowref = (GtkTreeRowReference *)rlt->data;
    GtkTreePath *path = gtk_tree_row_reference_get_path(rowref);
    gtk_tree_row_reference_free(rowref);
    if(path)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        gtk_tree_store_remove(GTK_TREE_STORE(model), &iter);
      }
      gtk_tree_path_free(path);
    }
  }
  g_list_free(rl);
}

static gboolean _lib_masks_selection_change_r(GtkTreeModel *model, GtkTreeSelection *selection,
                                              GtkTreeIter *iter, struct dt_iop_module_t *module,
                                              const int selectid, int throw_event, const int level)
{
  gboolean found = FALSE;

  GtkTreeIter i = *iter;
  do
  {
    int id = -1;
    dt_iop_module_t *mod;
    _lib_masks_get_values(model, &i, &mod, NULL, &id);

    if((id == selectid)
       && ((level == 1)
           || (module == NULL || (mod && (!g_strcmp0(module->op, mod->op))))))
    {
      gtk_tree_selection_select_iter(selection, &i);
      found = TRUE;
      break;
    }

    // check for children if any
    GtkTreeIter child, parent = i;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _lib_masks_selection_change_r(model, selection, &child, module, selectid, throw_event, level + 1);
      if(found)
      {
        break;
      }
    }
  } while(gtk_tree_model_iter_next(model, &i) == TRUE);

  return found;
}

static void _lib_masks_selection_change(dt_lib_module_t *self, struct dt_iop_module_t *module, const int selectid, const int throw_event)
{
  dt_lib_masks_t *lm = (dt_lib_masks_t *)self->data;
  if(!lm->treeview) return;

  // we first unselect all
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(lm->treeview));
  lm->gui_reset = 1;
  gtk_tree_selection_unselect_all(selection);
  lm->gui_reset = 0;

  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(lm->treeview));
  if(!model) return;

  lm->gui_reset = 1 - throw_event;
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

  // we go through all nodes
  if(valid)
  {
    gtk_tree_view_expand_all(GTK_TREE_VIEW(lm->treeview));
    const gboolean found = _lib_masks_selection_change_r(model, selection, &iter, module, selectid, throw_event, 1);
    if(!found) gtk_tree_view_collapse_all(GTK_TREE_VIEW(lm->treeview));
  }

  lm->gui_reset = 0;

  _update_all_properties(lm);
}

static GdkPixbuf *_get_pixbuf_from_cairo(DTGTKCairoPaintIconFunc paint, const int width, const int height)
{
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BUTTON_FG, 1.0);
  paint(cr, 0, 0, width, height, 0, NULL);
  cairo_destroy(cr);
  guchar *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, width, height);
  return gdk_pixbuf_new_from_data(data, GDK_COLORSPACE_RGB, TRUE, 8, width, height,
                                  cairo_image_surface_get_stride(cst), NULL, NULL);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_masks_t *d = (dt_lib_masks_t *)g_malloc0(sizeof(dt_lib_masks_t));
  self->data = (void *)d;
  d->gui_reset = 0;

  // initialise all masks pixbuf. This is needed for the "automatic" cell renderer of the treeview
  const int bs2 = DT_PIXEL_APPLY_DPI(13);
  d->ic_inverse = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_inverse, bs2, bs2);
  d->ic_used = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_used, bs2, bs2);
  d->ic_union = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_union, bs2 * 2, bs2);
  d->ic_intersection = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_intersection, bs2 * 2, bs2);
  d->ic_difference = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_difference, bs2 * 2, bs2);
  d->ic_exclusion = _get_pixbuf_from_cairo(dtgtk_cairo_paint_masks_exclusion, bs2 * 2, bs2);

  // initialise widgets
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  GtkWidget *label = gtk_label_new(_("created shapes"));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);

  d->bt_gradient = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_gradient, 0, NULL);
  dt_action_define(DT_ACTION(self), N_("shapes"), N_("add gradient"), d->bt_gradient, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(d->bt_gradient), "button-press-event", G_CALLBACK(_bt_add_shape), GINT_TO_POINTER(DT_MASKS_GRADIENT));
  gtk_widget_set_tooltip_text(d->bt_gradient, _("add gradient"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_gradient), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_gradient, FALSE, FALSE, 0);

  d->bt_path = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_path, 0, NULL);
  dt_action_define(DT_ACTION(self), N_("shapes"), N_("add path"), d->bt_path, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(d->bt_path), "button-press-event", G_CALLBACK(_bt_add_shape), GINT_TO_POINTER(DT_MASKS_PATH));
  gtk_widget_set_tooltip_text(d->bt_path, _("add path"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_path), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_path, FALSE, FALSE, 0);

  d->bt_ellipse = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_ellipse, 0, NULL);
  dt_action_define(DT_ACTION(self), N_("shapes"), N_("add ellipse"), d->bt_ellipse, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(d->bt_ellipse), "button-press-event", G_CALLBACK(_bt_add_shape), GINT_TO_POINTER(DT_MASKS_ELLIPSE));
  gtk_widget_set_tooltip_text(d->bt_ellipse, _("add ellipse"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_ellipse), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_ellipse, FALSE, FALSE, 0);

  d->bt_circle = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_circle, 0, NULL);
  dt_action_define(DT_ACTION(self), N_("shapes"), N_("add circle"), d->bt_circle, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(d->bt_circle), "button-press-event", G_CALLBACK(_bt_add_shape), GINT_TO_POINTER(DT_MASKS_CIRCLE));
  gtk_widget_set_tooltip_text(d->bt_circle, _("add circle"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_circle), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_circle, FALSE, FALSE, 0);

  d->bt_brush = dtgtk_togglebutton_new(dtgtk_cairo_paint_masks_brush, 0, NULL);
  dt_action_define(DT_ACTION(self), N_("shapes"), N_("add brush"), d->bt_brush, &dt_action_def_toggle);
  g_signal_connect(G_OBJECT(d->bt_brush), "button-press-event", G_CALLBACK(_bt_add_shape), GINT_TO_POINTER(DT_MASKS_BRUSH));
  gtk_widget_set_tooltip_text(d->bt_brush, _("add brush"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bt_brush), FALSE);
  gtk_box_pack_end(GTK_BOX(hbox), d->bt_brush, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  d->treeview = gtk_tree_view_new();
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(col, "shapes");
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->treeview), col);

  GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_OP, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_OP_VISIBLE);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_start(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_INVERSE, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_INVERSE_VISIBLE);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", TREE_TEXT);
  gtk_tree_view_column_add_attribute(col, renderer, "editable", TREE_EDITABLE);
  g_signal_connect(renderer, "edited", (GCallback)_tree_cell_edited, self);
  renderer = gtk_cell_renderer_pixbuf_new();
  gtk_tree_view_column_pack_end(col, renderer, FALSE);
  gtk_tree_view_column_set_attributes(col, renderer, "pixbuf", TREE_IC_USED, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "visible", TREE_IC_USED_VISIBLE);

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->treeview));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
  gtk_tree_selection_set_select_function(selection, _tree_restrict_select, d, NULL);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(d->treeview), FALSE);
  gtk_widget_set_has_tooltip(d->treeview, TRUE);
  g_signal_connect(d->treeview, "query-tooltip", G_CALLBACK(_tree_query_tooltip), NULL);
  g_signal_connect(selection, "changed", G_CALLBACK(_tree_selection_change), d);
  g_signal_connect(d->treeview, "button-press-event", (GCallback)_tree_button_pressed, self);

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_scroll_wrap(d->treeview, 200, "plugins/darkroom/masks/heightview"), FALSE, FALSE, 0);

  dt_gui_new_collapsible_section
    (&d->cs,
     "plugins/darkroom/masks/expand_properties",
     _("properties"),
     GTK_BOX(self->widget));
  gtk_widget_set_no_show_all(GTK_WIDGET(d->cs.container), TRUE);

  for(int i = 0; i < DT_MASKS_PROPERTY_LAST; i++)
  {
    d->property[i] = dt_bauhaus_slider_new_action(DT_ACTION(self), _masks_properties[i].min,
                                                  _masks_properties[i].max, 0, 0.0, 2);
    dt_bauhaus_widget_set_label(d->property[i], N_("properties"), _masks_properties[i].name);
    dt_bauhaus_slider_set_format(d->property[i], _masks_properties[i].format);
    d->last_value[i] = dt_bauhaus_slider_get(d->property[i]);
    gtk_box_pack_start(GTK_BOX(d->cs.container), d->property[i], FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(d->property[i]), "value-changed", G_CALLBACK(_property_changed), GINT_TO_POINTER(i));
  }

  d->none_label = dt_ui_label_new(_("no shapes selected"));
  gtk_box_pack_start(GTK_BOX(d->cs.container), d->none_label, FALSE, FALSE, 0);

  // set proxy functions
  darktable.develop->proxy.masks.module = self;
  darktable.develop->proxy.masks.list_change = _lib_masks_recreate_list;
  darktable.develop->proxy.masks.list_update = _lib_masks_update_list;
  darktable.develop->proxy.masks.list_remove = _lib_masks_remove_item;
  darktable.develop->proxy.masks.selection_change = _lib_masks_selection_change;
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

