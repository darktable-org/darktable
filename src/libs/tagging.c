/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <math.h>

#define FLOATING_ENTRY_WIDTH DT_PIXEL_APPLY_DPI(150)

DT_MODULE(1)

static gboolean _lib_tagging_tag_redo(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self);
static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self);

typedef struct dt_lib_tagging_t
{
  char keyword[1024];
  GtkEntry *entry;
  GtkTreeView *attached_view, *dictionary_view;
  GtkWidget *attach_button, *detach_button, *new_button, *import_button, *export_button, *attached_window, *dictionary_window;
  GtkWidget *toggle_tree_button, *toggle_suggestion_button, *toggle_sort_button, *toggle_hide_button, *toggle_dttags_button;
  gulong tree_button_handler, suggestion_button_handler, sort_button_handler, hide_button_handler;
  gulong dttags_button_handler;
  GtkListStore *attached_liststore, *dictionary_liststore;
  GtkTreeStore *dictionary_treestore;
  GtkTreeModelFilter *dictionary_listfilter, *dictionary_treefilter;
  GtkWidget *floating_tag_window;
  GList *floating_tag_imgs;
  gboolean tree_flag, suggestion_flag, sort_count_flag, hide_path_flag, dttags_flag;
  char *collection;
  GtkEntryCompletion *completion;
  char *last_tag;
} dt_lib_tagging_t;

typedef struct dt_tag_op_t
{
  gint tagid;
  char *newtagname;
  char *oldtagname;
  gboolean tree_flag;
} dt_tag_op_t;

typedef enum dt_lib_tagging_cols_t
{
  DT_LIB_TAGGING_COL_TAG = 0,
  DT_LIB_TAGGING_COL_ID,
  DT_LIB_TAGGING_COL_PATH,
  DT_LIB_TAGGING_COL_SYNONYM,
  DT_LIB_TAGGING_COL_COUNT,
  DT_LIB_TAGGING_COL_SEL,
  DT_LIB_TAGGING_COL_FLAGS,
  DT_LIB_TAGGING_COL_VISIBLE,
  DT_LIB_TAGGING_NUM_COLS
} dt_lib_tagging_cols_t;

typedef enum dt_tag_sort_id
{
  DT_TAG_SORT_PATH_ID,
  DT_TAG_SORT_NAME_ID,
  DT_TAG_SORT_COUNT_ID
} dt_tag_sort_id;

const char *name(dt_lib_module_t *self)
{
  return _("tagging");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v1[] = {"lighttable", "darkroom", "map", "tethering", NULL};
  static const char *v2[] = {"lighttable", "map", "tethering", NULL};

  if(dt_conf_get_bool("plugins/darkroom/tagging/visible"))
    return v1;
  else
    return v2;
}

uint32_t container(dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
  else
    return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "attach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "detach"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "new"), 0, 0);
  dt_accel_register_lib(self, NC_("accel", "tag"), GDK_KEY_t, GDK_CONTROL_MASK);
  dt_accel_register_lib(self, NC_("accel", "redo last tag"), GDK_KEY_t, GDK_MOD1_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  dt_accel_connect_button_lib(self, "attach", d->attach_button);
  dt_accel_connect_button_lib(self, "detach", d->detach_button);
  dt_accel_connect_button_lib(self, "new", d->new_button);
  dt_accel_connect_lib(self, "tag", g_cclosure_new(G_CALLBACK(_lib_tagging_tag_show), self, NULL));
  dt_accel_connect_lib(self, "redo last tag", g_cclosure_new(G_CALLBACK(_lib_tagging_tag_redo), self, NULL));
}

static void _update_atdetach_buttons(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  const GList *imgs = dt_view_get_images_to_act_on(TRUE, FALSE);
  const gboolean has_act_on = imgs != NULL;

  const gint dict_tags_sel_cnt =
    gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->dictionary_view)));

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->attached_view));
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->attached_view));
  GtkTreeIter iter;
  gboolean attached_tags_sel = FALSE;
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    // check this is a darktable tag
    char *path;
    gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &path, -1);
    if(!g_str_has_prefix(path, "darktable|"))
      attached_tags_sel = TRUE;
    g_free(path);
  }

  gtk_widget_set_sensitive(GTK_WIDGET(d->attach_button), has_act_on && dict_tags_sel_cnt > 0);
  gtk_widget_set_sensitive(GTK_WIDGET(d->detach_button), has_act_on && attached_tags_sel);
}

static void _propagate_sel_to_parents(GtkTreeModel *model, GtkTreeIter *iter)
{
  guint sel;
  GtkTreeIter parent, child = *iter;
  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == DT_TS_NO_IMAGE)
      gtk_tree_store_set(GTK_TREE_STORE(model), &parent,
                         DT_LIB_TAGGING_COL_SEL, DT_TS_SOME_IMAGES, -1);
    child = parent;
  }
}

static gboolean _set_matching_tag_visibility(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  gboolean visible;
  gchar *tagname = NULL;
  gchar *synonyms = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &tagname, DT_LIB_TAGGING_COL_SYNONYM, &synonyms, -1);
  if (!d->keyword[0])
    visible = TRUE;
  else
  {
    if (synonyms && synonyms[0]) tagname = dt_util_dstrcat(tagname, ", %s", synonyms);
    gchar *haystack = g_utf8_strdown(tagname, -1);
    gchar *needle = g_utf8_strdown(d->keyword, -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    g_free(haystack);
    g_free(needle);
  }
  if (d->tree_flag)
    gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_VISIBLE, visible, -1);
  else
    gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_TAGGING_COL_VISIBLE, visible, -1);
  g_free(tagname);
  g_free(synonyms);
  return FALSE;
}

static gboolean _tree_reveal_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  gboolean state;
  GtkTreeIter parent, child = *iter;

  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_VISIBLE, &state, -1);
  if(!state) return FALSE;

  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_VISIBLE, &state, -1);
    gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_TAGGING_COL_VISIBLE, TRUE, -1);
    child = parent;
  }
  return FALSE;
}

static void _sort_attached_list(dt_lib_module_t *self, gboolean force)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (force && d->sort_count_flag)
  {
    // ugly but when sorted by count _tree_tagname_show() is not triggered
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->attached_liststore), DT_TAG_SORT_NAME_ID, GTK_SORT_ASCENDING);
  }
  const gint sort = d->sort_count_flag ? DT_TAG_SORT_COUNT_ID : d->hide_path_flag ? DT_TAG_SORT_NAME_ID : DT_TAG_SORT_PATH_ID;
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->attached_liststore), sort, GTK_SORT_ASCENDING);
}

static void _sort_dictionary_list(dt_lib_module_t *self, gboolean force)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (!d->tree_flag)
  {
    if (force && d->sort_count_flag)
    {
      // ugly but when sorted by count _tree_tagname_show() is not triggered
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->dictionary_liststore), DT_TAG_SORT_NAME_ID, GTK_SORT_ASCENDING);
    }
    const gint sort = d->sort_count_flag ? DT_TAG_SORT_COUNT_ID : d->hide_path_flag ? DT_TAG_SORT_NAME_ID : DT_TAG_SORT_PATH_ID;
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->dictionary_liststore), sort, GTK_SORT_ASCENDING);
  }
  else
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(d->dictionary_treestore), DT_TAG_SORT_PATH_ID, GTK_SORT_ASCENDING);
}

// find a tag on the tree
static gboolean _find_tag_iter_tagname(GtkTreeModel *model, GtkTreeIter *iter,
                                       const char *tagname)
{
  gboolean found = FALSE;
  if(!tagname) return found;
  char *path;
  do
  {
    gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &path, -1);
    found = !g_strcmp0(tagname, path);
    g_free(path);
    if(found) return found;
    GtkTreeIter child, parent = *iter;
    if(gtk_tree_model_iter_children(model, &child, &parent))
    {
      found = _find_tag_iter_tagname(model, &child, tagname);
      if(found)
      {
        *iter = child;
        return found;
      }
    }
  } while(gtk_tree_model_iter_next(model, iter));
  return found;
}

// make the tag visible on view
static void _show_tag_on_view(GtkTreeView *view, const char *tagname)
{
  if(tagname)
  {
    char *lt = g_strdup(tagname);
    char *t = g_strstrip(lt);
    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      if(_find_tag_iter_tagname(model, &iter, t))
      {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        gtk_tree_view_expand_to_path(view, path);
        gtk_tree_view_scroll_to_cell(view, path, NULL, TRUE, 0.5, 0.5);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
        gtk_tree_selection_select_iter(selection, &iter);
      }
    }
    g_free(lt);
  }
}

