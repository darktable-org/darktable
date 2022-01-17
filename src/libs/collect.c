/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

#include "libs/collect.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "common/history.h"
#include "common/map_locations.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <locale.h>

DT_MODULE(3)

#define MAX_RULES 10

#define PARAM_STRING_SIZE 256 // FIXME: is this enough !?

typedef struct _widgets_sort_t
{
  GtkWidget *box;
  GtkWidget *sort;
  GtkWidget *direction;
} _widgets_sort_t;

typedef struct _widgets_rating_t
{
  GtkWidget *comparator;
  GtkWidget *combo;
} _widgets_rating_t;

typedef struct _widgets_aspect_ratio_t
{
  GtkWidget *mode;
  GtkWidget *value;
} _widgets_aspect_ratio_t;

typedef struct dt_lib_collect_rule_t
{
  int num;

  dt_collection_properties_t prop;

  GtkWidget *w_main;
  GtkWidget *w_operator;
  GtkWidget *w_prop;
  GtkWidget *w_close;

  GtkWidget *w_widget_box;
  GtkWidget *w_raw_text;
  GtkWidget *w_raw_switch;
  GtkWidget *w_special_box;
  void *w_specific; // structure which contains all the widgets specific to the rule type
  GtkWidget *w_expand;
  GtkWidget *w_view_sw;
  GtkTreeView *w_view;
  GtkTreeModel *filter;
  int manual_widget_set; // when we update manually the widget, we don't want events to be handled

  gchar *searchstring;
} dt_lib_collect_rule_t;

typedef struct dt_lib_collect_t
{
  dt_lib_collect_rule_t rule[MAX_RULES];
  int nb_rules;

  GtkWidget *rules_box;
  _widgets_sort_t *sort;
  gboolean manual_sort_set;

  gboolean singleclick;
  struct dt_lib_collect_params_t *params;
#ifdef _WIN32
  GVolumeMonitor *vmonitor;
#else
  GUnixMountMonitor *vmonitor;
#endif
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
  DT_LIB_COLLECT_COL_VISIBLE,
  DT_LIB_COLLECT_COL_UNREACHABLE,
  DT_LIB_COLLECT_COL_COUNT,
  DT_LIB_COLLECT_COL_INDEX,
  DT_LIB_COLLECT_NUM_COLS
} dt_lib_collect_cols_t;

typedef struct _range_t
{
  gboolean strict;
  gchar *start;
  gchar *stop;
  GtkTreePath *path1;
  GtkTreePath *path2;
} _range_t;

static void _lib_collect_gui_update(dt_lib_module_t *self);
static void _lib_folders_update_collection(const gchar *filmroll);
// static void entry_changed(GtkEntry *entry, dt_lib_collect_rule_t *dr);
static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next,
                               gpointer self);
static void _event_row_activated_with_event(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                                            GdkEventButton *event, dt_lib_collect_rule_t *rule);

int last_state = 0;

const char *name(dt_lib_module_t *self)
{
  return _("collections");
}

void *legacy_params(struct dt_lib_module_t *self,
                    const void *const old_params, const size_t old_params_size, const int old_version,
                    int *new_version, size_t *new_size)
{
  if(old_version == 1)
  {
    /* from v1 to v2 we have reordered the filters */
    dt_lib_collect_params_t *o = (dt_lib_collect_params_t *)old_params;

    if(o->rules > MAX_RULES)
      /* preset is corrupted, return NULL and drop the preset */
      return NULL;

    dt_lib_collect_params_t *n = (dt_lib_collect_params_t *)malloc(old_params_size);

    const int table[DT_COLLECTION_PROP_LAST] =
      {
        DT_COLLECTION_PROP_FILMROLL,
        DT_COLLECTION_PROP_FOLDERS,
        DT_COLLECTION_PROP_CAMERA,
        DT_COLLECTION_PROP_TAG,
        DT_COLLECTION_PROP_DAY,
        DT_COLLECTION_PROP_TIME,
        DT_COLLECTION_PROP_HISTORY,
        DT_COLLECTION_PROP_COLORLABEL,

        // spaces for the metadata, see metadata.h
        DT_COLLECTION_PROP_COLORLABEL + 1,
        DT_COLLECTION_PROP_COLORLABEL + 2,
        DT_COLLECTION_PROP_COLORLABEL + 3,
        DT_COLLECTION_PROP_COLORLABEL + 4,
        DT_COLLECTION_PROP_COLORLABEL + 5,

        DT_COLLECTION_PROP_LENS,
        DT_COLLECTION_PROP_FOCAL_LENGTH,
        DT_COLLECTION_PROP_ISO,
        DT_COLLECTION_PROP_APERTURE,
        DT_COLLECTION_PROP_EXPOSURE,
        DT_COLLECTION_PROP_ASPECT_RATIO,
        DT_COLLECTION_PROP_FILENAME,
        DT_COLLECTION_PROP_GEOTAGGING,
        DT_COLLECTION_PROP_GROUPING,
        DT_COLLECTION_PROP_LOCAL_COPY,
        DT_COLLECTION_PROP_MODULE,
        DT_COLLECTION_PROP_ORDER
      };

    n->rules = o->rules;

    for(int r=0; r<o->rules; r++)
    {
      n->rule[r].item = table[o->rule[r].item];
      n->rule[r].mode = o->rule[r].mode;
      memcpy(n->rule[r].string, o->rule[r].string, PARAM_STRING_SIZE);
    }

    *new_size = old_params_size;
    *new_version = 2;

    return (void *)n;
  }
  else if(old_version == 2)
  {
    /* from v2 to v3 we have added 4 new timestamp filters and 2 metadata filters */
    dt_lib_collect_params_t *old = (dt_lib_collect_params_t *)old_params;

    if(old->rules > MAX_RULES)
      /* preset is corrupted, return NULL and drop the preset */
      return NULL;

    dt_lib_collect_params_t *new = (dt_lib_collect_params_t *)malloc(old_params_size);

    const int table[DT_COLLECTION_PROP_LAST] =
      {
        DT_COLLECTION_PROP_FILMROLL,
        DT_COLLECTION_PROP_FOLDERS,
        DT_COLLECTION_PROP_FILENAME,
        DT_COLLECTION_PROP_CAMERA,
        DT_COLLECTION_PROP_LENS,
        DT_COLLECTION_PROP_APERTURE,
        DT_COLLECTION_PROP_EXPOSURE,
        DT_COLLECTION_PROP_FOCAL_LENGTH,
        DT_COLLECTION_PROP_ISO,
        DT_COLLECTION_PROP_DAY,
        DT_COLLECTION_PROP_TIME,
        DT_COLLECTION_PROP_GEOTAGGING,
        DT_COLLECTION_PROP_ASPECT_RATIO,
        DT_COLLECTION_PROP_TAG,
        DT_COLLECTION_PROP_COLORLABEL,

        // spaces for the metadata, see metadata.h
        DT_COLLECTION_PROP_COLORLABEL + 1,
        DT_COLLECTION_PROP_COLORLABEL + 2,
        DT_COLLECTION_PROP_COLORLABEL + 3,
        DT_COLLECTION_PROP_COLORLABEL + 4,
        DT_COLLECTION_PROP_COLORLABEL + 5,

        DT_COLLECTION_PROP_GROUPING,
        DT_COLLECTION_PROP_LOCAL_COPY,
        DT_COLLECTION_PROP_HISTORY,
        DT_COLLECTION_PROP_MODULE,
        DT_COLLECTION_PROP_ORDER
      };

    new->rules = old->rules;

    for(int r = 0; r < old->rules; r++)
    {
      new->rule[r].item = table[old->rule[r].item];
      new->rule[r].mode = old->rule[r].mode;
      memcpy(new->rule[r].string, old->rule[r].string, PARAM_STRING_SIZE);
    }

    *new_size = old_params_size;
    *new_version = 3;

    return (void *)new;
  }

  return NULL;
}

void init_presets(dt_lib_module_t *self)
{
  dt_lib_collect_params_t params;

#define CLEAR_PARAMS(r) {                \
    memset(&params, 0, sizeof(params));  \
    params.rules = 1;                    \
    params.rule[0].mode = 0;             \
    params.rule[0].item = r;             \
  }

  // based on aspect-ratio

  /*CLEAR_PARAMS(DT_COLLECTION_PROP_ASPECT_RATIO);
  g_strlcpy(params.rule[0].string, "= 1", PARAM_STRING_SIZE);

  dt_lib_presets_add(_("square"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_ASPECT_RATIO);
  g_strlcpy(params.rule[0].string, "> 1", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("landscape"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_ASPECT_RATIO);
  g_strlcpy(params.rule[0].string, "< 1", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("portrait"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  // based on date/time
  struct tm tt;
  char datetime_today[100] = { 0 };
  char datetime_24hrs[100] = { 0 };
  char datetime_30d[100] = { 0 };

  const time_t now = time(NULL);
  const time_t ONE_DAY = (24 * 60 * 60);
  const time_t last24h = now - ONE_DAY;
  const time_t last30d = now - (ONE_DAY * 30);

  (void)localtime_r(&now, &tt);
  strftime(datetime_today, 100, "%Y:%m:%d", &tt);

  (void)localtime_r(&last24h, &tt);
  strftime(datetime_24hrs, 100, "> %Y:%m:%d %H:%M", &tt);

  (void)localtime_r(&last30d, &tt);
  strftime(datetime_30d, 100, "> %Y:%m:%d", &tt);

  // presets based on import
  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_today, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: today"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_24hrs, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 24h"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_30d, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 30 days"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  // presets based on image metadata (image taken)
  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_today, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: today"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_24hrs, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 24h"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_30d, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 30 days"), self->plugin_name, self->version(),
                       &params, sizeof(params), TRUE);*/

  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=0", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : all except rejected"), self->plugin_name, self->version(), &params,
                     sizeof(params), TRUE);
  CLEAR_PARAMS(DT_COLLECTION_PROP_RATING);
  g_strlcpy(params.rule[0].string, ">=2", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("rating : ★ ★"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_COLORLABEL);
  g_strlcpy(params.rule[0].string, "red", PARAM_STRING_SIZE);
  dt_lib_presets_add(_("color labels : red"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

#undef CLEAR_PARAMS
}

static gboolean _rule_is_time_property(dt_collection_properties_t prop)
{
  return (prop == DT_COLLECTION_PROP_TIME
       || prop == DT_COLLECTION_PROP_IMPORT_TIMESTAMP
       || prop == DT_COLLECTION_PROP_CHANGE_TIMESTAMP
       || prop == DT_COLLECTION_PROP_EXPORT_TIMESTAMP
       || prop == DT_COLLECTION_PROP_PRINT_TIMESTAMP);
}
static gboolean _rule_allow_comparison(dt_collection_properties_t prop)
{
  return (prop == DT_COLLECTION_PROP_APERTURE
       || prop == DT_COLLECTION_PROP_FOCAL_LENGTH
       || prop == DT_COLLECTION_PROP_ISO
       || prop == DT_COLLECTION_PROP_EXPOSURE
       || prop == DT_COLLECTION_PROP_ASPECT_RATIO
       || prop == DT_COLLECTION_PROP_RATING);
}
static gboolean _rule_use_treeview(dt_collection_properties_t prop)
{
  return (prop == DT_COLLECTION_PROP_FOLDERS
       || prop == DT_COLLECTION_PROP_TAG
       || prop == DT_COLLECTION_PROP_GEOTAGGING
       || prop == DT_COLLECTION_PROP_DAY
       || _rule_is_time_property(prop));
}
static gboolean _rule_allow_range(dt_collection_properties_t prop)
{
  return (prop == DT_COLLECTION_PROP_DAY
       || _rule_is_time_property(prop)
       || _rule_allow_comparison(prop));
}

/* Update the params struct with active ruleset */
static void _lib_collect_update_params(dt_lib_collect_t *d)
{
  /* reset params */
  dt_lib_collect_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_collect_params_t));

  /* for each active rule set update params */
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  char confname[200] = { 0 };
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
    const char *string = dt_conf_get_string_const(confname);
    if(string != NULL)
    {
      g_strlcpy(p->rule[i].string, string, PARAM_STRING_SIZE);
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
  char confname[200] = { 0 };

  gboolean reset_view_filter = FALSE;
  for(uint32_t i = 0; i < p->rules; i++)
  {
    /* set item */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1u", i);
    dt_conf_set_int(confname, p->rule[i].item);

    /* set mode */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1u", i);
    dt_conf_set_int(confname, p->rule[i].mode);

    /* set string */
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1u", i);
    dt_conf_set_string(confname, p->rule[i].string);

    /* if one of the rules is a rating filter, the view rating filter will be reset to all */
    if(p->rule[i].item == DT_COLLECTION_PROP_RATING)
    {
      reset_view_filter = TRUE;
    }
  }

  if(reset_view_filter)
  {
    dt_view_filter_reset(darktable.view_manager, FALSE);
  }

  /* set number of rules */
  g_strlcpy(confname, "plugins/lighttable/collect/num_rules", sizeof(confname));
  dt_conf_set_int(confname, p->rules);

  /* update internal params */
  _lib_collect_update_params(self->data);

  /* update ui */
  _lib_collect_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return 0;
}


const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "map", "print", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static void view_popup_menu_onSearchFilmroll(GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

  GtkTreeSelection *selection;
  GtkTreeIter iter, child;
  GtkTreeModel *model;

  gchar *tree_path = NULL;
  gchar *new_path = NULL;

  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  if (!gtk_tree_selection_get_selected(selection, &model, &iter))
    return;

  child = iter;
  gtk_tree_model_iter_parent(model, &iter, &child);
  gtk_tree_model_get(model, &child, DT_LIB_COLLECT_COL_PATH, &tree_path, -1);

  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
          _("search filmroll"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
          _("_open"), _("_cancel"));

  if(tree_path != NULL)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), tree_path);
  else
    goto error;

  // run the dialog
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gint id = -1;
    sqlite3_stmt *stmt;
    gchar *query = NULL;

    gchar *uri = NULL;
    uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(filechooser));
    new_path = g_filename_from_uri(uri, NULL, NULL);
    g_free(uri);
    if(new_path)
    {
      gchar *old = NULL;

      gchar *q_tree_path = g_strdup_printf("%s%%", tree_path);
      query = "SELECT id, folder FROM main.film_rolls WHERE folder LIKE ?1";
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, q_tree_path, -1, SQLITE_TRANSIENT);
      g_free(q_tree_path);
      q_tree_path = NULL;
      query = NULL;

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        old = (gchar *)sqlite3_column_text(stmt, 1);

        query = g_strdup("UPDATE main.film_rolls SET folder=?1 WHERE id=?2");

        gchar trailing[1024] = { 0 };
        gchar final[1024] = { 0 };

        if(g_strcmp0(old, tree_path))
        {
          g_strlcpy(trailing, old + strlen(tree_path) + 1, sizeof(trailing));
          g_snprintf(final, sizeof(final), "%s/%s", new_path, trailing);
        }
        else
        {
          g_strlcpy(final, new_path, sizeof(final));
        }

        sqlite3_stmt *stmt2;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt2, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt2, 1, final, -1, SQLITE_STATIC);
        DT_DEBUG_SQLITE3_BIND_INT(stmt2, 2, id);
        sqlite3_step(stmt2);
        sqlite3_finalize(stmt2);
      }
      sqlite3_finalize(stmt);
      g_free(query);

      /* reset filter so that view isn't empty */
      dt_view_filter_reset(darktable.view_manager, FALSE);

      // refresh the folders status
      dt_film_set_folder_status();

      /* update collection to view missing filmroll */
      _lib_folders_update_collection(new_path);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
    }
    else
      goto error;
  }
  g_free(tree_path);
  g_free(new_path);
  g_object_unref(filechooser);
  return;

error:
  /* Something wrong happened */
  g_object_unref(filechooser);
  dt_control_log(_("problem selecting new path for the filmroll in %s"), tree_path);

  g_free(tree_path);
  g_free(new_path);
}

static void view_popup_menu_onRemove(GtkWidget *menuitem, gpointer userdata)
{
  GtkTreeView *treeview = GTK_TREE_VIEW(userdata);

  GtkTreeSelection *selection;
  GtkTreeIter iter, model_iter;
  GtkTreeModel *model;

  /* Get info about the filmroll (or parent) selected */
  model = gtk_tree_view_get_model(treeview);
  selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));
  if (gtk_tree_selection_get_selected(selection, &model, &iter))
  {
    gchar *filmroll_path = NULL;
    gchar *fullq = NULL;

    gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &filmroll_path, -1);

    /* Clean selected images, and add to the table those which are going to be deleted */
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);

    fullq = g_strdup_printf("INSERT INTO main.selected_images"
                            " SELECT id"
                            " FROM main.images"
                            " WHERE film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s%%')",
                            filmroll_path);
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);
    g_free(filmroll_path);

    if (dt_control_remove_images())
    {
      gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(model), &model_iter, &iter);

      if (gtk_tree_model_get_flags(model) == GTK_TREE_MODEL_LIST_ONLY)
      {
        gtk_list_store_remove(GTK_LIST_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model))),
                              &model_iter);
      }
      else
      {
        gtk_tree_store_remove(GTK_TREE_STORE(gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(model))),
                              &model_iter);
      }
    }

    g_free(fullq);
  }
}

