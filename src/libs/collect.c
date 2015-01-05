/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika, henrik andersson.
    copyright (c) 2012 Jose Carlos Garcia Sogo

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
#include "common/darktable.h"
#include "common/film.h"
#include "common/collection.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "libs/collect.h"
#include "views/view.h"

DT_MODULE(1)

#define MAX_RULES 10

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct dt_lib_collect_rule_t
{
  int num;
  GtkWidget *hbox;
  GtkComboBox *combo;
  GtkWidget *text;
  GtkWidget *button;
} dt_lib_collect_rule_t;

typedef struct dt_lib_collect_t
{
  dt_lib_collect_rule_t rule[MAX_RULES];
  int active_rule;

  GtkTreeView *view;
  GtkTreeModel *treemodel;
  GtkTreeModel *listmodel;
  GtkEntryCompletion *autocompletion;
  GtkCellRenderer *auto_renderer;
  GtkScrolledWindow *scrolledwindow;
  gboolean update_query_on_sel_change;

  struct dt_lib_collect_params_t *params;
} dt_lib_collect_t;

typedef struct dt_lib_collect_params_rule_t 
{
    uint32_t item : 16;
    uint32_t mode : 16;
    char string[PARAM_STRING_SIZE];
} dt_lib_collect_params_rule_t;

typedef struct dt_lib_collect_params_t
{
  uint32_t rules;
  dt_lib_collect_params_rule_t rule[MAX_RULES];
} dt_lib_collect_params_t;

typedef enum dt_lib_collect_cols_t
{
  DT_LIB_COLLECT_COL_TEXT = 0,
  DT_LIB_COLLECT_COL_ID,
  DT_LIB_COLLECT_COL_TOOLTIP,
  DT_LIB_COLLECT_COL_PATH,
  DT_LIB_COLLECT_COL_STRIKETROUGTH,
  DT_LIB_COLLECT_NUM_COLS
} dt_lib_collect_cols_t;

const char *name()
{
  return _("collect images");
}

void init_presets(dt_lib_module_t *self)
{
}

static void _lib_collect_gui_update(dt_lib_module_t *self);
static void selection_change (GtkTreeSelection *selection, dt_lib_collect_t *d);
static void update_selection (dt_lib_collect_rule_t *dr, gboolean exact);
static void entry_changed (GtkEditable *editable, dt_lib_collect_rule_t *d);

/* Update the params struct with active ruleset */
static void _lib_collect_update_params(dt_lib_collect_t *d)
{
  /* reset params */
  dt_lib_collect_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_collect_params_t));

  /* for each active rule set update params */
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200];
  for(int i = 0; i <= active; i++)
  {
    /* get item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);

    /* get mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *string = dt_conf_get_string(confname);
    if(string != NULL)
    {
      snprintf(p->rule[i].string, PARAM_STRING_SIZE, "%s", string);
      g_free(string);
    }

    // fprintf(stderr,"[%i] %d,%d,%s\n",i, p->rule[i].item, p->rule[i].mode,  p->rule[i].string);
  }

  p->rules = active + 1;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _lib_collect_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_collect_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_collect_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_collect_params_t *p = (dt_lib_collect_params_t *)params;
  char confname[200];

  for(uint32_t i = 0; i < p->rules; i++)
  {
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    dt_conf_set_string(confname, p->rule[i].string);
  }

  /* set number of rules */
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/num_rules");
  dt_conf_set_int(confname, p->rules);

  /* update internal params */
  _lib_collect_update_params(self->data);

  /* update ui */
  _lib_collect_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection);

  return 0;
}


uint32_t views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static dt_lib_collect_t* get_collect(dt_lib_collect_rule_t *r)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)(((char *)r) - r->num*sizeof(dt_lib_collect_rule_t));
  return d;
}

static gboolean match_string (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  // we just search for an entry which begin like the text in the entry
  
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *) data;
  gchar *val = NULL;

  const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(dr->combo));
  if(item == DT_COLLECTION_PROP_FILMROLL || item == DT_COLLECTION_PROP_TAG || item == DT_COLLECTION_PROP_FOLDERS)
    gtk_tree_model_get (model, iter, DT_LIB_COLLECT_COL_PATH, &val, -1);
  else
    gtk_tree_model_get (model, iter, DT_LIB_COLLECT_COL_TEXT, &val, -1);

  // if the path start with the entry text, then we expand
  if (g_str_has_prefix(val,gtk_entry_get_text(GTK_ENTRY(dr->text))))
  {
    dt_lib_collect_t *d = get_collect(dr);
    gtk_tree_view_expand_to_path(d->view, path);
  }

  return FALSE;
}