static void _init_treeview(dt_lib_module_t *self, const int which)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GList *tags = NULL;
  uint32_t count;
  GtkTreeIter iter;
  GtkTreeView *view;
  GtkTreeModel *store;
  GtkTreeModel *model;
  gboolean no_sel = FALSE;

  if(which == 0) // tags of selected images
  {
    const int imgsel = dt_control_get_mouse_over_id();
    no_sel = imgsel > 0 || dt_selected_images_count() == 1;
    count = dt_tag_get_attached(imgsel, &tags, d->dttags_flag ? FALSE : TRUE);
    view = d->attached_view;
    model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    store = model;
  }
  else // dictionary_view tags of typed text
  {
    if (!d->tree_flag && d->suggestion_flag)
      count = dt_tag_get_suggestions(&tags);
    else
      count = dt_tag_get_with_usage(&tags);
    view = d->dictionary_view;
    model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    if (d->tree_flag)
      store = GTK_TREE_MODEL(d->dictionary_treestore);
    else
      store = GTK_TREE_MODEL(d->dictionary_liststore);
  }
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
  if (which && d->tree_flag)
  {
    gtk_tree_store_clear(GTK_TREE_STORE(store));
    {
      char **last_tokens = NULL;
      int last_tokens_length = 0;
      GtkTreeIter last_parent = { 0 };
      GList *sorted_tags = dt_sort_tag(tags, 0);  // ordered by full tag name
      tags = sorted_tags;
      for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
      {
        const gchar *tag = ((dt_tag_t *)taglist->data)->tag;
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
              gtk_tree_model_iter_parent(GTK_TREE_MODEL(store), &parent, &last_parent);
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
            gtk_tree_store_insert(GTK_TREE_STORE(store), &iter, common_length > 0 ? &parent : NULL, -1);
            gtk_tree_store_set(GTK_TREE_STORE(store), &iter,
                              DT_LIB_TAGGING_COL_TAG, *token,
                              DT_LIB_TAGGING_COL_ID, (token == &tokens[tokens_length-1]) ?
                                                     ((dt_tag_t *)taglist->data)->id : 0,
                              DT_LIB_TAGGING_COL_PATH, pth2,
                              DT_LIB_TAGGING_COL_COUNT, (token == &tokens[tokens_length-1]) ?
                                                        ((dt_tag_t *)taglist->data)->count : 0,
                              DT_LIB_TAGGING_COL_SEL, ((dt_tag_t *)taglist->data)->select,
                              DT_LIB_TAGGING_COL_FLAGS, ((dt_tag_t *)taglist->data)->flags,
                              DT_LIB_TAGGING_COL_SYNONYM, ((dt_tag_t *)taglist->data)->synonym,
                              DT_LIB_TAGGING_COL_VISIBLE, TRUE,
                              -1);
            if (((dt_tag_t *)taglist->data)->select)
              _propagate_sel_to_parents(GTK_TREE_MODEL(store), &iter);
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
    }
    if (d->keyword[0])
    {
      gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_set_matching_tag_visibility, self);
      gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_tree_reveal_func, NULL);
      gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
      gtk_tree_view_expand_all(d->dictionary_view);
    }
    else gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
  }
  else
  {
    gtk_list_store_clear(GTK_LIST_STORE(store));
    if(count > 0 && tags)
    {
      for (GList *tag = tags; tag; tag = g_list_next(tag))
      {
        const char *subtag = g_strrstr(((dt_tag_t *)tag->data)->tag, "|");
        gtk_list_store_append(GTK_LIST_STORE(store), &iter);
        gtk_list_store_set(GTK_LIST_STORE(store), &iter,
                          DT_LIB_TAGGING_COL_TAG, !subtag ? ((dt_tag_t *)tag->data)->tag : subtag + 1,
                          DT_LIB_TAGGING_COL_ID, ((dt_tag_t *)tag->data)->id,
                          DT_LIB_TAGGING_COL_PATH, ((dt_tag_t *)tag->data)->tag,
                          DT_LIB_TAGGING_COL_COUNT, ((dt_tag_t *)tag->data)->count,
                          DT_LIB_TAGGING_COL_SEL, no_sel ? DT_TS_NO_IMAGE :
                                                  ((dt_tag_t *)tag->data)->select,
                          DT_LIB_TAGGING_COL_FLAGS, ((dt_tag_t *)tag->data)->flags,
                          DT_LIB_TAGGING_COL_SYNONYM, ((dt_tag_t *)tag->data)->synonym,
                          DT_LIB_TAGGING_COL_VISIBLE, TRUE,
                          -1);
      }
    }
    if (which && d->keyword[0])
    {
      gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_set_matching_tag_visibility, self);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
  }
  if (which)
    _sort_dictionary_list(self, FALSE);
  else
    _sort_attached_list(self, FALSE);
  // Free result...
  dt_tag_free_result(&tags);
}

static void _tree_tagname_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                               GtkTreeModel *model, GtkTreeIter *iter,
                               gpointer data, gboolean dictionary_view)
{
  dt_lib_module_t *self = (dt_lib_module_t *)data;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  guint tagid;
  gchar *name;
  gchar *path;
  guint count;
  gchar *coltext;
  gint flags;

  gtk_tree_model_get(model, iter,
                     DT_LIB_TAGGING_COL_ID, &tagid,
                     DT_LIB_TAGGING_COL_TAG, &name,
                     DT_LIB_TAGGING_COL_COUNT, &count,
                     DT_LIB_TAGGING_COL_FLAGS, &flags,
                     DT_LIB_TAGGING_COL_PATH, &path, -1);
  const gboolean hide = dictionary_view ? (d->tree_flag ? TRUE : d->hide_path_flag) : d->hide_path_flag;
  const gboolean istag = !(flags & DT_TF_CATEGORY) && tagid;
  if ((dictionary_view && !count) || (!dictionary_view && count <= 1))
  {
    coltext = g_markup_printf_escaped(istag ? "%s" : "<i>%s</i>", hide ? name : path);
  }
  else
  {
    coltext = g_markup_printf_escaped(istag ? "%s (%d)" : "<i>%s</i> (%d)", hide ? name : path, count);
  }
  g_object_set(renderer, "markup", coltext, NULL);
  g_free(coltext);
  g_free(name);
  g_free(path);
}

static void _tree_tagname_show_attached(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                                        GtkTreeModel *model, GtkTreeIter *iter,
                                        gpointer data)
{
  _tree_tagname_show(col, renderer, model, iter, data, 0);
}

static void _tree_tagname_show_dictionary(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                                          GtkTreeModel *model, GtkTreeIter *iter,
                                          gpointer data)
{
  _tree_tagname_show(col, renderer, model, iter, data, 1);
}

static void _tree_select_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                              GtkTreeModel *model, GtkTreeIter *iter,
                              gpointer data)
{
  guint tagid;
  guint select;
  gboolean active = FALSE;
  gboolean inconsistent = FALSE;

  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tagid,
                                  DT_LIB_TAGGING_COL_SEL, &select, -1);
  if (!tagid)
  {
    if (select != DT_TS_NO_IMAGE) inconsistent = TRUE;
  }
  else
  {
    if (select == DT_TS_ALL_IMAGES) active = TRUE;
    else if (select == DT_TS_SOME_IMAGES) inconsistent = TRUE;
  }
  g_object_set(renderer, "active", active, "inconsistent", inconsistent, NULL);
}

static void _postponed_update(dt_lib_module_t *self)
{
  _init_treeview(self, 0);
  _update_atdetach_buttons(self);
}

static void _lib_tagging_redraw_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_queue_postponed_update(self, _postponed_update);
}

static void _lib_tagging_tags_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _init_treeview(self, 0);
  _init_treeview(self, 1);
}

static void _collection_updated_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                        int next, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  d->collection[0] = '\0';
  _update_atdetach_buttons(self);
}

static void _raise_signal_tag_changed(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  // when collection is on tag any attach & detach becomes very slow
  // speeding up when jumping from tag collection to the other
  // the cost is that tag collection doesn't reflect the tag changes real time
  if (!d->collection[0])
  {
    // raises change only for other modules
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
  }
}

// find a tag on the tree
static gboolean _find_tag_iter_tagid(GtkTreeModel *model, GtkTreeIter *iter, gint tagid)
{
  gint tag;
  do
  {
    gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tag, -1);
    if (tag == tagid)
    {
      return TRUE;
    }
    GtkTreeIter child, parent = *iter;
    if (gtk_tree_model_iter_children(model, &child, &parent))
      if (_find_tag_iter_tagid(model, &child, tagid))
      {
        *iter = child;
        return TRUE;
      }
  } while (gtk_tree_model_iter_next(model, iter));
  return FALSE;
}

// calculate the indeterminated state (1) where needed on the tree
static void _calculate_sel_on_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    gint sel = DT_TS_NO_IMAGE;
    gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == DT_TS_ALL_IMAGES)
    {
      _propagate_sel_to_parents(model, &parent);
    }
    if (gtk_tree_model_iter_children(model, &child, &parent))
      _calculate_sel_on_path(model, &child, FALSE);
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}

// reset the indeterminated selection (1) on the tree
static void _reset_sel_on_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    if (gtk_tree_model_iter_children(model, &child, &parent))
    {
      gint sel = DT_TS_NO_IMAGE;
      gtk_tree_model_get(model, &parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
      if (sel == DT_TS_SOME_IMAGES)
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &parent,
                           DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE, -1);
      }
      _reset_sel_on_path(model, &child, FALSE);
    }
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}

// reset all selection (1 & 2) on the tree
static void _reset_sel_on_path_full(GtkTreeModel *model, GtkTreeIter *iter, gboolean root)
{
  GtkTreeIter child, parent = *iter;
  do
  {
    if(GTK_IS_TREE_STORE(model))
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), &parent,
                         DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE, -1);
      if (gtk_tree_model_iter_children(model, &child, &parent))
        _reset_sel_on_path_full(model, &child, FALSE);
    }
    else
    {
      gtk_list_store_set(GTK_LIST_STORE(model), &parent,
                         DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE, -1);
    }
  } while (!root && gtk_tree_model_iter_next(model, &parent));
}

//  try to find a node fully attached (2) which is the root of the update loop. If not the full tree will be used
static void _find_root_iter_iter(GtkTreeModel *model, GtkTreeIter *iter, GtkTreeIter *parent)
{
  guint sel;
  GtkTreeIter child = *iter;
  while (gtk_tree_model_iter_parent(model, parent, &child))
  {
    gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == DT_TS_ALL_IMAGES)
    {
      char *path = NULL;
      gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_PATH, &path, -1);
      g_free(path);
      return; // no need to go further
    }
    child = *parent;
  }
  *parent = child;  // last before root
  char *path = NULL;
  gtk_tree_model_get(model, parent, DT_LIB_TAGGING_COL_PATH, &path, -1);
  g_free(path);
}

// with tag detach update the tree selection
static void _calculate_sel_on_tree(GtkTreeModel *model, GtkTreeIter *iter)
{
  GtkTreeIter parent;
  if (iter)
  {
    // only on sub-tree
    _find_root_iter_iter(model, iter, &parent);
    _reset_sel_on_path(model, &parent, TRUE);
    _calculate_sel_on_path(model, &parent, TRUE);
  }
  else
  {
    // on full tree
    if(gtk_tree_model_get_iter_first(model, &parent))
    {
      _reset_sel_on_path(model, &parent, FALSE);
      _calculate_sel_on_path(model, &parent, FALSE);
    }
  }
}

// get the new selected images and update the tree selection
static void _update_sel_on_tree(GtkTreeModel *model)
{
  GList *tags = NULL;
  const guint count = dt_tag_get_attached(-1, &tags, TRUE);
  if(count > 0 && tags)
  {
    GtkTreeIter parent;
    if(gtk_tree_model_get_iter_first(model, &parent))
    {
      _reset_sel_on_path_full(model, &parent, FALSE);
      for (GList *tag = tags; tag; tag = g_list_next(tag))
      {
        GtkTreeIter iter = parent;
        if (_find_tag_iter_tagid(model, &iter, ((dt_tag_t *)tag->data)->id))
        {
          if(GTK_IS_TREE_STORE(model))
          {
            gtk_tree_store_set(GTK_TREE_STORE(model), &iter,
                               DT_LIB_TAGGING_COL_SEL, ((dt_tag_t *)tag->data)->select, -1);
            _propagate_sel_to_parents(model, &iter);
          }
          else
          {
            gtk_list_store_set(GTK_LIST_STORE(model), &iter,
                               DT_LIB_TAGGING_COL_SEL, ((dt_tag_t *)tag->data)->select, -1);
          }
        }
      }
    }
  }
  dt_tag_free_result(&tags);
}