static void _view_popup_menu(GtkWidget *treeview, GdkEventButton *event, dt_lib_collect_rule_t *rule)
{
  GtkWidget *menu, *menuitem;

  menu = gtk_menu_new();

  menuitem = gtk_menu_item_new_with_label(_("search filmroll..."));
  g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_onSearchFilmroll, treeview);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);

  menuitem = gtk_menu_item_new_with_label(_("remove..."));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), menuitem);
  g_signal_connect(menuitem, "activate", (GCallback)view_popup_menu_onRemove, treeview);

  gtk_widget_show_all(GTK_WIDGET(menu));

  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static dt_lib_collect_t *get_collect(dt_lib_collect_rule_t *r)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)(((char *)r) - r->num * sizeof(dt_lib_collect_rule_t));
  return d;
}

static gboolean _event_view_onButtonPressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_collect_rule_t *rule)
{
  dt_lib_collect_t *d = get_collect(rule);
  /* Get tree path for row that was clicked */
  GtkTreePath *path = NULL;
  int get_path = gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL);

  if(event->type == GDK_DOUBLE_BUTTON_PRESS)
  {
    if(event->state == last_state)
    {
      if(gtk_tree_view_row_expanded(GTK_TREE_VIEW(treeview), path))
        gtk_tree_view_collapse_row(GTK_TREE_VIEW(treeview), path);
      else
        gtk_tree_view_expand_row(GTK_TREE_VIEW(treeview), path, FALSE);
    }
    last_state = event->state;
  }

  if(((rule->prop == DT_COLLECTION_PROP_FOLDERS || rule->prop == DT_COLLECTION_PROP_FILMROLL)
      && event->type == GDK_BUTTON_PRESS && event->button == 3)
     || (!d->singleclick && event->type == GDK_2BUTTON_PRESS && event->button == 1)
     || (d->singleclick && event->type == GDK_BUTTON_PRESS && event->button == 1)
     || ((rule->prop == DT_COLLECTION_PROP_FOLDERS || rule->prop == DT_COLLECTION_PROP_FILMROLL)
         && (event->type == GDK_BUTTON_PRESS && event->button == 1
             && (dt_modifier_is(event->state, GDK_SHIFT_MASK) || dt_modifier_is(event->state, GDK_CONTROL_MASK)))))
  {
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(treeview));

    if(get_path)
    {
      if(d->singleclick && dt_modifier_is(event->state, GDK_SHIFT_MASK)
         && gtk_tree_selection_count_selected_rows(selection) > 0
         && _rule_allow_range(rule->prop))
      {
        // range selection
        GList *sels = gtk_tree_selection_get_selected_rows(selection, NULL);
        GtkTreePath *path2 = (GtkTreePath *)sels->data;
        gtk_tree_selection_unselect_all(selection);
        if(gtk_tree_path_compare(path, path2) > 0)
          gtk_tree_selection_select_range(selection, path, path2);
        else
          gtk_tree_selection_select_range(selection, path2, path);
        g_list_free_full(sels, (GDestroyNotify)gtk_tree_path_free);
      }
      else
      {
        gtk_tree_selection_unselect_all(selection);
        gtk_tree_selection_select_path(selection, path);
      }
    }

    /* single click on folder with the right mouse button? */
    if(((rule->prop == DT_COLLECTION_PROP_FOLDERS) || (rule->prop == DT_COLLECTION_PROP_FILMROLL))
       && (event->type == GDK_BUTTON_PRESS && event->button == 3)
       && !(dt_modifier_is(event->state, GDK_SHIFT_MASK) || dt_modifier_is(event->state, GDK_CONTROL_MASK)))
    {
      _event_row_activated_with_event(GTK_TREE_VIEW(treeview), path, NULL, event, rule);
      _view_popup_menu(treeview, event, rule);
    }
    else
    {
      _event_row_activated_with_event(GTK_TREE_VIEW(treeview), path, NULL, event, rule);
    }

    gtk_tree_path_free(path);

    if(_rule_use_treeview(rule->prop) && !dt_modifier_is(event->state, GDK_SHIFT_MASK))
      return FALSE; /* we allow propagation (expand/collapse row) */
    else
      return TRUE; /* we stop propagation */
  }
  return FALSE; /* we did not handle this */
}

static gboolean _event_view_onPopupMenu(GtkWidget *treeview, dt_lib_collect_rule_t *rule)
{
  if(rule->prop != DT_COLLECTION_PROP_FOLDERS)
  {
    return FALSE;
  }

  _view_popup_menu(treeview, NULL, rule);

  return TRUE; /* we handled this */
}

static gboolean list_select(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), -1);

  if(strcmp(haystack, needle) == 0)
  {
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(rule->w_view), path);
    gtk_tree_view_scroll_to_cell(rule->w_view, path, NULL, FALSE, 0.2, 0);
  }

  g_free(haystack);
  g_free(needle);
  g_free(str);

  return FALSE;
}

static gboolean range_select(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  _range_t *range = (_range_t *)data;

  gboolean rep = FALSE;

  // first, we want to set first or last item if this is just a comparison
  if(!range->start && !range->path1)
  {
    range->path1 = gtk_tree_path_new_from_indices(0, -1);
  }
  if(!range->stop && !range->path2)
  {
    const int nb = gtk_tree_model_iter_n_children(model, NULL);
    range->path2 = gtk_tree_path_new_from_indices(nb - 1, -1);
  }
  gchar *str = NULL;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = NULL;
  if(range->path1 && range->stop)
    needle = g_utf8_strdown(range->stop, -1);
  else if(range->start)
    needle = g_utf8_strdown(range->start, -1);

  if(needle && strcmp(haystack, needle) == 0)
  {
    if(range->path1)
    {
      if(range->strict) gtk_tree_path_prev(path);
      range->path2 = gtk_tree_path_copy(path);
      rep = TRUE;
    }
    else
    {
      if(range->strict) gtk_tree_path_next(path);
      range->path1 = gtk_tree_path_copy(path);
      if(range->start && range->stop && !strcmp(range->start, range->stop))
      {
        range->path2 = gtk_tree_path_copy(path);
        rep = TRUE;
      }
    }
  }

  g_free(haystack);
  g_free(needle);
  g_free(str);

  return rep;
}

/*int _combo_get_active_collection(GtkWidget *combo)
{
  return GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(combo)) - 1;
}

gboolean _combo_set_active_collection(GtkWidget *combo, const int property)
{
  const gboolean found = dt_bauhaus_combobox_set_from_value(combo, property + 1);
  // make sure we have a valid collection
  if(!found) dt_bauhaus_combobox_set_from_value(combo, DT_COLLECTION_PROP_FILMROLL + 1);
  return found;
}*/

static gboolean tree_expand(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gchar *txt = NULL;
  gboolean startwildcard = FALSE;
  gboolean expanded = FALSE;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_TEXT, &txt, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), -1);
  gchar *txt2 = g_utf8_strdown(txt, -1);

  if(g_str_has_prefix(needle, "%")) startwildcard = TRUE;
  if(g_str_has_suffix(needle, "%")) needle[strlen(needle) - 1] = '\0';
  if(g_str_has_suffix(haystack, "%")) haystack[strlen(haystack) - 1] = '\0';
  if(rule->prop == DT_COLLECTION_PROP_TAG || rule->prop == DT_COLLECTION_PROP_GEOTAGGING)
  {
    if(g_str_has_suffix(needle, "|")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(haystack, "|")) haystack[strlen(haystack) - 1] = '\0';
  }
  else if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
  {
    if(g_str_has_suffix(needle, "*")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(needle, "/")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(haystack, "/")) haystack[strlen(haystack) - 1] = '\0';
  }
  else
  {
    if(DT_COLLECTION_PROP_DAY == rule->prop || _rule_is_time_property(rule->prop))
    {
      if(g_str_has_suffix(needle, ":")) needle[strlen(needle) - 1] = '\0';
      if(g_str_has_suffix(haystack, ":")) haystack[strlen(haystack) - 1] = '\0';
    }
  }
  /*if(dr->typing && g_strrstr(txt2, needle) != NULL)
  {
    gtk_tree_view_expand_to_path(dr->w_view, path);
    expanded = TRUE;
  }*/

  if(strlen(needle)==0)
  {
    //nothing to do, we keep the tree collapsed
  }
  else if(strcmp(haystack, needle) == 0)
  {
    gtk_tree_view_expand_to_path(rule->w_view, path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(rule->w_view), path);
    gtk_tree_view_scroll_to_cell(rule->w_view, path, NULL, FALSE, 0.2, 0);
    expanded = TRUE;
  }
  else if(startwildcard && g_strrstr(haystack, needle+1) != NULL)
  {
    gtk_tree_view_expand_to_path(rule->w_view, path);
    expanded = TRUE;
  }
  else if(rule->prop != DT_COLLECTION_PROP_FOLDERS && g_str_has_prefix(haystack, needle))
  {
    gtk_tree_view_expand_to_path(rule->w_view, path);
    expanded = TRUE;
  }

  g_free(haystack);
  g_free(needle);
  g_free(txt2);
  g_free(str);
  g_free(txt);

  return expanded; // if we expanded the path, we can stop iteration (saves half on average)
}

/*
static gboolean list_match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gboolean visible = FALSE;
  gboolean was_visible;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &was_visible, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  const gchar *needle = dr->searchstring;

  const int property = _combo_get_active_collection(dr->combo);
  if(property == DT_COLLECTION_PROP_APERTURE
     || property == DT_COLLECTION_PROP_FOCAL_LENGTH
     || property == DT_COLLECTION_PROP_ISO
     || property == DT_COLLECTION_PROP_RATING)
  {
    // handle of numeric value, which can have some operator before the text
    visible = TRUE;
    gchar *operator, *number, *number2;
    dt_collection_split_operator_number(needle, &number, &number2, &operator);
    if(number)
    {
      const float nb1 = g_strtod(number, NULL);
      const float nb2 = g_strtod(haystack, NULL);
      if(operator && strcmp(operator, ">") == 0)
      {
        visible = (nb2 > nb1);
      }
      else if(operator && strcmp(operator, ">=") == 0)
      {
        visible = (nb2 >= nb1);
      }
      else if(operator && strcmp(operator, "<") == 0)
      {
        visible = (nb2 < nb1);
      }
      else if(operator && strcmp(operator, "<=") == 0)
      {
        visible = (nb2 <= nb1);
      }
      else if(operator && strcmp(operator, "<>") == 0)
      {
        visible = (nb1 != nb2);
      }
      else if(operator && number2 && strcmp(operator, "[]") == 0)
      {
        const float nb3 = g_strtod(number2, NULL);
        visible = (nb2 >= nb1 && nb2 <= nb3);
      }
      else
      {
        visible = (nb1 == nb2);
      }
    }
    g_free(operator);
    g_free(number);
    g_free(number2);
  }
  else if (property == DT_COLLECTION_PROP_FILENAME && strchr(needle,',') != NULL)
  {
    GList *list = dt_util_str_to_glist(",", needle);

    for (const GList *l = list; l; l = g_list_next(l))
    {
      const char *name = (char *)l->data;
      if((visible = (g_strrstr(haystack, name + (name[0]=='%')) != NULL))) break;
    }

    g_list_free_full(list, g_free);

  }
  else
  {
    if (needle[0] == '%')
      needle++;
    if (!needle[0])
    {
      // empty search string matches all
      visible = TRUE;
    }
    else if (!needle[1])
    {
      // single-char search, use faster strchr instead of strstr
      visible = (strchr(haystack, needle[0]) != NULL);
    }
    else
    {
      visible = (g_strrstr(haystack, needle) != NULL);
    }
  }

  g_free(haystack);
  g_free(str);

  if (visible != was_visible)
    gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}

static gboolean tree_match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gboolean cur_state, visible;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &cur_state, -1);

  if(dr->typing == FALSE && !cur_state)
  {
    visible = TRUE;
  }
  else
  {
    gchar *haystack = g_utf8_strdown(str, -1),
          *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->w_raw_text)), -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    g_free(haystack);
    g_free(needle);
  }

  g_free(str);

  gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}

static gboolean tree_reveal_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  gboolean state;
  GtkTreeIter parent, child = *iter;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);
  if(!state) return FALSE;

  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_model_get(model, &parent, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);
    gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_COLLECT_COL_VISIBLE, TRUE, -1);
    child = parent;
  }

  return FALSE;
}

static void tree_set_visibility(GtkTreeModel *model, gpointer data)
{
  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_match_string, data);

  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_reveal_func, NULL);
}*/

static void _lib_folders_update_collection(const gchar *filmroll)
{

  // remove from selected images where not in this query.
  sqlite3_stmt *stmt = NULL;
  const gchar *cquery = dt_collection_get_query(darktable.collection);
  if(cquery && cquery[0] != '\0')
  {
    gchar *complete_query = g_strdup_printf(
                          "DELETE FROM main.selected_images WHERE imgid NOT IN (%s)",
                          cquery);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), complete_query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* free allocated strings */
    g_free(complete_query);
  }

  /* raise signal of collection change, only if this is an original */
  if(!darktable.collection->clone)
  {
    dt_collection_memory_update();
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, DT_COLLECTION_CHANGE_NEW_QUERY,
                                  DT_COLLECTION_PROP_UNDEF, NULL, -1);
  }
}

static void _conf_update_rule(dt_lib_collect_rule_t *rule)
{
  const gchar *text = gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text));
  const dt_lib_collect_mode_t mode = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(rule->w_operator)));

  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", rule->num);
  dt_conf_set_string(confname, text);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", rule->num);
  dt_conf_set_int(confname, rule->prop);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", rule->num);
  dt_conf_set_int(confname, mode);
}

static GtkTreeModel *_create_filtered_model(GtkTreeModel *model, dt_lib_collect_rule_t *rule)
{
  GtkTreeModel *filter = NULL;
  GtkTreePath *path = NULL;

  if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
  {
    // we search a common path to all the folders
    // we'll use it as root
    GtkTreeIter child, iter;
    int level = 0;

    while(gtk_tree_model_iter_n_children(model, level > 0 ? &iter : NULL) > 0)
    {
      if(level > 0)
      {
        sqlite3_stmt *stmt = NULL;
        gchar *pth = NULL;
        int id = -1;
        // Check if this path also matches a filmroll
        gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &pth, -1);
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                    "SELECT id FROM main.film_rolls WHERE folder LIKE ?1", -1, &stmt, NULL);
        DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, pth, -1, SQLITE_TRANSIENT);
        if(sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        g_free(pth);

        if(id != -1)
        {
          // we go back to the parent, in order to show this folder
          if(!gtk_tree_model_iter_parent(model, &child, &iter)) level = 0;
          iter = child;
          break;
        }
      }
      if(gtk_tree_model_iter_n_children(model, level > 0 ? &iter : NULL) != 1) break;

      gtk_tree_model_iter_children(model, &child, level > 0 ? &iter : NULL);
      iter = child;
      level++;
    }

    if(level > 0)
    {
      if(gtk_tree_model_iter_n_children(model, &iter) == 0 &&
         gtk_tree_model_iter_parent(model, &child, &iter))
      {
        path = gtk_tree_model_get_path(model, &child);
      }
      else
      {
        path = gtk_tree_model_get_path(model, &iter);
      }
    }
  }

  // Create filter and set virtual root
  filter = gtk_tree_model_filter_new(model, path);
  gtk_tree_path_free(path);

  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(filter), DT_LIB_COLLECT_COL_VISIBLE);

  return filter;
}

static int string_array_length(char **list)
{
  int length = 0;
  for(; *list; list++) length++;
  return length;
}

// returns a NULL terminated array of path components
static char **split_path(const char *path)
{
  if(!path || !*path) return NULL;

  char **result;
  char **tokens = g_strsplit(path, G_DIR_SEPARATOR_S, -1);

#ifdef _WIN32

  if(! (g_ascii_isalpha(tokens[0][0]) && tokens[0][strlen(tokens[0]) - 1] == ':') )
  {
    g_strfreev(tokens);
    tokens = NULL;
  }

  result = tokens;

#else

  // there are size + 1 elements in tokens -- the final NULL! we want to ignore it.
  const unsigned int size = g_strv_length(tokens);

  result = malloc(sizeof(char *) * size);
  for(unsigned int i = 0; i < size; i++)
    result[i] = tokens[i + 1];

  g_free(tokens[0]);
  g_free(tokens);

#endif

  return result;
}

typedef struct name_key_tuple_t
{
  char *name, *collate_key;
  int count, status;
} name_key_tuple_t;

static void free_tuple(gpointer data)
{
  name_key_tuple_t *tuple = (name_key_tuple_t *)data;
  g_free(tuple->name);
  g_free(tuple->collate_key);
  free(tuple);
}

static gint sort_folder_tag(gconstpointer a, gconstpointer b)
{
  const name_key_tuple_t *tuple_a = (const name_key_tuple_t *)a;
  const name_key_tuple_t *tuple_b = (const name_key_tuple_t *)b;

  return g_strcmp0(tuple_a->collate_key, tuple_b->collate_key);
}

