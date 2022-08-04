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
#include "lua/preferences.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "lua/call.h"
#include "lua/widget/widget.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

typedef enum
{
  pref_enum,
  pref_dir,
  pref_file,
  pref_string,
  pref_bool,
  pref_int,
  pref_float,
  pref_lua,
} lua_pref_type;


typedef struct enum_data_t
{
  char *default_value;
  luaA_Type enum_type;
} enum_data_t;
typedef struct dir_data_t
{
  char *default_value;
} dir_data_t;
typedef struct file_data_t
{
  char *default_value;
} file_data_t;
typedef struct string_data_t
{
  char *default_value;
} string_data_t;
typedef struct bool_data_t
{
  gboolean default_value;
} bool_data_t;
typedef struct int_data_t
{
  int default_value;
} int_data_t;

typedef struct float_data_t
{
  float default_value;
} float_data_t;
typedef struct lua_data_t
{
  char *default_value;
} lua_data_t;

typedef union all_data_t
{
  enum_data_t enum_data;
  dir_data_t dir_data;
  file_data_t file_data;
  string_data_t string_data;
  bool_data_t bool_data;
  int_data_t int_data;
  float_data_t float_data;
  lua_data_t lua_data;
} all_data_t;

struct pref_element;
typedef void (update_widget_function)(struct pref_element* , GtkWidget* , GtkWidget* );
static void update_widget_enum(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_dir(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_file(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_string(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_bool(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_int(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_float(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);
static void update_widget_lua(struct pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev);

typedef struct pref_element
{
  char *script;
  char *name;
  char *label;
  char *tooltip;
  char *tooltip_reset;
  lua_pref_type type;
  struct pref_element *next;
  all_data_t type_data;
  // The widget used for this preference
  GtkWidget *widget;
  update_widget_function* update_widget;


} pref_element;

static void destroy_pref_element(pref_element *elt)
{
  free(elt->script);
  free(elt->name);
  free(elt->label);
  free(elt->tooltip);
  free(elt->tooltip_reset);
  if(elt->widget) g_object_unref(elt->widget);
  switch(elt->type)
  {
    case pref_enum:
      free(elt->type_data.enum_data.default_value);
      break;
    case pref_dir:
      free(elt->type_data.dir_data.default_value);
      break;
    case pref_file:
      free(elt->type_data.file_data.default_value);
      break;
    case pref_string:
      free(elt->type_data.string_data.default_value);
      break;
    case pref_lua:
      free(elt->type_data.lua_data.default_value);
      break;
    case pref_bool:
    case pref_int:
    case pref_float:
    default:
      break;
  }
  free(elt);
}

static pref_element *pref_list = NULL;

// get all the darktablerc keys
static int get_keys(lua_State *L)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  GList* keys = g_hash_table_get_keys(darktable.conf->table);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);

  keys = g_list_sort(keys, (GCompareFunc) strcmp);
  lua_newtable(L);
  int table_index = 1;
  for(const GList* key = keys; key; key = g_list_next(key))
  {
    lua_pushstring(L, key->data);
    lua_seti(L, -2, table_index);
    table_index++;
  }
  g_list_free(keys);
  return 1;
}

static void get_pref_name(char *tgt, size_t size, const char *script, const char *name)
{
  snprintf(tgt, size, "lua/%s/%s", script, name);
}

// read lua and darktable prefs
static int read_pref(lua_State *L)
{
  const char *script = luaL_checkstring(L, 1);
  const char *name = luaL_checkstring(L, 2);
  lua_pref_type i;
  luaA_to(L, lua_pref_type, &i, 3);

  char pref_name[1024];
  if(strcmp(script, "darktable") != 0)
    get_pref_name(pref_name, sizeof(pref_name), script, name);
  else
    snprintf(pref_name, sizeof(pref_name), "%s", name);
  switch(i)
  {
    case pref_enum:
    {
      const char *str = dt_conf_get_string_const(pref_name);
      lua_pushstring(L, str);
      break;
    }
    case pref_dir:
    {
      const char *str = dt_conf_get_string_const(pref_name);
      lua_pushstring(L, str);
      break;
    }
    case pref_file:
    {
      const char *str = dt_conf_get_string_const(pref_name);
      lua_pushstring(L, str);
      break;
    }
    case pref_string:
    {
      const char *str = dt_conf_get_string_const(pref_name);
      lua_pushstring(L, str);
      break;
    }
    case pref_bool:
      lua_pushboolean(L, dt_conf_get_bool(pref_name));
      break;
    case pref_int:
      lua_pushinteger(L, dt_conf_get_int(pref_name));
      break;
    case pref_float:
      lua_pushnumber(L, dt_conf_get_float(pref_name));
      break;
    case pref_lua:
    {
      const char *str = dt_conf_get_string_const(pref_name);
      lua_pushstring(L, str);
      break;
    }
  }
  return 1;
}

// write lua prefs
static int write_pref(lua_State *L)
{
  const char *script = luaL_checkstring(L, 1);
  const char *name = luaL_checkstring(L, 2);
  int i, tmp;
  luaA_to(L, lua_pref_type, &i, 3);

  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), script, name);
  switch(i)
  {
    case pref_enum:
      luaA_to_type(L, luaA_type_find(L,pref_name), &tmp, 4);
      dt_conf_set_string(pref_name, lua_tostring(L, 4));
      break;
    case pref_dir:
      dt_conf_set_string(pref_name, luaL_checkstring(L, 4));
      break;
    case pref_file:
      dt_conf_set_string(pref_name, luaL_checkstring(L, 4));
      break;
    case pref_string:
      dt_conf_set_string(pref_name, luaL_checkstring(L, 4));
      break;
    case pref_bool:
      luaL_checktype(L, 4, LUA_TBOOLEAN);
      dt_conf_set_bool(pref_name, lua_toboolean(L, 4));
      break;
    case pref_int:
      dt_conf_set_int(pref_name, luaL_checkinteger(L, 4));
      break;
    case pref_float:
      dt_conf_set_float(pref_name, luaL_checknumber(L, 4));
      break;
    case pref_lua:
      dt_conf_set_string(pref_name, luaL_checkstring(L, 4));
      break;
  }
  return 0;
}

// destroy lua prefs
static int destroy_pref(lua_State *L)
{
  gboolean result;
  const char *script = luaL_checkstring(L, 1);
  const char *name = luaL_checkstring(L, 2);

  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), script, name);
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  result = g_hash_table_remove(darktable.conf->table, pref_name);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  lua_pushboolean(L, result);
  return 1;
}