// delete a tag in the tree (tree or list)
static void _delete_tree_tag(GtkTreeModel *model, GtkTreeIter *iter, gboolean tree)
{
  guint tagid = 0;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if (tree)
  {
    if (tagid)
    {
      gtk_tree_store_set(GTK_TREE_STORE(model), iter,
                         DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE,
                         DT_LIB_TAGGING_COL_ID, 0,
                         DT_LIB_TAGGING_COL_COUNT, 0, -1);
      _calculate_sel_on_tree(model, iter);
      GtkTreeIter child, parent = *iter;
      if (!gtk_tree_model_iter_children(model, &child, &parent))
        gtk_tree_store_remove(GTK_TREE_STORE(model), iter);
    }
  }
  else
  {
    gtk_list_store_remove(GTK_LIST_STORE(model), iter);
  }
}

// delete a branch of the tag tree
static void _delete_tree_path(GtkTreeModel *model, GtkTreeIter *iter, gboolean root, gboolean tree)
{
  if (tree) // the treeview is tree. It handles the hierarchy itself (parent / child)
  {
    GtkTreeIter child, parent = *iter;
    gboolean valid = TRUE;
    do
    {
      if (gtk_tree_model_iter_children(model, &child, &parent))
        _delete_tree_path(model, &child, FALSE, tree);
      GtkTreeIter tobedel = parent;
      valid = gtk_tree_model_iter_next(model, &parent);
      if (root)
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), &tobedel,
                           DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE,
                           DT_LIB_TAGGING_COL_COUNT, 0, -1);

        char *path2 = NULL;
        gtk_tree_model_get(model, &tobedel, DT_LIB_TAGGING_COL_PATH, &path2, -1);
        g_free(path2);

        _calculate_sel_on_tree(model, &tobedel);
      }
      char *path = NULL;
      gtk_tree_model_get(model, &tobedel, DT_LIB_TAGGING_COL_PATH, &path, -1);
      g_free(path);
      gtk_tree_store_remove(GTK_TREE_STORE(model), &tobedel);
    } while (!root && valid);
  }
  else  // treeview is a list. The hierarchy of tags is found with the root (left part) of tagname
  {
    GtkTreeIter child;
    char *path = NULL;
    gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &path, -1);
    guint pathlen = strlen(path);
    gboolean valid = gtk_tree_model_get_iter_first(model, &child);
    while(valid)
    {
      char *path2 = NULL;
      gtk_tree_model_get(model, &child, DT_LIB_TAGGING_COL_PATH, &path2, -1);
      GtkTreeIter tobedel = child;
      valid = gtk_tree_model_iter_next (model, &child);
      if (strlen(path2) >= pathlen)
      {
        char letter = path2[pathlen];
        path2[pathlen] = '\0';
        if (g_strcmp0(path, path2) == 0)
        {
          path2[pathlen] = letter;
          gtk_list_store_remove(GTK_LIST_STORE(model), &tobedel);
        }
      }
      g_free(path2);
    }
    g_free(path);
  }
}

static void _lib_selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _init_treeview(self, 0);
  if (!d->tree_flag && d->suggestion_flag)
  {
    _init_treeview(self, 1);
  }
  else
    _update_sel_on_tree(d->tree_flag ? GTK_TREE_MODEL(d->dictionary_treestore)
                                    : GTK_TREE_MODEL(d->dictionary_liststore));

  _update_atdetach_buttons(self);
}

static void _set_keyword(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *beg = g_strrstr(gtk_entry_get_text(d->entry), ",");

  if(!beg)
    beg = gtk_entry_get_text(d->entry);
  else
  {
    if(*beg == ',') beg++;
    if(*beg == ' ') beg++;
  }
  g_strlcpy(d->keyword, beg, sizeof(d->keyword));
}

static gboolean _update_tag_name_per_name(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, dt_tag_op_t *to)
{
  char *tagname;
  char *newtagname = to->newtagname;
  char *oldtagname = to->oldtagname;
  gtk_tree_model_get(model, iter, DT_LIB_TAGGING_COL_PATH, &tagname, -1);
  if (g_str_has_prefix(tagname, oldtagname))
  {
    if (strlen(tagname) == strlen(oldtagname))
    {
      // rename the tag itself
      if (to->tree_flag)
      {
        char *subtag = g_strrstr(to->newtagname, "|");
        subtag = (!subtag) ? newtagname : subtag + 1;
        gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_PATH,
                           newtagname, DT_LIB_TAGGING_COL_TAG, subtag, -1);
      }
      else
      {
        gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_TAGGING_COL_PATH,
                           newtagname, DT_LIB_TAGGING_COL_TAG, newtagname, -1);
      }
    }
    else if (strlen(tagname) > strlen(oldtagname) && tagname[strlen(oldtagname)] == '|')
    {
      // rename similar path
      char *newpath = g_strconcat(newtagname, &tagname[strlen(oldtagname)] , NULL);
      if (to->tree_flag)
      {
        gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_TAGGING_COL_PATH,
                           newpath, -1);
      }
      else
      {
        gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_TAGGING_COL_PATH,
                           newpath, DT_LIB_TAGGING_COL_TAG, newpath, -1);
      }
      g_free(newpath);
    }
  }
  g_free(tagname);
  return FALSE;
}

void init_presets(dt_lib_module_t *self)
{

}

void *get_params(dt_lib_module_t *self, int *size)
{
  char *params = NULL;
  *size = 0;
  GList *tags = NULL;
  const guint count = dt_tag_get_attached(-1, &tags, TRUE);

  if(count)
  {
    for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
    {
      params = dt_util_dstrcat(params, "%d,", ((dt_tag_t *)taglist->data)->id);
    }
    dt_tag_free_result(&tags);
    *size = strlen(params);
    params[*size-1]='\0';
  }
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params || !size) return 1;
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  const char *buf = (char *)params;
  if (buf && buf[0])
  {
    GtkTreeModel *model = gtk_tree_view_get_model(d->dictionary_view);
    GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
    GtkTreeIter iter;
    gchar **tokens = g_strsplit(buf, ",", 0);
    if(tokens)
    {
      gchar **entry = tokens;
      gboolean some_attached = FALSE;
      while(*entry)
      {
        guint tagid = strtoul(*entry, NULL, 0);
        // attach tag on images to act on
        const gboolean attached = dt_tag_attach(tagid, -1, TRUE, TRUE);
        if(attached) some_attached = TRUE;

        const guint count = dt_tag_images_count(tagid);
        if(gtk_tree_model_get_iter_first(store, &iter))
        {
          if (_find_tag_iter_tagid(store, &iter, tagid))
          {
            if (d->tree_flag)
            {
              gtk_tree_store_set(GTK_TREE_STORE(store), &iter,
                                 DT_LIB_TAGGING_COL_COUNT, count,
                                 DT_LIB_TAGGING_COL_SEL, DT_TS_ALL_IMAGES, -1);
              _calculate_sel_on_tree(GTK_TREE_MODEL(store), &iter);
            }
            else
            {
              gtk_list_store_set(GTK_LIST_STORE(store), &iter,
                                 DT_LIB_TAGGING_COL_COUNT, count,
                                 DT_LIB_TAGGING_COL_SEL, DT_TS_ALL_IMAGES, -1);
            }
          }
        }
        entry++;
      }
      g_strfreev(tokens);
      if(some_attached)
      {
        _init_treeview(self, 0);
        _raise_signal_tag_changed(self);
        dt_image_synch_xmp(-1);
      }
    }
  }
  return 0;
}

static void _attach_selected_tag(dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->dictionary_view);
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)
     && !gtk_tree_model_get_iter_first(model, &iter))
    return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if(tagid <= 0) return;

  // attach tag on images to act on
  if(dt_tag_attach(tagid, -1, TRUE, TRUE))
  {
    /** record last tag used */
    g_free(d->last_tag);
    d->last_tag = g_strdup(dt_tag_get_name(tagid));

    _init_treeview(self, 0);
    if (d->tree_flag || !d->suggestion_flag)
    {
      const uint32_t count = dt_tag_images_count(tagid);
      GtkTreeIter store_iter;
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model),
                                &store_iter, &iter);
      if (d->tree_flag)
      {
        gtk_tree_store_set(GTK_TREE_STORE(store), &store_iter,
                           DT_LIB_TAGGING_COL_COUNT, count,
                           DT_LIB_TAGGING_COL_SEL, DT_TS_ALL_IMAGES, -1);
        _propagate_sel_to_parents(GTK_TREE_MODEL(store), &store_iter);
      }
      else
      {
        gtk_list_store_set(GTK_LIST_STORE(store), &store_iter,
                           DT_LIB_TAGGING_COL_COUNT, count,
                           DT_LIB_TAGGING_COL_SEL, DT_TS_ALL_IMAGES, -1);
      }
    }
    else
    {
      _init_treeview(self, 1);
    }
    _raise_signal_tag_changed(self);
    dt_image_synch_xmp(-1);
  }
}

static void _detach_selected_tag(GtkTreeView *view, dt_lib_module_t *self, dt_lib_tagging_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if(tagid <= 0) return;

  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE);
  if(!imgs) return;

  GList *affected_images = dt_tag_get_images_from_list(imgs, tagid);
  if(affected_images)
  {
    const gboolean res = dt_tag_detach_images(tagid, affected_images, TRUE);

    _init_treeview(self, 0);
    if (d->tree_flag || !d->suggestion_flag)
    {
      const guint count = dt_tag_images_count(tagid);
      model = gtk_tree_view_get_model(d->dictionary_view);
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      if(gtk_tree_model_get_iter_first(store, &iter))
      {
        if (_find_tag_iter_tagid(store, &iter, tagid))
        {
          if (d->tree_flag)
          {
            gtk_tree_store_set(GTK_TREE_STORE(store), &iter,
                               DT_LIB_TAGGING_COL_COUNT, count,
                               DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE, -1);
            _calculate_sel_on_tree(GTK_TREE_MODEL(store), &iter);
          }
          else
          {
            gtk_list_store_set(GTK_LIST_STORE(store), &iter,
                               DT_LIB_TAGGING_COL_COUNT, count,
                               DT_LIB_TAGGING_COL_SEL, DT_TS_NO_IMAGE, -1);
          }
        }
      }
    }
    else
    {
      _init_treeview(self, 1);
    }
    if(res)
    {
      _raise_signal_tag_changed(self);
      dt_image_synch_xmps(affected_images);
    }
    g_list_free(affected_images);
  }
}

static void _attach_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _attach_selected_tag(self, d);
}

static void _detach_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _detach_selected_tag(d->attached_view, self, d);
}

