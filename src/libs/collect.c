/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "libs/collect.h"
#include "views/view.h"

DT_MODULE(1)

#define MAX_RULES 10

typedef struct dt_lib_collect_rule_t
{
  long int num;
  GtkWidget *hbox;
  GtkComboBox *combo;
  GtkWidget *text;
  GtkWidget *button;
}
dt_lib_collect_rule_t;

typedef struct dt_lib_collect_t
{
  dt_lib_collect_rule_t rule[MAX_RULES];
  int active_rule;
  GtkTreeView *view;
  GtkScrolledWindow *scrolledwindow;
  
  struct dt_lib_collect_params_t *params;
}
dt_lib_collect_t;

#define PARAM_STRING_SIZE 256  // FIXME: is this enough !?
typedef struct dt_lib_collect_params_t
{
  uint32_t rules;
  struct {
    uint32_t item:16;
    uint32_t mode:16;
    char string[PARAM_STRING_SIZE];
  } rule[MAX_RULES];
} dt_lib_collect_params_t;

typedef enum dt_lib_collect_cols_t
{
  DT_LIB_COLLECT_COL_TEXT=0,
  DT_LIB_COLLECT_COL_ID,
  DT_LIB_COLLECT_COL_TOOLTIP,
  DT_LIB_COLLECT_COL_PATH,
  DT_LIB_COLLECT_NUM_COLS
}
dt_lib_collect_cols_t;


static void _lib_collect_gui_update (dt_lib_module_t *d);


const char*
name ()
{
  return _("collect images");
}


void init_presets(dt_lib_module_t *self)
{
}

/* Update the params struct with active ruleset */
static void _lib_collect_update_params(dt_lib_collect_t *d) {
  /* reset params */
  dt_lib_collect_params_t *p = d->params;
  memset(p,0,sizeof(dt_lib_collect_params_t));

  /* for each active rule set update params */
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1, 0, (MAX_RULES-1));
  char confname[200];
  for (int i=0; i<=active; i++) {
    /* get item */
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", i);
    p->rule[i].item = dt_conf_get_int(confname);
    
    /* get mode */
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", i);
    p->rule[i].mode = dt_conf_get_int(confname);

    /* get string */
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", i);
    gchar* string = dt_conf_get_string(confname);
    if (string != NULL) {
      snprintf(p->rule[i].string,PARAM_STRING_SIZE,"%s", string);
      g_free(string);
    }

    fprintf(stderr,"[%i] %d,%d,%s\n",i, p->rule[i].item, p->rule[i].mode,  p->rule[i].string);
  }
  
  p->rules = active+1;

}

void *get_params(dt_lib_module_t *self, int *size)
{
  _lib_collect_update_params(self->data);

  /* allocate a copy of params to return, freed by caller */
  *size = sizeof(dt_lib_collect_params_t);
  void *p = malloc(*size);
  memcpy(p,((dt_lib_collect_t *)self->data)->params,*size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  /* update conf settings from params */
  dt_lib_collect_params_t *p = (dt_lib_collect_params_t *)params;
  char confname[200];
  
  for (int i=0; i<p->rules; i++) {
    /* set item */
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", i);
    dt_conf_set_int(confname, p->rule[i].item);
    
    /* set mode */
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", i);
    dt_conf_set_int(confname, p->rule[i].mode);
    
    /* set string */
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", i);
    dt_conf_set_string(confname, p->rule[i].string);
  }

  /* set number of rules */
  snprintf(confname, 200, "plugins/lighttable/collect/num_rules");
  dt_conf_set_int(confname, p->rules);
  
  /* update ui */
  _lib_collect_gui_update(self);

  /* update view */
  dt_collection_update_query(darktable.collection);

  return 0;
}


uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

static dt_lib_collect_t*
get_collect (dt_lib_collect_rule_t *r)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)(((char *)r) - r->num*sizeof(dt_lib_collect_rule_t));
  return d;
}