static void response_callback_enum(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    char *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cur_elt->widget));
    dt_conf_set_string(pref_name, text);
    g_free(text);
  }
}


static void response_callback_dir(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(cur_elt->widget));
    dt_conf_set_string(pref_name, folder);
    g_free(folder);
  }
}


static void response_callback_file(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    gchar *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(cur_elt->widget));
    dt_conf_set_string(pref_name, file);
    g_free(file);
  }
}


static void response_callback_string(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_string(pref_name, gtk_entry_get_text(GTK_ENTRY(cur_elt->widget)));
  }
}


static void response_callback_bool(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_bool(pref_name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cur_elt->widget)));
  }
}


static void response_callback_int(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_int(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
  }
}


static void response_callback_float(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_float(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
  }
}


static void response_callback_lua(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_DELETE_EVENT)
  {
    dt_lua_lock_silent();
    lua_State * L = darktable.lua_state.state;
    lua_pushcfunction(L, dt_lua_widget_trigger_callback);
    luaA_push(L, lua_widget, &cur_elt->widget);
    lua_pushstring(L, "set_pref");
    lua_call(L, 2, 0);
    dt_lua_unlock();
  }
}


static gboolean reset_widget_enum(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_combo_box_set_active(GTK_COMBO_BOX(cur_elt->widget), 0);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_dir(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(cur_elt->widget),
                                        cur_elt->type_data.dir_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_file(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(cur_elt->widget),
                                  cur_elt->type_data.file_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_string(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), cur_elt->type_data.string_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_bool(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cur_elt->widget),
                                 cur_elt->type_data.bool_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_int(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), cur_elt->type_data.int_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_float(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), cur_elt->type_data.float_data.default_value);
    return TRUE;
  }
  return FALSE;
}


static gboolean reset_widget_lua(GtkWidget *label, GdkEventButton *event, pref_element *cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    gchar *old_str = dt_conf_get_string(pref_name);
    dt_conf_set_string(pref_name, cur_elt->type_data.lua_data.default_value);
    dt_lua_lock_silent();
    lua_State * L = darktable.lua_state.state;
    lua_pushcfunction(L, dt_lua_widget_trigger_callback);
    luaA_push(L, lua_widget, &cur_elt->widget);
    luaA_push(L, lua_widget, &cur_elt->widget);
    lua_pushstring(L, "set_pref");
    lua_call(L, 3, 0);
    dt_lua_unlock();
    dt_conf_set_string(pref_name, old_str);
    g_free(old_str);
    return TRUE;
  }
  return FALSE;
}