// create a key such that  _("not tagged") & "darktable|" are coming first,
// and the rest is ordered such that sub tags are coming directly behind their parent
static char *tag_collate_key(char *tag)
{
  const size_t len = strlen(tag);
  char *result = g_malloc(len + 2);
  if(!g_strcmp0(tag, _("not tagged")))
    *result = '\1';
  else if(g_str_has_prefix(tag, "darktable|"))
    *result = '\2';
  else
    *result = '\3';
  memcpy(result + 1, tag, len + 1);
  for(char *iter = result + 1; *iter; iter++)
    if(*iter == '|') *iter = '\1';
  return result;
}


void tree_count_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter,
                     gpointer data)
{
  gchar *name;
  guint count;

  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_TEXT, &name, DT_LIB_COLLECT_COL_COUNT, &count, -1);
  if (!count)
  {
    g_object_set(renderer, "text", name, NULL);
  }
  else
  {
    gchar *coltext = g_strdup_printf("%s (%d)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    g_free(coltext);
  }

  g_free(name);
}

static gint _listview_sort_model_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, dt_lib_module_t *self)
{
  gint ia, ib;
  gtk_tree_model_get(model, a, DT_LIB_COLLECT_COL_INDEX, &ia, -1);
  gtk_tree_model_get(model, b, DT_LIB_COLLECT_COL_INDEX, &ib, -1);
  return ib - ia;
}

static const char *UNCATEGORIZED_TAG = N_("uncategorized");
static void _widget_rule_tree_view(dt_lib_collect_rule_t *rule)
{
  // update related list
  // dt_lib_collect_t *d = get_collect(rule);
  char *format_separator = "";

  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_FOLDERS:
      format_separator = "%s" G_DIR_SEPARATOR_S;
      break;
    case DT_COLLECTION_PROP_TAG:
    case DT_COLLECTION_PROP_GEOTAGGING:
      format_separator = "%s|";
      break;
    case DT_COLLECTION_PROP_DAY:
    case DT_COLLECTION_PROP_TIME:
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
      format_separator = "%s:";
      break;
    default:
      break;
  }

  _conf_update_rule(rule);

  GtkTreeModel *model;

  if(rule->w_view)
  {
    model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(rule->filter));
  }
  else
  {
    // tree creation/recreation
    sqlite3_stmt *stmt;
    GtkTreeIter uncategorized = { 0 };
    GtkTreeIter temp;

    rule->w_view = GTK_TREE_VIEW(gtk_tree_view_new());
    gtk_container_add(GTK_CONTAINER(rule->w_view_sw), GTK_WIDGET(rule->w_view));
    gtk_tree_view_set_headers_visible(rule->w_view, FALSE);
    g_signal_connect(G_OBJECT(rule->w_view), "button-press-event", G_CALLBACK(_event_view_onButtonPressed), rule);
    g_signal_connect(G_OBJECT(rule->w_view), "popup-menu", G_CALLBACK(_event_view_onPopupMenu), rule);

    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(rule->w_view, col);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, renderer, tree_count_show, NULL, NULL);
    g_object_set(renderer, "strikethrough", TRUE, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
    gtk_tree_view_column_add_attribute(col, renderer, "strikethrough-set", DT_LIB_COLLECT_COL_UNREACHABLE);

    model = GTK_TREE_MODEL(gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_UINT,
                                              G_TYPE_UINT));
    // since we'll be inserting elements in the same order that we want them displayed, there is no need for the
    // overhead of having the tree view sort itself
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                         GTK_SORT_ASCENDING);
    rule->filter = gtk_tree_model_filter_new(model, NULL);
    gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(rule->filter), DT_LIB_COLLECT_COL_VISIBLE);

    /* query construction */
    gchar *where_ext = dt_collection_get_extended_where(darktable.collection, rule->num);
    gchar *query = 0;
    switch(rule->prop)
    {
      case DT_COLLECTION_PROP_FOLDERS:
        query = g_strdup_printf("SELECT folder, film_rolls_id, COUNT(*) AS count, status"
                                " FROM main.images AS mi"
                                " JOIN (SELECT fr.id AS film_rolls_id, folder, status"
                                "       FROM main.film_rolls AS fr"
                                "       JOIN memory.film_folder AS ff"
                                "       ON fr.id = ff.id)"
                                "   ON film_id = film_rolls_id "
                                " WHERE %s"
                                " GROUP BY folder, film_rolls_id", where_ext);
        break;
      case DT_COLLECTION_PROP_TAG:
      {
        const gboolean is_insensitive =
          dt_conf_is_equal("plugins/lighttable/tagging/case_sensitivity", "insensitive");

        if(is_insensitive)
          query = g_strdup_printf("SELECT name, 1 AS tagid, SUM(count) AS count"
                                  " FROM (SELECT tagid, COUNT(*) as count"
                                  "   FROM main.images AS mi"
                                  "   JOIN main.tagged_images"
                                  "     ON id = imgid "
                                  "   WHERE %s"
                                  "   GROUP BY tagid)"
                                  " JOIN (SELECT lower(name) AS name, id AS tag_id FROM data.tags)"
                                  "   ON tagid = tag_id"
                                  "   GROUP BY name", where_ext);
        else
          query = g_strdup_printf("SELECT name, tagid, count"
                                  " FROM (SELECT tagid, COUNT(*) AS count"
                                  "  FROM main.images AS mi"
                                  "  JOIN main.tagged_images"
                                  "     ON id = imgid "
                                  "  WHERE %s"
                                  "  GROUP BY tagid)"
                                  " JOIN (SELECT name, id AS tag_id FROM data.tags)"
                                  "   ON tagid = tag_id"
                                  , where_ext);

        query = dt_util_dstrcat(query, " UNION ALL "
                                       "SELECT '%s' AS name, 0 as id, COUNT(*) AS count "
                                       "FROM main.images AS mi "
                                       "WHERE mi.id NOT IN"
                                       "  (SELECT DISTINCT imgid FROM main.tagged_images AS ti"
                                       "   WHERE ti.tagid NOT IN memory.darktable_tags)",
                                _("not tagged"));
      }
      break;
      case DT_COLLECTION_PROP_GEOTAGGING:
        query = g_strdup_printf("SELECT "
                                " CASE WHEN mi.longitude IS NULL"
                                "           OR mi.latitude IS null THEN \'%s\'"
                                "      ELSE CASE WHEN ta.imgid IS NULL THEN \'%s\'"
                                "                ELSE \'%s\' || ta.tagname"
                                "                END"
                                "      END AS name,"
                                " ta.tagid AS tag_id, COUNT(*) AS count"
                                " FROM main.images AS mi"
                                " LEFT JOIN (SELECT imgid, t.id AS tagid, SUBSTR(t.name, %d) AS tagname"
                                "   FROM main.tagged_images AS ti"
                                "   JOIN data.tags AS t"
                                "     ON ti.tagid = t.id"
                                "   JOIN data.locations AS l"
                                "     ON l.tagid = t.id"
                                "   ) AS ta ON ta.imgid = mi.id"
                                " WHERE %s"
                                " GROUP BY name, tag_id",
                                _("not tagged"), _("tagged"), _("tagged"),
                                (int)strlen(dt_map_location_data_tag_root()) + 1, where_ext);
        break;
      case DT_COLLECTION_PROP_DAY:
        query = g_strdup_printf("SELECT SUBSTR(datetime_taken, 1, 10) AS date, 1, COUNT(*) AS count"
                                " FROM main.images AS mi"
                                " WHERE datetime_taken IS NOT NULL AND %s"
                                " GROUP BY date", where_ext);
        break;
      case DT_COLLECTION_PROP_TIME:
        query = g_strdup_printf("SELECT datetime_taken AS date, 1, COUNT(*) AS count"
                                " FROM main.images AS mi"
                                " WHERE datetime_taken IS NOT NULL AND %s"
                                " GROUP BY date", where_ext);
        break;
      case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
      case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
      case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
      case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
        {
          const int local_property = rule->prop;
          char *colname = NULL;

          switch(local_property)
          {
            case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
              colname = "import_timestamp";
              break;
            case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
              colname = "change_timestamp";
              break;
            case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
              colname = "export_timestamp";
              break;
            case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
              colname = "print_timestamp";
              break;
          }
        query = g_strdup_printf("SELECT strftime('%%Y:%%m:%%d %%H:%%M:%%S', %s, 'unixepoch', 'localtime') AS date, 1, COUNT(*) AS count"
                                " FROM main.images AS mi"
                                " WHERE %s <> -1"
                                " AND %s"
                                " GROUP BY date", colname, colname, where_ext);
        break;
        }
        default:
          break;
    }

    g_free(where_ext);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

    char **last_tokens = NULL;
    int last_tokens_length = 0;
    GtkTreeIter last_parent = { 0 };

    // we need to sort the names ourselves and not let sqlite handle this
    // because it knows nothing about path separators.
    GList *sorted_names = NULL;
    guint index = 0;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char* sqlite_name = (const char *)sqlite3_column_text(stmt, 0);
      char *name = sqlite_name == NULL ? g_strdup("") : g_strdup(sqlite_name);
      gchar *collate_key = NULL;

      const int count = sqlite3_column_int(stmt, 2);

      if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
      {
        char *name_folded = g_utf8_casefold(name, -1);
        char *name_folded_slash = g_strconcat(name_folded, G_DIR_SEPARATOR_S, NULL);
        collate_key = g_utf8_collate_key_for_filename(name_folded_slash, -1);
        g_free(name_folded_slash);
        g_free(name_folded);
      }
      else
        collate_key = tag_collate_key(name);

      name_key_tuple_t *tuple = (name_key_tuple_t *)malloc(sizeof(name_key_tuple_t));
      tuple->name = name;
      tuple->collate_key = collate_key;
      tuple->count = count;
      tuple->status = rule->prop == DT_COLLECTION_PROP_FOLDERS ? sqlite3_column_int(stmt, 3) : -1;
      sorted_names = g_list_prepend(sorted_names, tuple);
    }
    sqlite3_finalize(stmt);
    g_free(query);
    // this order should not be altered. the right feeding of the tree relies on it.
    sorted_names = g_list_sort(sorted_names, sort_folder_tag);
    const gboolean sort_descend = dt_conf_get_bool("plugins/collect/descending");
    if (!sort_descend)
      sorted_names = g_list_reverse(sorted_names);

    gboolean no_uncategorized = (rule->prop == DT_COLLECTION_PROP_TAG)
                                    ? dt_conf_get_bool("plugins/lighttable/tagging/no_uncategorized")
                                    : TRUE;

    for(GList *names = sorted_names; names; names = g_list_next(names))
    {
      name_key_tuple_t *tuple = (name_key_tuple_t *)names->data;
      char *name = tuple->name;
      const int count = tuple->count;
      const int status = tuple->status;
      if(name == NULL) continue; // safeguard against degenerated db entries

      // this is just for tags
      gboolean uncategorized_found = FALSE;
      if(!no_uncategorized && strchr(name, '|') == NULL)
      {
        char *next_name = g_strdup(names->next ? ((name_key_tuple_t *)names->next->data)->name : "");
        if(strlen(next_name) >= strlen(name) + 1 && next_name[strlen(name)] == '|')
          next_name[strlen(name)] = '\0';

        if(g_strcmp0(next_name, name) && g_strcmp0(name, _("not tagged")))
        {
          /* add uncategorized root iter if not exists */
          if(!uncategorized.stamp)
          {
            gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &uncategorized, NULL, -1,
                                              DT_LIB_COLLECT_COL_TEXT, _(UNCATEGORIZED_TAG),
                                              DT_LIB_COLLECT_COL_PATH, "", DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                                              DT_LIB_COLLECT_COL_INDEX, index, -1);
            index++;
          }

          /* adding an uncategorized tag */
          gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &temp, &uncategorized, 0,
                                            DT_LIB_COLLECT_COL_TEXT, name,
                                            DT_LIB_COLLECT_COL_PATH, name, DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                                            DT_LIB_COLLECT_COL_COUNT, count, DT_LIB_COLLECT_COL_INDEX, index, -1);
          uncategorized_found = TRUE;
          index++;
        }
        g_free(next_name);
      }

      if(!uncategorized_found)
      {
        char **tokens;
        if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
          tokens = split_path(name);
        else if(rule->prop == DT_COLLECTION_PROP_DAY)
          tokens = g_strsplit(name, ":", -1);
        else if(_rule_is_time_property(rule->prop))
          tokens = g_strsplit_set(name, ": ", 4);
        else
          tokens = g_strsplit(name, "|", -1);

        if(tokens != NULL)
        {
          // find the number of common parts at the beginning of tokens and last_tokens
          GtkTreeIter parent = last_parent;
          const int tokens_length = string_array_length(tokens);
          int common_length = 0;
          if(last_tokens)
          {
            while(tokens[common_length]
                  && last_tokens[common_length]
                  && !g_strcmp0(tokens[common_length], last_tokens[common_length]))
            {
              common_length++;
            }

            // point parent iter to where the entries should be added
            for(int i = common_length; i < last_tokens_length; i++)
            {
              gtk_tree_model_iter_parent(model, &parent, &last_parent);
              last_parent = parent;
            }
          }

          // insert everything from tokens past the common part

          char *pth = NULL;
#ifndef _WIN32
          if(rule->prop == DT_COLLECTION_PROP_FOLDERS) pth = g_strdup("/");
#endif
          for(int i = 0; i < common_length; i++)
            pth = dt_util_dstrcat(pth, format_separator, tokens[i]);

          for(char **token = &tokens[common_length]; *token; token++)
          {
            GtkTreeIter iter;

            pth = dt_util_dstrcat(pth, format_separator, *token);
            if(_rule_is_time_property(rule->prop) && !*(token + 1)) pth[10] = ' ';

            gchar *pth2 = g_strdup(pth);
            pth2[strlen(pth2) - 1] = '\0';
            gtk_tree_store_insert_with_values(GTK_TREE_STORE(model), &iter, common_length > 0 ? &parent : NULL, 0,
                                              DT_LIB_COLLECT_COL_TEXT, *token,
                                              DT_LIB_COLLECT_COL_PATH, pth2, DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                                              DT_LIB_COLLECT_COL_COUNT, (*(token + 1)?0:count),
                                              DT_LIB_COLLECT_COL_INDEX, index,
                                              DT_LIB_COLLECT_COL_UNREACHABLE, (*(token + 1) ? 0 : !status), -1);
            index++;
            // also add the item count to parents
            if((rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop)) && !*(token + 1))
            {
              guint parentcount;
              GtkTreeIter parent2, child = iter;

              while(gtk_tree_model_iter_parent(model, &parent2, &child))
              {
                gtk_tree_model_get(model, &parent2, DT_LIB_COLLECT_COL_COUNT, &parentcount, -1);
                gtk_tree_store_set(GTK_TREE_STORE(model), &parent2, DT_LIB_COLLECT_COL_COUNT, count + parentcount, -1);
                child = parent2;
              }
            }

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
    }
    g_list_free_full(sorted_names, free_tuple);

    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(rule->w_view), DT_LIB_COLLECT_COL_TOOLTIP);

    rule->filter = _create_filtered_model(model, rule);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(rule->w_view));
    if(rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop))
    {
      gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    }
    else
    {
      gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    }

    gtk_tree_view_set_model(GTK_TREE_VIEW(rule->w_view), rule->filter);
    gtk_widget_set_no_show_all(GTK_WIDGET(rule->w_view), FALSE);
    gtk_widget_show_all(GTK_WIDGET(rule->w_view));

    g_object_unref(model);
    g_strfreev(last_tokens);
  }

  // we update tree expansion and selection
  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(rule->w_view));
  gtk_tree_view_collapse_all(rule->w_view);

  if(rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop))
  {
    // test selection range [xxx;xxx]
    GRegex *regex;
    GMatchInfo *match_info;
    int match_count;

    regex = g_regex_new("^\\s*\\[\\s*(.*)\\s*;\\s*(.*)\\s*\\]\\s*$", 0, 0, NULL);
    g_regex_match_full(regex, gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), -1, 0, 0, &match_info, NULL);
    match_count = g_match_info_get_match_count(match_info);

    if(match_count == 3)
    {
      _range_t *range = (_range_t *)calloc(1, sizeof(_range_t));
      /* inversed as dates are in reverse order */
      range->start = g_match_info_fetch(match_info, 2);
      range->stop = g_match_info_fetch(match_info, 1);

      gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)range_select, range);
      if(range->path1 && range->path2)
      {
        gtk_tree_selection_select_range(gtk_tree_view_get_selection(rule->w_view), range->path1, range->path2);
      }
      g_free(range->start);
      g_free(range->stop);
      gtk_tree_path_free(range->path1);
      gtk_tree_path_free(range->path2);
      free(range);
    }
    else
      gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)tree_expand, rule);

    g_match_info_free(match_info);
    g_regex_unref(regex);
  }
  else
    gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)tree_expand, rule);
}

