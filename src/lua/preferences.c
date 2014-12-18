/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include <glib.h>
#include "lua/preferences.h"
#include <stdlib.h>
#include <string.h>
#include "control/conf.h"
#include "gui/gtk.h"

typedef enum
{
  pref_string,
  pref_bool,
  pref_int,
  pref_float,
  pref_file,
  pref_dir,
  pref_enum
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
  int min;
  int max;
  int default_value;
} int_data_t;

typedef struct float_data_t
{
  float min;
  float max;
  float step;
  float default_value;
} float_data_t;

typedef union all_data_t
{
  string_data_t string_data;
  bool_data_t bool_data;
  int_data_t int_data;
  float_data_t float_data;
  file_data_t file_data;
  dir_data_t dir_data;
  enum_data_t enum_data;
} all_data_t;
typedef struct pref_element
{
  char *script;
  char *name;
  char *label;
  char *tooltip;
  lua_pref_type type;
  struct pref_element *next;
  all_data_t type_data;
  // ugly hack, written every time the dialog is displayed and unused when not displayed
  // value is set when window is created, it is correct when window is destroyed, but is invalid out of that
  // context
  GtkWidget *widget;

} pref_element;

static void destroy_pref_element(pref_element *elt)
{
  free(elt->script);
  free(elt->name);
  free(elt->label);
  free(elt->tooltip);
  // don't free the widget field
  switch(elt->type)
  {
    case pref_enum:
      free(elt->type_data.enum_data.default_value);
    case pref_dir:
      free(elt->type_data.dir_data.default_value);
      break;
    case pref_file:
      free(elt->type_data.file_data.default_value);
      break;
    case pref_string:
      free(elt->type_data.string_data.default_value);
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

static void get_pref_name(char *tgt, size_t size, const char *script, const char *name)
{
  snprintf(tgt, size, "lua/%s/%s", script, name);
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

      int value = 0;
      built_elt->type_data.enum_data.default_value = strdup(luaL_checkstring(L, cur_param));
      while(!lua_isnoneornil(L, cur_param))
      {
        luaA_enum_value_type(L, enum_type, &value, luaL_checkstring(L, cur_param));
        cur_param++;
        value++;
      }

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.enum_data.default_value);
      break;
    }
    case pref_dir:
      built_elt->type_data.dir_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.dir_data.default_value);
      break;
    case pref_file:
      built_elt->type_data.file_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.file_data.default_value);
      break;
    case pref_string:
      built_elt->type_data.string_data.default_value = strdup(luaL_checkstring(L, cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_string(pref_name, built_elt->type_data.string_data.default_value);
      break;
    case pref_bool:
      luaL_checktype(L, cur_param, LUA_TBOOLEAN);
      built_elt->type_data.bool_data.default_value = lua_toboolean(L, cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_bool(pref_name, built_elt->type_data.bool_data.default_value);
      break;
    case pref_int:
      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.int_data.default_value = lua_tointeger(L, cur_param);
      cur_param++;

      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.int_data.min = lua_tointeger(L, cur_param);
      cur_param++;

      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.int_data.max = lua_tointeger(L, cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_int(pref_name, built_elt->type_data.int_data.default_value);
      break;
    case pref_float:
      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.float_data.default_value = lua_tonumber(L, cur_param);
      cur_param++;

      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.float_data.min = lua_tonumber(L, cur_param);
      cur_param++;

      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.float_data.max = lua_tonumber(L, cur_param);
      cur_param++;

      luaL_checktype(L, cur_param, LUA_TNUMBER);
      built_elt->type_data.float_data.step = lua_tonumber(L, cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name))
        dt_conf_set_float(pref_name, built_elt->type_data.float_data.default_value);
      break;
  }
  return 0;
}


static int register_pref(lua_State *L)
{
  // wrapper to catch lua errors in a clean way
  pref_element *built_elt = NULL;
  lua_pushcfunction(L, register_pref_sub);
  lua_insert(L, 1);
  lua_pushlightuserdata(L, &built_elt);
  int result = lua_pcall(L, lua_gettop(L) - 1, 0, 0);
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


static int read_pref(lua_State *L)
{
  const char *script = luaL_checkstring(L, 1);
  const char *name = luaL_checkstring(L, 2);
  lua_pref_type i;
  luaA_to(L, lua_pref_type, &i, 3);

  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), script, name);
  switch(i)
  {
    case pref_enum:
    {
      char *str = dt_conf_get_string(pref_name);
      lua_pushstring(L, str);
      g_free(str);
      break;
    }
    case pref_dir:
    {
      char *str = dt_conf_get_string(pref_name);
      lua_pushstring(L, str);
      g_free(str);
      break;
    }
    case pref_file:
    {
      char *str = dt_conf_get_string(pref_name);
      lua_pushstring(L, str);
      g_free(str);
      break;
    }
    case pref_string:
    {
      char *str = dt_conf_get_string(pref_name);
      lua_pushstring(L, str);
      g_free(str);
      break;
    }
    case pref_bool:
      lua_pushboolean(L, dt_conf_get_bool(pref_name));
      break;
    case pref_int:
      lua_pushnumber(L, dt_conf_get_int(pref_name));
      break;
    case pref_float:
      lua_pushnumber(L, dt_conf_get_float(pref_name));
      break;
  }
  return 1;
}

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
      luaA_to(L, pref_name, &tmp, 4);
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
  }
  return 0;
}