static void update_widget_enum(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_enum), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_enum), cur_elt);
  gtk_combo_box_set_active(GTK_COMBO_BOX(cur_elt->widget), 0);
  const char *value = dt_conf_get_string_const(pref_name);
  do {
    char * active_entry = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(cur_elt->widget));
    if(!active_entry)
    {
      gtk_combo_box_set_active(GTK_COMBO_BOX(cur_elt->widget), -1);
      g_free(active_entry);
      break;
    }
    else if(!strcmp(active_entry, value))
    {
      g_free(active_entry);
      break;
    }
    else
    {
      gtk_combo_box_set_active(GTK_COMBO_BOX(cur_elt->widget), gtk_combo_box_get_active(GTK_COMBO_BOX(cur_elt->widget)) + 1);
      g_free(active_entry);
    }
  } while(true);
}


static void update_widget_dir(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  const char *str = dt_conf_get_string_const(pref_name);
  gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(cur_elt->widget), str);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_dir), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_dir), cur_elt);
}


static void update_widget_file(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  const char *str = dt_conf_get_string_const(pref_name);
  gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(cur_elt->widget), str);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_file), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_file), cur_elt);
}


static void update_widget_string(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_string), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_string), cur_elt);
  const char *str = dt_conf_get_string_const(pref_name);
  gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), str);
}


static void update_widget_bool(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_bool), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_bool), cur_elt);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cur_elt->widget), dt_conf_get_bool(pref_name));
}


static void update_widget_int(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_int(pref_name));
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_int), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_int), cur_elt);
}


static void update_widget_float(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_float(pref_name));
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_float), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_float), cur_elt);
}


static void update_widget_lua(pref_element* cur_elt, GtkWidget* dialog, GtkWidget* labelev)
{
  dt_lua_lock_silent();
  lua_State * L = darktable.lua_state.state;
  lua_pushcfunction(L, dt_lua_widget_trigger_callback);
  luaA_push(L, lua_widget, &cur_elt->widget);
  lua_pushstring(L, "reset");
  lua_call(L, 2, 0);
  dt_lua_unlock();
  g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_lua), cur_elt);
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_lua), cur_elt);
}