static _range_t *_rule_get_range(dt_lib_collect_rule_t *rule)
{
  _range_t *range = NULL;
  // test selection range [xxx;xxx]
  GRegex *regex;
  GMatchInfo *match_info;

  regex = g_regex_new("^\\s*\\[\\s*(.*)\\s*;\\s*(.*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    range = (_range_t *)calloc(1, sizeof(_range_t));
    range->start = g_match_info_fetch(match_info, 1);
    range->stop = g_match_info_fetch(match_info, 2);
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // test compare > >= < <= =
  regex = g_regex_new("^\\s*(>=|<=|>|<|=)\\s*(.*)\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    range = (_range_t *)calloc(1, sizeof(_range_t));
    gchar *comp = g_match_info_fetch(match_info, 1);
    if(!strcmp(comp, ">"))
    {
      range->strict = TRUE;
      range->start = g_match_info_fetch(match_info, 2);
    }
    else if(!strcmp(comp, "<"))
    {
      range->strict = TRUE;
      range->stop = g_match_info_fetch(match_info, 2);
    }
    else if(!strcmp(comp, "<="))
    {
      range->stop = g_match_info_fetch(match_info, 2);
    }
    else if(!strcmp(comp, ">="))
    {
      range->start = g_match_info_fetch(match_info, 2);
    }
    else if(!strcmp(comp, "="))
    {
      range->stop = g_match_info_fetch(match_info, 2);
      range->start = g_match_info_fetch(match_info, 2);
    }
    g_free(comp);
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  return range;
}

static void _widget_rule_list_view(dt_lib_collect_rule_t *rule)
{
  // update related list
  dt_lib_collect_t *d = get_collect(rule);

  _conf_update_rule(rule);

  GtkTreeModel *model;
  if(rule->w_view)
  {
    model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(rule->filter));
  }
  else
  {
    sqlite3_stmt *stmt;
    GtkTreeIter iter;

    // we recreate the list view from scratch
    rule->w_view = GTK_TREE_VIEW(gtk_tree_view_new());
    gtk_container_add(GTK_CONTAINER(rule->w_view_sw), GTK_WIDGET(rule->w_view));
    gtk_tree_view_set_headers_visible(rule->w_view, FALSE);
    g_signal_connect(G_OBJECT(rule->w_view), "button-press-event", G_CALLBACK(_event_view_onButtonPressed), rule);
    g_signal_connect(G_OBJECT(rule->w_view), "popup-menu", G_CALLBACK(_event_view_onPopupMenu), rule);

    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_append_column(rule->w_view, col);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, renderer, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, renderer, tree_count_show, NULL, NULL);
    g_object_set(renderer, "strikethrough", TRUE, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
    gtk_tree_view_column_add_attribute(col, renderer, "strikethrough-set", DT_LIB_COLLECT_COL_UNREACHABLE);

    model = GTK_TREE_MODEL(gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING,
                                              G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_UINT,
                                              G_TYPE_UINT));
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(model), DT_LIB_COLLECT_COL_INDEX,
                                    (GtkTreeIterCompareFunc)_listview_sort_model_func, d, NULL);
    rule->filter = gtk_tree_model_filter_new(model, NULL);
    gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(rule->filter), DT_LIB_COLLECT_COL_VISIBLE);

    gchar *where_ext = dt_collection_get_extended_where(darktable.collection, rule->num);
    char query[1024] = { 0 };

    switch(rule->prop)
    {
      case DT_COLLECTION_PROP_CAMERA:; // camera
        int index = 0;
        gchar *makermodel_query = g_strdup_printf("SELECT maker, model, COUNT(*) AS count "
                                                  "FROM main.images AS mi WHERE %s GROUP BY maker, model", where_ext);

        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                makermodel_query,
                                -1, &stmt, NULL);

        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          const char *exif_maker = (char *)sqlite3_column_text(stmt, 0);
          const char *exif_model = (char *)sqlite3_column_text(stmt, 1);
          const int count = sqlite3_column_int(stmt, 2);

          gchar *value =  dt_collection_get_makermodel(exif_maker, exif_model);

          gtk_list_store_append(GTK_LIST_STORE(model), &iter);
          gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_COLLECT_COL_TEXT, value,
                             DT_LIB_COLLECT_COL_ID, index, DT_LIB_COLLECT_COL_TOOLTIP, value,
                             DT_LIB_COLLECT_COL_PATH, value, DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                             DT_LIB_COLLECT_COL_COUNT, count,
                             -1);

          g_free(value);
          index++;
        }
        g_free(makermodel_query);
        break;

      case DT_COLLECTION_PROP_HISTORY: // History
        // images without history are counted as if they were basic
        g_snprintf(query, sizeof(query),
                   "SELECT CASE"
                   "       WHEN basic_hash == current_hash THEN '%s'"
                   "       WHEN auto_hash == current_hash THEN '%s'"
                   "       WHEN current_hash IS NOT NULL THEN '%s'"
                   "       ELSE '%s'"
                   "     END as altered, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " LEFT JOIN (SELECT DISTINCT imgid, basic_hash, auto_hash, current_hash"
                   "            FROM main.history_hash) ON id = imgid"
                   " WHERE %s"
                   " GROUP BY altered"
                   " ORDER BY altered ASC",
                   _("basic"), _("auto applied"), _("altered"), _("basic"), where_ext);
        break;

      case DT_COLLECTION_PROP_LOCAL_COPY: // local copy, 2 hardcoded alternatives
        g_snprintf(query, sizeof(query),
                   "SELECT CASE "
                   "         WHEN (flags & %d) THEN '%s'"
                   "         ELSE '%s'"
                   "       END as lcp, 1, COUNT(*) AS count"
                   " FROM main.images AS mi "
                   " WHERE %s"
                   " GROUP BY lcp ORDER BY lcp ASC",
                   DT_IMAGE_LOCAL_COPY, _("copied locally"),  _("not copied locally"), where_ext);
        break;

      case DT_COLLECTION_PROP_ASPECT_RATIO: // aspect ratio, 3 hardcoded alternatives
        g_snprintf(query, sizeof(query),
                   "SELECT ROUND(aspect_ratio,1), 1, COUNT(*) AS count"
                   " FROM main.images AS mi "
                   " WHERE %s"
                   " GROUP BY ROUND(aspect_ratio,1)", where_ext);
        break;

      case DT_COLLECTION_PROP_COLORLABEL: // colorlabels
        g_snprintf(query, sizeof(query),
                   "SELECT CASE color"
                   "         WHEN 0 THEN '%s'"
                   "         WHEN 1 THEN '%s'"
                   "         WHEN 2 THEN '%s'"
                   "         WHEN 3 THEN '%s'"
                   "         WHEN 4 THEN '%s' "
                   "         ELSE ''"
                   "       END, color, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " JOIN "
                   "   (SELECT imgid AS color_labels_id, color FROM main.color_labels)"
                   " ON id = color_labels_id "
                   " WHERE %s"
                   " GROUP BY color"
                   " ORDER BY color DESC",
                   _("red"), _("yellow"), _("green"), _("blue"), _("purple"), where_ext);
        break;

      case DT_COLLECTION_PROP_LENS: // lens
        g_snprintf(query, sizeof(query),
                   "SELECT lens, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY lens"
                   " ORDER BY lens", where_ext);
        break;

      case DT_COLLECTION_PROP_FOCAL_LENGTH: // focal length
        g_snprintf(query, sizeof(query),
                   "SELECT CAST(focal_length AS INTEGER) AS focal_length, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY CAST(focal_length AS INTEGER)"
                   " ORDER BY CAST(focal_length AS INTEGER)",
                   where_ext);
        break;

      case DT_COLLECTION_PROP_ISO: // iso
        g_snprintf(query, sizeof(query),
                   "SELECT CAST(iso AS INTEGER) AS iso, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY iso"
                   " ORDER BY iso",
                   where_ext);
        break;

      case DT_COLLECTION_PROP_APERTURE: // aperture
        g_snprintf(query, sizeof(query),
                   "SELECT ROUND(aperture,1) AS aperture, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY aperture"
                   " ORDER BY aperture",
                   where_ext);
        break;

      case DT_COLLECTION_PROP_EXPOSURE: // exposure
        g_snprintf(query, sizeof(query),
                   "SELECT CASE"
                   "         WHEN (exposure < 0.4) THEN '1/' || CAST(1/exposure + 0.9 AS INTEGER) "
                   "         ELSE ROUND(exposure,2) || '\"'"
                   "       END as _exposure, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY _exposure"
                   " ORDER BY exposure",
                  where_ext);
        break;

      case DT_COLLECTION_PROP_FILENAME: // filename
        g_snprintf(query, sizeof(query),
                   "SELECT filename, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY filename"
                   " ORDER BY filename", where_ext);
        break;

      case DT_COLLECTION_PROP_GROUPING: // Grouping, 2 hardcoded alternatives
        g_snprintf(query, sizeof(query),
                   "SELECT CASE"
                   "         WHEN id = group_id THEN '%s'"
                   "         ELSE '%s'"
                   "       END as group_leader, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " WHERE %s"
                   " GROUP BY group_leader"
                   " ORDER BY group_leader ASC",
                   _("group leaders"),  _("group followers"), where_ext);
        break;

      case DT_COLLECTION_PROP_MODULE: // module
        snprintf(query, sizeof(query),
                 "SELECT m.name AS module_name, 1, COUNT(*) AS count"
                 " FROM main.images AS mi"
                 " JOIN (SELECT DISTINCT imgid, operation FROM main.history WHERE enabled = 1) AS h"
                 "  ON h.imgid = mi.id"
                 " JOIN memory.darktable_iop_names AS m"
                 "  ON m.operation = h.operation"
                 " WHERE %s"
                 " GROUP BY module_name"
                 " ORDER BY module_name",
                 where_ext);
        break;

      case DT_COLLECTION_PROP_ORDER: // modules order
        {
          char *orders = NULL;
          for(int i = 0; i < DT_IOP_ORDER_LAST; i++)
          {
            orders = dt_util_dstrcat(orders, "WHEN mo.version = %d THEN '%s' ",
                                     i, _(dt_iop_order_string(i)));
          }
          orders = dt_util_dstrcat(orders, "ELSE '%s' ", _("none"));
          snprintf(query, sizeof(query),
                   "SELECT CASE %s END as ver, 1, COUNT(*) AS count"
                   " FROM main.images AS mi"
                   " LEFT JOIN (SELECT imgid, version FROM main.module_order) mo"
                   "  ON mo.imgid = mi.id"
                   " WHERE %s"
                   " GROUP BY ver"
                   " ORDER BY ver",
                   orders, where_ext);
          g_free(orders);
        }
        break;

      case DT_COLLECTION_PROP_RATING: // image rating
        {
          g_snprintf(query, sizeof(query),
                     "SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating, 1,"
                     " COUNT(*) AS count"
                     " FROM main.images AS mi"
                     " WHERE %s"
                     " GROUP BY rating"
                     " ORDER BY rating", where_ext);
        }
        break;

      default:
        if(rule->prop >= DT_COLLECTION_PROP_METADATA
           && rule->prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
        {
          const int keyid = dt_metadata_get_keyid_by_display_order(rule->prop - DT_COLLECTION_PROP_METADATA);
          const char *name = (gchar *)dt_metadata_get_name(keyid);
          char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
          const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
          g_free(setting);
          if(!hidden)
          {
            snprintf(query, sizeof(query),
                     "SELECT"
                     " CASE WHEN value IS NULL THEN '%s' ELSE value END AS value,"
                     " 1, COUNT(*) AS count,"
                     " CASE WHEN value IS NULL THEN 0 ELSE 1 END AS force_order"
                     " FROM main.images AS mi"
                     " LEFT JOIN (SELECT id AS meta_data_id, value FROM main.meta_data WHERE key = %d)"
                     "  ON id = meta_data_id"
                     " WHERE %s"
                     " GROUP BY value"
                     " ORDER BY force_order, value",
                     _("not defined"), keyid, where_ext);
          }
        }
        else
        // filmroll
        {
          gchar *order_by = NULL;
          const char *filmroll_sort = dt_conf_get_string_const("plugins/collect/filmroll_sort");
          if(strcmp(filmroll_sort, "id") == 0)
            order_by = g_strdup("film_rolls_id DESC");
          else
            if(dt_conf_get_bool("plugins/collect/descending"))
              order_by = g_strdup("folder DESC");
            else
              order_by = g_strdup("folder");

          g_snprintf(query, sizeof(query),
                     "SELECT folder, film_rolls_id, COUNT(*) AS count, status"
                     " FROM main.images AS mi"
                     " JOIN (SELECT fr.id AS film_rolls_id, folder, status"
                     "       FROM main.film_rolls AS fr"
                     "        JOIN memory.film_folder AS ff"
                     "        ON ff.id = fr.id)"
                     "   ON film_id = film_rolls_id "
                     " WHERE %s"
                     " GROUP BY folder"
                     " ORDER BY %s", where_ext, order_by);

          g_free(order_by);
        }
        break;
    }

    g_free(where_ext);

    if(strlen(query) > 0)
    {
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const char *folder = (const char *)sqlite3_column_text(stmt, 0);
        if(folder == NULL) continue; // safeguard against degenerated db entries

        gtk_list_store_append(GTK_LIST_STORE(model), &iter);
        int status = 0;
        if(rule->prop == DT_COLLECTION_PROP_FILMROLL)
        {
          folder = dt_image_film_roll_name(folder);
          status = !sqlite3_column_int(stmt, 3);
        }
        const gchar *value = (gchar *)sqlite3_column_text(stmt, 0);
        const int count = sqlite3_column_int(stmt, 2);

        // replace invalid utf8 characters if any
        gchar *text = g_strdup(value);
        gchar *ptr = text;
        while(!g_utf8_validate(ptr, -1, (const gchar **)&ptr)) ptr[0] = '?';

        gchar *escaped_text = g_markup_escape_text(text, -1);

        gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_COLLECT_COL_TEXT, folder,
                           DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1), DT_LIB_COLLECT_COL_TOOLTIP,
                           escaped_text, DT_LIB_COLLECT_COL_PATH, value, DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                           DT_LIB_COLLECT_COL_COUNT, count, DT_LIB_COLLECT_COL_UNREACHABLE, status,
                           -1);

        g_free(text);
        g_free(escaped_text);
      }
      sqlite3_finalize(stmt);
    }

    gtk_tree_view_set_tooltip_column(rule->w_view, DT_LIB_COLLECT_COL_TOOLTIP);

    rule->filter = _create_filtered_model(model, rule);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(rule->w_view);
    if(_rule_allow_comparison(rule->prop))
    {
      gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    }
    else
    {
      gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    }

    gtk_tree_view_set_model(rule->w_view, rule->filter);
    gtk_widget_set_no_show_all(GTK_WIDGET(rule->w_view), FALSE);
    gtk_widget_show_all(GTK_WIDGET(rule->w_view));

    g_object_unref(model);
  }

  /*// if needed, we restrict the tree to matching entries
  if(rule->typing && (rule->prop == DT_COLLECTION_PROP_CAMERA
                    || rule->prop == DT_COLLECTION_PROP_FILENAME
                    || rule->prop == DT_COLLECTION_PROP_FILMROLL
                    || rule->prop == DT_COLLECTION_PROP_LENS
                    || rule->prop == DT_COLLECTION_PROP_APERTURE
                    || rule->prop == DT_COLLECTION_PROP_FOCAL_LENGTH
                    || rule->prop == DT_COLLECTION_PROP_ISO
                    || rule->prop == DT_COLLECTION_PROP_MODULE
                    || rule->prop == DT_COLLECTION_PROP_ORDER
                    || rule->prop == DT_COLLECTION_PROP_RATING
                    || (rule->prop >= DT_COLLECTION_PROP_METADATA
                        && rule->prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)))
  {
    gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(rule->text)), -1);
    if(g_str_has_suffix(needle, "%")) needle[strlen(needle) - 1] = '\0';
    rule->searchstring = needle;
    gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)list_match_string, dr);
    rule->searchstring = NULL;
    g_free(needle);
  }*/
  // we update list selection
  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(rule->w_view));

  if(_rule_allow_comparison(rule->prop))
  {
    _range_t *range = _rule_get_range(rule);
    if(range)
    {
      gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)range_select, range);
      if(range->path1 && range->path2)
      {
        gtk_tree_selection_select_range(gtk_tree_view_get_selection(rule->w_view), range->path1, range->path2);
      }
      g_free(range->start);
      g_free(range->stop);
      gtk_tree_path_free(range->path1);
      gtk_tree_path_free(range->path2);
      g_free(range);
    }
    else
      gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)list_select, rule);
  }
  else
    gtk_tree_model_foreach(rule->filter, (GtkTreeModelForeachFunc)list_select, rule);
}

static void _widget_rule_view_update(dt_lib_collect_rule_t *rule)
{
  if(_rule_use_treeview(rule->prop))
    _widget_rule_tree_view(rule);
  else
    _widget_rule_list_view(rule);
}