static void callback_enum(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  char *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
  dt_conf_set_string(pref_name, text);
  g_free(text);
}
static void callback_dir(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_string(pref_name, gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(widget)));
}
static void callback_file(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_string(pref_name, gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget)));
}
static void callback_string(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_string(pref_name, gtk_entry_get_text(GTK_ENTRY(widget)));
}
static void callback_bool(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_bool(pref_name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}
static void callback_int(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_int(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}
static void callback_float(GtkWidget *widget, pref_element *cur_elt)
{
  char pref_name[1024];
  get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
  dt_conf_set_float(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}

static void response_callback_enum(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
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
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_string(pref_name, gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(cur_elt->widget)));
  }
}
static void response_callback_file(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_string(pref_name, gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(cur_elt->widget)));
  }
}
static void response_callback_string(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_string(pref_name, gtk_entry_get_text(GTK_ENTRY(cur_elt->widget)));
  }
}
static void response_callback_bool(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_bool(pref_name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cur_elt->widget)));
  }
}
static void response_callback_int(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_int(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
  }
}
static void response_callback_float(GtkDialog *dialog, gint response_id, pref_element *cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    dt_conf_set_float(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
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
void init_tab_lua(GtkWidget *dialog, GtkWidget *tab)
{
  if(!pref_list) return; // no option registered => don't create the tab
  char tooltip[1024];
  GtkWidget *label, *labelev, *viewport;
  GtkWidget *grid = gtk_grid_new();
  int line = 0;
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_row_homogeneous(GTK_GRID(grid), TRUE);
  gtk_widget_set_valign(grid, GTK_ALIGN_START);
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_margin_top(scroll, DT_PIXEL_APPLY_DPI(20));
  gtk_widget_set_margin_bottom(scroll, DT_PIXEL_APPLY_DPI(20));
  gtk_widget_set_margin_start(scroll, DT_PIXEL_APPLY_DPI(20));
  gtk_widget_set_margin_end(scroll, DT_PIXEL_APPLY_DPI(20));
  viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE); // doesn't seem to work from gtkrc
  gtk_container_add(GTK_CONTAINER(scroll), viewport);
  gtk_container_add(GTK_CONTAINER(viewport), grid);
  gtk_notebook_append_page(GTK_NOTEBOOK(tab), scroll, gtk_label_new(_("lua options")));

  pref_element *cur_elt = pref_list;
  while(cur_elt)
  {
    char pref_name[1024];
    get_pref_name(pref_name, sizeof(pref_name), cur_elt->script, cur_elt->name);
    label = gtk_label_new(cur_elt->label);
    gtk_widget_set_halign(GTK_WIDGET(label), GTK_ALIGN_START);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    switch(cur_elt->type)
    {
      case pref_enum:
        cur_elt->widget = gtk_combo_box_text_new();
        g_signal_connect(G_OBJECT(cur_elt->widget), "changed", G_CALLBACK(callback_enum), cur_elt);
        {
          gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cur_elt->widget),
                                         cur_elt->type_data.enum_data.default_value);
          const char *entry = luaA_enum_next_value_name_type(darktable.lua_state.state,
                                                             cur_elt->type_data.enum_data.enum_type, NULL);
          int entry_id = 0;
          while(entry)
          {
            if(strcmp(entry, cur_elt->type_data.enum_data.default_value) != 0)
            {
              gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(cur_elt->widget), entry);
              entry_id++;
              if(!strcmp(entry, dt_conf_get_string(pref_name)))
              {
                gtk_combo_box_set_active(GTK_COMBO_BOX(cur_elt->widget), entry_id);
              }
            }
            entry = luaA_enum_next_value_name_type(darktable.lua_state.state,
                                                   cur_elt->type_data.enum_data.enum_type, entry);
          }
        }
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"),
                 cur_elt->type_data.enum_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_enum), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_enum), cur_elt);
        break;
      case pref_dir:
        cur_elt->widget
            = gtk_file_chooser_button_new(_("select directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
        gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(cur_elt->widget), 20);
        gchar *str = dt_conf_get_string(pref_name);
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(cur_elt->widget), str);
        g_free(str);
        g_signal_connect(G_OBJECT(cur_elt->widget), "file-set", G_CALLBACK(callback_dir), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"),
                 cur_elt->type_data.dir_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_dir), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_dir), cur_elt);
        break;
      case pref_file:
        cur_elt->widget = gtk_file_chooser_button_new(_("select file"), GTK_FILE_CHOOSER_ACTION_OPEN);
        gtk_file_chooser_button_set_width_chars(GTK_FILE_CHOOSER_BUTTON(cur_elt->widget), 20);
        str = dt_conf_get_string(pref_name);
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(cur_elt->widget), str);
        g_free(str);
        g_signal_connect(G_OBJECT(cur_elt->widget), "file-set", G_CALLBACK(callback_file), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"),
                 cur_elt->type_data.file_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_file), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_file), cur_elt);
        break;
      case pref_string:
        cur_elt->widget = gtk_entry_new();
        str = dt_conf_get_string(pref_name);
        gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), str);
        g_free(str);
        g_signal_connect(G_OBJECT(cur_elt->widget), "activate", G_CALLBACK(callback_string), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"),
                 cur_elt->type_data.string_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_string), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_string), cur_elt);
        break;
      case pref_bool:
        cur_elt->widget = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cur_elt->widget), dt_conf_get_bool(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "toggled", G_CALLBACK(callback_bool), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"),
                 cur_elt->type_data.bool_data.default_value ? "true" : "false");
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_bool), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_bool), cur_elt);
        break;
      case pref_int:
        cur_elt->widget = gtk_spin_button_new_with_range(cur_elt->type_data.int_data.min,
                                                         cur_elt->type_data.int_data.max, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(cur_elt->widget), 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_int(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "value-changed", G_CALLBACK(callback_int), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%d'"),
                 cur_elt->type_data.int_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_int), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_int), cur_elt);
        break;
      case pref_float:
        cur_elt->widget = gtk_spin_button_new_with_range(cur_elt->type_data.float_data.min,
                                                         cur_elt->type_data.float_data.max,
                                                         cur_elt->type_data.float_data.step);
        // gtk_spin_button_set_digits(GTK_SPIN_BUTTON(cur_elt->widget), 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_float(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "value-changed", G_CALLBACK(callback_float), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%f'"),
                 cur_elt->type_data.float_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_float), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_float), cur_elt);
        break;
    }
    g_object_set(labelev, "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    g_object_set(cur_elt->widget, "tooltip-text", cur_elt->tooltip, (char *)NULL);
    gtk_grid_attach(GTK_GRID(grid), labelev, 0, line++, 1, 1);
    gtk_grid_attach_next_to(GTK_GRID(grid), cur_elt->widget, labelev, GTK_POS_RIGHT, 1, 1);
    cur_elt = cur_elt->next;
  }
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

  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L, "preferences");

  lua_pushcfunction(L, register_pref);
  lua_setfield(L, -2, "register");

  lua_pushcfunction(L, read_pref);
  lua_setfield(L, -2, "read");

  lua_pushcfunction(L, write_pref);
  lua_setfield(L, -2, "write");

  lua_pop(L, 1);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