static int register_pref_sub(lua_State *L)
{
  // to avoid leaks, we need to first get all params (which could raise errors) then alloc and fill the struct
  // this is complicated, but needed
  pref_element **tmp = lua_touserdata(L, -1);
  lua_pop(L, 1);
  *tmp = calloc(1, sizeof(pref_element));
  pref_element *built_elt = *tmp;
  int cur_param = 1;

  built_elt->script = strdup(luaL_checkstring(L, cur_param));
  cur_param++;

  built_elt->name = strdup(luaL_checkstring(L, cur_param));
  cur_param++;

  luaA_to(L, lua_pref_type, &built_elt->type, cur_param);
  cur_param++;

  built_elt->label = strdup(luaL_checkstring(L, cur_param));
  cur_param++;

  built_elt->tooltip = strdup(luaL_checkstring(L, cur_param));
  cur_param++;

  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), built_elt->script, built_elt->name);
  switch(built_elt->type)
  {
    case pref_enum:
    {
      luaA_Type enum_type = luaA_type_add(L, pref_name, sizeof(int));
      luaA_enum_type(L, enum_type, sizeof(int));
      built_elt->type_data.enum_data.enum_type = enum_type;
      built_elt->widget = gtk_combo_box_text_new();

      int value = 0;
      built_elt->type_data.enum_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;
      
      while(!lua_isnoneornil(L, cur_param))
      {
        luaA_enum_value_type(L, enum_type, &value, luaL_checkstring(L, cur_param));
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(built_elt->widget), luaL_checkstring(L, cur_param));
        cur_param++;
        value++;
      }

      if(!dt_conf_key_exists(pref_name)) {
        dt_conf_set_string(pref_name, built_elt->type_data.enum_data.default_value);
      }


      g_object_ref_sink(G_OBJECT(built_elt->widget));
      built_elt->tooltip_reset = g_strdup_printf(  _("Double-click to reset to `%s'"),
          built_elt->type_data.enum_data.default_value);
      built_elt->update_widget = update_widget_enum;
      break;
    }
    case pref_dir:
      built_elt->type_data.dir_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) {
        dt_conf_set_string(pref_name, built_elt->type_data.dir_data.default_value);
      }
      built_elt->widget = gtk_file_chooser_button_new(_("Select directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
      gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(built_elt->widget), 20);
      g_object_ref_sink(G_OBJECT(built_elt->widget));
      built_elt->tooltip_reset = g_strdup_printf( _("Double-click to reset to `%s'"), built_elt->type_data.dir_data.default_value);
      built_elt->update_widget = update_widget_dir;
      break;
    case pref_file:
      built_elt->type_data.file_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.file_data.default_value);

      built_elt->widget = gtk_file_chooser_button_new(_("Select file"), GTK_FILE_CHOOSER_ACTION_OPEN);
      gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(built_elt->widget), 20);
      built_elt->tooltip_reset= g_strdup_printf( _("Double click to reset to `%s'"), built_elt->type_data.file_data.default_value);
      g_object_ref_sink(G_OBJECT(built_elt->widget));
      built_elt->update_widget = update_widget_file;
      break;
    case pref_string:
      built_elt->type_data.string_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.string_data.default_value);

      built_elt->widget = gtk_entry_new();
      built_elt->tooltip_reset= g_strdup_printf( _("Double-click to reset to `%s'"),
          built_elt->type_data.string_data.default_value);
      g_object_ref_sink(G_OBJECT(built_elt->widget));
      built_elt->update_widget = update_widget_string;
      break;
    case pref_bool:
      luaL_checktype(L, cur_param, LUA_TBOOLEAN);
      built_elt->type_data.bool_data.default_value = lua_toboolean(L, cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_bool(pref_name, built_elt->type_data.bool_data.default_value);

      built_elt->widget = gtk_check_button_new();
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(built_elt->widget), dt_conf_get_bool(pref_name));
      g_object_ref_sink(G_OBJECT(built_elt->widget));
      built_elt->tooltip_reset = g_strdup_printf(  _("Double click to reset to `%s'"),
          built_elt->type_data.bool_data.default_value ? "true" : "false");
      built_elt->update_widget = update_widget_bool;
      break;
    case pref_int:
      {
        luaL_checktype(L, cur_param, LUA_TNUMBER);
        built_elt->type_data.int_data.default_value = lua_tointeger(L, cur_param);
        cur_param++;

        luaL_checktype(L, cur_param, LUA_TNUMBER);
        int min = lua_tointeger(L, cur_param);
        cur_param++;

        luaL_checktype(L, cur_param, LUA_TNUMBER);
        int max = lua_tointeger(L, cur_param);
        cur_param++;

        if(!dt_conf_key_exists(pref_name))
          dt_conf_set_int(pref_name, built_elt->type_data.int_data.default_value);
        built_elt->widget = gtk_spin_button_new_with_range(min, max, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(built_elt->widget), 0);
        g_object_ref_sink(G_OBJECT(built_elt->widget));
        built_elt->tooltip_reset = g_strdup_printf( _("Double-click to reset to `%d'"),
            built_elt->type_data.int_data.default_value);
        built_elt->update_widget = update_widget_int;
        break;
      }
    case pref_float:
      {
        luaL_checktype(L, cur_param, LUA_TNUMBER);
        built_elt->type_data.float_data.default_value = lua_tonumber(L, cur_param);
        cur_param++;

        luaL_checktype(L, cur_param, LUA_TNUMBER);
        float min = lua_tonumber(L, cur_param);
        cur_param++;

        luaL_checktype(L, cur_param, LUA_TNUMBER);
        float max = lua_tonumber(L, cur_param);
        cur_param++;

        luaL_checktype(L, cur_param, LUA_TNUMBER);
        float step = lua_tonumber(L, cur_param);
        cur_param++;

        if(!dt_conf_key_exists(pref_name))
          dt_conf_set_float(pref_name, built_elt->type_data.float_data.default_value);

        built_elt->widget = gtk_spin_button_new_with_range(min, max, step);
        built_elt->tooltip_reset = g_strdup_printf( _("Double click to reset to `%f'"),
            built_elt->type_data.float_data.default_value);
        g_object_ref_sink(G_OBJECT(built_elt->widget));
        built_elt->update_widget = update_widget_float;
        break;
      }
    case pref_lua:
      {
        built_elt->type_data.lua_data.default_value = strdup(luaL_checkstring(L, cur_param));
        cur_param++;

        if(!dt_conf_key_exists(pref_name))
          dt_conf_set_string(pref_name, built_elt->type_data.lua_data.default_value);

        built_elt->tooltip_reset= g_strdup_printf( _("Double-click to reset to `%s'"),
            built_elt->type_data.lua_data.default_value);

        lua_widget widget;
        luaA_to(L, lua_widget, &widget, cur_param);
        cur_param++;
        dt_lua_widget_bind(L, widget);
        built_elt->widget = widget->widget;
        built_elt->update_widget = update_widget_lua;

        luaL_checktype(L, cur_param, LUA_TFUNCTION);
        luaA_push(L, lua_widget, widget);
        lua_pushvalue(L, cur_param);
        dt_lua_widget_set_callback(L, -2, "set_pref");
        lua_pop(L,1);

        break;
      }
  }
  return 0;
}