static gboolean match_string_exact(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  // TODO handle wildcards at the beginning of the entry text
  
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *) data;
  gchar *val = NULL;

  const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(dr->combo));
  if(item == DT_COLLECTION_PROP_FILMROLL || item == DT_COLLECTION_PROP_TAG || item == DT_COLLECTION_PROP_FOLDERS)
    gtk_tree_model_get (model, iter, DT_LIB_COLLECT_COL_PATH, &val, -1);
  else
    gtk_tree_model_get (model, iter, DT_LIB_COLLECT_COL_TEXT, &val, -1);


  gchar *txt = g_strdup(gtk_entry_get_text(GTK_ENTRY(dr->text)));
  
  // if the entry and the path are exactly equals, then expand,select and stop the foreach
  if (strcmp(val,txt) == 0)
  {
    dt_lib_collect_t *d = get_collect(dr);
    gtk_tree_view_expand_to_path(d->view, path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(d->view), path);
    g_free(txt);
    return TRUE;
  }
  
  //we get the basepath to expand row if needed
  gchar *f = NULL;
  if (item == DT_COLLECTION_PROP_TAG) f = g_strrstr(txt,"|");
  else if (item == DT_COLLECTION_PROP_TAG) f = g_strrstr(txt,"/");
  
  if (!f)
  {
    g_free(txt);
    return FALSE;
  }
  gchar *end = txt + strlen(txt) - strlen(f) + 1;
  *(end) = 0;
  gchar *txt2 = g_strconcat(txt,"%",NULL);
  
  // and we compare that to the path, to see if we have to expand it
  if (strcmp(val,txt2) == 0)
  {
    dt_lib_collect_t *d = get_collect(dr);
    gtk_tree_view_expand_to_path(d->view, path);
  }
  g_free(txt);
  g_free(txt2);

  return FALSE;
}

void destroy_widget(gpointer data)
{
  GtkWidget *widget = (GtkWidget *)data;

  gtk_widget_destroy(widget);
}

static void set_properties(dt_lib_collect_rule_t *dr)
{
  int property = gtk_combo_box_get_active(dr->combo);
  const gchar *text = NULL;
  text = gtk_entry_get_text(GTK_ENTRY(dr->text));

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", dr->num);
  dt_conf_set_string(confname, text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", dr->num);
  dt_conf_set_int(confname, property);
}

static gboolean entry_autocompl_match_selected(GtkEntryCompletion *widget, GtkTreeModel *model, GtkTreeIter *iter, dt_lib_collect_t *d)
{
  gchar *text;
  gtk_tree_model_get (model, iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  gtk_entry_set_text(GTK_ENTRY(d->rule[d->active_rule].text), text);
  gtk_editable_set_position(GTK_EDITABLE(d->rule[d->active_rule].text), -1);
  
  // we update the selection
  update_selection(&d->rule[d->active_rule], TRUE);
  // we save the params
  set_properties(&d->rule[d->active_rule]);
  // and we update the query
  dt_collection_update_query(darktable.collection);
  
  return TRUE;
}

static gboolean entry_autocompl_match(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter, gpointer data)
{
  gchar *text;
  gtk_tree_model_get (gtk_entry_completion_get_model(completion), iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  if (!key || !text) return FALSE;
  return (g_str_has_prefix(text,key));
}

static void folders_view(dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;

  g_object_ref(d->treemodel);
  g_object_ref(d->listmodel);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(d->treemodel));
  gtk_list_store_clear(GTK_LIST_STORE(d->listmodel));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));

  set_properties (dr);

  /* query construction */
  char query[1024];
  snprintf(query, sizeof(query), "SELECT distinct folder, id FROM film_rolls ORDER BY UPPER(folder) DESC");

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    int level = 0;
    char *value;
    GtkTreeIter current, iter, iter2;

    char *folder = (char*)sqlite3_column_text(stmt, 0);
    if(folder == NULL) continue; // safeguard against degenerated db entries
    char **pch = g_strsplit(folder, "/", -1);

    if (pch != NULL)
    {
      int max_level = 0;
      int j = 1;
      while(pch[j] != NULL)
      {
        max_level++;
        j++;
      }
      max_level--;
      j=1;
      while (pch[j] != NULL)
      {
        gboolean found=FALSE;
        int children = gtk_tree_model_iter_n_children(d->treemodel,level>0?&current:NULL);
        /* find child with name, if not found create and continue */
        for (int k=0; k<children; k++)
        {
          if (gtk_tree_model_iter_nth_child(d->treemodel, &iter, level>0?&current:NULL, k))
          {
            gtk_tree_model_get (d->treemodel, &iter, 0, &value, -1);

            if (strcmp(value, pch[j])==0)
            {
              current = iter;
              found = TRUE;
              break;
            }
          }
        }

        /* lets add new dir and assign current */
        if (!found && pch[j] && *pch[j])
        {
          gchar *pth2 = NULL;
          gchar *pth3 = NULL;
          pth2 = dt_util_dstrcat(pth2, "/");

          for (int i = 0; i <= level; i++)
          {
            pth2 = dt_util_dstrcat(pth2, "%s/", pch[i + 1]);
          }
          pth3 = dt_util_dstrcat(pth3, pth2);
          if(level == max_level) pth2[strlen(pth2)-1] = '\0';
          else pth2 = dt_util_dstrcat(pth2, "%%");

          gtk_tree_store_insert(GTK_TREE_STORE(d->treemodel), &iter, level>0?&current:NULL,0);
          gtk_tree_store_set(GTK_TREE_STORE(d->treemodel), &iter, DT_LIB_COLLECT_COL_TEXT, pch[j],
                            DT_LIB_COLLECT_COL_PATH, pth2,
                            DT_LIB_COLLECT_COL_STRIKETROUGTH, !(g_file_test(pth3, G_FILE_TEST_IS_DIR)), -1);
          gtk_list_store_append(GTK_LIST_STORE(d->listmodel), &iter2);
          gtk_list_store_set(GTK_LIST_STORE(d->listmodel), &iter2, DT_LIB_COLLECT_COL_TEXT, pch[j],
                            DT_LIB_COLLECT_COL_PATH, pth2,
                            DT_LIB_COLLECT_COL_STRIKETROUGTH, FALSE, -1);
          current = iter;
        }

        level++;
        j++;
      }

      g_strfreev(pch);

    }
  }
  sqlite3_finalize(stmt);

  gtk_entry_completion_set_model(d->autocompletion,d->listmodel);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(d->autocompletion),d->auto_renderer, "text", DT_LIB_COLLECT_COL_PATH, NULL);
  gtk_entry_completion_set_popup_set_width(d->autocompletion,FALSE);
  gtk_entry_set_completion(GTK_ENTRY(dr->text),d->autocompletion);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), d->treemodel);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), FALSE);
  gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));

  update_selection(dr, TRUE);
  
  g_object_unref(d->treemodel);
  g_object_unref(d->listmodel);
}