static void _pop_menu_attached_attach_to_all(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->attached_view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->attached_view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter))
    return;
  guint tagid;
  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if(tagid <= 0) return;

  // attach tag on images to act on
  const gboolean res = dt_tag_attach(tagid, -1, TRUE, TRUE);

  /** record last tag used */
  g_free(d->last_tag);
  d->last_tag = g_strdup(dt_tag_get_name(tagid));

  _init_treeview(self, 0);

  const uint32_t count = dt_tag_images_count(tagid);
  model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view));
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    if(_find_tag_iter_tagid(model, &iter, tagid))
    {
      GtkTreeIter store_iter;
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model),
                                &store_iter, &iter);
      if (d->tree_flag)
      {
        gtk_tree_store_set(GTK_TREE_STORE(store), &store_iter, DT_LIB_TAGGING_COL_COUNT, count, -1);
      }
      else
      {
        gtk_list_store_set(GTK_LIST_STORE(store), &store_iter, DT_LIB_TAGGING_COL_COUNT, count, -1);
      }
    }
  }

  if(res)
  {
    _raise_signal_tag_changed(self);
    dt_image_synch_xmp(-1);
  }
}

static void _pop_menu_attached_detach(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _detach_selected_tag(d->attached_view, self, d);
}

static void _pop_menu_attached(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkWidget *menu, *menuitem;
  menu = gtk_menu_new();

  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->attached_view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->attached_view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    guint sel;
    gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_SEL, &sel, -1);
    if (sel == DT_TS_SOME_IMAGES)
    {
      menuitem = gtk_menu_item_new_with_label(_("attach tag to all"));
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_attached_attach_to_all, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      menuitem = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    }
  }

  menuitem = gtk_menu_item_new_with_label(_("detach tag"));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
  g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_attached_detach, self);

  gtk_widget_show_all(GTK_WIDGET(menu));

#if GTK_CHECK_VERSION(3, 22, 0)
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
#else
  /* Note: event can be NULL here when called from view_onPopupMenu;
   *  gdk_event_get_time() accepts a NULL argument */
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, (event != NULL) ? event->button : 0,
                 gdk_event_get_time((GdkEvent *)event));
#endif
}

static gboolean _click_on_view_attached(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  if((event->type == GDK_BUTTON_PRESS && event->button == 3)
    || (event->type == GDK_2BUTTON_PRESS && event->button == 1))
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
    {
      gboolean valid_tag = FALSE;
      GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->attached_view));
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
      {
        // check this is a darktable tag
        char *tagpath;
        gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagpath, -1);
        if(!g_str_has_prefix(tagpath, "darktable|"))
          valid_tag = TRUE;
        g_free(tagpath);
      }
      if(valid_tag)
      {
        gtk_tree_selection_select_path(selection, path);
        _update_atdetach_buttons(self);
        if(event->type == GDK_BUTTON_PRESS && event->button == 3)
        {
          _pop_menu_attached(view, event, self);
          gtk_tree_path_free(path);
          return TRUE;
        }
        else if(event->type == GDK_2BUTTON_PRESS && event->button == 1)
        {
          _detach_selected_tag(d->attached_view, self, d);
          gtk_tree_path_free(path);
          return TRUE;
        }
      }
    }
    gtk_tree_path_free(path);
  }
  return FALSE;
}

static void _new_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  const gchar *tag = gtk_entry_get_text(d->entry);
  if(!tag || tag[0] == '\0') return;

  const GList *imgs = dt_view_get_images_to_act_on(FALSE, TRUE);
  const gboolean res = dt_tag_attach_string_list(tag, imgs, TRUE);
  if(res) dt_image_synch_xmps(imgs);

  /** record last tag used */
  g_free(d->last_tag);
  d->last_tag = g_strdup(tag);

  /** clear input box */
  gtk_entry_set_text(d->entry, "");

  _init_treeview(self, 0);
  _init_treeview(self, 1);
  char *tagname = strrchr(d->last_tag, ',');
  if(res) _raise_signal_tag_changed(self);
  _show_tag_on_view(GTK_TREE_VIEW(d->dictionary_view),
                    tagname ? tagname + 1 : d->last_tag);
}

static gboolean _key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      _new_button_clicked(NULL, self);
      break;
    case GDK_KEY_Escape:
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      break;
    default:
      break;
  }
  return FALSE;
}

static void _clear_entry_button_callback(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  /** clear input box */
  gtk_entry_set_text(d->entry, "");
}

static void _tag_name_changed(GtkEntry *entry, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _set_keyword(self);
  GtkTreeModel *model = gtk_tree_view_get_model(d->dictionary_view);
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
  gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_set_matching_tag_visibility, self);
  if (d->tree_flag && d->keyword[0])
  {
    gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_tree_reveal_func, NULL);
    gtk_tree_view_expand_all(d->dictionary_view);
  }
}

static void _pop_menu_dictionary_delete_tag(GtkWidget *menuitem, dt_lib_module_t *self, gboolean branch)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->dictionary_view;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);
  if (!tagid) return;
  const guint img_count = dt_tag_remove(tagid, FALSE);

  if (img_count > 0 || dt_conf_get_bool("plugins/lighttable/tagging/ask_before_delete_tag"))
  {
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("delete tag?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                  _("cancel"), GTK_RESPONSE_NONE, _("delete"), GTK_RESPONSE_YES, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(area), vbox);
    text = g_strdup_printf(_("tag: %s "), tagname);
    label = gtk_label_new(text);
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
    g_free(text);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
    text = g_markup_printf_escaped(ngettext("do you really want to delete the tag `%s'?\n%d image is assigned this tag!",
             "do you really want to delete the tag `%s'?\n%d images are assigned this tag!", img_count), tagname, img_count);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), text);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    g_free(text);

  #ifdef GDK_WINDOWING_QUARTZ
    dt_osx_disallow_fullscreen(dialog);
  #endif
    gtk_widget_show_all(dialog);

    res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
  if(res != GTK_RESPONSE_YES)
  {
    g_free(tagname);
    return;
  }

  GList *tagged_images = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.tagged_images WHERE tagid=?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    tagged_images = g_list_append(tagged_images, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  dt_tag_remove(tagid, TRUE);
  dt_control_log(_("tag %s removed"), tagname);

  GtkTreeIter store_iter;
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
  gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model),
                            &store_iter, &iter);
  _delete_tree_tag(GTK_TREE_MODEL(store), &store_iter, d->tree_flag);
  _init_treeview(self, 0);

  dt_image_synch_xmps(tagged_images);
  g_list_free(tagged_images);
  g_free(tagname);
  _raise_signal_tag_changed(self);
}

static void _pop_menu_dictionary_delete_path(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  int res = GTK_RESPONSE_YES;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->dictionary_view;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);

  gint tag_count = 0;
  gint img_count = 0;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons( _("delete path"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                _("cancel"), GTK_RESPONSE_NONE, _("delete"), GTK_RESPONSE_YES, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);
  text = g_strdup_printf(_("tag: %s "), tagname);
  label = gtk_label_new(text);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  text = g_strdup_printf(ngettext("<u>%d</u> tag will be deleted.", "<u>%d</u> tags will be deleted.", tag_count), tag_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);
  text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  res = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  if (res != GTK_RESPONSE_YES)
  {
    g_free(tagname);
    return;
  }

  GList *tag_family = NULL;
  GList *tagged_images = NULL;
  dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  tag_count = dt_tag_remove_list(tag_family);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  dt_control_log(_("%d tags removed"), tag_count);

  GtkTreeIter store_iter;
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
  gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model),
                            &store_iter, &iter);
  _delete_tree_path(GTK_TREE_MODEL(store), &store_iter, TRUE, d->tree_flag);
  _init_treeview(self, 0);

  dt_tag_free_result(&tag_family);
  dt_image_synch_xmps(tagged_images);
  g_list_free(tagged_images);
  _raise_signal_tag_changed(self);
  g_free(tagname);
}

// create tag allows the user to create a single tag, which can be an element of the hierarchy or not
static void _pop_menu_dictionary_create_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  char *tagname;
  char *path;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->dictionary_view;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_TAG, &tagname,
        DT_LIB_TAGGING_COL_PATH, &path, DT_LIB_TAGGING_COL_ID, &tagid, -1);

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("create tag"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  label = gtk_label_new(_("name: "));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  GtkWidget *entry = gtk_entry_new();
  gtk_box_pack_end(GTK_BOX(box), entry, TRUE, TRUE, 0);

  GtkWidget *category;
  GtkWidget *private;
  GtkWidget *parent;
  GtkTextBuffer *buffer = NULL;
  GtkWidget *vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), vbox2, FALSE, TRUE, 0);

  text = g_strdup_printf(_("add to: \"%s\" "), path);
  parent = gtk_check_button_new_with_label(text);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(parent), TRUE);
  gtk_box_pack_end(GTK_BOX(vbox2), parent, FALSE, TRUE, 0);
  g_free(text);

  category = gtk_check_button_new_with_label(_("category"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(category), FALSE);
  gtk_box_pack_end(GTK_BOX(vbox2), category, FALSE, TRUE, 0);
  private = gtk_check_button_new_with_label(_("private"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(private), FALSE);
  gtk_box_pack_end(GTK_BOX(vbox2), private, FALSE, TRUE, 0);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_end(GTK_BOX(vbox), box, TRUE, TRUE, 0);
  label = gtk_label_new(_("synonyms: "));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  GtkWidget *synonyms = gtk_text_view_new();
  gtk_box_pack_end(GTK_BOX(box), synonyms, TRUE, TRUE, 0);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(synonyms), GTK_WRAP_WORD);
  buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(synonyms));

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *newtag = gtk_entry_get_text(GTK_ENTRY(entry));
    char *message = NULL;
    if (!newtag[0])
      message = _("empty tag is not allowed, aborting");
    char *new_tagname = NULL;
    const gboolean root = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(parent));
    if (!root)
    {
      new_tagname = g_strdup(path);
      new_tagname = dt_util_dstrcat(new_tagname, "|%s", newtag);
    }
    else new_tagname = g_strdup(newtag);

    if (dt_tag_exists(new_tagname, NULL))
      message = _("tag name already exists. aborting.");
    if (message)
    {
      GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                      GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
      gtk_dialog_run(GTK_DIALOG(warning_dialog));
      gtk_widget_destroy(warning_dialog);
      gtk_widget_destroy(dialog);
      g_free(tagname);
      return;
    }
    guint new_tagid = 0;
    if (dt_tag_new(new_tagname, &new_tagid))
    {
      const gint new_flags = ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(category)) ? DT_TF_CATEGORY : 0) |
                      (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(private)) ? DT_TF_PRIVATE : 0));
      if (new_tagid) dt_tag_set_flags(new_tagid, new_flags);
      GtkTextIter start, end;
      gtk_text_buffer_get_start_iter(buffer, &start);
      gtk_text_buffer_get_end_iter(buffer, &end);
      gchar *new_synonyms_list = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
      if (new_tagid && new_synonyms_list && new_synonyms_list[0])
        dt_tag_set_synonyms(new_tagid, new_synonyms_list);
      g_free(new_synonyms_list);
      _init_treeview(self, 1);
      _show_tag_on_view(view, new_tagname);
    }
    g_free(new_tagname);
  }
  _init_treeview(self, 0);
  gtk_widget_destroy(dialog);
  g_free(tagname);
}