static int register_pref(lua_State *L)
{
  // wrapper to catch lua errors in a clean way
  pref_element *built_elt = NULL;
  lua_pushcfunction(L, register_pref_sub);
  dt_lua_gtk_wrap(L);
  lua_insert(L, 1);
  lua_pushlightuserdata(L, &built_elt);
  int result = dt_lua_treated_pcall(L, lua_gettop(L) - 1, 0);
  if(result == LUA_OK)
  {
    built_elt->next = pref_list;
    pref_list = built_elt;
    return 0;
  }
  else
  {
    destroy_pref_element(built_elt);
    return lua_error(L);
  }
}


GtkGrid* init_tab_lua(GtkWidget *dialog, GtkWidget *stack)
{
  if(!pref_list) return NULL; // no option registered => don't create the tab
  GtkWidget *label, *labelev, *viewport;
  GtkWidget *grid = gtk_grid_new();
  int line = 0;
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE); // doesn't seem to work from gtkrc
  gtk_container_add(GTK_CONTAINER(scroll), viewport);
  gtk_container_add(GTK_CONTAINER(viewport), grid);
  gtk_stack_add_titled(GTK_STACK(stack), scroll, _("Lua options"), _("Lua options"));

  for(pref_element *cur_elt = pref_list; cur_elt; cur_elt = cur_elt->next)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    label = gtk_label_new(cur_elt->label);
    gtk_widget_set_halign(GTK_WIDGET(label), GTK_ALIGN_START);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    cur_elt->update_widget(cur_elt,dialog,labelev);
    gtk_widget_set_tooltip_text(labelev, cur_elt->tooltip_reset);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    gtk_widget_set_tooltip_text(cur_elt->widget, cur_elt->tooltip);
    gtk_grid_attach(GTK_GRID(grid), labelev, 0, line, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cur_elt->widget, 1, line, 1, 1);
    line++;
  }
  return GTK_GRID(grid);
}


void destroy_tab_lua(GtkGrid *grid)
{
  if(!grid) return;
  gtk_grid_remove_column(grid, 1); // detach all special widgets to avoid having them destroyed
}


int dt_lua_init_preferences(lua_State *L)
{
  luaA_enum(L, lua_pref_type);
  luaA_enum_value_name(L, lua_pref_type, pref_string, "string");
  luaA_enum_value_name(L, lua_pref_type, pref_bool, "bool");
  luaA_enum_value_name(L, lua_pref_type, pref_int, "integer");
  luaA_enum_value_name(L, lua_pref_type, pref_float, "float");
  luaA_enum_value_name(L, lua_pref_type, pref_file, "file");
  luaA_enum_value_name(L, lua_pref_type, pref_dir, "directory");
  luaA_enum_value_name(L, lua_pref_type, pref_enum, "enum");
  luaA_enum_value_name(L, lua_pref_type, pref_lua, "lua");

  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L, "preferences");

  lua_pushcfunction(L, register_pref);
  lua_setfield(L, -2, "register");

  lua_pushcfunction(L, read_pref);
  lua_setfield(L, -2, "read");

  lua_pushcfunction(L, write_pref);
  lua_setfield(L, -2, "write");

  lua_pushcfunction(L, destroy_pref);
  lua_setfield(L, -2, "destroy");

  lua_pushcfunction(L, get_keys);
  lua_setfield(L, -2, "get_keys");

  lua_pop(L, 1);
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