static const char *UNCATEGORIZED_TAG = N_("uncategorized");
static void tags_view(dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;
  GtkTreeIter uncategorized, temp;
  memset(&uncategorized, 0, sizeof(GtkTreeIter));

  GtkTreeView *view;
  GtkTreeModel *tagsmodel;

  view = d->view;
  tagsmodel = d->treemodel;
  g_object_ref(tagsmodel);
  g_object_ref(d->listmodel);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_tree_store_clear(GTK_TREE_STORE(tagsmodel));
  gtk_list_store_clear(GTK_LIST_STORE(d->listmodel));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));

  set_properties(dr);

  /* query construction */
  char query[1024];
  snprintf(query, sizeof(query), "SELECT distinct name, id FROM tags ORDER BY UPPER(name) DESC");

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(strchr((const char *)sqlite3_column_text(stmt, 0), '|') == 0)
    {
      /* add uncategorized root iter if not exists */
      if(!uncategorized.stamp)
      {
        gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &uncategorized, NULL, 0);
        gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &uncategorized,
                            DT_LIB_COLLECT_COL_TEXT, _(UNCATEGORIZED_TAG),
                            DT_LIB_COLLECT_COL_PATH, "", -1);
      }

      /* adding an uncategorized tag */
      gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &temp, &uncategorized, 0);
      gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &temp,
                          DT_LIB_COLLECT_COL_TEXT, sqlite3_column_text(stmt, 0),
                          DT_LIB_COLLECT_COL_PATH, sqlite3_column_text(stmt, 0), -1);
      GtkTreeIter iter2;
      gtk_list_store_append(GTK_LIST_STORE(d->listmodel), &iter2);
      gtk_list_store_set(GTK_LIST_STORE(d->listmodel), &iter2,
                          DT_LIB_COLLECT_COL_TEXT, sqlite3_column_text(stmt, 0),
                          DT_LIB_COLLECT_COL_PATH, sqlite3_column_text(stmt, 0),
                          DT_LIB_COLLECT_COL_STRIKETROUGTH, FALSE, -1);
    }
    else
    {
      int level = 0;
      char *value;
      GtkTreeIter current, iter, iter2;
      char **pch = g_strsplit((char *)sqlite3_column_text(stmt, 0), "|", -1);

      if(pch != NULL)
      {
        int max_level = 0;
        int j = 0;
        while(pch[j] != NULL)
        {
          max_level++;
          j++;
        }
        max_level--;
        j = 0;
        while(pch[j] != NULL)
        {
          gboolean found = FALSE;
          int children = gtk_tree_model_iter_n_children(tagsmodel, level > 0 ? &current : NULL);
          /* find child with name, if not found create and continue */
          for(int k = 0; k < children; k++)
          {
            if(gtk_tree_model_iter_nth_child(tagsmodel, &iter, level > 0 ? &current : NULL, k))
            {
              gtk_tree_model_get(tagsmodel, &iter, 0, &value, -1);

              if(strcmp(value, pch[j]) == 0)
              {
                current = iter;
                found = TRUE;
                break;
              }
            }
          }

          /* lets add new keyword and assign current */
          if(!found && pch[j] && *pch[j])
          {
            gchar *pth2 = NULL;
            pth2 = dt_util_dstrcat(pth2, "");

            for(int i = 0; i <= level; i++)
            {
              pth2 = dt_util_dstrcat(pth2, "%s|", pch[i]);
            }
            if(level == max_level)
              pth2[strlen(pth2) - 1] = '\0';
            else
              pth2 = dt_util_dstrcat(pth2, "%%");

            gtk_tree_store_insert(GTK_TREE_STORE(tagsmodel), &iter, level > 0 ? &current : NULL, 0);
            gtk_tree_store_set(GTK_TREE_STORE(tagsmodel), &iter, DT_LIB_COLLECT_COL_TEXT, pch[j],
                               DT_LIB_COLLECT_COL_PATH, pth2, -1);
            gtk_list_store_append(GTK_LIST_STORE(d->listmodel), &iter2);
            gtk_list_store_set(GTK_LIST_STORE(d->listmodel), &iter2, DT_LIB_COLLECT_COL_TEXT, pch[j],
                                DT_LIB_COLLECT_COL_PATH, pth2,
                                DT_LIB_COLLECT_COL_STRIKETROUGTH, FALSE, -1);
            current = iter;
          }

          level++;
          j++;
        }

        g_strfreev(pch);
      }
    }
  }
  sqlite3_finalize(stmt);

  gtk_entry_completion_set_model(d->autocompletion,d->listmodel);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(d->autocompletion),d->auto_renderer, "text", DT_LIB_COLLECT_COL_PATH, NULL);
  gtk_entry_completion_set_popup_set_width(d->autocompletion,FALSE);
  gtk_entry_set_completion(GTK_ENTRY(dr->text),d->autocompletion);

  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), tagsmodel);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), FALSE);
  gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));

  g_object_unref(tagsmodel);
  g_object_unref(d->listmodel);

  update_selection(dr, TRUE);
}