static gboolean
changed_callback (GtkEntry *entry, dt_lib_collect_rule_t *dr)
{
  // update related list
  dt_lib_collect_t *d = get_collect(dr);
  sqlite3_stmt *stmt;
  GtkTreeIter iter;
  GtkTreeView *view = d->view;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  g_object_ref(model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);
  gtk_list_store_clear(GTK_LIST_STORE(model));
  char query[1024];
  int property = gtk_combo_box_get_active(dr->combo);
  const gchar *text = gtk_entry_get_text(GTK_ENTRY(dr->text));
  gchar *escaped_text = dt_util_str_replace(text, "'", "''");
  char confname[200];
  snprintf(confname, 200, "plugins/lighttable/collect/string%1ld", dr->num);
  dt_conf_set_string (confname, text);
  snprintf(confname, 200, "plugins/lighttable/collect/item%1ld", dr->num);
  dt_conf_set_int (confname, property);

  switch(property)
  {
    case 0: // film roll
      snprintf(query, 1024, "select distinct folder, id from film_rolls where folder like '%%%s%%'  order by id DESC", escaped_text);
      break;
    case 1: // camera
      snprintf(query, 1024, "select distinct maker || ' ' || model as model, 1 from images where maker || ' ' || model like '%%%s%%' order by model", escaped_text);
      break;
    case 2: // tag
      snprintf(query, 1024, "SELECT distinct name, id FROM tags WHERE name LIKE '%%%s%%' ORDER BY UPPER(name)", escaped_text);
      break;
    case 4: // History, 2 hardcoded alternatives
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("altered"),
                          DT_LIB_COLLECT_COL_ID, 0,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("altered"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("not altered"),
                          DT_LIB_COLLECT_COL_ID, 1,
                          DT_LIB_COLLECT_COL_TOOLTIP,_("not altered"),
                          -1);
      goto entry_key_press_exit;
      break;

    case 5: // colorlabels
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("red"),
                          DT_LIB_COLLECT_COL_ID, 0,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("red"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("yellow"),
                          DT_LIB_COLLECT_COL_ID, 1,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("yellow"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("green"),
                          DT_LIB_COLLECT_COL_ID, 2,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("green"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("blue"),
                          DT_LIB_COLLECT_COL_ID, 3,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("blue"),
                          -1);
      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                          DT_LIB_COLLECT_COL_TEXT,_("purple"),
                          DT_LIB_COLLECT_COL_ID, 4,
                          DT_LIB_COLLECT_COL_TOOLTIP, _("purple"),
                          -1);
      goto entry_key_press_exit;
      break;

      // TODO: Add empty string for metadata?
      // TODO: Autogenerate this code?
    case 6: // title
      snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
               DT_METADATA_XMP_DC_TITLE, escaped_text);
      break;
    case 7: // description
      snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
               DT_METADATA_XMP_DC_DESCRIPTION, escaped_text);
      break;
    case 8: // creator
      snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
               DT_METADATA_XMP_DC_CREATOR, escaped_text);
      break;
    case 9: // publisher
      snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%' order by value",
               DT_METADATA_XMP_DC_PUBLISHER, escaped_text);
      break;
    case 10: // rights
      snprintf(query, 1024, "select distinct value, 1 from meta_data where key = %d and value like '%%%s%%'order by value ",
               DT_METADATA_XMP_DC_RIGHTS, escaped_text);
      break;
    case 11: // lens
      snprintf(query, 1024, "select distinct lens, 1 from images where lens like '%%%s%%' order by lens", escaped_text);
      break;
    case 12: // iso
      snprintf(query, 1024, "select distinct cast(iso as integer) as iso, 1 from images where iso like '%%%s%%' order by iso", escaped_text);
      break;
    case 13: // aperature
      snprintf(query, 1024, "select distinct round(aperture,1) as aperture, 1 from images where aperture like '%%%s%%' order by aperture", escaped_text);
      break;
    case 14: // filename
      snprintf(query, 1024, "select distinct filename, 1 from images where filename like '%%%s%%' order by filename", escaped_text);
      break;

    default: // case 3: // day
      snprintf(query, 1024, "SELECT DISTINCT datetime_taken, 1 FROM images WHERE datetime_taken LIKE '%%%s%%' ORDER BY datetime_taken DESC", escaped_text);
      break;
  }
  g_free(escaped_text);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gtk_list_store_append(GTK_LIST_STORE(model), &iter);
    const char *folder = (const char*)sqlite3_column_text(stmt, 0);
    if(property == 0) // film roll
    {
      folder = dt_image_film_roll_name(folder);
    }
    gchar *value =  (gchar *)sqlite3_column_text(stmt, 0);
    gchar *escaped_text = g_markup_escape_text(value, strlen(value));
    gtk_list_store_set (GTK_LIST_STORE(model), &iter,
                        DT_LIB_COLLECT_COL_TEXT, folder,
                        DT_LIB_COLLECT_COL_ID, sqlite3_column_int(stmt, 1),
                        DT_LIB_COLLECT_COL_TOOLTIP, escaped_text,
                        DT_LIB_COLLECT_COL_PATH, value,
                        -1);
  }
  sqlite3_finalize(stmt);