static void _widget_raw_set_tooltip(dt_lib_collect_rule_t *rule)
{
  if(_rule_allow_comparison(rule->prop))
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text, _("use <, <=, >, >=, <>, =, [;] as operators"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_RATING)
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text, _("use <, <=, >, >=, <>, =, [;] as operators\n"
                                                    "star rating: 0-5\n"
                                                    "rejected images: -1"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop))
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text,
                                _("use <, <=, >, >=, <>, =, [;] as operators\n"
                                  "type dates in the form : YYYY:MM:DD HH:MM:SS (only the year is mandatory)"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_FILENAME)
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text,
                                /* xgettext:no-c-format */
                                _("use `%' as wildcard and `,' to separate values"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_TAG)
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text,
                                /* xgettext:no-c-format */
                                _("use `%' as wildcard\n"
                                  /* xgettext:no-c-format */
                                  "click to include hierarchy + sub-hierarchies (suffix `*')\n"
                                  /* xgettext:no-c-format */
                                  "shift+click to include only the current hierarchy (no suffix)\n"
                                  /* xgettext:no-c-format */
                                  "ctrl+click to include only sub-hierarchies (suffix `|%')"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_GEOTAGGING)
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text,
                                /* xgettext:no-c-format */
                                _("use `%' as wildcard\n"
                                  /* xgettext:no-c-format */
                                  "click to include location + sub-locations (suffix `*')\n"
                                  /* xgettext:no-c-format */
                                  "shift+click to include only the current location (no suffix)\n"
                                  /* xgettext:no-c-format */
                                  "ctrl+click to include only sub-locations (suffix `|%')"));
  }
  else if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
  {
    gtk_widget_set_tooltip_text(rule->w_raw_text,
                                /* xgettext:no-c-format */
                                _("use `%' as wildcard\n"
                                  /* xgettext:no-c-format */
                                  "click to include current + sub-folders (suffix `*')\n"
                                  /* xgettext:no-c-format */
                                  "shift+click to include only the current folder (no suffix)\n"
                                  /* xgettext:no-c-format */
                                  "ctrl+click to include only sub-folders (suffix `|%')"));
  }
  else
  {
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(rule->w_raw_text, _("use `%' as wildcard"));
  }

  //set the combobox tooltip as well
  /*gchar *tip = gtk_widget_get_tooltip_text(rule->w_raw_text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(d->combo), tip);
  g_free(tip);*/
}

static void _event_rule_changed(GtkWidget *entry, dt_lib_collect_rule_t *rule)
{
  if(rule->manual_widget_set) return;

  // update the config files
  _conf_update_rule(rule);
  // if the tree/list view is expanded, we update it
  if(gtk_widget_get_visible(rule->w_view_sw)) _widget_rule_view_update(rule);

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static gboolean _event_rule_close(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *rule)
{
  if(rule->manual_widget_set) return TRUE;

  // decrease the nb of active rules
  dt_lib_collect_t *d = get_collect(rule);
  if(d->nb_rules <= 0) return FALSE;
  d->nb_rules--;
  dt_conf_set_int("plugins/lighttable/collect/num_rules", d->nb_rules);

  // move up all still active rules by one.
  for(int i = rule->num; i < MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
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

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  return TRUE;
}

static void _rating_decode(const gchar *txt, int *comp, int *mode)
{
  // --- First, the special cases
  // easy case : select all
  if(!strcmp(txt, ""))
  {
    *mode = DT_COLLECTION_FILTER_ALL;
    return;
  }
  // unstarred only
  if(!strcmp(txt, "0") || !strcmp(txt, "=0"))
  {
    *mode = DT_COLLECTION_FILTER_STAR_NO;
    return;
  }
  // rejected only
  if(!strcmp(txt, "-1") || !strcmp(txt, "=-1"))
  {
    *mode = DT_COLLECTION_FILTER_REJECT;
    return;
  }
  // all except rejected
  if(!strcmp(txt, "<>-1"))
  {
    *mode = DT_COLLECTION_FILTER_NOT_REJECT;
    return;
  }

  // --- Now we need to decode the comparator and the value
  if(g_str_has_prefix(txt, "<"))
    *comp = DT_COLLECTION_RATING_COMP_LT;
  else if(g_str_has_prefix(txt, "<="))
    *comp = DT_COLLECTION_RATING_COMP_LEQ;
  else if(g_str_has_prefix(txt, "="))
    *comp = DT_COLLECTION_RATING_COMP_EQ;
  else if(g_str_has_prefix(txt, ">="))
    *comp = DT_COLLECTION_RATING_COMP_GEQ;
  else if(g_str_has_prefix(txt, ">"))
    *comp = DT_COLLECTION_RATING_COMP_GT;
  else if(g_str_has_prefix(txt, "!="))
    *comp = DT_COLLECTION_RATING_COMP_NE;
  else
  {
    // can't read the format...
    return;
  }

  const gchar *txt2 = (*comp % 2) ? txt + 2 : txt + 1;

  const int val = atoi(txt2);
  if(val > 0 && val < 6) *mode = val + 1;
}

static void _rating_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_rating_t *rate = (_widgets_rating_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  const int mode = dt_bauhaus_combobox_get(rate->combo);
  const int comp = dt_bauhaus_combobox_get(rate->comparator);

  gchar *txt = dt_util_dstrcat(NULL, "%s", "");
  if(mode >= DT_COLLECTION_FILTER_STAR_1 && mode <= DT_COLLECTION_FILTER_STAR_5)
  {
    // for the stars, comparator is needed
    txt = dt_util_dstrcat(txt, "%s%d", dt_collection_comparator_name(comp), mode - 1);
  }
  else
  {
    // direct content
    if(mode == DT_COLLECTION_FILTER_STAR_NO)
      txt = dt_util_dstrcat(txt, "0");
    else if(mode == DT_COLLECTION_FILTER_REJECT)
      txt = dt_util_dstrcat(txt, "-1");
    else if(mode == DT_COLLECTION_FILTER_NOT_REJECT)
      txt = dt_util_dstrcat(txt, "<>-1");
  }

  gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), txt);
  gtk_widget_activate(rule->w_raw_text);
  g_free(txt);

  // we also update the visibility of the comparator widget
  gtk_widget_set_visible(rate->comparator,
                         (mode >= DT_COLLECTION_FILTER_STAR_1 && mode <= DT_COLLECTION_FILTER_STAR_5));
}

static gboolean _rating_update(dt_lib_collect_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int comp = DT_COLLECTION_RATING_COMP_GEQ;
  int mode = -1;
  _rating_decode(gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), &comp, &mode);

  // if we don't manage to decode, we don't refresh and return false
  if(mode < 0) return FALSE;

  rule->manual_widget_set++;
  _widgets_rating_t *rate = (_widgets_rating_t *)rule->w_specific;
  dt_bauhaus_combobox_set(rate->combo, mode);
  dt_bauhaus_combobox_set(rate->comparator, comp);
  gtk_widget_set_visible(rate->comparator,
                         (mode >= DT_COLLECTION_FILTER_STAR_1 && mode <= DT_COLLECTION_FILTER_STAR_5));
  rule->manual_widget_set--;
  return TRUE;
}

static void _rating_widget_init(dt_lib_collect_rule_t *rule, const dt_collection_properties_t prop,
                                const gchar *text, dt_lib_module_t *self)
{
  _widgets_rating_t *rate = (_widgets_rating_t *)g_malloc0(sizeof(_widgets_rating_t));

  int comp = DT_COLLECTION_RATING_COMP_GEQ;
  int mode = DT_COLLECTION_FILTER_ALL;
  _rating_decode(text, &comp, &mode);

  rule->w_special_box = gtk_overlay_new();

  DT_BAUHAUS_COMBOBOX_NEW_FULL(rate->comparator, self, NULL, N_("comparator"), _("which images should be shown"),
                               comp, _rating_changed, rule,
                               "<",  // DT_COLLECTION_RATING_COMP_LT = 0,
                               "≤",  // DT_COLLECTION_RATING_COMP_LEQ,
                               "=",  // DT_COLLECTION_RATING_COMP_EQ,
                               "≥",  // DT_COLLECTION_RATING_COMP_GEQ,
                               ">",  // DT_COLLECTION_RATING_COMP_GT,
                               "≠"); // DT_COLLECTION_RATING_COMP_NE,
  dt_bauhaus_widget_set_label(rate->comparator, NULL, NULL);
  gtk_widget_set_no_show_all(rate->comparator, TRUE);
  // we also update the visibility of the comparator widget
  gtk_widget_set_visible(rate->comparator, (mode > 1 && mode < 7));
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous(GTK_BOX(spacer), TRUE);
  gtk_box_pack_start(GTK_BOX(spacer), rate->comparator, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(spacer), gtk_grid_new(), FALSE, FALSE, 0);
  gtk_overlay_add_overlay(GTK_OVERLAY(rule->w_special_box), spacer);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(rule->w_special_box), spacer, TRUE);

  /* create the filter combobox */
  DT_BAUHAUS_COMBOBOX_NEW_FULL(rate->combo, self, NULL, N_("view"), _("which images should be shown"), mode,
                               _rating_changed, rule, N_("all"), N_("unstarred only"), "★", "★ ★", "★ ★ ★",
                               "★ ★ ★ ★", "★ ★ ★ ★ ★", N_("rejected only"), N_("all except rejected"));
  gtk_container_add(GTK_CONTAINER(rule->w_special_box), rate->combo);

  gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);
  rule->w_specific = rate;
}

typedef enum _ratio_mode_t
{
  RATIO_INVALID = -1,
  RATIO_ALL = 0,
  RATIO_LANDSCAPE,
  RATIO_PORTRAIT,
  RATIO_SQUARE,
  RATIO_PANO,
  RATIO_LEQ,
  RATIO_EQ,
  RATIO_GEQ,
  RATIO_NE
} _ratio_mode_t;

static void _ratio_decode(const gchar *txt, int *mode, float *value)
{
  // --- First, the special cases
  if(!strcmp(txt, ""))
  {
    *mode = RATIO_ALL;
    return;
  }
  if(!strcmp(txt, ">1") || !strcmp(txt, ">1.0"))
  {
    *mode = RATIO_LANDSCAPE;
    return;
  }
  if(!strcmp(txt, "<1") || !strcmp(txt, "<1.0"))
  {
    *mode = RATIO_PORTRAIT;
    return;
  }
  if(!strcmp(txt, "=1") || !strcmp(txt, "=1.0") || !strcmp(txt, "1") || !strcmp(txt, "1.0"))
  {
    *mode = RATIO_SQUARE;
    return;
  }
  if(!strcmp(txt, ">2") || !strcmp(txt, ">2.0"))
  {
    *mode = RATIO_PANO;
    return;
  }

  // --- Now we need to decode the comparator and the value
  if(g_str_has_prefix(txt, "<="))
    *mode = RATIO_LEQ;
  else if(g_str_has_prefix(txt, "="))
    *mode = RATIO_EQ;
  else if(g_str_has_prefix(txt, ">="))
    *mode = RATIO_GEQ;
  else if(g_str_has_prefix(txt, "!="))
    *mode = RATIO_NE;
  else
  {
    // can't read the format...
    return;
  }

  const gchar *txt2 = (*mode == 6) ? txt + 1 : txt + 2;

  *value = atof(txt2);
}

static void _ratio_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)user_data;
  if(rule->manual_widget_set) return;
  if(!rule->w_specific) return;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;

  // we recreate the right raw text and put it in the raw entry
  const int mode = dt_bauhaus_combobox_get(ratio->mode);
  const gchar *value = gtk_entry_get_text(GTK_ENTRY(ratio->value));

  gchar *txt = dt_util_dstrcat(NULL, "%s", "");
  switch(mode)
  {
    case RATIO_LANDSCAPE:
      txt = dt_util_dstrcat(txt, ">1");
      break;
    case RATIO_PORTRAIT:
      txt = dt_util_dstrcat(txt, "<1");
      break;
    case RATIO_SQUARE:
      txt = dt_util_dstrcat(txt, "=1");
      break;
    case RATIO_PANO:
      txt = dt_util_dstrcat(txt, ">2");
      break;
    case RATIO_LEQ:
      txt = dt_util_dstrcat(txt, "<=%s", value);
      break;
    case RATIO_EQ:
      txt = dt_util_dstrcat(txt, "=%s", value);
      break;
    case RATIO_GEQ:
      txt = dt_util_dstrcat(txt, ">=%s", value);
      break;
    case RATIO_NE:
      txt = dt_util_dstrcat(txt, "!=%s", value);
      break;
  }

  gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), txt);
  gtk_widget_activate(rule->w_raw_text);
  g_free(txt);

  // we also update the sensitivity of the numeric widget
  gtk_widget_set_sensitive(ratio->value, (mode >= RATIO_LEQ));
}

static gboolean _ratio_update(dt_lib_collect_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  int mode = RATIO_INVALID;
  float value = 1.0;
  _ratio_decode(gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)), &mode, &value);

  // if we don't manage to decode, we don't refresh and return false
  if(mode == RATIO_INVALID) return FALSE;

  rule->manual_widget_set++;
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)rule->w_specific;
  dt_bauhaus_combobox_set(ratio->mode, mode);
  // get the string of the value. Note that we need to temprorarily switch locale to C in order to ensure the point
  // as decimal separator
  char valstr[10] = { 0 };
  gchar *loc = setlocale(LC_NUMERIC, NULL);
  setlocale(LC_NUMERIC, "C");
  snprintf(valstr, sizeof(valstr), "%.2f", value);
  setlocale(LC_NUMERIC, loc);
  gtk_entry_set_text(GTK_ENTRY(ratio->value), valstr);
  gtk_widget_set_sensitive(ratio->value, (mode >= RATIO_LEQ));
  rule->manual_widget_set--;
  return TRUE;
}

static void _ratio_widget_init(dt_lib_collect_rule_t *rule, const dt_collection_properties_t prop,
                               const gchar *text, dt_lib_module_t *self)
{
  _widgets_aspect_ratio_t *ratio = (_widgets_aspect_ratio_t *)g_malloc0(sizeof(_widgets_aspect_ratio_t));

  int mode = RATIO_GEQ;
  float value = 1.0;
  _ratio_decode(text, &mode, &value);
  char valstr[10] = { 0 };
  snprintf(valstr, sizeof(valstr), "%f", value);

  rule->w_special_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  DT_BAUHAUS_COMBOBOX_NEW_FULL(ratio->mode, self, NULL, N_("ratio"), _("ratio to show"),
                               mode, _ratio_changed, rule,
                               N_("all"),
                               N_("landscape"),
                               N_("portrait"),
                               N_("square"),
                               N_("panoramic"),
                               "≤",
                               "=",
                               "≥",
                               "≠");
  dt_bauhaus_widget_set_label(ratio->mode, NULL, NULL);
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), ratio->mode, TRUE, TRUE, 0);

  ratio->value = gtk_entry_new();
  gtk_widget_set_tooltip_text(ratio->value, _("ratio value.\n"
                                              ">1 means landscaoe. <1 means portrait"));
  gtk_entry_set_width_chars(GTK_ENTRY(ratio->value), 5);
  gtk_entry_set_text(GTK_ENTRY(ratio->value), valstr);
  gtk_widget_set_sensitive(ratio->value, (mode >= RATIO_LEQ));
  gtk_box_pack_start(GTK_BOX(rule->w_special_box), ratio->value, FALSE, TRUE, 0);
  g_signal_connect(G_OBJECT(ratio->value), "activate", G_CALLBACK(_ratio_changed), rule);

  gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_special_box, TRUE, TRUE, 0);
  rule->w_specific = ratio;
}

static gboolean _widget_update(dt_lib_collect_rule_t *rule)
{
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      return _rating_update(rule);
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      return _ratio_update(rule);
    default:
      // no specific widgets
      return FALSE;
  }
  return FALSE;
}

static void _event_rule_expand(GtkWidget *widget, dt_lib_collect_rule_t *rule)
{
  if(rule->manual_widget_set) return;

  const gboolean expanded = gtk_widget_get_visible(rule->w_view_sw);

  if(expanded)
  {
    // hide the scrollwindow
    gtk_widget_set_visible(rule->w_view_sw, FALSE);
  }
  else
  {
    // recreate the list/tree view
    _widget_rule_view_update(rule);

    // show the scrollwindow
    gtk_widget_set_visible(rule->w_view_sw, TRUE);
  }

  // store the expanded state for this type of rule
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/expand_%d", rule->prop);
  dt_conf_set_bool(confname, !expanded);

  return;
}

static gboolean _event_completion_match_selected(GtkEntryCompletion *self, GtkTreeModel *model, GtkTreeIter *iter,
                                                 dt_lib_collect_rule_t *rule)
{
  gchar *text;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  gtk_entry_set_text(GTK_ENTRY(gtk_entry_completion_get_entry(self)), text);
  g_free(text);
  gtk_widget_activate(gtk_entry_completion_get_entry(self));

  return TRUE;
}

static gboolean _completion_match_func(GtkEntryCompletion *self, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gchar *key2 = g_strdup(key);
  g_strstrip(key2);
  if(!strcmp(key2, "") || !strcmp(key2, "%"))
  {
    g_free(key2);
    return TRUE;
  }
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)user_data;
  gchar *key3 = key2;
  if(_rule_allow_comparison(rule->prop))
  {
    if(g_str_has_prefix(key3, ">") || g_str_has_prefix(key3, "<")) key3 = key3 + 1;
    if(g_str_has_prefix(key3, "=")) key3 = key3 + 1;
  }
  GtkTreeModel *model = gtk_entry_completion_get_model(self);
  gchar *text;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  gchar *txt2 = g_utf8_strdown(text, -1);
  g_free(text);
  gboolean rep = (g_strrstr(txt2, key3) != NULL);
  g_free(txt2);
  g_free(key2);
  return rep;
}