static void list_view(dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;
  GtkTreeIter iter;

  GtkTreeView *view;
  GtkTreeModel *listmodel;

  view = d->view;
  listmodel = d->listmodel;
  g_object_ref(listmodel);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(listmodel));
  gtk_widget_hide(GTK_WIDGET(d->scrolledwindow));

  set_properties(dr);

  char query[1024];
  int property = gtk_combo_box_get_active(dr->combo);

  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL: // film roll
      snprintf(query, sizeof(query), "select distinct folder, id from film_rolls order by folder desc");
      break;
    case DT_COLLECTION_PROP_CAMERA: // camera
      snprintf(query, sizeof(query), "select distinct maker || ' ' || model as model, 1 from images order by model");
      break;
    case DT_COLLECTION_PROP_TAG: // tag
      // We shouldn't ever be here
      break;
    case DT_COLLECTION_PROP_HISTORY: // History, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("altered"),
                          DT_LIB_COLLECT_COL_PATH,_("altered"),
                          DT_LIB_COLLECT_COL_ID, 0,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("altered"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("not altered"),
                          DT_LIB_COLLECT_COL_PATH,_("not altered"),
                          DT_LIB_COLLECT_COL_ID, 1,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("not altered"),
                          -1);
      goto entry_key_press_exit;
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // Geotagging, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("tagged"),
                          DT_LIB_COLLECT_COL_PATH,_("tagged"),
                          DT_LIB_COLLECT_COL_ID, 0,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("tagged"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("not tagged"),
                          DT_LIB_COLLECT_COL_PATH,_("not tagged"),
                          DT_LIB_COLLECT_COL_ID, 1,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("not tagged"),
                          -1);
      goto entry_key_press_exit;
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabels
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("red"),
                          DT_LIB_COLLECT_COL_PATH,_("red"),
                          DT_LIB_COLLECT_COL_ID, 0,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("red"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("yellow"),
                          DT_LIB_COLLECT_COL_PATH,_("yellow"),
                          DT_LIB_COLLECT_COL_ID, 1,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("yellow"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("green"),
                          DT_LIB_COLLECT_COL_PATH,_("green"),
                          DT_LIB_COLLECT_COL_ID, 2,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("green"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("blue"),
                          DT_LIB_COLLECT_COL_PATH,_("blue"),
                          DT_LIB_COLLECT_COL_ID, 3,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("blue"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
      gtk_list_store_set (GTK_LIST_STORE(listmodel), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("purple"),
                          DT_LIB_COLLECT_COL_PATH,_("purple"),
                          DT_LIB_COLLECT_COL_ID, 4,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("purple"),
                          -1);
      goto entry_key_press_exit;
      break;

    // TODO: Add empty string for metadata?
    // TODO: Autogenerate this code?
    case DT_COLLECTION_PROP_TITLE: // title
      snprintf(query, sizeof(query), "select distinct value, 1 from meta_data where key = %d order by value",
                DT_METADATA_XMP_DC_TITLE);
      break;
    case DT_COLLECTION_PROP_DESCRIPTION: // description
      snprintf(query, sizeof(query), "select distinct value, 1 from meta_data where key = %d order by value",
                DT_METADATA_XMP_DC_DESCRIPTION);
      break;
    case DT_COLLECTION_PROP_CREATOR: // creator
      snprintf(query, sizeof(query), "select distinct value, 1 from meta_data where key = %d order by value",
                DT_METADATA_XMP_DC_CREATOR);
      break;
    case DT_COLLECTION_PROP_PUBLISHER: // publisher
      snprintf(query, sizeof(query), "select distinct value, 1 from meta_data where key = %d order by value",
                DT_METADATA_XMP_DC_PUBLISHER);
      break;
    case DT_COLLECTION_PROP_RIGHTS: // rights
      snprintf(query, sizeof(query), "select distinct value, 1 from meta_data where key = %d order by value ",
                DT_METADATA_XMP_DC_RIGHTS);
      break;
    case DT_COLLECTION_PROP_LENS: // lens
      snprintf(query, sizeof(query), "select distinct lens, 1 from images order by lens");
      break;
    case DT_COLLECTION_PROP_ISO: // iso
      snprintf(query, sizeof(query), "select distinct cast(iso as integer) as iso, 1 from images order by iso");
      break;
    case DT_COLLECTION_PROP_APERTURE: // aperture
      snprintf(query, sizeof(query), "select distinct round(aperture,1) as aperture, 1 from images order by aperture");    break;
      break;
    case DT_COLLECTION_PROP_FILENAME: // filename
      snprintf(query, sizeof(query), "select distinct filename, 1 from images order by filename");
      break;
    case DT_COLLECTION_PROP_FOLDERS: // folders
      // We shouldn't ever be here
      break;
    case DT_COLLECTION_PROP_DAY:
      snprintf(query, sizeof(query), "SELECT DISTINCT substr(datetime_taken, 1, 10), 1 FROM images ORDER BY datetime_taken DESC");
      break;
    default: // time
      snprintf(query, sizeof(query), "SELECT DISTINCT datetime_taken, 1 FROM images ORDER BY datetime_taken DESC");
      break;
  }

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(listmodel), &iter);
    const char *folder = (const char *)sqlite3_column_text(stmt, 0);
    if(property == 0) // film roll
    {
      folder = dt_image_film_roll_name(folder);
    }
    gchar *value = (gchar *)sqlite3_column_text(stmt, 0);

    // replace invalid utf8 characters if any
    gchar *text = g_strdup(value);
    gchar *ptr = text;
    while(!g_utf8_validate(ptr, -1, (const gchar **)&ptr)) ptr[0] = '?';
    gchar *escaped_text = g_markup_escape_text(text, -1);

    // we add the wildcards if needed
    // we do that here and not in the query creation, so the user can remove them if he want
    gchar *val_wild = NULL;
    switch(property)
    {
      case DT_COLLECTION_PROP_CAMERA:
      case DT_COLLECTION_PROP_FILENAME:
      case DT_COLLECTION_PROP_TITLE:
      case DT_COLLECTION_PROP_DESCRIPTION:
      case DT_COLLECTION_PROP_CREATOR:
      case DT_COLLECTION_PROP_PUBLISHER:
      case DT_COLLECTION_PROP_RIGHTS:
      case DT_COLLECTION_PROP_LENS:
        val_wild = dt_util_dstrcat(val_wild,"%%%s%%",value);
        break;
      case DT_COLLECTION_PROP_DAY:
      case DT_COLLECTION_PROP_TIME:
        val_wild = dt_util_dstrcat(val_wild,"%s%%",value);
        break;
      case DT_COLLECTION_PROP_ISO:
      case DT_COLLECTION_PROP_APERTURE:
      {
        gchar *operator, *number, *number2;
        dt_collection_split_operator_number(escaped_text, &number, &number2, &operator);
        if(!operator && !number) val_wild = dt_util_dstrcat(val_wild,"%%%s%%",value);
        else val_wild = dt_util_dstrcat(val_wild,"%s",value);
        g_free(operator);
        g_free(number);
        g_free(number2);
      }  
        break;
      default:
        val_wild = dt_util_dstrcat(val_wild,"%s",value);
        break;
    }

    gtk_list_store_set(GTK_LIST_STORE(listmodel), &iter, DT_LIB_COLLECT_COL_TEXT, folder,
                       DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1),
                       DT_LIB_COLLECT_COL_TOOLTIP, escaped_text,
                       DT_LIB_COLLECT_COL_PATH, val_wild,
                       DT_LIB_COLLECT_COL_STRIKETROUGTH, (property==DT_COLLECTION_PROP_FILMROLL && !g_file_test(value, G_FILE_TEST_IS_DIR)),
                       -1);
    g_free(text);
    g_free(escaped_text);
    g_free(val_wild);
  }
  sqlite3_finalize(stmt);

  goto entry_key_press_exit;