// edit tag allows the user to rename a single tag, which can be an element of the hierarchy and change other parameters
static void _pop_menu_dictionary_edit_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  char *tagname;
  char *synonyms_list;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->dictionary_view;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_SYNONYM, &synonyms_list, DT_LIB_TAGGING_COL_ID, &tagid, -1);
  char *subtag = g_strrstr(tagname, "|");
  if(subtag) subtag = subtag + 1;
  gint tag_count;
  gint img_count;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("edit tag"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);
  text = g_strdup_printf(_("tag: %s "), tagname);
  label = gtk_label_new(text);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  text = g_strdup_printf(ngettext("<u>%d</u> tag will be updated.", "<u>%d</u> tags will be updated.", tag_count), tag_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);
  text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  label = gtk_label_new(_("name: "));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), subtag ? subtag : tagname);
  gtk_box_pack_end(GTK_BOX(box), entry, TRUE, TRUE, 0);

  gint flags = 0;
  GtkWidget *category;
  GtkWidget *private;
  GtkTextBuffer *buffer = NULL;
  if (tagid)
  {
    GtkWidget *vbox2 = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(vbox), vbox2, FALSE, TRUE, 0);
    flags = dt_tag_get_flags(tagid);
    category = gtk_check_button_new_with_label(_("category"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(category), flags & DT_TF_CATEGORY);
    gtk_box_pack_end(GTK_BOX(vbox2), category, FALSE, TRUE, 0);
    private = gtk_check_button_new_with_label(_("private"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(private), flags & DT_TF_PRIVATE);
    gtk_box_pack_end(GTK_BOX(vbox2), private, FALSE, TRUE, 0);

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_end(GTK_BOX(vbox), box, TRUE, TRUE, 0);
    label = gtk_label_new(_("synonyms: "));
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
    GtkWidget *synonyms = gtk_text_view_new();
    gtk_box_pack_end(GTK_BOX(box), synonyms, TRUE, TRUE, 0);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(synonyms), GTK_WRAP_WORD);
    buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(synonyms));
    if (synonyms_list) gtk_text_buffer_set_text(buffer, synonyms_list, -1);
  }

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *newtag = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_strcmp0(newtag, subtag ? subtag : tagname) != 0)
    {
      // tag name has changed
      char *message = NULL;
      if (!newtag[0])
        message = _("empty tag is not allowed, aborting");
      if(strchr(newtag, '|') != 0)
        message = _("'|' character is not allowed for renaming tag.\nto modify the hierachy use rename path instead. Aborting.");
      if (message)
      {
        GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
        gtk_dialog_run(GTK_DIALOG(warning_dialog));
        gtk_widget_destroy(warning_dialog);
        gtk_widget_destroy(dialog);
        g_free(tagname);
        return;
      }

      GList *tag_family = NULL;
      GList *tagged_images = NULL;
      dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

      const int tagname_len = strlen(tagname);
      char *new_prefix_tag;
      if (subtag)
      {
        const int subtag_len = strlen(subtag);
        const char letter = tagname[tagname_len - subtag_len];
        tagname[tagname_len - subtag_len] = '\0';
        new_prefix_tag = g_strconcat(tagname, newtag, NULL);
        tagname[tagname_len - subtag_len] = letter;
      }
      else
        new_prefix_tag = (char *)newtag;

      // check if one of the new tagnames already exists.
      gboolean tagname_exists = FALSE;
      for (GList *taglist = tag_family; taglist && !tagname_exists; taglist = g_list_next(taglist))
      {
        char *new_tagname = g_strconcat(new_prefix_tag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
        tagname_exists = dt_tag_exists(new_tagname, NULL);
        if (tagname_exists)
        {
          GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                          GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                          _("at least one new tag name (%s) already exists, aborting"), new_tagname);
          gtk_dialog_run(GTK_DIALOG(warning_dialog));
          gtk_widget_destroy(warning_dialog);
          g_free(new_tagname);
          if (subtag) g_free(new_prefix_tag);
          gtk_widget_destroy(dialog);
          g_free(tagname);
          return;
        };
        g_free(new_tagname);
      }

      // rename related tags
      for (GList *taglist = tag_family; taglist; taglist = g_list_next(taglist))
      {
        char *new_tagname = g_strconcat(new_prefix_tag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
        dt_tag_rename(((dt_tag_t *)taglist->data)->id, new_tagname);
        g_free(new_tagname);
      }

      // update the store
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      dt_tag_op_t *to = g_malloc(sizeof(dt_tag_op_t));
      to->tree_flag = d->tree_flag;
      to->oldtagname = tagname;
      to->newtagname = new_prefix_tag;
      gint sort_column;
      GtkSortType sort_order;
      gtk_tree_sortable_get_sort_column_id(GTK_TREE_SORTABLE(store), &sort_column, &sort_order);
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);
      gtk_tree_model_foreach(store, (GtkTreeModelForeachFunc)_update_tag_name_per_name, to);
      gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), sort_column, sort_order);
      g_free(to);
      if (subtag) g_free(new_prefix_tag);

      _raise_signal_tag_changed(self);
      dt_tag_free_result(&tag_family);
      dt_image_synch_xmps(tagged_images);
      g_list_free(tagged_images);
    }

    if (tagid)
    {
      gint new_flags = ((gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(category)) ? DT_TF_CATEGORY : 0) |
                      (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(private)) ? DT_TF_PRIVATE : 0));
      GtkTextIter start, end;
      gtk_text_buffer_get_start_iter(buffer, &start);
      gtk_text_buffer_get_end_iter(buffer, &end);
      gchar *new_synonyms_list = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
      // refresh iter
      gtk_tree_selection_get_selected(selection, &model, &iter);
      GtkTreeIter store_iter;
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model),
                                                       &store_iter, &iter);
      if (new_flags != (flags & (DT_TF_CATEGORY | DT_TF_PRIVATE)))
      {
        new_flags = (flags & ~(DT_TF_CATEGORY | DT_TF_PRIVATE)) | new_flags;
        dt_tag_set_flags(tagid, new_flags);
        if (!d->tree_flag)
          gtk_list_store_set(GTK_LIST_STORE(store), &store_iter, DT_LIB_TAGGING_COL_FLAGS, new_flags, -1);
        else
          gtk_tree_store_set(GTK_TREE_STORE(store), &store_iter, DT_LIB_TAGGING_COL_FLAGS, new_flags, -1);
      }
      if (new_synonyms_list && g_strcmp0(synonyms_list, new_synonyms_list) != 0)
      {
        dt_tag_set_synonyms(tagid, new_synonyms_list);
        if (!d->tree_flag)
          gtk_list_store_set(GTK_LIST_STORE(store), &store_iter, DT_LIB_TAGGING_COL_SYNONYM, new_synonyms_list, -1);
        else
          gtk_tree_store_set(GTK_TREE_STORE(store), &store_iter, DT_LIB_TAGGING_COL_SYNONYM, new_synonyms_list, -1);
      }
      g_free(new_synonyms_list);
    }
  }
  _init_treeview(self, 0);
  gtk_widget_destroy(dialog);
  g_free(tagname);
}

// rename path allows the user to redefine a hierarchy
static void _pop_menu_dictionary_rename_path(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  char *tagname;
  gint tagid;
  gchar *text;
  GtkWidget *label;
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeView *view = d->dictionary_view;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tagname,
          DT_LIB_TAGGING_COL_ID, &tagid, -1);

  gint tag_count;
  gint img_count;
  dt_tag_count_tags_images(tagname, &tag_count, &img_count);
  if (tag_count == 0) return;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("rename path?"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("rename"), GTK_RESPONSE_YES, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 300, -1);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(area), vbox);
  text = g_strdup_printf(_("selected path: %s "), tagname);
  label = gtk_label_new(text);
  gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_box_pack_start(GTK_BOX(vbox), box, FALSE, TRUE, 0);
  text = g_strdup_printf(ngettext("<u>%d</u> tag will be updated.", "<u>%d</u> tags will be updated.", tag_count), tag_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);
  text = g_strdup_printf(ngettext("<u>%d</u> image will be updated", "<u>%d</u> images will be updated ", img_count), img_count);
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, TRUE, 0);
  g_free(text);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), tagname);
  gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, TRUE, 0);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    const char *newtag = gtk_entry_get_text(GTK_ENTRY(entry));
    if (g_strcmp0(newtag, tagname) == 0)
      return;  // no change
    char *message = NULL;
    if (!newtag[0])
      message = _("empty tag is not allowed, aborting");
    if (strchr(newtag, '|') == &newtag[0] || strchr(newtag, '|') == &newtag[strlen(newtag)-1] || strstr(newtag, "||"))
      message = _("'|' misplaced, empty tag is not allowed, aborting");
    if (message)
    {
      GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                      GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE, "%s", message);
      gtk_dialog_run(GTK_DIALOG(warning_dialog));
      gtk_widget_destroy(warning_dialog);
      gtk_widget_destroy(dialog);
      g_free(tagname);
      return;
    }
    GList *tag_family = NULL;
    GList *tagged_images = NULL;
    dt_tag_get_tags_images(tagname, &tag_family, &tagged_images);

    // check if one of the new tagnames already exists.
    const int tagname_len = strlen(tagname);
    gboolean tagname_exists = FALSE;
    for (GList *taglist = tag_family; taglist && !tagname_exists; taglist = g_list_next(taglist))
    {
      char *new_tagname = g_strconcat(newtag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
      tagname_exists = dt_tag_exists(new_tagname, NULL);
      if (tagname_exists)
      {
        GtkWidget *warning_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL,
                        GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                        _("at least one new tagname (%s) already exists, aborting."), new_tagname);
        gtk_dialog_run(GTK_DIALOG(warning_dialog));
        gtk_widget_destroy(warning_dialog);
      };
      g_free(new_tagname);
    }

    if (!tagname_exists)
    {
      for (GList *taglist = tag_family; taglist; taglist = g_list_next(taglist))
      {
        char *new_tagname = g_strconcat(newtag, &((dt_tag_t *)taglist->data)->tag[tagname_len], NULL);
        dt_tag_rename(((dt_tag_t *)taglist->data)->id, new_tagname);
        g_free(new_tagname);
      }
      _init_treeview(self, 0);
      _init_treeview(self, 1);
      dt_image_synch_xmps(tagged_images);
      _raise_signal_tag_changed(self);
      _show_tag_on_view(view, newtag);
    }
    dt_tag_free_result(&tag_family);
    g_list_free(tagged_images);
  }
  gtk_widget_destroy(dialog);
  g_free(tagname);
}