static gboolean _completion_flatten_foreach(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter,
                                            gpointer data)
{
  GtkTreeModel *listmodel = (GtkTreeModel *)data;
  gchar *text;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  GtkTreeIter niter;
  gtk_tree_store_append(GTK_TREE_STORE(listmodel), &niter, NULL);
  gtk_tree_store_set(GTK_TREE_STORE(listmodel), &niter, DT_LIB_COLLECT_COL_TEXT, text, DT_LIB_COLLECT_COL_PATH,
                     text, -1);
  g_free(text);
  return FALSE;
}

static GtkTreeModel *_completion_flatten_tree(dt_lib_collect_rule_t *rule)
{
  GtkTreeModel *model = GTK_TREE_MODEL(gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT,
                                                          G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN,
                                                          G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_UINT));
  GtkTreeModel *treemodel = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(rule->filter));

  gtk_tree_model_foreach(treemodel, _completion_flatten_foreach, model);

  return model;
}

static void _widget_init_completion(dt_lib_collect_rule_t *rule)
{
  // if needed, we remove the old completion
  if(gtk_entry_get_completion(GTK_ENTRY(rule->w_raw_text)) != NULL)
  {
    // TODO
    // g_free(gtk_entry_get_completion(GTK_ENTRY(rule->w_raw_text)));
  }

  GtkEntryCompletion *completion = gtk_entry_completion_new();
  GtkTreeModel *model = NULL;
  // gtkentrycompletion doesn't seems to handle treeview : only root nodes are shown
  // even if the completion match function correctly tests all the nodes...
  // maybe ther's a solution, but I've not found it.
  // As a workaround, let's flatten the treeview...
  if(_rule_use_treeview(rule->prop))
    model = _completion_flatten_tree(rule);
  else
    model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(rule->filter));
  gtk_entry_completion_set_minimum_key_length(completion, 0);
  gtk_entry_completion_set_model(completion, model);
  gtk_entry_completion_set_text_column(completion, DT_LIB_COLLECT_COL_TEXT);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(_event_completion_match_selected), rule);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, rule, NULL);
  gtk_entry_set_completion(GTK_ENTRY(rule->w_raw_text), completion);
}

static gboolean _widget_init_special(dt_lib_collect_rule_t *rule, const gchar *text, dt_lib_module_t *self)
{
  // if the widgets already exits, destroy them
  if(rule->w_special_box)
  {
    gtk_widget_destroy(rule->w_special_box);
    rule->w_special_box = NULL;
    g_free(rule->w_specific);
    rule->w_specific = NULL;
  }

  // initialize the specific entries if any
  gboolean widgets_ok = FALSE;
  switch(rule->prop)
  {
    case DT_COLLECTION_PROP_RATING:
      _rating_widget_init(rule, rule->prop, text, self);
      widgets_ok = _widget_update(rule);
      break;
    case DT_COLLECTION_PROP_ASPECT_RATIO:
      _ratio_widget_init(rule, rule->prop, text, self);
      widgets_ok = _widget_update(rule);
      break;
    default:
      // nothing to do
      break;
  }

  // set the visibility for the eventual special widgets
  if(rule->w_special_box)
  {
    gtk_widget_show_all(rule->w_special_box); // we ensure all the childs widgets are shown by default
    gtk_widget_set_no_show_all(rule->w_special_box, TRUE);

    // special/raw state is stored per rule property
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/raw_%d", rule->prop);
    const gboolean special = (widgets_ok && !dt_conf_get_bool(confname));

    gtk_widget_set_visible(rule->w_special_box, special);
    gtk_widget_set_visible(rule->w_raw_text, !special);
  }
  else
    gtk_widget_set_visible(rule->w_raw_text, TRUE);

  return (rule->w_special_box != NULL);
}

static void _event_rule_change_type(GtkWidget *widget, dt_lib_module_t *self)
{
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  if(mode == rule->prop) return;

  const dt_collection_properties_t oldprop = rule->prop;
  rule->prop = mode;
  gtk_label_set_label(GTK_LABEL(rule->w_prop), dt_collection_name(mode));

  // increase the nb of use of this property
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/nb_use_%d", mode);
  dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);

  // re-init the special widgets
  _widget_init_special(rule, "", self);

  // reset the raw entry
  gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), "");
  gtk_widget_set_visible(rule->w_raw_switch, (rule->w_specific != NULL));
  _widget_raw_set_tooltip(rule);

  // reconstruct the tree/list view
  gtk_widget_destroy(GTK_WIDGET(rule->w_view));
  rule->w_view = NULL;
  _widget_rule_view_update(rule);
  _widget_init_completion(rule);

  // update the config files
  _conf_update_rule(rule);

  // when tag was/become the first rule, we need to handle the order
  if(rule->num == 0)
  {
    gboolean order_request = FALSE;
    uint32_t order = 0;
    if(oldprop != DT_COLLECTION_PROP_TAG && rule->prop == DT_COLLECTION_PROP_TAG)
    {
      // save global order
      const uint32_t sort = dt_collection_get_sort_field(darktable.collection);
      const gboolean descending = dt_collection_get_sort_descending(darktable.collection);
      dt_conf_set_int("plugins/lighttable/collect/order", sort | (descending ? DT_COLLECTION_ORDER_FLAG : 0));
    }
    else if(oldprop == DT_COLLECTION_PROP_TAG && rule->prop != DT_COLLECTION_PROP_TAG)
    {
      // restore global order
      order = dt_conf_get_int("plugins/lighttable/collect/order");
      order_request = TRUE;
      dt_collection_set_tag_id((dt_collection_t *)darktable.collection, 0);
    }
    if(order_request) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE, order);
  }

  // update the query without throwing signal everywhere
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static void _event_append_rule(GtkWidget *widget, dt_lib_module_t *self)
{
  // add new rule
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  const int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "collect_id"));
  char confname[200] = { 0 };

  if(mode >= 0)
  {
    // add an empty rule
    if(d->nb_rules >= MAX_RULES)
    {
      dt_control_log("You can't have more than %d rules", MAX_RULES);
      return;
    }
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", d->nb_rules);
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", d->nb_rules);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", d->nb_rules);
    dt_conf_set_string(confname, "");
    d->nb_rules++;
    dt_conf_set_int("plugins/lighttable/collect/num_rules", d->nb_rules);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/nb_use_%d", mode);
    dt_conf_set_int(confname, dt_conf_get_int(confname) + 1);
  }
  else
  {
    // append a preset
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    const gchar *presetname = gtk_label_get_label(GTK_LABEL(child));
    sqlite3_stmt *stmt;
    gchar *query = g_strdup_printf("SELECT op_params"
                                   " FROM data.presets"
                                   " WHERE operation=?1 AND op_version=?2 AND name=?3");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, presetname, -1, SQLITE_TRANSIENT);
    g_free(query);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      void *op_params = (void *)sqlite3_column_blob(stmt, 0);
      dt_lib_collect_params_t *p = (dt_lib_collect_params_t *)op_params;
      if(d->nb_rules + p->rules > MAX_RULES)
      {
        dt_control_log("You can't have more than %d rules", MAX_RULES);
        sqlite3_finalize(stmt);
        return;
      }
      for(uint32_t i = 0; i < p->rules; i++)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", d->nb_rules);
        dt_conf_set_int(confname, p->rule[i].item);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", d->nb_rules);
        dt_conf_set_int(confname, p->rule[i].mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", d->nb_rules);
        dt_conf_set_string(confname, p->rule[i].string);
        d->nb_rules++;
      }
      dt_conf_set_int("plugins/lighttable/collect/num_rules", d->nb_rules);
    }
    sqlite3_finalize(stmt);
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void _widget_add_rule_popup_item(GtkMenuShell *pop, const gchar *name, const int id, const gboolean title,
                                        dt_lib_collect_rule_t *rule, dt_lib_module_t *self)
{
  GtkWidget *smt = gtk_menu_item_new_with_label(name);
  if(title)
  {
    gtk_widget_set_name(smt, "collect-popup-title");
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
    gtk_label_set_xalign(GTK_LABEL(child), 1.0);
    gtk_widget_set_sensitive(smt, FALSE);
  }
  else
  {
    gtk_widget_set_name(smt, "collect-popup-item");
    g_object_set_data(G_OBJECT(smt), "collect_id", GINT_TO_POINTER(id));
    if(rule)
    {
      g_object_set_data(G_OBJECT(smt), "rule", rule);
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_rule_change_type), self);
    }
    else
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_append_rule), self);
  }
  gtk_menu_shell_append(pop, smt);
}

static int _prop_most_used_sort(gconstpointer a, gconstpointer b)
{
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/nb_use_%d", GPOINTER_TO_INT(a));
  const int ai = dt_conf_get_int(confname);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/nb_use_%d", GPOINTER_TO_INT(b));
  const int bi = dt_conf_get_int(confname);
  return bi - ai;
}

static gboolean _widget_rule_popup(GtkWidget *widget, dt_lib_collect_rule_t *rule, dt_lib_module_t *self)
{
  if(rule && rule->manual_widget_set) return TRUE;

#define ADD_COLLECT_ENTRY(menu, value)                                                                            \
  _widget_add_rule_popup_item(menu, dt_collection_name(value), value, FALSE, rule, self);

  // we show a popup with all the possible rules
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  // most used properties
  if(!rule) _widget_add_rule_popup_item(pop, _("append new rule"), 0, TRUE, rule, self);
  const int nbitems = dt_conf_get_int("plugins/lighttable/collect/most_used_nb");
  GSList *props = NULL;
  for(int i = 0; i < DT_COLLECTION_PROP_LAST; i++) props = g_slist_prepend(props, GINT_TO_POINTER(i));
  props = g_slist_sort(props, _prop_most_used_sort);
  int j = 0;
  for(GSList *l = props; l && j < nbitems; l = g_slist_next(l), j++)
  {
    const dt_collection_properties_t p = GPOINTER_TO_INT(l->data);
    ADD_COLLECT_ENTRY(pop, p);
  }

  // all properties submenu
  GtkWidget *smt = gtk_menu_item_new_with_label(_("other rules properties"));
  GtkMenuShell *spop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(spop), "collect-popup");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(smt), GTK_WIDGET(spop));
  gtk_menu_shell_append(pop, smt);

  // the differents categories
  _widget_add_rule_popup_item(spop, _("files"), 0, TRUE, rule, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FILENAME);

  _widget_add_rule_popup_item(spop, _("metadata"), 0, TRUE, rule, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
    {
      ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_METADATA + i);
    }
  }
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GEOTAGGING);

  _widget_add_rule_popup_item(spop, _("times"), 0, TRUE, rule, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  _widget_add_rule_popup_item(spop, _("capture details"), 0, TRUE, rule, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ISO);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ASPECT_RATIO);

  _widget_add_rule_popup_item(spop, _("darktable"), 0, TRUE, rule, self);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(spop, DT_COLLECTION_PROP_ORDER);

  // show the preset part
  if(!rule)
  {
    // separator
    _widget_add_rule_popup_item(pop, " ", 0, TRUE, rule, self);

    _widget_add_rule_popup_item(pop, _("append preset"), 0, TRUE, rule, self);

    const gboolean hide_default = dt_conf_get_bool("plugins/lighttable/hide_default_presets");
    const gboolean default_first = dt_conf_get_bool("modules/default_presets_first");

    sqlite3_stmt *stmt;
    // order like the pref value
    gchar *query = g_strdup_printf("SELECT name, writeprotect, description"
                                   " FROM data.presets"
                                   " WHERE operation=?1 AND op_version=?2"
                                   " ORDER BY writeprotect %s, LOWER(name), rowid",
                                   default_first ? "DESC" : "ASC");
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, self->plugin_name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, self->version());
    g_free(query);

    // collect all presets for op from db
    int last_wp = -1;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      // default vs built-in stuff
      const gboolean writeprotect = sqlite3_column_int(stmt, 1);
      if(hide_default && writeprotect)
      {
        // skip default module if set to hide them.
        continue;
      }
      if(last_wp == -1)
      {
        last_wp = writeprotect;
      }
      else if(last_wp != writeprotect)
      {
        last_wp = writeprotect;
        _widget_add_rule_popup_item(pop, " ", 0, TRUE, rule, self);
      }

      const char *name = (char *)sqlite3_column_text(stmt, 0);
      _widget_add_rule_popup_item(pop, name, -1, FALSE, rule, self);
    }
    sqlite3_finalize(stmt);
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

static gboolean _event_add_rule(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  _widget_rule_popup(widget, NULL, self);
  return TRUE;
}

static gboolean _event_rule_change_popup(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_collect_rule_t *rule = (dt_lib_collect_rule_t *)g_object_get_data(G_OBJECT(widget), "rule");
  _widget_rule_popup(rule->w_prop, rule, self);
  return TRUE;
}

static gboolean _event_rule_raw_switch(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  if(rule->manual_widget_set) return TRUE;
  const gboolean raw = gtk_widget_get_visible(rule->w_raw_text);
  // if we switch from raw to specific, we need to update the widgets first
  if(raw)
  {
    // validate the raw entry first
    _event_rule_changed(rule->w_raw_text, rule);

    // try to update the specific widgets. If it fails
    // that means we can't show specific widgets for the current raw value
    if(!_widget_update(rule)) return TRUE;
  }

  gtk_widget_set_visible(rule->w_raw_text, !raw);
  gtk_widget_set_visible(rule->w_special_box, raw);

  // store the raw state for this type of rule
  char confname[200] = { 0 };
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/raw_%d", rule->prop);
  dt_conf_set_bool(confname, !raw);

  return TRUE;
}

static void _event_entry_changed(GtkEntry *entry, dt_lib_collect_rule_t *rule)
{
  if(rule->manual_widget_set) return;
  // if the tree/list view is expanded, we update it
  if(gtk_widget_get_visible(rule->w_view_sw)) _widget_rule_view_update(rule);
}