entry_key_press_exit:
  // we setup the autocompletion of the entry
  switch(property)
  {
    case DT_COLLECTION_PROP_FILMROLL:
    case DT_COLLECTION_PROP_CAMERA:
    case DT_COLLECTION_PROP_FILENAME:
    case DT_COLLECTION_PROP_TITLE:
    case DT_COLLECTION_PROP_DESCRIPTION:
    case DT_COLLECTION_PROP_CREATOR:
    case DT_COLLECTION_PROP_PUBLISHER:
    case DT_COLLECTION_PROP_RIGHTS:
    case DT_COLLECTION_PROP_LENS:
      gtk_entry_completion_set_model(d->autocompletion,listmodel);
      gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(d->autocompletion),d->auto_renderer, "text", DT_LIB_COLLECT_COL_TEXT, NULL);
      gtk_entry_completion_set_popup_set_width(d->autocompletion,TRUE);
      gtk_entry_set_completion(GTK_ENTRY(dr->text),d->autocompletion);
      break;
  }
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), listmodel);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), FALSE);
  gtk_widget_show_all(GTK_WIDGET(d->scrolledwindow));
  g_object_unref(listmodel);

  update_selection(dr,TRUE);
}

static void update_selection(dt_lib_collect_rule_t *dr, gboolean exact)
{
  dt_lib_collect_t *d = get_collect(dr);
  GtkTreeModel *model = gtk_tree_view_get_model(d->view);
  
  // we deselect and collapse everything
  d->update_query_on_sel_change = FALSE;
  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(d->view));
  gtk_tree_view_collapse_all(d->view);
  d->update_query_on_sel_change = TRUE;
  
  // we crawl throught the tree to find what we want
  if (exact) gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc)match_string_exact, dr);
  else gtk_tree_model_foreach (model, (GtkTreeModelForeachFunc)match_string, dr);
}

static void update_view(dt_lib_collect_rule_t *dr)
{
  dt_lib_collect_t *d = get_collect(dr);
  d->update_query_on_sel_change = FALSE;

  gtk_entry_set_completion(GTK_ENTRY(dr->text), NULL); 

  int property = gtk_combo_box_get_active(dr->combo);

  if(property == DT_COLLECTION_PROP_FOLDERS)
    folders_view(dr);
  else if(property == DT_COLLECTION_PROP_TAG)
    tags_view(dr);
  else
    list_view(dr);
  d->update_query_on_sel_change = TRUE;
}