static void _pop_menu_dictionary_goto_tag_collection(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->dictionary_view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *path;
    guint count;
    gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &path, DT_LIB_TAGGING_COL_COUNT, &count, -1);
    if (count)
    {
      if (!d->collection[0]) dt_collection_serialize(d->collection, 4096);
      char *tag_collection = NULL;
      tag_collection = dt_util_dstrcat(tag_collection, "1:0:%d:%s$", DT_COLLECTION_PROP_TAG, path);
      dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
      dt_collection_deserialize(tag_collection);
      dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
      g_free(tag_collection);
    }
    g_free(path);
  }
}

static void _pop_menu_dictionary_goto_collection_back(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (d->collection[0])
  {
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
    dt_collection_deserialize(d->collection);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
    d->collection[0] = '\0';
  }
}

static void _pop_menu_dictionary_copy_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->dictionary_view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    char *tag;
    gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_PATH, &tag, -1);
    gtk_entry_set_text(d->entry, tag);
    g_free(tag);
    gtk_entry_grab_focus_without_selecting(d->entry);
  }
}

static void _pop_menu_dictionary_attach_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _attach_selected_tag(self, d);
}

static void _pop_menu_dictionary_detach_tag(GtkWidget *menuitem, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  _detach_selected_tag(d->dictionary_view, self, d);
}

static void _pop_menu_dictionary(GtkWidget *treeview, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeIter iter;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->dictionary_view));
  if(gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    guint count;
    guint tagid;
    gtk_tree_model_get(model, &iter,
                       DT_LIB_TAGGING_COL_ID, &tagid,
                       DT_LIB_TAGGING_COL_COUNT, &count, -1);

    GtkWidget *menu, *menuitem;
    menu = gtk_menu_new();

    if (tagid)
    {
      menuitem = gtk_menu_item_new_with_label(_("attach tag"));
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_attach_tag, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

      menuitem = gtk_menu_item_new_with_label(_("detach tag"));
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_detach_tag, self);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    }
    if (d->tree_flag || !d->suggestion_flag)
    {
      menuitem = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

      if (tagid)
      {
        menuitem = gtk_menu_item_new_with_label(_("delete tag"));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
        g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_delete_tag, self);
      }

      menuitem = gtk_menu_item_new_with_label(_("delete path"));
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_delete_path, self);

      menuitem = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      menuitem = gtk_menu_item_new_with_label(_("create tag..."));
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_create_tag, self);

      menuitem = gtk_menu_item_new_with_label(_("edit tag..."));
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_edit_tag, self);
    }

    if (d->tree_flag)
    {
      menuitem = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      menuitem = gtk_menu_item_new_with_label(_("rename path..."));
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_rename_path, self);
    }

    menuitem = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
    menuitem = gtk_menu_item_new_with_label(_("copy to entry"));
    g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_copy_tag, self);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

    if (d->collection[0])
    {
      char *collection = g_malloc(4096);
      dt_collection_serialize(collection, 4096);
      if (g_strcmp0(d->collection, collection) == 0) d->collection[0] = '\0';
      g_free(collection);
    }
    if (count || d->collection[0])
    {
      menuitem = gtk_separator_menu_item_new();
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      if (count)
      {
        menuitem = gtk_menu_item_new_with_label(_("go to tag collection"));
        g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_goto_tag_collection, self);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      }
      if (d->collection[0])
      {
        menuitem = gtk_menu_item_new_with_label(_("go back to work"));
        g_signal_connect(menuitem, "activate", (GCallback)_pop_menu_dictionary_goto_collection_back, self);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
      }
    }
    gtk_widget_show_all(GTK_WIDGET(menu));

#if GTK_CHECK_VERSION(3, 22, 0)
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
#else
    /* Note: event can be NULL here when called from view_onPopupMenu;
     *  gdk_event_get_time() accepts a NULL argument */
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, (event != NULL) ? event->button : 0,
                   gdk_event_get_time((GdkEvent *)event));
#endif
  }
}

static gboolean _click_on_view_dictionary(GtkWidget *view, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  if((event->type == GDK_BUTTON_PRESS && event->button == 3)
    || (d->tree_flag && event->type == GDK_BUTTON_PRESS && event->button == 1 && event->state & GDK_SHIFT_MASK)
    || (event->type == GDK_2BUTTON_PRESS && event->button == 1))
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    GtkTreePath *path = NULL;
    // Get tree path for row that was clicked
    if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL))
    {
      gtk_tree_selection_select_path(selection, path);
      _update_atdetach_buttons(self);
      if(event->type == GDK_BUTTON_PRESS && event->button == 3)
      {
        _pop_menu_dictionary(view, event, self);
        gtk_tree_path_free(path);
        return TRUE;
      }
      else if(d->tree_flag && event->type == GDK_BUTTON_PRESS && event->button == 1 && event->state & GDK_SHIFT_MASK)
      {
        gtk_tree_view_expand_row(GTK_TREE_VIEW(view), path, TRUE);
        return TRUE;
      }
      else if(event->type == GDK_2BUTTON_PRESS && event->button == 1)
      {
        _attach_selected_tag(self, d);
        gtk_tree_path_free(path);
        return TRUE;
      }
    }
    gtk_tree_path_free(path);
  }
  return FALSE;
}

static gboolean _mouse_scroll_attached(GtkWidget *treeview, GdkEventScroll *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (event->state & GDK_CONTROL_MASK)
  {
    const gint increment = DT_PIXEL_APPLY_DPI(10.0);
    const gint min_height = DT_PIXEL_APPLY_DPI(100.0);
    const gint max_height = DT_PIXEL_APPLY_DPI(500.0);
    gint width, height;
    gtk_widget_get_size_request (GTK_WIDGET(d->attached_window), &width, &height);
    height = height + increment * event->delta_y;
    height = (height < min_height) ? min_height : (height > max_height) ? max_height : height;
    gtk_widget_set_size_request(GTK_WIDGET(d->attached_window), -1, (gint)height);
    dt_conf_set_int("plugins/lighttable/tagging/heightattachedwindow", (gint)height);
    return TRUE;
  }
  return FALSE;
}

static gboolean _mouse_scroll_dictionary(GtkWidget *treeview, GdkEventScroll *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (event->state & GDK_CONTROL_MASK)
  {
    const gint increment = DT_PIXEL_APPLY_DPI(10.0);
    const gint min_height = DT_PIXEL_APPLY_DPI(100.0);
    const gint max_height = DT_PIXEL_APPLY_DPI(1000.0);
    gint width, height;
    gtk_widget_get_size_request (GTK_WIDGET(d->dictionary_window), &width, &height);
    height = height + increment * event->delta_y;
    height = (height < min_height) ? min_height : (height > max_height) ? max_height : height;
    gtk_widget_set_size_request(GTK_WIDGET(d->dictionary_window), -1, (gint)height);
    dt_conf_set_int("plugins/lighttable/tagging/heightdictionarywindow", (gint)height);
    return TRUE;
  }
  return FALSE;
}

static gboolean _row_tooltip_setup(GtkWidget *treeview, gint x, gint y, gboolean kb_mode,
      GtkTooltip* tooltip, dt_lib_module_t *self)
{
  gboolean res = FALSE;
  GtkTreePath *path = NULL;
  // Get tree path mouse position
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), x, y, &path, NULL, NULL, NULL))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(treeview));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, path))
    {
      char *tagname;
      guint tagid;
      guint flags;
      char *synonyms;
      gtk_tree_model_get(model, &iter, DT_LIB_TAGGING_COL_ID, &tagid, DT_LIB_TAGGING_COL_TAG, &tagname,
              DT_LIB_TAGGING_COL_FLAGS, &flags, DT_LIB_TAGGING_COL_SYNONYM, &synonyms, -1);
      if (tagid)
      {
        if ((flags & DT_TF_PRIVATE) || (synonyms && synonyms[0]))
        {
          char *text = dt_util_dstrcat(NULL, _("%s"), tagname);
          text = dt_util_dstrcat(text, " %s\n", (flags & DT_TF_PRIVATE) ? _("(private)") : "");
          text = dt_util_dstrcat(text, "synonyms: %s", (synonyms && synonyms[0]) ? synonyms : " - ");
          gtk_tooltip_set_text(tooltip, text);
          g_free(text);
          res = TRUE;
        }
        g_free(synonyms);
      }
      g_free(tagname);
    }
  }
  gtk_tree_path_free(path);

  return res;
}

static void _import_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select a keyword file"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_import"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    ssize_t count = dt_tag_import(filename);
    if(count < 0)
      dt_control_log(_("error importing tags"));
    else
      dt_control_log(_("%zd tags imported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_free(last_dirname);
  gtk_widget_destroy(filechooser);
  _init_treeview(self, 1);
}

static void _export_button_clicked(GtkButton *button, dt_lib_module_t *self)
{
  GDateTime *now = g_date_time_new_now_local();
  char *export_filename = g_date_time_format(now, "darktable_tags_%F_%R.txt");
  char *last_dirname = dt_conf_get_string("plugins/lighttable/tagging/last_import_export_location");
  if(!last_dirname || !*last_dirname)
  {
    g_free(last_dirname);
    last_dirname = g_strdup(g_get_home_dir());
  }

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("Select file to export to"), GTK_WINDOW(win),
                                                       GTK_FILE_CHOOSER_ACTION_SAVE,
                                                       _("_cancel"), GTK_RESPONSE_CANCEL,
                                                       _("_export"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(filechooser), TRUE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_dirname);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(filechooser), export_filename);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *dirname = g_path_get_dirname(filename);
    dt_conf_set_string("plugins/lighttable/tagging/last_import_export_location", dirname);
    const ssize_t count = dt_tag_export(filename);
    if(count < 0)
      dt_control_log(_("error exporting tags"));
    else
      dt_control_log(_("%zd tags exported"), count);
    g_free(filename);
    g_free(dirname);
  }

  g_date_time_unref(now);
  g_free(last_dirname);
  g_free(export_filename);
  gtk_widget_destroy(filechooser);
}