// initialise or update a rule widget. Return if the a new widget has been created
static gboolean _widget_init(dt_lib_collect_rule_t *rule, const dt_collection_properties_t prop, const gchar *text,
                             const dt_lib_collect_mode_t mode, const int pos, dt_lib_module_t *self)
{
  rule->manual_widget_set++;

  const gboolean newmain = (rule->w_main == NULL);
  const gboolean newprop = (prop != rule->prop);
  GtkWidget *hbox = NULL;

  rule->prop = prop;

  if(newmain)
  {
    // the main box
    rule->w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(rule->w_main, "collect-rule-widget");

    // the first line
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), hbox, TRUE, TRUE, 0);
    gtk_widget_set_name(hbox, "collect-header-box");

    // operator type
    rule->w_operator = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(rule->w_operator), _("and not"));
    gtk_widget_set_name(rule->w_operator, "collect-operator");
    gtk_widget_set_tooltip_text(rule->w_operator, _("define how this rule should interact with the previous one"));
    gtk_box_pack_start(GTK_BOX(hbox), rule->w_operator, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(rule->w_operator), "changed", G_CALLBACK(_event_rule_changed), rule);
  }

  gtk_combo_box_set_active(GTK_COMBO_BOX(rule->w_operator), (pos > 0) ? mode : -1);
  gtk_widget_set_sensitive(rule->w_operator, (pos > 0));

  // property
  if(newmain)
  {
    GtkWidget *eb = gtk_event_box_new();
    rule->w_prop = gtk_label_new(dt_collection_name(prop));
    gtk_widget_set_name(rule->w_prop, "section_label");
    gtk_widget_set_tooltip_text(rule->w_prop, _("rule property"));
    gtk_container_add(GTK_CONTAINER(eb), rule->w_prop);
    g_object_set_data(G_OBJECT(eb), "rule", rule);
    g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_event_rule_change_popup), self);
    gtk_box_pack_start(GTK_BOX(hbox), eb, TRUE, TRUE, 0);
  }
  else if(newprop)
  {
    gtk_label_set_label(GTK_LABEL(rule->w_prop), dt_collection_name(prop));
  }

  if(newmain)
  {
    // in order to ensure the property is correctly centered, we add an invisible widget at the right
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *false_cb = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("or"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(false_cb), _("and not"));
    gtk_widget_set_sensitive(false_cb, FALSE);
    gtk_widget_set_name(false_cb, "collect-operator");
    gtk_container_add(GTK_CONTAINER(overlay), false_cb);
    gtk_box_pack_start(GTK_BOX(hbox), overlay, FALSE, FALSE, 0);

    // remove button
    rule->w_close = dtgtk_button_new(dtgtk_cairo_paint_cancel, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_name(GTK_WIDGET(rule->w_close), "basics-link");
    gtk_widget_set_tooltip_text(rule->w_close, _("remove this collect rule"));
    g_signal_connect(G_OBJECT(rule->w_close), "button-press-event", G_CALLBACK(_event_rule_close), rule);
    gtk_widget_set_halign(rule->w_close, GTK_ALIGN_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), rule->w_close);
    gtk_widget_set_no_show_all(rule->w_close, TRUE);

    // expand button
    rule->w_expand = dtgtk_togglebutton_new(dtgtk_cairo_paint_treelist, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_tooltip_text(rule->w_expand, _("show/hide the list of proposals"));
    gtk_widget_set_halign(rule->w_expand, GTK_ALIGN_START);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), rule->w_expand);
    g_signal_connect(G_OBJECT(rule->w_expand), "clicked", G_CALLBACK(_event_rule_expand), rule);
  }

  // we only show the close button if there's more than 1 rule
  dt_lib_collect_t *d = get_collect(rule);
  gtk_widget_set_visible(rule->w_close, (d->nb_rules > 1));

  if(newmain)
  {
    // the second line
    rule->w_widget_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_widget_box, TRUE, TRUE, 0);
    gtk_widget_set_name(rule->w_widget_box, "collect-module-hbox");

    // the raw entry
    rule->w_raw_text = gtk_entry_new();
    g_signal_connect(G_OBJECT(rule->w_raw_text), "activate", G_CALLBACK(_event_rule_changed), rule);
    g_signal_connect(G_OBJECT(rule->w_raw_text), "changed", G_CALLBACK(_event_entry_changed), rule);
    gtk_widget_set_no_show_all(rule->w_raw_text, TRUE);
    gtk_box_pack_start(GTK_BOX(rule->w_widget_box), rule->w_raw_text, TRUE, TRUE, 0);
  }

  const gboolean newraw = g_strcmp0(text, gtk_entry_get_text(GTK_ENTRY(rule->w_raw_text)));

  if(newraw) gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), text);
  if(newprop) _widget_raw_set_tooltip(rule);

  // initialize the specific entries if any
  if(newmain || newprop || newraw) _widget_init_special(rule, text, self);

  if(newmain)
  {
    // the button to switch from raw to specific widgets (only shown if there's some)
    rule->w_raw_switch = dtgtk_button_new(dtgtk_cairo_paint_sorting, CPF_STYLE_FLAT, NULL);
    gtk_widget_set_name(GTK_WIDGET(rule->w_raw_switch), "control-button");
    gtk_widget_set_tooltip_text(rule->w_raw_switch, _("switch from raw UI to more friendly widgets"));
    g_signal_connect(G_OBJECT(rule->w_raw_switch), "button-press-event", G_CALLBACK(_event_rule_raw_switch), rule);
    gtk_box_pack_end(GTK_BOX(rule->w_widget_box), rule->w_raw_switch, FALSE, TRUE, 0);
    gtk_widget_set_no_show_all(rule->w_raw_switch, TRUE);
  }
  gtk_widget_set_visible(rule->w_raw_switch, (rule->w_specific != NULL));

  if(newmain)
  {
    // the listview/treeview
    rule->w_view_sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(rule->w_view_sw, -1, DT_PIXEL_APPLY_DPI(300));
    gtk_box_pack_start(GTK_BOX(rule->w_main), rule->w_view_sw, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(rule->w_view_sw, TRUE);
  }

  // we create the list/treeview for real
  if(newmain || newprop || newraw) _widget_rule_view_update(rule);

  if(newmain || newprop)
  {
    // the expanded state is stored per rule property
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/expand_%d", rule->prop);
    const gboolean expanded = dt_conf_get_bool(confname);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(rule->w_expand), expanded);
    // and we set its visibility
    gtk_widget_set_visible(rule->w_view_sw, expanded);
  }

  // we can now fill the entry completion
  if(newmain || newprop) _widget_init_completion(rule);

  rule->manual_widget_set--;
  return newmain;
}

static void _lib_collect_gui_update(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  ++darktable.gui->reset;
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  d->nb_rules = active + 1;
  char confname[200] = { 0 };

  // create or update defined rules
  for(int i = 0; i < d->nb_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    const dt_collection_properties_t prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    const gchar *txt = dt_conf_get_string_const(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    const dt_lib_collect_mode_t rmode = dt_conf_get_int(confname);
    if(_widget_init(&d->rule[i], prop, txt, rmode, i, self))
      gtk_box_pack_start(GTK_BOX(d->rules_box), d->rule[i].w_main, FALSE, TRUE, 0);
    gtk_widget_show_all(d->rule[i].w_main);
  }

  // remove all remaining rules
  for(int i = d->nb_rules; i < MAX_RULES; i++)
  {
    d->rule[i].prop = 0;
    if(d->rule[i].w_main)
    {
      gtk_widget_destroy(d->rule[i].w_main);
      d->rule[i].w_main = NULL;
      d->rule[i].w_view = NULL;
      d->rule[i].w_special_box = NULL;
    }
  }

  --darktable.gui->reset;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILMROLL);
  dt_conf_set_int("plugins/lighttable/collect/mode0", 0);
  dt_conf_set_string("plugins/lighttable/collect/string0", "");

  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void _event_row_activated_with_event(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col,
                                            GdkEventButton *event, dt_lib_collect_rule_t *rule)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;

  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(gtk_tree_selection_count_selected_rows(selection) < 1) return;
  GList *sels = gtk_tree_selection_get_selected_rows(selection, &model);
  GtkTreePath *path1 = (GtkTreePath *)sels->data;
  if(!gtk_tree_model_get_iter(model, &iter, path1))
  {
    g_list_free_full(sels, (GDestroyNotify)gtk_tree_path_free);
    return;
  }

  gchar *text;
  gboolean order_request = FALSE;
  int order;

  gboolean force_update_view = FALSE;

  gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);

  if(text && strlen(text) > 0)
  {
    if(dt_modifier_is(event->state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    {
      if(rule->prop == DT_COLLECTION_PROP_FILMROLL)
      {
        // go to corresponding folder collection
        //_combo_set_active_collection(d->rule[active].combo, DT_COLLECTION_PROP_FOLDERS);
      }
      else if(rule->prop == DT_COLLECTION_PROP_FOLDERS)
      {
        // go to corresponding filmroll collection
        //_combo_set_active_collection(d->rule[active].combo, DT_COLLECTION_PROP_FILMROLL);
        force_update_view = TRUE;
      }
    }
    else if(gtk_tree_selection_count_selected_rows(selection) > 1
            && _rule_allow_range(rule->prop))
    {
      /* this is a range selection */
      GtkTreeIter iter2;
      GtkTreePath *path2 = (GtkTreePath *)g_list_last(sels)->data;
      if(!gtk_tree_model_get_iter(model, &iter2, path2)) return;

      gchar *text2;
      gtk_tree_model_get(model, &iter2, DT_LIB_COLLECT_COL_PATH, &text2, -1);

      gchar *n_text;
      if(rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop))
        n_text = g_strdup_printf("[%s;%s]", text2, text); /* dates are in reverse order */
      else
        n_text = g_strdup_printf("[%s;%s]", text, text2);

      g_free(text);
      g_free(text2);
      text = n_text;
    }
    else if(rule->prop == DT_COLLECTION_PROP_TAG || rule->prop == DT_COLLECTION_PROP_GEOTAGGING
            || rule->prop == DT_COLLECTION_PROP_FOLDERS)
    {
      if(gtk_tree_model_iter_has_child(model, &iter))
      {
        /* if a tag has children, ctrl-clicking on a parent node should display all images under this hierarchy. */
        if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
        {
          gchar *n_text = g_strconcat(text, "|%", NULL);
          g_free(text);
          text = n_text;
        }
        /* if a tag has children, left-clicking on a parent node should display all images in and under this
         * hierarchy. */
        else if(!dt_modifier_is(event->state, GDK_SHIFT_MASK))
        {
          gchar *n_text = g_strconcat(text, "*", NULL);
          g_free(text);
          text = n_text;
        }
      }
      else if(rule->num == 0 && g_strcmp0(text, _("not tagged")))
      {
        // first filter is tag and the row is a leave
        uint32_t sort = DT_COLLECTION_SORT_NONE;
        gboolean descending = FALSE;
        const uint32_t tagid = dt_tag_get_tag_id_by_name(text);
        if(tagid)
        {
          order_request = TRUE;
          if(dt_tag_get_tag_order_by_id(tagid, &sort, &descending))
          {
            order = sort | (descending  ? DT_COLLECTION_ORDER_FLAG : 0);
          }
          else
          {
            // the tag order is not set yet - default order (filename)
            order = DT_COLLECTION_SORT_FILENAME;
            dt_tag_set_tag_order_by_id(tagid, order & ~DT_COLLECTION_ORDER_FLAG,
                                       order & DT_COLLECTION_ORDER_FLAG);
          }
          dt_collection_set_tag_id((dt_collection_t *)darktable.collection, tagid);
        }
        else dt_collection_set_tag_id((dt_collection_t *)darktable.collection, 0);
      }
    }
  }
  g_list_free_full(sels, (GDestroyNotify)gtk_tree_path_free);

  gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), text);
  gtk_editable_set_position(GTK_EDITABLE(rule->w_raw_text), -1);
  g_free(text);

  if(rule->prop == DT_COLLECTION_PROP_TAG || (rule->prop == DT_COLLECTION_PROP_FOLDERS && !force_update_view)
     || rule->prop == DT_COLLECTION_PROP_DAY || _rule_is_time_property(rule->prop)
     || rule->prop == DT_COLLECTION_PROP_COLORLABEL || rule->prop == DT_COLLECTION_PROP_GEOTAGGING
     || rule->prop == DT_COLLECTION_PROP_HISTORY || rule->prop == DT_COLLECTION_PROP_LOCAL_COPY
     || rule->prop == DT_COLLECTION_PROP_GROUPING)
  {
    _conf_update_rule(rule); // we just have to set the selection
  }
  // else
  //   update_view(d->rule + active); // we have to update visible items too

  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  if(order_request)
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE, order);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  dt_control_queue_redraw_center();
}

/*static void entry_activated(GtkWidget *entry, dt_lib_collect_rule_t *rule)
{
  if(rule->prop != DT_COLLECTION_PROP_FOLDERS
     && rule->prop != DT_COLLECTION_PROP_TAG
     && rule->prop != DT_COLLECTION_PROP_GEOTAGGING
     && rule->prop != DT_COLLECTION_PROP_DAY
     && !is_time_property(rule->prop))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(rule->w_view);

    int rows = gtk_tree_model_iter_n_children(model, NULL);

    // if only one row, press enter in entry box to fill it with the row value
    if(rows == 1)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter_first(model, &iter))
      {
        gchar *text;
        gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);

        gtk_entry_set_text(GTK_ENTRY(rule->w_raw_text), text);
        gtk_editable_set_position(GTK_EDITABLE(rule->w_raw_text), -1);
        g_free(text);
      }
    }
  }
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
  dt_control_queue_redraw_center();
}*/

int position()
{
  return 400;
}

/*static gboolean entry_focus_in_callback(GtkWidget *w, GdkEventFocus *event, dt_lib_collect_rule_t *d)
{
  dt_lib_collect_t *c = get_collect(d);
  if(c->active_rule != d->num)
  {
    c->active_rule = d->num;
    //update_view(c->rule + c->active_rule);
  }

  return FALSE;
}

static void menuitem_mode(GtkMenuItem *menuitem, dt_lib_collect_rule_t *rule)
{
  // add next row with and operator
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  if(active < MAX_RULES)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", active);
    const dt_lib_collect_mode_t mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "menuitem_mode"));
    dt_conf_set_int(confname, mode);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active + 1);
    dt_lib_collect_t *d = get_collect(rule);
    d->active_rule = active;
  }
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void menuitem_mode_change(GtkMenuItem *menuitem, dt_lib_collect_rule_t *rule)
{
  // add next row with and operator
  const int num = rule->num + 1;
  if(num < MAX_RULES && num > 0)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", num);
    const dt_lib_collect_mode_t mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "menuitem_mode"));
    dt_conf_set_int(confname, mode);
  }

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}*/

static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;

  // update tree
  // d->view_rule = -1;
  // d->rule[d->active_rule].typing = FALSE;

  // determine if we want to refresh the tree or not
  gboolean refresh = TRUE;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD && changed_property != DT_COLLECTION_PROP_UNDEF)
  {
    // if we only reload the collection, that means that we don't change the query itself
    // so we only rebuild the treeview if a used property has changed
    refresh = FALSE;
    for(int i = 0; i <= d->nb_rules; i++)
    {
      if(d->rule[i].prop == changed_property)
      {
        refresh = TRUE;
        break;
      }
    }
  }

  if(refresh) _lib_collect_gui_update(self);
}


static void filmrolls_updated(gpointer instance, gpointer self)
{
  // TODO: We should update the count of images here
  _lib_collect_gui_update(self);
}

static void filmrolls_imported(gpointer instance, int film_id, gpointer self)
{
  _lib_collect_gui_update(self);
}

static void preferences_changed(gpointer instance, gpointer self)
{
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void filmrolls_removed(gpointer instance, gpointer self)
{
  /*dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;

  // update tree
  if (d->view_rule != DT_COLLECTION_PROP_FOLDERS)
  {
    d->view_rule = -1;
  }
  d->rule[d->active_rule].typing = FALSE;*/
  _lib_collect_gui_update(self);
}

static void tag_changed(gpointer instance, gpointer self)
{
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  // we check if one of the rules is TAG
  gboolean needs_update = FALSE;
  for(int i = 0; i < d->nb_rules && !needs_update; i++)
  {
    needs_update = needs_update || d->rule[i].prop == DT_COLLECTION_PROP_TAG;
  }
  if(needs_update)
  {
    // we have tags as one of rules, needs reload.
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_TAG, NULL);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
  }
}

static void _geotag_changed(gpointer instance, GList *imgs, const int locid, gpointer self)
{
  // if locid <> NULL this event doesn't concern collect module
  if(!locid)
  {
    dt_lib_module_t *dm = (dt_lib_module_t *)self;
    dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
    // update tree
    gboolean needs_update = FALSE;
    for(int i = 0; i < d->nb_rules && !needs_update; i++)
    {
      needs_update = needs_update || d->rule[i].prop == DT_COLLECTION_PROP_GEOTAGGING;
    }
    if(needs_update)
    {
      _lib_collect_gui_update(self);

      //need to reload collection since we have geotags as active collection filter
      dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GEOTAGGING,
                                 NULL);
      dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                        darktable.view_manager->proxy.module_collect.module);
    }
  }
}

static void metadata_changed(gpointer instance, int type, gpointer self)
{
  /* TODO
  dt_lib_module_t *dm = (dt_lib_module_t *)self;
  dt_lib_collect_t *d = (dt_lib_collect_t *)dm->data;
  if(type == DT_METADATA_SIGNAL_HIDDEN
     || type == DT_METADATA_SIGNAL_SHOWN)
  {
    // hidden/shown metadata have changed - update the collection list
    for(int i = 0; i < MAX_RULES; i++)
    {
      g_signal_handlers_block_matched(d->rule[i].combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, combo_changed, NULL);
      dt_bauhaus_combobox_clear(d->rule[i].combo);
      _populate_collect_combo(d->rule[i].combo);
      if(d->rule[i].prop != -1) // && !_combo_set_active_collection(d->rule[i].combo, property))
      {
        // this one has been hidden - remove entry
        // g_signal_handlers_block_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        gtk_entry_set_text(GTK_ENTRY(d->rule[i].w_raw_text), "");
        // g_signal_handlers_unblock_matched(d->rule[i].text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed,
        // NULL); d->rule[i].typing = FALSE;
        _conf_update_rule(&d->rule[i]);
      }
      // g_signal_handlers_unblock_matched(d->rule[i].combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, combo_changed, NULL);
    }
  }

  // update collection if metadata have been hidden or a metadata collection is active
  const dt_collection_properties_t prop = d->rule[d->active_rule].prop;
  if(type == DT_METADATA_SIGNAL_HIDDEN
     || (prop >= DT_COLLECTION_PROP_METADATA
         && prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER))
  {
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_METADATA,
                               NULL);
  }*/
}

/*static void menuitem_clear(GtkMenuItem *menuitem, dt_lib_collect_rule_t *rule)
{
  // remove this row, or if 1st, clear text entry box
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int active = CLAMP(_a, 1, MAX_RULES);
  dt_lib_collect_t *d = get_collect(rule);
  if(active > 1)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active - 1);
    if(d->active_rule >= active - 1) d->active_rule = active - 2;
  }
  else
  {
    dt_conf_set_int("plugins/lighttable/collect/mode0", DT_LIB_COLLECT_MODE_AND);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "");
  }
  // move up all still active rules by one.
  for(int i = rule->num; i < MAX_RULES - 1; i++)
  {
    char confname[200] = { 0 };
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

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
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
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), d);

    mi = gtk_menu_item_new_with_label(_("add more images"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_OR));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), d);

    mi = gtk_menu_item_new_with_label(_("exclude images"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND_NOT));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), d);
  }
  else if(d->num < active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("change to: and"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), d);

    mi = gtk_menu_item_new_with_label(_("change to: or"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_OR));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), d);

    mi = gtk_menu_item_new_with_label(_("change to: except"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND_NOT));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), d);
  }

  gtk_widget_show_all(GTK_WIDGET(menu));

  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);

  return TRUE;
}*/