static gboolean is_up_to_date (dt_lib_collect_t *d)
{
  // we verify the nb of rules
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES-1));
  if (!gtk_widget_get_visible(d->rule[active].hbox)) return FALSE;
  if (active < MAX_RULES && gtk_widget_get_visible(d->rule[active+1].hbox)) return FALSE;
  
  // we verify each rules
  char confname[200];
  for(int i=0; i<=active; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(d->rule[i].combo)) != dt_conf_get_int(confname)) return FALSE;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    if (text)
    {
      if (strcmp(gtk_entry_get_text(GTK_ENTRY(d->rule[i].text)),text) != 0) return FALSE;
      g_free(text);
    }
    if (i != active)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i+1);
      const int mode = dt_conf_get_int(confname);
      GtkDarktableButton *button = DTGTK_BUTTON(d->rule[i].button);
      if(mode == DT_LIB_COLLECT_MODE_AND && button->icon != dtgtk_cairo_paint_and) return FALSE;
      if(mode == DT_LIB_COLLECT_MODE_OR && button->icon != dtgtk_cairo_paint_or) return FALSE;
      if(mode == DT_LIB_COLLECT_MODE_AND_NOT && button->icon != dtgtk_cairo_paint_andnot) return FALSE;
    }
  }
  return TRUE;
}

static void _lib_collect_gui_update(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  if (is_up_to_date(d)) return;

  const int old = darktable.gui->reset;
  darktable.gui->reset = 1;
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200];

  gtk_widget_set_no_show_all(GTK_WIDGET(d->scrolledwindow), TRUE);

  for(int i = 0; i < MAX_RULES; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, TRUE);
    gtk_widget_set_visible(d->rule[i].hbox, FALSE);
  }
  for(int i = 0; i <= active; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, FALSE);
    gtk_widget_set_visible(d->rule[i].hbox, TRUE);
    gtk_widget_show_all(d->rule[i].hbox);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->rule[i].combo), dt_conf_get_int(confname));
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    if(text)
    {
      g_signal_handlers_block_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
      gtk_entry_set_text(GTK_ENTRY(d->rule[i].text), text);
      gtk_editable_set_position(GTK_EDITABLE(d->rule[i].text), -1);
      g_signal_handlers_unblock_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
      g_free(text);
    }

    GtkDarktableButton *button = DTGTK_BUTTON(d->rule[i].button);
    if(i == MAX_RULES - 1)
    {
      // only clear
      button->icon = dtgtk_cairo_paint_cancel;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule"), (char *)NULL);
    }
    else if(i == active)
    {
      button->icon = dtgtk_cairo_paint_dropdown;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule or add new rules"), (char *)NULL);
    }
    else
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i + 1);
      const int mode = dt_conf_get_int(confname);
      if(mode == DT_LIB_COLLECT_MODE_AND) button->icon = dtgtk_cairo_paint_and;
      if(mode == DT_LIB_COLLECT_MODE_OR) button->icon = dtgtk_cairo_paint_or;
      if(mode == DT_LIB_COLLECT_MODE_AND_NOT) button->icon = dtgtk_cairo_paint_andnot;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule"), (char *)NULL);
    }
  }

  // update list of proposals
  update_view(d->rule + d->active_rule);
  darktable.gui->reset = old;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_string("plugins/lighttable/collect/string0", "");
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection);
}

static void combo_changed(GtkComboBox *combo, dt_lib_collect_rule_t *d)
{
  if(darktable.gui->reset) return;
  g_signal_handlers_block_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(d->text), "");
  g_signal_handlers_unblock_matched(d->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  dt_lib_collect_t *c = get_collect(d);
  c->active_rule = d->num;

  update_view(d);
  dt_collection_update_query(darktable.collection);
}

static void selection_change (GtkTreeSelection *selection, dt_lib_collect_t *d)
{
  if (!d->update_query_on_sel_change) return;
  
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;

  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gchar *text;

  const int active = d->active_rule;

  gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);

  g_signal_handlers_block_matched(d->rule[active].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(d->rule[active].text), text);
  gtk_editable_set_position(GTK_EDITABLE(d->rule[active].text), -1);
  g_signal_handlers_unblock_matched(d->rule[active].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed,
                                    NULL);
  g_free(text);

  set_properties(&d->rule[active]);

  dt_collection_update_query(darktable.collection);
  dt_control_queue_redraw_center();
}

static void entry_activated(GtkWidget *entry, dt_lib_collect_rule_t *d)
{
  // we update the selection
  update_selection(d, TRUE);
  // we save the params
  set_properties(d);
  // and we update the query
  dt_collection_update_query(darktable.collection);
}

static void entry_changed(GtkEditable *editable, dt_lib_collect_rule_t *d)
{
  update_selection(d, FALSE);
}

int position()
{
  return 400;
}

static void entry_focus_in_callback(GtkWidget *w, GdkEventFocus *event, dt_lib_collect_rule_t *d)
{
  dt_lib_collect_t *c = get_collect(d);
  if (c->active_rule == d->num) return; // we don't have to change the view
  c->active_rule = d->num;
  update_view(c->rule + c->active_rule);
}