entry_key_press_exit:
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(view), DT_LIB_COLLECT_COL_TOOLTIP);
  gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
  g_object_unref(model);
  return FALSE;
}

static void
_lib_collect_gui_update (dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  const int old = darktable.gui->reset;
  darktable.gui->reset = 1;
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules") - 1, 0, (MAX_RULES-1));
  char confname[200];
  for(int i=0; i<MAX_RULES; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, TRUE);
    gtk_widget_set_visible(d->rule[i].hbox, FALSE);
  }
  for(int i=0; i<=active; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, FALSE);
    gtk_widget_set_visible(d->rule[i].hbox, TRUE);
    gtk_widget_show_all(d->rule[i].hbox);
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", i);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->rule[i].combo), dt_conf_get_int(confname));
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    if(text)
    {
      gtk_entry_set_text(GTK_ENTRY(d->rule[i].text), text);
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
      snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", i+1);
      const int mode = dt_conf_get_int(confname);
      if(mode == DT_LIB_COLLECT_MODE_AND)     button->icon = dtgtk_cairo_paint_and;
      if(mode == DT_LIB_COLLECT_MODE_OR)      button->icon = dtgtk_cairo_paint_or;
      if(mode == DT_LIB_COLLECT_MODE_AND_NOT) button->icon = dtgtk_cairo_paint_andnot;
      g_object_set(G_OBJECT(button), "tooltip-text", _("clear this rule"), (char *)NULL);
    }
  }
  // update list of proposals
  changed_callback(NULL, d->rule + d->active_rule);
  darktable.gui->reset = old;
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/item0", 0);
  dt_conf_set_string("plugins/lighttable/collect/string0", "%");
  dt_collection_set_query_flags(darktable.collection,COLLECTION_QUERY_FULL);
  dt_collection_update_query(darktable.collection);
}

static void
combo_changed (GtkComboBox *combo, dt_lib_collect_rule_t *d)
{
  if(darktable.gui->reset) return;
  gtk_entry_set_text(GTK_ENTRY(d->text), "");
  dt_lib_collect_t *c = get_collect(d);
  c->active_rule = d->num;
  changed_callback(NULL, d);
  dt_collection_update_query(darktable.collection);
}

static void
row_activated (GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_collect_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;
  gchar *text;
  const int active = d->active_rule;
  const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(d->rule[active].combo));
  if(item == 0) // get full path for film rolls:
    gtk_tree_model_get (model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
  else
    gtk_tree_model_get (model, &iter, DT_LIB_COLLECT_COL_TEXT, &text, -1);
  gtk_entry_set_text(GTK_ENTRY(d->rule[active].text), text);
  g_free(text);
  changed_callback(NULL, d->rule + active);
  dt_collection_update_query(darktable.collection);
  dt_control_queue_redraw_center();
}