static void view_set_click(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  d->singleclick = dt_conf_get_bool("plugins/lighttable/collect/single-click");
}

/*static void _populate_collect_combo(GtkWidget *w)
{
#define ADD_COLLECT_ENTRY(value)                                                              \
  dt_bauhaus_combobox_add_full(w, dt_collection_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, \
                               GUINT_TO_POINTER(value + 1), NULL, TRUE)

    dt_bauhaus_combobox_add_section(w, _("files"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILMROLL);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOLDERS);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILENAME);

    dt_bauhaus_combobox_add_section(w, _("metadata"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TAG);
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
      const gchar *name = dt_metadata_get_name(keyid);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
      g_free(setting);
      const int meta_type = dt_metadata_get_type(keyid);
      if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden)
      {
        ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_METADATA + i);
      }
    }
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_COLORLABEL);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GEOTAGGING);

    dt_bauhaus_combobox_add_section(w, _("times"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DAY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TIME);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_PRINT_TIMESTAMP);

    dt_bauhaus_combobox_add_section(w, _("capture details"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CAMERA);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LENS);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_APERTURE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOCAL_LENGTH);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ISO);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ASPECT_RATIO);

    dt_bauhaus_combobox_add_section(w, _("darktable"));
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GROUPING);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LOCAL_COPY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_HISTORY);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MODULE);
    ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ORDER);

#undef ADD_COLLECT_ENTRY
}*/

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("collections settings"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 _("cancel"), GTK_RESPONSE_NONE,
                                                 _("save"), GTK_RESPONSE_YES, NULL);
  dt_prefs_init_dialog_collect(dialog);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

#ifdef _WIN32
void _mount_changed(GVolumeMonitor *volume_monitor, GMount *mount, dt_lib_module_t *self)
#else
void _mount_changed(GUnixMountMonitor *monitor, dt_lib_module_t *self)
#endif
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  dt_film_set_folder_status();
  // very rough update (rebuild the view). As these events are not too many that remains acceptable
  // adding film_id to treeview and listview would be cleaner to update just the parameter "reachable"

  for(int i = 0; i < d->nb_rules; i++)
  {
    if(d->rule[i].prop != DT_COLLECTION_PROP_FOLDERS && d->rule[i].prop != DT_COLLECTION_PROP_FILMROLL) return;
    if(!d->rule[i].w_view) return;

    // destroy the tree/list
    gtk_widget_destroy(GTK_WIDGET(d->rule[i].w_view));
    d->rule[i].w_view = NULL;

    if(d->rule[i].prop == DT_COLLECTION_PROP_FOLDERS)
    {
      _widget_rule_tree_view(&d->rule[i]);
    }
    else if(d->rule[i].prop == DT_COLLECTION_PROP_FILMROLL)
    {
      _widget_rule_list_view(&d->rule[i]);
    }
  }
}

static void _history_pretty_print(const char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    const int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);

    if(n == 3)
    {
      if(k > 0) switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                   item == 0 ? dt_image_film_roll_name(str) : str);
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _event_history_apply(GtkWidget *widget, dt_lib_module_t *self)
{
  const int hid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "history"));
  if(hid < 0 || hid >= dt_conf_get_int("plugins/lighttable/recentcollect/max_items")) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", hid);
  const char *line = dt_conf_get_string_const(confname);
  if(line && line[0] != '\0') dt_collection_deserialize(line);
}

static gboolean _event_history_show(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  // we show a popup with all the history entries
  GtkMenuShell *pop = GTK_MENU_SHELL(gtk_menu_new());
  gtk_widget_set_name(GTK_WIDGET(pop), "collect-popup");
  gtk_widget_set_size_request(GTK_WIDGET(pop), 200, -1);

  const int maxitems = dt_conf_get_int("plugins/lighttable/recentcollect/max_items");
  const int numitems = dt_conf_get_int("plugins/lighttable/recentcollect/num_items");

  for(int i = 0; i < maxitems && i < numitems; i++)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", i);
    const char *line = dt_conf_get_string_const(confname);
    if(line && line[0] != '\0')
    {
      char str[2048] = { 0 };
      _history_pretty_print(line, str, sizeof(str));
      GtkWidget *smt = gtk_menu_item_new_with_label(str);
      gtk_widget_set_name(smt, "collect-popup-item");
      gtk_widget_set_tooltip_text(smt, str);
      // GtkWidget *child = gtk_bin_get_child(GTK_BIN(smt));
      g_object_set_data(G_OBJECT(smt), "history", GINT_TO_POINTER(i));
      g_signal_connect(G_OBJECT(smt), "activate", G_CALLBACK(_event_history_apply), self);
      gtk_menu_shell_append(pop, smt);
    }
  }

  dt_gui_menu_popup(GTK_MENU(pop), widget, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
  return TRUE;
}

/* save the images order if the first collect filter is on tag*/
static void _sort_set_tag_order(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  if(darktable.collection->tagid)
  {
    const uint32_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
    const gboolean descending = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));
    dt_tag_set_tag_order_by_id(darktable.collection->tagid, sort, descending);
  }
}

// set the sort order to the collection and update the query
static void _sort_update_query(dt_lib_module_t *self, gboolean update_filter)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  const dt_collection_sort_t sort = GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(d->sort->sort));
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->sort->direction));

  // if needed, we sync the filter bar
  if(update_filter) dt_view_filter_update_sort(darktable.view_manager, sort, reverse);

  // we update the collection
  dt_collection_set_sort(darktable.collection, sort, reverse);

  /* save the images order */
  _sort_set_tag_order(self);

  /* sometimes changes */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_SORT, NULL);
}

// update the sort asc/desc arrow
static void _sort_update_arrow(GtkWidget *widget)
{
  const gboolean reverse = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  if(reverse)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN,
                                 NULL);
  else
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(widget);
}

static void _sort_reverse_changed(GtkDarktableToggleButton *widget, dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_arrow(GTK_WIDGET(widget));
  _sort_update_query(self, TRUE);
}

static void _sort_combobox_changed(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  if(d->manual_sort_set) return;

  _sort_update_query(self, TRUE);
}

// this proxy function is primary called when the sort part of the filter bar is changed
static void _proxy_set_sort(dt_lib_module_t *self, dt_collection_sort_t sort, gboolean asc)
{
  // we update the widgets
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  d->manual_sort_set = TRUE;
  dt_bauhaus_combobox_set(d->sort->sort, sort);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->sort->direction), asc);
  _sort_update_arrow(d->sort->direction);
  d->manual_sort_set = FALSE;

  // we update the collection
  _sort_update_query(self, FALSE);
}

static _widgets_sort_t *_sort_get_widgets(dt_lib_module_t *self)
{
  _widgets_sort_t *wsort = (_widgets_sort_t *)g_malloc0(sizeof(_widgets_sort_t));
  wsort->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(wsort->box, "collect-rule-widget");
  const dt_collection_sort_t sort = dt_collection_get_sort_field(darktable.collection);
  wsort->sort = dt_bauhaus_combobox_new_full(DT_ACTION(self), NULL, N_("sort by"),
                                             _("determine the sort order of shown images"), sort,
                                             _sort_combobox_changed, self, NULL);

#define ADD_SORT_ENTRY(value)                                                                                     \
  dt_bauhaus_combobox_add_full(wsort->sort, dt_collection_sort_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,      \
                               GUINT_TO_POINTER(value), NULL, TRUE)

  ADD_SORT_ENTRY(DT_COLLECTION_SORT_FILENAME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DATETIME);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_IMPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CHANGE_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_EXPORT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PRINT_TIMESTAMP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_RATING);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ID);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_COLOR);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_GROUP);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_PATH);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_CUSTOM_ORDER);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_TITLE);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_DESCRIPTION);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_ASPECT_RATIO);
  ADD_SORT_ENTRY(DT_COLLECTION_SORT_SHUFFLE);

#undef ADD_SORT_ENTRY

  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->sort, TRUE, TRUE, 0);

  /* reverse order checkbutton */
  wsort->direction = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_UP, NULL);
  gtk_widget_set_name(GTK_WIDGET(wsort->direction), "control-button");
  if(darktable.collection->params.descending)
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(wsort->direction), dtgtk_cairo_paint_solid_arrow,
                                 CPF_DIRECTION_DOWN, NULL);
  gtk_widget_set_halign(wsort->direction, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(wsort->box), wsort->direction, FALSE, TRUE, 0);
  /* select the last value and connect callback */
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wsort->direction),
                               dt_collection_get_sort_descending(darktable.collection));
  g_signal_connect(G_OBJECT(wsort->direction), "toggled", G_CALLBACK(_sort_reverse_changed), self);

  gtk_widget_show_all(wsort->box);

  return wsort;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)calloc(1, sizeof(dt_lib_collect_t));

  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  d->nb_rules = 0;
  d->params = (dt_lib_collect_params_t *)g_malloc0(sizeof(dt_lib_collect_params_t));
  view_set_click(NULL, self);

  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
  }

  // the box to insert the collect rules
  d->rules_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(GTK_WIDGET(d->rules_box), 200, "plugins/lighttable/collect/windowheight"),
                     TRUE, TRUE, 0);

  // the sorting part
  GtkWidget *label = dt_ui_section_label_new(_("sorting"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  d->sort = _sort_get_widgets(self);
  gtk_box_pack_start(GTK_BOX(self->widget), d->sort->box, FALSE, TRUE, 0);

  // the botton buttons
  label = dt_ui_section_label_new(_("actions"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, TRUE, 0);
  GtkWidget *bhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_end(GTK_BOX(self->widget), bhbox, TRUE, TRUE, 0);
  GtkWidget *btn = dt_ui_button_new(_("add rule..."), _("append new rule to collect images"), NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_add_rule), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, TRUE, TRUE, 0);
  btn = dt_ui_button_new(_("add sort..."), _("append new sorting"), NULL);
  // g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_add_rule), self);
  gtk_box_pack_start(GTK_BOX(bhbox), btn, FALSE, TRUE, 0);
  btn = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT, NULL);
  g_signal_connect(G_OBJECT(btn), "button-press-event", G_CALLBACK(_event_history_show), self);
  gtk_widget_set_tooltip_text(btn, _("revert to a previous set of rules"));
  gtk_box_pack_start(GTK_BOX(bhbox), btn, FALSE, TRUE, 0);
  gtk_widget_show_all(bhbox);

  /* setup proxy */
  darktable.view_manager->proxy.module_collect.module = self;
  darktable.view_manager->proxy.module_collect.update = _lib_collect_gui_update;
  darktable.view_manager->proxy.module_collect.set_sort = _proxy_set_sort;

  _lib_collect_gui_update(self);

  if(d->rule[0].prop == DT_COLLECTION_PROP_TAG)
  {
    const char *tag = dt_conf_get_string_const("plugins/lighttable/collect/string0");
    dt_collection_set_tag_id((dt_collection_t *)darktable.collection, dt_tag_get_tag_id_by_name(tag));
  }

#ifdef _WIN32
  d->vmonitor = g_volume_monitor_get();
  g_signal_connect(G_OBJECT(d->vmonitor), "mount-changed", G_CALLBACK(_mount_changed), self);
  g_signal_connect(G_OBJECT(d->vmonitor), "mount-added", G_CALLBACK(_mount_changed), self);
#else
  d->vmonitor = g_unix_mount_monitor_get();
  g_signal_connect(G_OBJECT(d->vmonitor), "mounts-changed", G_CALLBACK(_mount_changed), self);
#endif

  // force redraw collection images because of late update of the table memory.darktable_iop_names
  const int _a = dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1;
  const int active = CLAMP(_a, 0, (MAX_RULES - 1));
  for(int i = 0; i <= active; i++)
  {
    if(d->rule[i].prop == DT_COLLECTION_PROP_MODULE)
    {
      dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_MODULE,
                                 NULL);
      break;
    }
  }

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, G_CALLBACK(collection_updated),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED, G_CALLBACK(filmrolls_updated),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(preferences_changed),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_IMPORTED, G_CALLBACK(filmrolls_imported),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED, G_CALLBACK(filmrolls_removed),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED, G_CALLBACK(tag_changed),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, G_CALLBACK(_geotag_changed),
                            self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_CHANGED, G_CALLBACK(metadata_changed), self);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(view_set_click), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(collection_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_imported), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(preferences_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_removed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(tag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(view_set_click), self);
  darktable.view_manager->proxy.module_collect.module = NULL;
  free(d->params);

  /* cleanup mem */
  g_object_unref(d->vmonitor);

  /* TODO: Make sure we are cleaning up all allocations */

  free(self->data);
  self->data = NULL;
}

#ifdef USE_LUA
static int new_rule_cb(lua_State*L)
{
  dt_lib_collect_params_rule_t rule;
  memset(&rule, 0, sizeof(dt_lib_collect_params_rule_t));
  luaA_push(L, dt_lib_collect_params_rule_t, &rule);
  return 1;
}

static int filter_cb(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));

  int size;
  dt_lib_collect_params_t *p = get_params(self, &size);
  // put it in stack so memory is not lost if a lua exception is raised

  if(lua_gettop(L) > 0)
  {
    luaL_checktype(L, 1, LUA_TTABLE);
    dt_lib_collect_params_t *new_p = get_params(self, &size);
    new_p->rules = 0;

    do
    {
      lua_pushinteger(L, new_p->rules + 1);
      lua_gettable(L, 1);
      if(lua_isnil(L, -1)) break;
      luaA_to(L, dt_lib_collect_params_rule_t, &new_p->rule[new_p->rules], -1);
      new_p->rules++;
    } while(new_p->rules < MAX_RULES);

    if(new_p->rules == MAX_RULES) {
      lua_pushinteger(L, new_p->rules + 1);
      lua_gettable(L, 1);
      if(!lua_isnil(L, -1)) {
        luaL_error(L, "Number of rules given exceeds max allowed (%d)", MAX_RULES);
      }
    }
    set_params(self, new_p, size);
    free(new_p);

  }

  lua_newtable(L);
  for(int i = 0; i < p->rules; i++)
  {
    luaA_push(L, dt_lib_collect_params_rule_t, &p->rule[i]);
    lua_seti(L, -2, i + 1);  // lua tables are 1 based
  }
  free(p);
  return 1;
}

static int mode_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_collect_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    dt_lib_collect_mode_t value;
    luaA_to(L, dt_lib_collect_mode_t, &value, 3);
    rule->mode = value;
    return 0;
  }

  const dt_lib_collect_mode_t tmp = rule->mode; // temp buffer because of bitfield in the original struct
  luaA_push(L, dt_lib_collect_mode_t, &tmp);
  return 1;
}

static int item_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_collect_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    dt_collection_properties_t value;
    luaA_to(L, dt_collection_properties_t, &value, 3);
    rule->item = value;
    return 0;
  }

  const dt_collection_properties_t tmp = rule->item; // temp buffer because of bitfield in the original struct
  luaA_push(L, dt_collection_properties_t, &tmp);
  return 1;
}

static int data_member(lua_State *L)
{
  dt_lib_collect_params_rule_t *rule = luaL_checkudata(L, 1, "dt_lib_collect_params_rule_t");

  if(lua_gettop(L) > 2)
  {
    size_t tgt_size;
    const char*data = luaL_checklstring(L, 3, &tgt_size);
    if(tgt_size > PARAM_STRING_SIZE)
    {
      return luaL_error(L, "string '%s' too long (max is %d)", data, PARAM_STRING_SIZE);
    }
    g_strlcpy(rule->string, data, sizeof(rule->string));
    return 0;
  }

  lua_pushstring(L, rule->string);
  return 1;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, filter_cb, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "filter");
  lua_pushcfunction(L, new_rule_cb);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "new_rule");

  dt_lua_init_type(L, dt_lib_collect_params_rule_t);
  lua_pushcfunction(L, mode_member);
  dt_lua_type_register(L, dt_lib_collect_params_rule_t, "mode");
  lua_pushcfunction(L, item_member);
  dt_lua_type_register(L, dt_lib_collect_params_rule_t, "item");
  lua_pushcfunction(L, data_member);
  dt_lua_type_register(L, dt_lib_collect_params_rule_t, "data");


  luaA_enum(L, dt_lib_collect_mode_t);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_AND);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_OR);
  luaA_enum_value(L, dt_lib_collect_mode_t, DT_LIB_COLLECT_MODE_AND_NOT);

  luaA_enum(L, dt_collection_properties_t);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FILMROLL);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FOLDERS);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_CAMERA);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_TAG);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_DAY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_TIME);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_PRINT_TIMESTAMP);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_HISTORY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_RATING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_COLORLABEL);

  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    if(dt_metadata_get_type(i) != DT_METADATA_TYPE_INTERNAL)
    {
      const char *name = dt_metadata_get_name(i);
      gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
      const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
      g_free(setting);

      if(!hidden)
        luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_METADATA + i);
    }
  }

  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_LENS);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FOCAL_LENGTH);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ISO);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_APERTURE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ASPECT_RATIO);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_EXPOSURE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_FILENAME);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_GEOTAGGING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_LOCAL_COPY);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_GROUPING);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_MODULE);
  luaA_enum_value(L, dt_collection_properties_t, DT_COLLECTION_PROP_ORDER);
}
#endif
#undef MAX_RULES
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