static void menuitem_and(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_or(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_and_not(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_and(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_or(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
  }
  dt_collection_update_query(darktable.collection);
}

static void menuitem_change_and_not(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
  }
  dt_collection_update_query(darktable.collection);
}

static void collection_updated(gpointer instance, gpointer self)
{
  _lib_collect_gui_update(self);
}


static void filmrolls_updated(gpointer instance, gpointer self)
{
  _lib_collect_gui_update(self);
}

static void filmrolls_imported(gpointer instance, int film_id, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;

  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  int active = 0;
  d->active_rule = active;

  // reset active rules
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  _lib_collect_gui_update(self);
}

static void filmrolls_removed(gpointer instance, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;

  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  int active = 0;
  d->active_rule = active;

  // reset active rules
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_string("plugins/lighttable/collect/string0", "");
  _lib_collect_gui_update(self);
}

static void menuitem_clear(GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // remove this row, or if 1st, clear text entry box
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  dt_lib_collect_t *c = get_collect(d);
  if(active > 1)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active - 1);
    if(c->active_rule >= active - 1) c->active_rule = active - 2;
  }
  else
  {
    dt_conf_set_int("plugins/lighttable/collect/mode0", DT_LIB_COLLECT_MODE_AND);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "");
    gtk_combo_box_set_active(d->combo, 0);
    gtk_entry_set_text(GTK_ENTRY(d->text), "");
  }
  // move up all still active rules by one.
  for(int i = d->num; i < MAX_RULES - 1; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i + 1);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i + 1);
    const int item = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i + 1);
    gchar *string = dt_conf_get_string(confname);
    if(string)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
      dt_conf_set_int(confname, mode);
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
      dt_conf_set_int(confname, item);
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
      dt_conf_set_string(confname, string);
      g_free(string);
    }
  }

  dt_collection_update_query(darktable.collection);
}

static gboolean popup_button_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *d)
{
  if(event->button != 1) return FALSE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *mi;
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);

  mi = gtk_menu_item_new_with_label(_("clear this rule"));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_clear), d);

  if(d->num == active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("narrow down search"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_and), d);

    mi = gtk_menu_item_new_with_label(_("add more images"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_or), d);

    mi = gtk_menu_item_new_with_label(_("exclude images"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_and_not), d);
  }
  else if(d->num < active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("change to: and"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_and), d);

    mi = gtk_menu_item_new_with_label(_("change to: or"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_or), d);

    mi = gtk_menu_item_new_with_label(_("change to: except"));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_change_and_not), d);
  }

  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, event->button, event->time);
  gtk_widget_show_all(menu);

  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)calloc(1, sizeof(dt_lib_collect_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  d->active_rule = 0;
  d->params = (dt_lib_collect_params_t *)malloc(sizeof(dt_lib_collect_params_t));
  d->update_query_on_sel_change = TRUE;

  GtkBox *box;
  GtkWidget *w;

  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
    box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
    d->rule[i].hbox = GTK_WIDGET(box);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
    w = gtk_combo_box_text_new();
    d->rule[i].combo = GTK_COMBO_BOX(w);
    for(int k = 0; k < dt_lib_collect_string_cnt; k++)
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(w), _(dt_lib_collect_string[k]));
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);
    w = gtk_entry_new();
    d->rule[i].text = w;
    dt_gui_key_accel_block_on_focus_connect(d->rule[i].text);
    gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(w), "focus-in-event", G_CALLBACK(entry_focus_in_callback), d->rule + i);

    /* xgettext:no-c-format */
    g_object_set(G_OBJECT(w), "tooltip-text", _("type your query, use `%' as wildcard"), (char *)NULL);
    gtk_widget_add_events(w, GDK_KEY_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(entry_changed), d->rule + i);
    g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), d->rule + i);
    gtk_box_pack_start(box, w, TRUE, TRUE, 0);
    w = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
    d->rule[i].button = w;
    gtk_widget_set_events(w, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(popup_button_callback), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);
    gtk_widget_set_size_request(w, DT_PIXEL_APPLY_DPI(13), DT_PIXEL_APPLY_DPI(13));
  }

  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  d->scrolledwindow = GTK_SCROLLED_WINDOW(sw);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(d->scrolledwindow), DT_PIXEL_APPLY_DPI(300));
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  gtk_tree_view_set_headers_visible(view, FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(view), -1, DT_PIXEL_APPLY_DPI(300));
  gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(view));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(d->view);
  g_signal_connect(selection, "changed", G_CALLBACK(selection_change), d);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_COLLECT_COL_TEXT);
  //this is used for filmroll and folder only
  g_object_set(renderer, "strikethrough", TRUE, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "strikethrough-set", DT_LIB_COLLECT_COL_STRIKETROUGTH);

  GtkTreeModel *listmodel
      = GTK_TREE_MODEL(gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_BOOLEAN));
  d->listmodel = listmodel;
  GtkTreeModel *treemodel
      = GTK_TREE_MODEL(gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                          G_TYPE_STRING, G_TYPE_BOOLEAN));
  d->treemodel = treemodel;

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw), TRUE, TRUE, 0);

  d->autocompletion = gtk_entry_completion_new();
  d->auto_renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(d->autocompletion),d->auto_renderer,TRUE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(d->autocompletion),d->auto_renderer, "text", DT_LIB_COLLECT_COL_PATH);
  gtk_entry_completion_set_match_func(d->autocompletion, entry_autocompl_match, d, NULL);
  g_signal_connect(G_OBJECT(d->autocompletion), "match-selected", G_CALLBACK(entry_autocompl_match_selected), d);

  /* setup proxy */
  darktable.view_manager->proxy.module_collect.module = self;
  darktable.view_manager->proxy.module_collect.update = _lib_collect_gui_update;

  // TODO: This should be done in a more generic place, not gui_init
  _lib_collect_gui_update(self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, G_CALLBACK(collection_updated),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED, G_CALLBACK(filmrolls_updated),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, G_CALLBACK(filmrolls_imported),
                            self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED, G_CALLBACK(filmrolls_removed),
                            self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  for(int i = 0; i < MAX_RULES; i++) dt_gui_key_accel_block_on_focus_disconnect(d->rule[i].text);

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(collection_updated), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(filmrolls_updated), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(filmrolls_imported), self);
  darktable.view_manager->proxy.module_collect.module = NULL;
  g_free(((dt_lib_collect_t *)self->data)->params);

  /* TODO: Make sure we are cleaning up all allocations */

  g_free(self->data);
  self->data = NULL;
}


