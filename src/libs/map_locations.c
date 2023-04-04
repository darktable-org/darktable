/*
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
#include "common/collection.h"
#include "common/debug.h"
#include "common/map_locations.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "libs/lib.h"

// map position module uses the tag dictionary with dt_geo_tag_root as a prefix.
// Synonym field is used to store positions coordinates in ascii format.

static void _signal_location_change(dt_lib_module_t *self);
static void _show_location(dt_lib_module_t *self);

DT_MODULE(1)

const char *name(dt_lib_module_t *self)
{
  return _("locations");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_MAP;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

typedef struct dt_lib_map_locations_t
{
  GtkWidget *shape_button;
  gulong shape_button_handler;
  GtkWidget *new_button;
  GtkWidget *show_all_button;
  GtkWidget *hide_button;
  GtkWidget *view;
  GtkCellRenderer *renderer;
  GtkTreeSelection *selection;
  GtkTreeViewColumn *name_col;
  GList *polygons;
} dt_lib_map_locations_t;

typedef struct dt_loc_op_t
{
  char *newtagname;
  char *oldtagname;
} dt_loc_op_t;

int position(const dt_lib_module_t *self)
{
  return 995;
}

typedef enum dt_map_positions_cols_t
{
  DT_MAP_LOCATION_COL_ID = 0,
  DT_MAP_LOCATION_COL_TAG,
  DT_MAP_LOCATION_COL_PATH,
  DT_MAP_LOCATION_COL_COUNT,
  DT_MAP_LOCATION_NUM_COLS
} dt_map_positions_cols_t;

typedef enum dt_map_position_name_sort_id
{
  DT_MAP_POSITION_SORT_NAME_ID,
} dt_map_position_name_sort_id;

const DTGTKCairoPaintIconFunc location_shapes[] = { dtgtk_cairo_paint_masks_circle,   // MAP_LOCATION_SHAPE_ELLIPSE
                                                    dtgtk_cairo_paint_rect_landscape, // MAP_LOCATION_SHAPE_RECTANGLE
                                                    dtgtk_cairo_paint_polygon};       // MAP_LOCATION_SHAPE_POLYGONS

// find a tag on the tree
static gboolean _find_tag_iter_id(GtkTreeModel *model, GtkTreeIter *iter,
                                  const guint locid)
{
  gboolean found = FALSE;
  if(locid <= 0) return found;
  guint id;
  do
  {
    gtk_tree_model_get(model, iter, DT_MAP_LOCATION_COL_ID, &id, -1);
    found = id == locid;
    if(found) return found;
    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_tag_iter_id(model, &child, locid);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

static void _locations_tree_update(dt_lib_module_t *self, const guint locid)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GList *tags = dt_map_location_get_locations_by_path("", TRUE);
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));

  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                       GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);
  gtk_tree_store_clear(GTK_TREE_STORE(model));
  if(tags)
  {
    char **last_tokens = NULL;
    int last_tokens_length = 0;
    GtkTreeIter last_parent = { 0 };
    GList *sorted_tags = dt_sort_tag(tags, 0);  // ordered by full tag name
    tags = sorted_tags;
    for(GList *stag = tags; stag; stag = g_list_next(stag))
    {
      GtkTreeIter iter;
      const gchar *tag = ((dt_map_location_t *)stag->data)->tag;
      if(tag == NULL) continue;
      char **tokens;
      tokens = g_strsplit(tag, "|", -1);
      if(tokens)
      {
        // find the number of common parts at the beginning of tokens and last_tokens
        GtkTreeIter parent = last_parent;
        const int tokens_length = g_strv_length(tokens);
        int common_length = 0;
        if(last_tokens)
        {
          while(tokens[common_length] && last_tokens[common_length] &&
                !g_strcmp0(tokens[common_length], last_tokens[common_length]))
          {
            common_length++;
          }

          // point parent iter to where the entries should be added
          for(int i = common_length; i < last_tokens_length; i++)
          {
            gtk_tree_model_iter_parent(GTK_TREE_MODEL(model), &parent, &last_parent);
            last_parent = parent;
          }
        }

        // insert everything from tokens past the common part
        char *pth = NULL;
        for(int i = 0; i < common_length; i++)
          pth = dt_util_dstrcat(pth, "%s|", tokens[i]);

        for(char **token = &tokens[common_length]; *token; token++)
        {
          pth = dt_util_dstrcat(pth, "%s|", *token);
          gchar *pth2 = g_strdup(pth);
          pth2[strlen(pth2) - 1] = '\0';
          gtk_tree_store_insert(GTK_TREE_STORE(model), &iter, common_length > 0 ? &parent : NULL, -1);
          gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                            DT_MAP_LOCATION_COL_TAG, *token,
                            DT_MAP_LOCATION_COL_ID, (token == &tokens[tokens_length-1]) ?
                                                     ((dt_map_location_t *)stag->data)->id : 0,
                            DT_MAP_LOCATION_COL_PATH, pth2,
                            DT_MAP_LOCATION_COL_COUNT, (token == &tokens[tokens_length-1]) ?
                                                      ((dt_map_location_t *)stag->data)->count : 0,
                            -1);
          common_length++;
          parent = iter;
          g_free(pth2);
        }
        g_free(pth);

        // remember things for the next round
        if(last_tokens) g_strfreev(last_tokens);
        last_tokens = tokens;
        last_parent = parent;
        last_tokens_length = tokens_length;
      }
    }
    g_strfreev(last_tokens);
    dt_map_location_free_result(&tags);
  }
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), model);
  g_object_unref(model);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                       DT_MAP_POSITION_SORT_NAME_ID,
                                       GTK_SORT_ASCENDING);
  if(locid)
  {
    // try to select the right record
    GtkTreeIter iter;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      if(_find_tag_iter_id(model, &iter, locid))
      {
        gtk_tree_selection_select_iter(selection, &iter);
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(d->view), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(d->view), path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), path, d->name_col, FALSE);
        gtk_tree_path_free(path);
      }
    }
  }
}

static void _display_buttons(dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->new_button))), _("new sub-location"));
  }
  else
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->new_button))), _("new location"));
  }
}

static void _tree_name_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                            GtkTreeModel *model, GtkTreeIter *iter,
                            gpointer data)
{
  guint locid;
  gchar *name;
  gchar *path;
  guint count;
  gchar *coltext;
  gtk_tree_model_get(model, iter,
                     DT_MAP_LOCATION_COL_ID, &locid,
                     DT_MAP_LOCATION_COL_TAG, &name,
                     DT_MAP_LOCATION_COL_COUNT, &count,
                     DT_MAP_LOCATION_COL_PATH, &path, -1);
  if(count < 1)
  {
    coltext = g_markup_printf_escaped(locid ? "%s" : "<i>%s</i>", name);
  }
  else
  {
    coltext = g_markup_printf_escaped(locid ? "%s (%d)" : "<i>%s</i> (%d)", name, count);
  }
  g_object_set(renderer, "markup", coltext, NULL);
  g_free(coltext);
  g_free(name);
  g_free(path);
}

static void _new_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter, parent;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  char *name = NULL;
  char *path = NULL;
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_PATH, &path, -1);
    parent = iter;
  }
  name = path ? g_strconcat(path, "|", NULL) : g_strdup("");
  const int base_len = strlen(name);
  int i = 1;
  name = dt_util_dstrcat(name, "%s", _("new location"));
  char *new_name = g_strdup(name);
  while(dt_map_location_name_exists(new_name))
  {
    g_free(new_name);
    new_name = g_strdup_printf("%s %d", name, i);
    i++;
  }

  // add the new record to the tree
  gtk_tree_store_insert(GTK_TREE_STORE(model), &iter, path ? &parent : NULL, -1);
  gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                     DT_MAP_LOCATION_COL_TAG, &new_name[base_len],
                     DT_MAP_LOCATION_COL_ID, -1,
                     DT_MAP_LOCATION_COL_PATH, new_name,
                     DT_MAP_LOCATION_COL_COUNT, 0,
                    -1);
  g_free(new_name);
  g_free(name);
  g_free(path);

  // set the new record editable
  g_object_set(G_OBJECT(d->renderer), "editable", TRUE, NULL);
  GtkTreePath *path2 = gtk_tree_model_get_path(model, &iter);
  gtk_tree_view_expand_to_path(GTK_TREE_VIEW(d->view), path2);
  gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(d->view), path2, NULL, TRUE, 0.5, 0.5);
  gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), path2, d->name_col, TRUE);
  gtk_tree_path_free(path2);
}

static void _shape_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  int shape = dt_conf_get_int("plugins/map/locationshape");
  shape++;
  if((shape > G_N_ELEMENTS(location_shapes) - 1) ||
     (!d->polygons && shape == MAP_LOCATION_SHAPE_POLYGONS))
    shape = 0;
  dt_conf_set_int("plugins/map/locationshape", shape);

  g_signal_handler_block (d->shape_button, d->shape_button_handler);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->shape_button), FALSE);
  dtgtk_togglebutton_set_paint((GtkDarktableToggleButton *)d->shape_button, location_shapes[shape], 0, NULL);
  g_signal_handler_unblock (d->shape_button, d->shape_button_handler);
}

static void _show_all_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  dt_conf_set_bool("plugins/map/showalllocations",
                  gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->show_all_button)));
  dt_view_map_location_action(darktable.view_manager, MAP_LOCATION_ACTION_UPDATE_OTHERS);
}

// delete a path of the tag tree
static void _delete_tree_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  gboolean valid = TRUE;
  do
  {
    if(gtk_tree_model_iter_children(model, &child, &parent))
      _delete_tree_path(model, &child, FALSE);
    GtkTreeIter tobedel = parent;
    valid = gtk_tree_model_iter_next(model, &parent);
    char *path = NULL;
    gtk_tree_model_get(model, &tobedel, DT_MAP_LOCATION_COL_PATH, &path, -1);
    g_free(path);
    gtk_tree_store_remove(GTK_TREE_STORE(model), &tobedel);
  } while(!root  && valid);
}

static gboolean _update_tag_name_per_name(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_loc_op_t *to)
{
  char *tagname;
  char *newtagname = to->newtagname;
  char *oldtagname = to->oldtagname;
  gtk_tree_model_get(model, iter, DT_MAP_LOCATION_COL_PATH, &tagname, -1);
  if(g_str_has_prefix(tagname, oldtagname))
  {
    if(strlen(tagname) == strlen(oldtagname))
    {
      // rename the tag itself
      char *subtag = g_strrstr(to->newtagname, "|");
      subtag = (!subtag) ? newtagname : subtag + 1;
      gtk_tree_store_set(GTK_TREE_STORE(model), iter,
                         DT_MAP_LOCATION_COL_PATH, newtagname,
                         DT_MAP_LOCATION_COL_TAG, subtag, -1);
    }
    else if(strlen(tagname) > strlen(oldtagname) && tagname[strlen(oldtagname)] == '|')
    {
      // rename similar path
      char *newpath = g_strconcat(newtagname, &tagname[strlen(oldtagname)] , NULL);
      gtk_tree_store_set(GTK_TREE_STORE(model), iter,
                         DT_MAP_LOCATION_COL_PATH, newpath, -1);
      g_free(newpath);
    }
  }
  g_free(tagname);
  return FALSE;
}

static void _view_map_geotag_changed(gpointer instance, GList *imgs, const int newlocid, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;

  // one of the other location has been clicked on the map
  if(newlocid)
  {
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      if(_find_tag_iter_id(model, &iter, newlocid))
      {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
        gtk_tree_selection_select_iter(selection, &iter);
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(GTK_TREE_VIEW(d->view), path);
        gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(d->view), path, NULL, TRUE, 0.5, 0.5);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), path, d->name_col, FALSE);
        gtk_tree_path_free(path);
        _show_location(self);
        _display_buttons(self);
      }
    }
  }
  else
  {
    for(GList* img = imgs; img; img = g_list_next(img))
    {
      // find new locations for that image
      GList *tags = dt_map_location_find_locations(GPOINTER_TO_INT(img->data));
      // update locations for that image
      dt_map_location_update_locations(GPOINTER_TO_INT(img->data), tags);
      g_list_free(tags);
    }
    // update count on the treeview
    GList *locs = dt_map_location_get_locations_by_path("", TRUE);
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      for(GList *loc = locs; loc; loc = g_list_next(loc))
      {
        const guint locid = ((dt_map_location_t *)loc->data)->id;
        GtkTreeIter iter2 = iter;
        if(_find_tag_iter_id(model, &iter2, locid))
        {
          gtk_tree_store_set(GTK_TREE_STORE(model), &iter2,
                             DT_MAP_LOCATION_COL_COUNT, ((dt_map_location_t *)loc->data)->count,
                             -1);
        }
      }
    }
    dt_map_location_free_result(&locs);
  }
}

static void _view_map_location_changed(gpointer instance, GList *polygons, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  const int shape = dt_conf_get_int("plugins/map/locationshape");
  if((shape == MAP_LOCATION_SHAPE_POLYGONS) && !polygons)
  {
    g_signal_handler_block (d->shape_button, d->shape_button_handler);
    dtgtk_togglebutton_set_paint((GtkDarktableToggleButton *)d->shape_button,
                                 location_shapes[MAP_LOCATION_SHAPE_ELLIPSE], 0, NULL);
    g_signal_handler_unblock (d->shape_button, d->shape_button_handler);
    dt_conf_set_int("plugins/map/locationshape", MAP_LOCATION_SHAPE_ELLIPSE);
  }
  d->polygons = polygons;
}

static void _signal_location_change(dt_lib_module_t *self)
{
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, (GList *)NULL, 0);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
}

static void _name_editing_done(GtkCellEditable *editable, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  gboolean canceled = TRUE;
  g_object_get(editable, "editing-canceled", &canceled, NULL);
  const gchar *name = gtk_entry_get_text(GTK_ENTRY(editable));
  const gboolean reset = name[0] ? FALSE : TRUE;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *path = NULL;
    char *leave = NULL;
    guint locid;
    gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_ID, &locid,
                                     DT_MAP_LOCATION_COL_PATH, &path,
                                     DT_MAP_LOCATION_COL_TAG, &leave, -1);
    if(reset && locid)
      canceled = TRUE;  // empty name for a location is not allowed
    if(!canceled)
    {
      const gboolean isroot = !strcmp(path, leave);
      const int path_len = strlen(path);
      char *new_path;
      if(!isroot)
      {
        const int leave_len = strlen(leave);
        const char letter = path[path_len - leave_len];
        path[path_len - leave_len] = '\0';
        new_path = g_strconcat(path, name, NULL);
        path[path_len - leave_len] = letter;
      }
      else
        new_path = g_strdup((char *)name);

      gboolean new_exists = FALSE;
      GList *new_existing = NULL;
      if(!reset) // in case of reset we rely on dt_tag_rename to be safe
        new_existing = dt_map_location_get_locations_by_path(new_path, FALSE);
      if(new_existing)
      {
        dt_map_location_free_result(&new_existing);
        new_exists = TRUE;
      }
      if(!new_exists)
      {
        // new name is free, we can work
        if(locid == -1)
        {
          // new location - create and show it
          locid = dt_map_location_new(new_path);
          if(locid != -1)
          {
            // add the location on the map
            dt_map_location_data_t g;
            g.shape = dt_conf_get_int("plugins/map/locationshape");
            g.lon = g.lat = DT_INVALID_GPS_COORDINATE;
            g.delta1 = g.delta2 = 0.0;
            g.polygons = d->polygons;
            dt_view_map_add_location(darktable.view_manager, &g, locid);
            const int count = dt_map_location_get_images_count(locid);
            if(g_strstr_len(name, -1, "|"))
            {
              // the user wants to insert some group(s). difficult to handle the tree => reset
              _locations_tree_update(self, locid);
            }
            else
            {
              gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                                 DT_MAP_LOCATION_COL_ID, locid,
                                 DT_MAP_LOCATION_COL_PATH, new_path,
                                 DT_MAP_LOCATION_COL_TAG, name,
                                 DT_MAP_LOCATION_COL_COUNT, count,
                                  -1);
            }
          }
          else canceled = TRUE;
        }
        else
        {
          // existing location - rename it
          GList *children = dt_map_location_get_locations_by_path(path, FALSE);
          for(GList *tag = children; tag; tag = g_list_next(tag))
          {
            // reset on leave is not possible. should be safe
            const char *new_part = &((dt_map_location_t *)tag->data)->tag[path_len + (reset ? 1 :0)];
            char *new_name = g_strconcat(new_path, new_part, NULL);
            dt_map_location_rename(((dt_map_location_t *)tag->data)->id, new_name);
            g_free(new_name);
          }
          dt_map_location_free_result(&children);

          // update the store
          if(reset  || g_strstr_len(name, -1, "|"))
          {
            // the user wants to insert some group(s). difficult to handle the tree => reset
            _locations_tree_update(self, locid);
          }
          else
          {
            dt_loc_op_t to;
            to.oldtagname = path;
            to.newtagname = new_path;
            gint sort_column;
            GtkSortType sort_order;
            gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(model),
                                                 &sort_column, &sort_order);
            gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                                 GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                                 GTK_SORT_ASCENDING);
            gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)_update_tag_name_per_name, &to);
            gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model),
                                                 sort_column, sort_order);
          }
          _signal_location_change(self);
        }
      }
      else
      {
        dt_control_log(_("location name \'%s\' already exists"), new_path);
        canceled = TRUE;
      }
      g_free(new_path);
    }
    if(canceled)
    {
      if(locid == -1)
      {
        // if new we have to remove the new location from tree
        _delete_tree_path(model, &iter, TRUE);
        gtk_tree_selection_unselect_all(selection);
      }
    }
    g_free(path);
    g_free(leave);
  }
  g_object_set(G_OBJECT(d->renderer), "editable", FALSE, NULL);
  _display_buttons(self);
}

static void _name_start_editing(GtkCellRenderer *renderer, GtkCellEditable *editable,
                          char *path, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  if(GTK_IS_ENTRY(editable))
  {
    // set up the editable with name (without number)
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
    GtkTreeIter iter;
    GtkTreePath *new_path = gtk_tree_path_new_from_string(path);
    if(gtk_tree_model_get_iter(model, &iter, new_path))
    {
      char *name = NULL;
      gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_TAG, &name, -1);
      gtk_entry_set_text(GTK_ENTRY(editable), name);
      g_free(name);
    }
    gtk_tree_path_free(new_path);

    g_signal_connect(G_OBJECT(editable), "editing-done", G_CALLBACK(_name_editing_done), self);
  }
}

static gint _sort_position_names_func(GtkTreeModel *model,
                                      GtkTreeIter *a, GtkTreeIter *b,
                                      dt_lib_module_t *self)
{
  char *tag_a = NULL;
  char *tag_b = NULL;
  gtk_tree_model_get(model, a, DT_MAP_LOCATION_COL_PATH, &tag_a, -1);
  gtk_tree_model_get(model, b, DT_MAP_LOCATION_COL_PATH, &tag_b, -1);
  if(tag_a == NULL) tag_a = g_strdup("");
  if(tag_b == NULL) tag_b = g_strdup("");
  const gboolean sort = g_ascii_strncasecmp(tag_a, tag_b, -1);
  g_free(tag_a);
  g_free(tag_b);
  return sort;
}

static void _pop_menu_edit_location(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
    g_object_set(G_OBJECT(d->renderer), "editable", TRUE, NULL);
    gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), path, d->name_col, TRUE);
    gtk_tree_path_free(path);
    _display_buttons(self);
  }
}

static void _pop_menu_delete_location(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    guint locid = 0;
    gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_ID, &locid, -1);
    if(locid > 0)
    {
      dt_view_map_location_action(darktable.view_manager, MAP_LOCATION_ACTION_REMOVE);
      dt_map_location_delete(locid);
      _signal_location_change(self);
    }
    // update the treeview
    GtkTreeIter parent;
    if(gtk_tree_model_iter_parent(model, &parent, &iter))
    {
      guint parentid;
      gtk_tree_model_get(model, &parent, DT_MAP_LOCATION_COL_ID, &parentid, -1);
      if(parentid > 0)
      {
        _delete_tree_path(model, &iter, TRUE);
        gtk_tree_selection_unselect_all(selection);
      }
      else
      {
        // parent is a node (not a location). reset treeview
        _locations_tree_update(self, 0);
      }
    }
    else
    {
      _delete_tree_path(model, &iter, TRUE);
      gtk_tree_selection_unselect_all(selection);
    }
  }
  _display_buttons(self);
}

static void _show_location(dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    guint locid;
    gtk_tree_model_get(model, &iter,
                       DT_MAP_LOCATION_COL_ID, &locid, -1);
    if(locid)
    {
      dt_map_location_data_t *p = dt_map_location_get_data(locid);
      dt_view_map_add_location(darktable.view_manager, p, locid);
      g_free(p);
    }
    else
    {
      // this is not a location (only a parent). remove location from map if any
      dt_view_map_location_action(darktable.view_manager, MAP_LOCATION_ACTION_REMOVE);
    }
  }
}

static gboolean _set_location_collection(dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *name;
    gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_PATH, &name, -1);
    char *collection = g_strdup_printf("1:0:%d:%s|%s$",
                                       DT_COLLECTION_PROP_GEOTAGGING,
                                       _("tagged"), name);
    dt_collection_deserialize(collection, FALSE);
    g_free(collection);
    g_free(name);
    return TRUE;
  }
  return FALSE;
}

static void _pop_menu_update_filmstrip(GtkWidget *menuitem, dt_lib_module_t *self)
{
  _set_location_collection(self);
}

static void _pop_menu_goto_collection(GtkWidget *menuitem, dt_lib_module_t *self)
{
  if(_set_location_collection(self))
    dt_view_manager_switch(darktable.view_manager, "lighttable");
}

static void _pop_menu_view(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  GtkWidget *menu, *menuitem;
  menu = gtk_menu_new();

  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));

  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    guint locid = 0;
    gtk_tree_model_get(model, &iter, DT_MAP_LOCATION_COL_ID, &locid, -1);
    GtkTreeIter child, parent = iter;
    const gboolean children = gtk_tree_model_iter_children(model, &child, &parent);

    menuitem = gtk_menu_item_new_with_label(_("edit location"));
    g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_edit_location, self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    menuitem = gtk_menu_item_new_with_label(_("delete location"));
    g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_delete_location, self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    if(children)
    {
      gtk_widget_set_sensitive(menuitem, FALSE);
    }

    menuitem = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    menuitem = gtk_menu_item_new_with_label(_("update filmstrip"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    if(!locid)
    {
      gtk_widget_set_sensitive(menuitem, FALSE);
    }
    g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_update_filmstrip, self);
    menuitem = gtk_menu_item_new_with_label(_("go to collection (lighttable)"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_goto_collection, self);
    if(!locid)
    {
      gtk_widget_set_sensitive(menuitem, FALSE);
    }
  }

  gtk_widget_show_all(GTK_WIDGET(menu));

  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static gboolean _force_selection_changed(gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  gtk_tree_selection_unselect_all(d->selection);
  return FALSE;
}

static void _selection_changed(GtkTreeSelection *selection, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    _show_location(self);
  }
  else
  {
    dt_view_map_location_action(darktable.view_manager, MAP_LOCATION_ACTION_REMOVE);
  }
  _display_buttons(self);
}

static gboolean _click_on_view(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)self->data;

  gboolean editing;
  g_object_get(G_OBJECT(d->renderer), "editing", &editing, NULL);
  if(editing)
  {
    dt_control_log(_("terminate edit (press enter or escape) before selecting another location"));
    return TRUE;
  }

  const int button_pressed = (event->type == GDK_BUTTON_PRESS) ? event->button : 0;
  const gboolean ctrl_pressed = dt_modifier_is(event->state, GDK_CONTROL_MASK);
  if((button_pressed == 3)
     || (button_pressed == 1 && !ctrl_pressed)
     || (button_pressed == 1 && ctrl_pressed)
    )
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x,
                                     (gint)event->y, &path, NULL, NULL, NULL))
    {
      if(button_pressed == 3)
      {
        gtk_tree_selection_select_path(selection, path);
        _pop_menu_view(view, event, self);
        gtk_tree_path_free(path);
        _display_buttons(self);
        return TRUE;
      }
      else if(button_pressed == 1 && !ctrl_pressed)
      {
        if(gtk_tree_selection_path_is_selected(selection, path))
          g_timeout_add(100, _force_selection_changed, self);
        gtk_tree_path_free(path);
        return FALSE;
      }
      else if(button_pressed == 1 && ctrl_pressed)
      {
        gtk_tree_selection_select_path(selection, path);
        g_object_set(G_OBJECT(d->renderer), "editable", TRUE, NULL);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(d->view), path, d->name_col, TRUE);
        gtk_tree_path_free(path);
        _display_buttons(self);
        return TRUE;
      }
    }
    else
    {
      g_timeout_add(10, _force_selection_changed, self);
      return FALSE;
    }
  }
  return FALSE;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_map_locations_t *d = (dt_lib_map_locations_t *)g_malloc0(sizeof(dt_lib_map_locations_t));
  self->data = d;

  self->widget =  gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = GTK_WIDGET(view);
  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_resize_wrap(d->view, 100, "plugins/map/heightlocationwindow"), TRUE, TRUE, 0);
  gtk_tree_view_set_headers_visible(view, FALSE);
  GtkTreeStore *treestore = gtk_tree_store_new(DT_MAP_LOCATION_NUM_COLS, G_TYPE_UINT,
                                               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(treestore), DT_MAP_POSITION_SORT_NAME_ID,
                  (GtkTreeIterCompareFunc)_sort_position_names_func, self, NULL);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  gtk_tree_view_set_expander_column(view, col);
  d->name_col = col;

  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_MAP_LOCATION_COL_TAG);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _tree_name_show, (gpointer)self, NULL);
//  g_object_set(renderer, "editable", TRUE, NULL);
  g_signal_connect(G_OBJECT(renderer), "editing-started", G_CALLBACK(_name_start_editing), self);
  d->renderer = renderer;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  d->selection = selection;
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(treestore));
  g_object_unref(treestore);
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_click_on_view), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(view),
                              _("list of user locations,"
                                "\nclick to show or hide a location on the map:"
                                "\n - wheel scroll inside the shape to resize it"
                                "\n - <shift> or <ctrl> scroll to modify the width or the height"
                                "\n - click inside the shape and drag it to change its position"
                                "\n - ctrl+click to move an image from inside the location"
                                "\nctrl+click to edit a location name"
                                "\n - a pipe \'|\' symbol breaks the name into several levels"
                                "\n - to remove a group of locations clear its name"
                                "\n - press enter to validate the new name, escape to cancel the edit"
                                "\nright-click for other actions: delete location and go to collection"));

  // buttons
  GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  int shape = dt_conf_get_int("plugins/map/locationshape");
  if(shape == MAP_LOCATION_SHAPE_POLYGONS)
  {
    shape = MAP_LOCATION_SHAPE_ELLIPSE;
    dt_conf_set_int("plugins/map/locationshape", shape);
  }
  d->shape_button = dtgtk_togglebutton_new(location_shapes[shape], 0, NULL);
  gtk_box_pack_start(hbox, d->shape_button, FALSE, TRUE, 0);
  d->shape_button_handler = g_signal_connect(G_OBJECT(d->shape_button), "clicked",
                                             G_CALLBACK(_shape_button_clicked), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->shape_button ),
                              _("select the shape of the location\'s limits on the map, circle or rectangle"
                                "\nor even polygon if available (select first a polygon place in 'find location' module)"));

  d->new_button = dt_action_button_new(self, N_("new location"), _new_button_clicked, self,
                                       _("add a new location on the center of the visible map"), 0, 0);
  gtk_box_pack_start(hbox, d->new_button, TRUE, TRUE, 0);

  dt_conf_set_bool("plugins/map/showalllocations", FALSE);
  d->show_all_button = gtk_check_button_new_with_label(_("show all"));
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->show_all_button))), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(d->show_all_button,
                              _("show all locations which are on the visible map"));
  gtk_box_pack_end(hbox, d->show_all_button, FALSE, FALSE, 8);
  g_signal_connect(G_OBJECT(d->show_all_button), "clicked", G_CALLBACK(_show_all_button_clicked), self);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), FALSE, TRUE, 0);

  _locations_tree_update(self,0);
  _display_buttons(self);

  g_signal_connect(G_OBJECT(selection), "changed", G_CALLBACK(_selection_changed), self);

  // connect geotag changed signal
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED,
                                  G_CALLBACK(_view_map_geotag_changed), (gpointer)self);
  // connect location changed signal
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_LOCATION_CHANGED,
                                  G_CALLBACK(_view_map_location_changed), (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_view_map_location_changed), self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