static void _update_layout(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view));

  const gboolean active_s = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_suggestion_button));
  d->suggestion_flag = (dt_conf_key_exists("plugins/lighttable/tagging/nosuggestion")
                        && !dt_conf_get_bool("plugins/lighttable/tagging/nosuggestion"));
  if (active_s != d->suggestion_flag)
  {
    g_signal_handler_block (d->toggle_suggestion_button, d->suggestion_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_suggestion_button), d->suggestion_flag);
    g_signal_handler_unblock (d->toggle_suggestion_button, d->suggestion_button_handler);
  }

  const gboolean active_t = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_tree_button));
  d->tree_flag = dt_conf_get_bool("plugins/lighttable/tagging/treeview");
  if (active_t != d->tree_flag)
  {
    g_signal_handler_block (d->toggle_tree_button, d->tree_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_tree_button), d->tree_flag);
    g_signal_handler_unblock (d->toggle_tree_button, d->tree_button_handler);
  }

  if (d->tree_flag)
  {
    if (model == GTK_TREE_MODEL(d->dictionary_listfilter))
    {
      g_object_ref(model);
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->dictionary_view), NULL);
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      gtk_list_store_clear(GTK_LIST_STORE(store));
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->dictionary_view), GTK_TREE_MODEL(d->dictionary_treefilter));
      g_object_unref(d->dictionary_treefilter);
      if (d->completion) gtk_entry_set_completion(d->entry, NULL);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(d->toggle_suggestion_button), FALSE);
  }
  else
  {
    if (model == GTK_TREE_MODEL(d->dictionary_treefilter))
    {
      g_object_ref(model);
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->dictionary_view), NULL);
      GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model));
      gtk_tree_store_clear(GTK_TREE_STORE(store));
      gtk_tree_view_set_model(GTK_TREE_VIEW(d->dictionary_view), GTK_TREE_MODEL(d->dictionary_listfilter));
      g_object_unref(d->dictionary_listfilter);
      if (d->completion) gtk_entry_set_completion(d->entry, d->completion);
    }
    gtk_widget_set_sensitive(GTK_WIDGET(d->toggle_suggestion_button), TRUE);
  }

  const gboolean active_c = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_sort_button));
  d->sort_count_flag = dt_conf_get_bool("plugins/lighttable/tagging/listsortedbycount");
  if (active_c != d->sort_count_flag)
  {
    g_signal_handler_block (d->toggle_sort_button, d->sort_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_sort_button), d->sort_count_flag);
    g_signal_handler_unblock (d->toggle_sort_button, d->sort_button_handler);
  }

  const gboolean active_h = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_hide_button));
  d->hide_path_flag = dt_conf_get_bool("plugins/lighttable/tagging/hidehierarchy");
  if (active_h != d->hide_path_flag)
  {
    g_signal_handler_block (d->toggle_hide_button, d->hide_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_hide_button), d->hide_path_flag);
    g_signal_handler_unblock (d->toggle_hide_button, d->hide_button_handler);
  }

  const gboolean active_d = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_dttags_button));
  d->dttags_flag = dt_conf_get_bool("plugins/lighttable/tagging/dttags");
  if (active_d != d->dttags_flag)
  {
    g_signal_handler_block (d->toggle_dttags_button, d->dttags_button_handler);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_dttags_button), d->dttags_flag);
    g_signal_handler_unblock (d->toggle_dttags_button, d->dttags_button_handler);
  }
}

static void _toggle_suggestion_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  const gboolean new_state = !dt_conf_get_bool("plugins/lighttable/tagging/nosuggestion");
  dt_conf_set_bool("plugins/lighttable/tagging/nosuggestion", new_state);
  _update_layout(self);
  _init_treeview(self, 1);
}

static void _toggle_tree_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  const gboolean new_state = !dt_conf_get_bool("plugins/lighttable/tagging/treeview");
  dt_conf_set_bool("plugins/lighttable/tagging/treeview", new_state);
  _update_layout(self);
  _init_treeview(self, 1);
}

static gint _sort_tree_count_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, dt_lib_module_t *self)
{
  guint count_a = 0;
  guint count_b = 0;
  gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_COUNT, &count_a, -1);
  gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_COUNT, &count_b, -1);
  return (count_b - count_a);
}

static gint _sort_tree_tag_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, dt_lib_module_t *self)
{
  char *tag_a = NULL;
  char *tag_b = NULL;
  gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_TAG, &tag_a, -1);
  gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_TAG, &tag_b, -1);
  if(tag_a == NULL) tag_a = g_strdup("");
  if(tag_b == NULL) tag_b = g_strdup("");
  const gboolean sort = g_ascii_strcasecmp(tag_a, tag_b);
  g_free(tag_a);
  g_free(tag_b);
  return sort;
}

static gint _sort_tree_path_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, dt_lib_module_t *self)
{
  char *tag_a = NULL;
  char *tag_b = NULL;
  gtk_tree_model_get(model, a, DT_LIB_TAGGING_COL_PATH, &tag_a, -1);
  gtk_tree_model_get(model, b, DT_LIB_TAGGING_COL_PATH, &tag_b, -1);
  if(tag_a)
  {
    for(char *letter = tag_a; *letter; letter++)
      if(*letter == '|') *letter = '\1';
  }
  else
    tag_a = g_strdup("");

  if(tag_b)
  {
    for(char *letter = tag_b; *letter; letter++)
      if(*letter == '|') *letter = '\1';
  }
  else
    tag_b = g_strdup("");

  const gboolean sort = g_ascii_strcasecmp(tag_a, tag_b);
  g_free(tag_a);
  g_free(tag_b);
  return sort;
}

static void _toggle_sort_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  const gboolean new_state = !dt_conf_get_bool("plugins/lighttable/tagging/listsortedbycount");
  dt_conf_set_bool("plugins/lighttable/tagging/listsortedbycount", new_state);
  _update_layout(self);
  _sort_attached_list(self, FALSE);
  _sort_dictionary_list(self, FALSE);
}

static void _toggle_hide_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  const gboolean new_state = !dt_conf_get_bool("plugins/lighttable/tagging/hidehierarchy");
  dt_conf_set_bool("plugins/lighttable/tagging/hidehierarchy", new_state);
  _update_layout(self);
  _sort_attached_list(self, TRUE);
  _sort_dictionary_list(self, TRUE);
}

static void _toggle_dttags_button_callback(GtkToggleButton *source, dt_lib_module_t *self)
{
  const gboolean new_state = !dt_conf_get_bool("plugins/lighttable/tagging/dttags");
  dt_conf_set_bool("plugins/lighttable/tagging/dttags", new_state);
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  d->dttags_flag = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->toggle_dttags_button));
  _init_treeview(self, 0);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  // clear entry box and query
  gtk_entry_set_text(d->entry, "");
  _set_keyword(self);
  _init_treeview(self, 1);
  _update_atdetach_buttons(self);
}

int position()
{
  return 500;
}

static gboolean _match_selected_func(GtkEntryCompletion *completion, GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  const int column = gtk_entry_completion_get_text_column(completion);
  char *tag = NULL;

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING) return TRUE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }

  gtk_tree_model_get(model, iter, column, &tag, -1);

  gint cut_off, cur_pos = gtk_editable_get_position(e);

  gchar *currentText = gtk_editable_get_chars(e, 0, -1);
  const gchar *lastTag = g_strrstr(currentText, ",");
  if(lastTag == NULL)
  {
    cut_off = 0;
  }
  else
  {
    cut_off = (int)(g_utf8_strlen(currentText, -1) - g_utf8_strlen(lastTag, -1))+1;
  }
  free(currentText);

  gtk_editable_delete_text(e, cut_off, cur_pos);
  cur_pos = cut_off;
  gtk_editable_insert_text(e, tag, -1, &cur_pos);
  gtk_editable_set_position(e, cur_pos);
  return TRUE;
}

static gboolean _completion_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gboolean res = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);

  if(!GTK_IS_EDITABLE(e))
  {
    return FALSE;
  }

  const gint cur_pos = gtk_editable_get_position(e);
  const gboolean onLastTag = (g_strstr_len(&key[cur_pos], -1, ",") == NULL);
  if(!onLastTag)
  {
    return FALSE;
  }

  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  const int column = gtk_entry_completion_get_text_column(completion);
  char *tag = NULL;

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING)
  {
    return FALSE;
  }

  gtk_tree_model_get(model, iter, column, &tag, -1);

  const gchar *lastTag = g_strrstr(key, ",");
  if(lastTag != NULL)
  {
    lastTag++;
  }
  else
  {
    lastTag = key;
  }
  if(lastTag[0] == '\0' && key[0] != '\0')
  {
    return FALSE;
  }

  if(tag)
  {
    char *normalized = g_utf8_normalize(tag, -1, G_NORMALIZE_ALL);
    if(normalized)
    {
      char *casefold = g_utf8_casefold(normalized, -1);
      if(casefold)
      {
        res = g_strstr_len(casefold, -1, lastTag) != NULL;
      }
      g_free(casefold);
    }
    g_free(normalized);
    g_free(tag);
  }

  return res;
}