static void
entry_activated (GtkWidget *entry, dt_lib_collect_rule_t *d)
{
  changed_callback(NULL, d);
  dt_lib_collect_t *c = get_collect(d);
  GtkTreeView *view = c->view;
  GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
  gint rows = gtk_tree_model_iter_n_children(model, NULL);
  if(rows == 1)
  {
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter_first(model, &iter))
    {
      gchar *text;
      const int item = gtk_combo_box_get_active(GTK_COMBO_BOX(d->combo));
      if(item == 0) // get full path for film rolls:
        gtk_tree_model_get (model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
      else
        gtk_tree_model_get (model, &iter, DT_LIB_COLLECT_COL_TEXT, &text, -1);
      gtk_entry_set_text(GTK_ENTRY(d->text), text);
      g_free(text);
      changed_callback(NULL, d);
    }
  }
  dt_collection_update_query(darktable.collection);
}

int
position ()
{
  return 400;
}

static void
entry_focus_in_callback (GtkWidget *w, GdkEventFocus *event, dt_lib_collect_rule_t *d)
{
  dt_lib_collect_t *c = get_collect(d);
  c->active_rule = d->num;
  changed_callback(NULL, c->rule + c->active_rule);
}

#if 0
static void
focus_in_callback (GtkWidget *w, GdkEventFocus *event, dt_lib_module_t *self)
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkEntry *entry = GTK_ENTRY(self->text);
  GtkTreeView *view;
  int count = 1 + count_film_rolls(gtk_entry_get_text(entry));
  int ht = get_font_height(view, "Dreggn");
  const int size = MAX(2*ht, MIN(win->allocation.height/2, count*ht));
  gtk_widget_set_size_request(view, -1, size);
}

static void
hide_callback (GObject    *object,
               GParamSpec *param_spec,
               GtkWidget *view)
{
  GtkExpander *expander;
  expander = GTK_EXPANDER (object);
  if (!gtk_expander_get_expanded (expander))
    gtk_widget_set_size_request(view, -1, -1);
}
#endif

static void
menuitem_and (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active+1);
    dt_lib_collect_t *c = get_collect(d);
    c->active_rule = active;
  }
  dt_collection_update_query(darktable.collection);
}

static void
menuitem_or (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active+1);
  }
  dt_collection_update_query(darktable.collection);
}

static void
menuitem_and_not (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);
  if(active < 10)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", active);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", active);
    dt_conf_set_string(confname, "");
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active+1);
  }
  dt_collection_update_query(darktable.collection);
}

static void
menuitem_change_and (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND);
  }
  dt_collection_update_query(darktable.collection);
}

static void
menuitem_change_or (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with or operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_OR);
  }
  dt_collection_update_query(darktable.collection);
}

static void
menuitem_change_and_not (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // add next row with and not operator
  const int num = d->num + 1;
  if(num < 10 && num > 0)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", num);
    dt_conf_set_int(confname, DT_LIB_COLLECT_MODE_AND_NOT);
  }
  dt_collection_update_query(darktable.collection);
}

static void
collection_updated(gpointer instance,gpointer self)
{
  _lib_collect_gui_update((dt_lib_module_t *)self);
}