#ifdef USE_LUA
typedef dt_lib_collect_params_rule_t* dt_lua_lib_collect_params_rule_t;
static int filter_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_lib_check_error(L,self);
  dt_lib_collect_params_t old_params;
  int size;
  
  dt_lib_collect_params_t *p = get_params(self, &size);
  // put it in stack so memory is not lost if a lua exception is raised
  memcpy(&old_params, p,size);
  free(p);
  if(lua_gettop(L) > 0) {
    dt_lib_collect_params_t params;
    luaA_to(L,dt_lib_collect_params_t,&params,1);
    set_params(self, &params,size);
    lua_pop(L,1);
  }
  luaA_push(L,dt_lib_collect_params_t,&old_params);
  return 1;
}

static int param_len(lua_State *L)
{
  dt_lib_collect_params_t params;
  luaA_to(L,dt_lib_collect_params_t,&params,1);
  lua_pushnumber(L, params.rules);
  return 1;
}

static int param_index(lua_State *L)
{
  dt_lib_collect_params_t* params =  lua_touserdata(L,1);
  int index = luaL_checkinteger(L,2);
  if(lua_gettop(L) > 2) {
    if(index < 1 || index > params->rules+1 || index > MAX_RULES) {
      return luaL_error(L,"incorrect write index for object of type dt_lib_collect_params_t\n");
    }
    if(lua_isnil(L,3)) {
      for(int i = index; i< params->rules -1 ;i++){
        memcpy(&params->rule[index-1],&params->rule[index],sizeof(dt_lib_collect_params_rule_t));
      }
      params->rules--;
    } else if(dt_lua_isa(L,3,dt_lua_lib_collect_params_rule_t)){
      if(index == params->rules+1) {
        params->rules++;
      }
      dt_lib_collect_params_rule_t *rule;
      luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,3);
      memcpy(&params->rule[index-1],rule,sizeof(dt_lib_collect_params_rule_t));
    } else {
      return luaL_error(L,"incorrect type for field of dt_lib_collect_params_t\n");
    }
  }
  if(index < 1 || index > params->rules) {
    return luaL_error(L,"incorrect read index for object of type dt_lib_collect_params_t\n");
  }
  dt_lib_collect_params_rule_t* tmp = &params->rule[index-1];
  luaA_push(L,dt_lua_lib_collect_params_rule_t,&tmp);
  lua_getuservalue(L,-1);
  lua_pushvalue(L,1);
  lua_setfield(L,-2,"containing_object");//prevent GC from killing the child object
  lua_pop(L,1);
  return 1;
}

static int mode_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);
  if(lua_gettop(L) > 2) {
    dt_lib_collect_mode_t value;
    luaA_to(L,dt_lib_collect_mode_t,&value,3);
    rule->mode = value;
    return 0;
  }
  const dt_lib_collect_mode_t tmp = rule->mode; // temp buffer because of bitfield in the original struct
  luaA_push(L,dt_lib_collect_mode_t,&tmp);
  return 1;
}

static int item_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);

  if(lua_gettop(L) > 2) {
    dt_collection_properties_t value;
    luaA_to(L,dt_collection_properties_t,&value,3);
    rule->item = value;
    return 0;
  }
  const dt_collection_properties_t tmp = rule->item; // temp buffer because of bitfield in the original struct
  luaA_push(L,dt_collection_properties_t,&tmp);
  return 1;
}

static int data_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule;
  luaA_to(L,dt_lua_lib_collect_params_rule_t,&rule,1);

  if(lua_gettop(L) > 2) {
    size_t tgt_size;
    const char*data = luaL_checklstring(L,3,&tgt_size);
    if(tgt_size > PARAM_STRING_SIZE)
    {
      return luaL_error(L, "string '%s' too long (max is %d)", data, PARAM_STRING_SIZE);
    }
    memcpy(rule->string,data,strlen(data));
    return 0;
  }
  lua_pushstring(L,rule->string);
  return 1;
}



void init(struct dt_lib_module_t *self)
{

  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, filter_cb,1);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "filter");

  dt_lua_init_type(L,dt_lib_collect_params_t);
  lua_pushcfunction(L,param_len);
  lua_pushcfunction(L,param_index);
  dt_lua_type_register_number(L,dt_lib_collect_params_t);

  dt_lua_init_type(L,dt_lua_lib_collect_params_rule_t);
  lua_pushcfunction(L,mode_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "mode");
  lua_pushcfunction(L,item_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "item");
  lua_pushcfunction(L,data_member);
  dt_lua_type_register(L, dt_lua_lib_collect_params_rule_t, "data");
  

  luaA_enum(L,dt_lib_collect_mode_t);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_AND);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_OR);
  luaA_enum_value(L,dt_lib_collect_mode_t,DT_LIB_COLLECT_MODE_AND_NOT);

  luaA_enum(L,dt_collection_properties_t);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FILMROLL);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FOLDERS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_CAMERA);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TAG);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_DAY);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TIME);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_HISTORY);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_COLORLABEL);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_TITLE);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_DESCRIPTION);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_CREATOR);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_PUBLISHER);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_RIGHTS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_LENS);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_ISO);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_APERTURE);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_FILENAME);
  luaA_enum_value(L,dt_collection_properties_t,DT_COLLECTION_PROP_GEOTAGGING);

}
#endif
#undef MAX_RULES
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