static void _tree_selection_changed(GtkTreeSelection *treeselection, gpointer data)
{
  _update_atdetach_buttons((dt_lib_module_t *)data);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)malloc(sizeof(dt_lib_tagging_t));
  self->data = (void *)d;
  d->last_tag = NULL;
  self->timeout_handle = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  GtkBox *box, *hbox;
  GtkWidget *button;
  GtkWidget *w;
  GtkTreeView *view;
  GtkTreeModel *model;
  GtkListStore *liststore;
  GtkTreeStore *treestore;
  GtkTreeViewColumn *col;
  GtkCellRenderer *renderer;
  gint height;

  // attached_view
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
  w = gtk_scrolled_window_new(NULL, NULL);
  d->attached_window = w;
  height = dt_conf_get_int("plugins/lighttable/tagging/heightattachedwindow");
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(height ? height : 100));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->attached_view = view;
  gtk_tree_view_set_headers_visible(view, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_PATH_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_path_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_NAME_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_tag_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_COUNT_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_count_func, self, NULL);
  d->attached_liststore = liststore;
  g_object_set(G_OBJECT(view), "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(view), "query-tooltip", G_CALLBACK(_row_tooltip_setup), (gpointer)self);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_toggle_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _tree_select_show, NULL, NULL);
  g_object_set(renderer, "indicator-size", 8, NULL);  // too big by default

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _tree_tagname_show_attached, (gpointer)self, NULL);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(liststore));
  g_object_unref(liststore);
  gtk_widget_set_tooltip_text(GTK_WIDGET(view), _("attached tags,\ndouble-click to detach"
                                                  "\nright-click for other actions on attached tag,"
                                                  "\nctrl-wheel scroll to resize the window"));
  dt_gui_add_help_link(GTK_WIDGET(view), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_click_on_view_attached), (gpointer)self);
  g_signal_connect(G_OBJECT(view), "scroll-event", G_CALLBACK(_mouse_scroll_attached), (gpointer)self);
  g_signal_connect(gtk_tree_view_get_selection(view), "changed", G_CALLBACK(_tree_selection_changed), self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(view));

  // attach/detach buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  d->attach_button = dt_ui_button_new(_("attach"), _("attach tag to all selected images"), "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, d->attach_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->attach_button), "clicked", G_CALLBACK(_attach_button_clicked), (gpointer)self);

  d->detach_button = dt_ui_button_new(_("detach"), _("detach tag from all selected images"), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(d->detach_button), "clicked", G_CALLBACK(_detach_button_clicked), (gpointer)self);
  gtk_box_pack_start(hbox, d->detach_button, TRUE, TRUE, 0);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_minus_simple, CPF_STYLE_FLAT, NULL);
  d->toggle_hide_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list with / without hierarchy"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->hide_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(_toggle_hide_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_sorting, CPF_STYLE_FLAT, NULL);
  d->toggle_sort_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle sort by name or by count"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->sort_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(_toggle_sort_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_check_mark, CPF_STYLE_FLAT, NULL);
  d->toggle_dttags_button = button;
  d->dttags_flag = FALSE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->toggle_dttags_button), FALSE);
  gtk_widget_set_tooltip_text(button, _("toggle show or not darktable tags"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->dttags_button_handler =
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_toggle_dttags_button_callback), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // dictionary_view
  box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);

  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  // text entry
  w = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(w), "");
  gtk_entry_set_width_chars(GTK_ENTRY(w), 0);
  gtk_widget_set_tooltip_text(w, _("enter tag name"));
  dt_gui_add_help_link(w, "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, w, TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(w), GDK_KEY_RELEASE_MASK);
  g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(_tag_name_changed), (gpointer)self);
  g_signal_connect(G_OBJECT(w), "key-press-event", G_CALLBACK(_key_pressed), (gpointer)self);
  d->entry = GTK_ENTRY(w);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->entry));

  button = dtgtk_button_new(dtgtk_cairo_paint_multiply_small, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(button, _("clear entry"));
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_clear_entry_button_callback), (gpointer)self);
  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  // dictionary_view tree view
  w = gtk_scrolled_window_new(NULL, NULL);
  d->dictionary_window = w;
  height = dt_conf_get_int("plugins/lighttable/tagging/heightdictionarywindow");
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(height ? height : 300));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(box, w, TRUE, TRUE, 0);
  view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->dictionary_view = view;
  gtk_tree_view_set_headers_visible(view, FALSE);
  liststore = gtk_list_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_PATH_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_path_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_NAME_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_tag_func, self, NULL);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(liststore), DT_TAG_SORT_COUNT_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_count_func, self, NULL);
  d->dictionary_liststore = liststore;
  model = gtk_tree_model_filter_new(GTK_TREE_MODEL(liststore), NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(model), DT_LIB_TAGGING_COL_VISIBLE);
  d->dictionary_listfilter = GTK_TREE_MODEL_FILTER(model);
  treestore = gtk_tree_store_new(DT_LIB_TAGGING_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN);
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(treestore), DT_TAG_SORT_PATH_ID,
                  (GtkTreeIterCompareFunc)_sort_tree_path_func, self, NULL);
  d->dictionary_treestore = treestore;
  model = gtk_tree_model_filter_new(GTK_TREE_MODEL(treestore), NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(model), DT_LIB_TAGGING_COL_VISIBLE);
  d->dictionary_treefilter = GTK_TREE_MODEL_FILTER(model);

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_toggle_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_cell_renderer_toggle_set_activatable(GTK_CELL_RENDERER_TOGGLE(renderer), TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _tree_select_show, NULL, NULL);
  g_object_set(renderer, "indicator-size", 8, NULL);  // too big by default

  col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, _tree_tagname_show_dictionary, (gpointer)self, NULL);
  gtk_tree_view_set_expander_column(view, col);

  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(view), _("tag dictionary,\ndouble-click to attach,"
                                                      "\nright-click for other actions on selected tag,"
                                                      "\nctrl-wheel scroll to resize the window"));
  dt_gui_add_help_link(GTK_WIDGET(view), "tagging.html#tagging_usage");
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_click_on_view_dictionary), (gpointer)self);
  g_signal_connect(G_OBJECT(view), "scroll-event", G_CALLBACK(_mouse_scroll_dictionary), (gpointer)self);
  gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(view));
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(d->dictionary_listfilter));
  g_object_unref(d->dictionary_listfilter);
  g_object_set(G_OBJECT(view), "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(view), "query-tooltip", G_CALLBACK(_row_tooltip_setup), (gpointer)self);
  g_signal_connect(gtk_tree_view_get_selection(view), "changed", G_CALLBACK(_tree_selection_changed), self);

  // buttons
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

  d->new_button = dt_ui_button_new(_("new"), _("create a new tag with the\nname you entered"), "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, d->new_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->new_button), "clicked", G_CALLBACK(_new_button_clicked), (gpointer)self);

  d->import_button = dt_ui_button_new(C_("verb", "import..."), _("import tags from a Lightroom keyword file"), "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, d->import_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->import_button), "clicked", G_CALLBACK(_import_button_clicked), (gpointer)self);

  d->export_button = dt_ui_button_new(C_("verb", "export..."), _("export all tags to a Lightroom keyword file"), "tagging.html#tagging_usage");
  gtk_box_pack_start(hbox, d->export_button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->export_button), "clicked", G_CALLBACK(_export_button_clicked), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_treelist, CPF_STYLE_FLAT, NULL);
  d->toggle_tree_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list / tree view"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->tree_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(_toggle_tree_button_callback), (gpointer)self);

  button = dtgtk_togglebutton_new(dtgtk_cairo_paint_plus_simple, CPF_STYLE_FLAT, NULL);
  d->toggle_suggestion_button = button;
  gtk_widget_set_tooltip_text(button, _("toggle list with / without suggestion"));
  dt_gui_add_help_link(button, "tagging.html#tagging_usage");
  gtk_box_pack_end(hbox, button, FALSE, TRUE, 0);
  d->suggestion_button_handler = g_signal_connect(G_OBJECT(button), "clicked",
                                            G_CALLBACK(_toggle_suggestion_button_callback), (gpointer)self);

  gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, TRUE, 0);

  if (!dt_conf_get_bool("plugins/lighttable/tagging/no_entry_completion"))
  {
    // add entry completion
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view)));
    gtk_entry_completion_set_text_column(completion, DT_LIB_TAGGING_COL_PATH);
    gtk_entry_completion_set_inline_completion(completion, TRUE);
    gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
    gtk_entry_set_completion(d->entry, completion);
    d->completion = completion;
  }
  else d->completion = NULL;

  /* connect to mouse over id */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_lib_tagging_redraw_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_lib_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_collection_updated_callback), self);

  d->collection = g_malloc(4096);
  _update_layout(self);
  _init_treeview(self, 0);
  _set_keyword(self);
  _init_treeview(self, 1);
  _update_atdetach_buttons(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_cancel_postponed_update(self);
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  dt_gui_key_accel_block_on_focus_disconnect(GTK_WIDGET(d->entry));
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_tagging_redraw_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_tagging_tags_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_collection_updated_callback), self);
  g_free(d->collection);
  free(self->data);
  self->data = NULL;
}

// http://stackoverflow.com/questions/4631388/transparent-floating-gtkentry
static gboolean _lib_tagging_tag_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      g_list_free(d->floating_tag_imgs);
      gtk_widget_destroy(d->floating_tag_window);
      gtk_window_present(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
      return TRUE;
    case GDK_KEY_Tab:
      return TRUE;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const gchar *tag = gtk_entry_get_text(GTK_ENTRY(entry));
      const gboolean res = dt_tag_attach_string_list(tag, d->floating_tag_imgs, TRUE);
      if(res) dt_image_synch_xmps(d->floating_tag_imgs);
      g_list_free(d->floating_tag_imgs);

      /** record last tag used */
      g_free(d->last_tag);
      d->last_tag = g_strdup(tag);

      _init_treeview(self, 0);
      _init_treeview(self, 1);
      gtk_widget_destroy(d->floating_tag_window);
      gtk_window_present(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
      if(res) _raise_signal_tag_changed(self);

      return TRUE;
    }
  }
  return FALSE; /* event not handled */
}

static gboolean _lib_tagging_tag_destroy(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_destroy(GTK_WIDGET(user_data));
  return FALSE;
}

static gboolean _lib_tagging_tag_redo(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;

  if(d->last_tag)
  {
    const GList *imgs = dt_view_get_images_to_act_on(TRUE, TRUE);
    const gboolean res = dt_tag_attach_string_list(d->last_tag, imgs, TRUE);
    if(res) dt_image_synch_xmps(imgs);
    _init_treeview(self, 0);
    _init_treeview(self, 1);
    if(res) _raise_signal_tag_changed(self);
  }
  return TRUE;
}

static gboolean _lib_tagging_tag_show(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                      GdkModifierType modifier, dt_lib_module_t *self)
{
  dt_lib_tagging_t *d = (dt_lib_tagging_t *)self->data;
  if (d->tree_flag)
  {
    dt_control_log(_("tag shortcut is not active with tag tree view. please switch to list view"));
    return TRUE;  // doesn't work properly with tree treeview
  }

  d->floating_tag_imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(FALSE, TRUE));
  gint x, y;
  gint px, py, w, h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  gdk_window_get_origin(gtk_widget_get_window(center), &px, &py);

  w = gdk_window_get_width(gtk_widget_get_window(center));
  h = gdk_window_get_height(gtk_widget_get_window(center));

  x = px + 0.5 * (w - FLOATING_ENTRY_WIDTH);
  y = py + h - 50;

  /* put the floating box at the mouse pointer */
  //   gint pointerx, pointery;
  //   GdkDevice *device =
  //   gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gtk_widget_get_display(widget)));
  //   gdk_window_get_device_position (gtk_widget_get_window (widget), device, &pointerx, &pointery, NULL);
  //   x = px + pointerx + 1;
  //   y = py + pointery + 1;

  d->floating_tag_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_tag_window);
#endif
  /* stackoverflow.com/questions/1925568/how-to-give-keyboard-focus-to-a-pop-up-gtk-window */
  gtk_widget_set_can_focus(d->floating_tag_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_tag_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_tag_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_tag_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_tag_window, 0.8);
  gtk_window_move(GTK_WINDOW(d->floating_tag_window), x, y);

  GtkWidget *entry = gtk_entry_new();
  gtk_widget_set_size_request(entry, FLOATING_ENTRY_WIDTH, -1);
  gtk_widget_add_events(entry, GDK_FOCUS_CHANGE_MASK);

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, gtk_tree_view_get_model(GTK_TREE_VIEW(d->dictionary_view)));
  gtk_entry_completion_set_text_column(completion, DT_LIB_TAGGING_COL_PATH);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_set_width(completion, FALSE);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(_match_selected_func), self);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_container_add(GTK_CONTAINER(d->floating_tag_window), entry);
  g_signal_connect(entry, "focus-out-event", G_CALLBACK(_lib_tagging_tag_destroy), d->floating_tag_window);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(_lib_tagging_tag_key_press), self);

  gtk_widget_show_all(d->floating_tag_window);
  gtk_widget_grab_focus(entry);
  gtk_window_present(GTK_WINDOW(d->floating_tag_window));

  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