static void
menuitem_clear (GtkMenuItem *menuitem, dt_lib_collect_rule_t *d)
{
  // remove this row, or if 1st, clear text entry box
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);
  dt_lib_collect_t *c = get_collect(d);
  if(active > 1)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", active-1);
    if(c->active_rule >= active-1) c->active_rule = active - 2;
  }
  else
  {
    dt_conf_set_int("plugins/lighttable/collect/mode0", DT_LIB_COLLECT_MODE_AND);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "");
  }
  // move up all still active rules by one.
  for(int i=d->num; i<MAX_RULES-1; i++)
  {
    char confname[200];
    snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", i+1);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, 200, "plugins/lighttable/collect/item%1d", i+1);
    const int item = dt_conf_get_int(confname);
    snprintf(confname, 200, "plugins/lighttable/collect/string%1d", i+1);
    gchar *string = dt_conf_get_string(confname);
    if(string)
    {
      snprintf(confname, 200, "plugins/lighttable/collect/mode%1d", i);
      dt_conf_set_int(confname, mode);
      snprintf(confname, 200, "plugins/lighttable/collect/item%1d", i);
      dt_conf_set_int(confname, item);
      snprintf(confname, 200, "plugins/lighttable/collect/string%1d", i);
      dt_conf_set_string(confname, string);
      g_free(string);
    }
  }
  dt_collection_update_query(darktable.collection);
}

static gboolean
popup_button_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *d)
{
  if(event->button != 1) return FALSE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *mi;
  const int active = CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);

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

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)malloc(sizeof(dt_lib_collect_t));

  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 5);
  gtk_widget_set_size_request(self->widget, 100, -1);
  d->active_rule = 0;
  d->params = (dt_lib_collect_params_t*)malloc(sizeof(dt_lib_collect_params_t));

  dt_control_signal_connect(darktable.signals, 
			    DT_SIGNAL_COLLECTION_CHANGED,
			    G_CALLBACK(collection_updated),
			    self);
  
  GtkBox *box;
  GtkWidget *w;
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  GtkListStore *liststore;

  for(int i=0; i<MAX_RULES; i++)
  {
    d->rule[i].num = i;
    box = GTK_BOX(gtk_hbox_new(FALSE, 5));
    d->rule[i].hbox = GTK_WIDGET(box);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
    w = gtk_combo_box_new_text();
    d->rule[i].combo = GTK_COMBO_BOX(w);
    for(int k=0; k<dt_lib_collect_string_cnt; k++)
      gtk_combo_box_append_text(GTK_COMBO_BOX(w), _(dt_lib_collect_string[k]));
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(combo_changed), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);
    w = gtk_entry_new();
    dt_gui_key_accel_block_on_focus(w);
    d->rule[i].text = w;
    gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(w), "focus-in-event", G_CALLBACK(entry_focus_in_callback), d->rule + i);

    /* xgettext:no-c-format */
    g_object_set(G_OBJECT(w), "tooltip-text", _("type your query, use `%' as wildcard"), (char *)NULL);
    gtk_widget_add_events(w, GDK_KEY_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(changed_callback), d->rule + i);
    g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), d->rule + i);
    gtk_box_pack_start(box, w, TRUE, TRUE, 0);
    w = dtgtk_button_new(dtgtk_cairo_paint_presets, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER);
    d->rule[i].button = w;
    gtk_widget_set_events(w, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(w), "button-press-event", G_CALLBACK(popup_button_callback), d->rule + i);
    gtk_box_pack_start(box, w, FALSE, FALSE, 0);
    gtk_widget_set_size_request(w, 13, 13);
  }

  d->scrolledwindow = GTK_SCROLLED_WINDOW(sw);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(view));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(sw), TRUE, TRUE, 0);
  gtk_tree_view_set_headers_visible(view, FALSE);
  liststore = gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING);
  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  gtk_widget_set_size_request(GTK_WIDGET(view), -1, 300);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_add_attribute(col, renderer, "text", DT_LIB_COLLECT_COL_TEXT);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), GTK_SELECTION_SINGLE);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(liststore));
  g_signal_connect(G_OBJECT (view), "row-activated", G_CALLBACK (row_activated), d);

  /* setup proxy */
  darktable.view_manager->proxy.module_collect.module = self;
  darktable.view_manager->proxy.module_collect.update = _lib_collect_gui_update;

  _lib_collect_gui_update(self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(collection_updated), self);
  darktable.view_manager->proxy.module_collect.module = NULL;
  free(((dt_lib_collect_t*)self->data)->params);
  free(self->data);
  self->data = NULL;
}

#undef MAX_RULES
