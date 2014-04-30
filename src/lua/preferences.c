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

static const char *pref_type_name[] =
{
  "string",
  "bool",
  "integer",
  "float",
  NULL
};

typedef enum
{
  pref_string,
  pref_bool,
  pref_int,
  pref_float,
} pref_type;


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
} all_data_t;
typedef struct pref_element
{
  char*script;
  char*name;
  char* label;
  char* tooltip;
  pref_type type;
  struct pref_element* next;
  all_data_t type_data;
  // ugly hack, written every time the dialog is displayed and unused when not displayed
  // value is set when window is created, it is correct when window is destroyed, but is invalid out of that context
  GtkWidget * widget;

} pref_element;

static void destroy_pref_element(pref_element*elt)
{
  free(elt->script);
  free(elt->name);
  free(elt->label);
  free(elt->tooltip);
  // don't free the widget field
  switch(elt->type)
  {
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

static pref_element* pref_list=NULL;

static void get_pref_name(char *tgt,size_t size,const char*script,const char*name)
{
  snprintf(tgt,size,"lua/%s/%s",script,name);
}

static int register_pref(lua_State*L)
{
  // to avoid leaks, we need to first get all params (which could raise errors) then alloc and fill the struct
  // this is complicated, but needed
  pref_element * built_elt = malloc(sizeof(pref_element));
  memset(built_elt,0,sizeof(pref_element));
  int cur_param =1;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->script = strdup(lua_tostring(L,cur_param));
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->name = strdup(lua_tostring(L,cur_param));
  cur_param++;

  int i;
  const char* type_name = lua_tostring(L,cur_param);
  if(!type_name) goto error;
  for (i=0; pref_type_name[i]; i++)
    if (strcmp(pref_type_name[i], type_name) == 0)
    {
      built_elt->type =i;
      break;
    }
  if(!pref_type_name[i]) goto error;
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->label = strdup(lua_tostring(L,cur_param));
  cur_param++;

  if(!lua_isstring(L,cur_param)) goto error;
  built_elt->tooltip = strdup(lua_tostring(L,cur_param));
  cur_param++;

  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),built_elt->script,built_elt->name);
  switch(built_elt->type)
  {
    case pref_string:
      if(!lua_isstring(L,cur_param)) goto error;
      built_elt->type_data.string_data.default_value = strdup(lua_tostring(L,cur_param));
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) dt_conf_set_string(pref_name,built_elt->type_data.string_data.default_value);
      break;
    case pref_bool:
      if(!lua_isboolean(L,cur_param)) goto error;
      built_elt->type_data.bool_data.default_value = lua_toboolean(L,cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) dt_conf_set_bool(pref_name,built_elt->type_data.bool_data.default_value);
      break;
    case pref_int:
      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.int_data.default_value = lua_tointeger(L,cur_param);
      cur_param++;

      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.int_data.min = lua_tointeger(L,cur_param);
      cur_param++;

      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.int_data.max = lua_tointeger(L,cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) dt_conf_set_int(pref_name,built_elt->type_data.int_data.default_value);
      break;
    case pref_float:
      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.float_data.default_value = lua_tonumber(L,cur_param);
      cur_param++;

      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.float_data.min = lua_tonumber(L,cur_param);
      cur_param++;

      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.float_data.max = lua_tonumber(L,cur_param);
      cur_param++;

      if(!lua_isnumber(L,cur_param)) goto error;
      built_elt->type_data.float_data.step = lua_tonumber(L,cur_param);
      cur_param++;

      if(!dt_conf_key_exists(pref_name)) dt_conf_set_float(pref_name,built_elt->type_data.float_data.default_value);
      break;
  }


  built_elt->next = pref_list;
  pref_list = built_elt;
  return 0;
error:
  destroy_pref_element(built_elt);
  return luaL_argerror(L,cur_param,NULL);
}
static int read_pref(lua_State*L)
{
  const char *script = luaL_checkstring(L,1);
  const char *name = luaL_checkstring(L,2);
  const char* type_name = luaL_checkstring(L,3);
  int i;
  for (i=0; pref_type_name[i]; i++)
    if (strcmp(pref_type_name[i], type_name) == 0)
    {
      break;
    }
  if(!pref_type_name[i]) luaL_argerror(L,3,NULL);

  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),script,name);
  switch(i)
  {
    case pref_string:
    {
      char *str = dt_conf_get_string(pref_name);
      lua_pushstring(L,str);
      g_free(str);
      break;
    }
    case pref_bool:
      lua_pushboolean(L,dt_conf_get_bool(pref_name));
      break;
    case pref_int:
      lua_pushnumber(L,dt_conf_get_int(pref_name));
      break;
    case pref_float:
      lua_pushnumber(L,dt_conf_get_float(pref_name));
      break;
  }
  return 1;
}

static int write_pref(lua_State*L)
{
  const char *script = luaL_checkstring(L,1);
  const char *name = luaL_checkstring(L,2);
  const char* type_name = luaL_checkstring(L,3);
  int i;
  for (i=0; pref_type_name[i]; i++)
    if (strcmp(pref_type_name[i], type_name) == 0)
    {
      break;
    }
  if(!pref_type_name[i]) luaL_argerror(L,3,NULL);

  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),script,name);
  switch(i)
  {
    case pref_string:
      dt_conf_set_string(pref_name,luaL_checkstring(L,4));
      break;
    case pref_bool:
      luaL_checktype(L,4,LUA_TBOOLEAN);
      dt_conf_set_bool(pref_name,lua_toboolean(L,4));
      break;
    case pref_int:
      dt_conf_set_int(pref_name,luaL_checkinteger(L,4));
      break;
    case pref_float:
      dt_conf_set_float(pref_name,luaL_checknumber(L,4));
      break;
  }
  return 0;
}


static void callback_string(GtkWidget *widget, pref_element*cur_elt )
{
  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
  dt_conf_set_string(pref_name,gtk_entry_get_text(GTK_ENTRY(widget)));
}
static void callback_bool(GtkWidget *widget, pref_element*cur_elt )
{
  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
  dt_conf_set_bool(pref_name,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}
static void callback_int(GtkWidget *widget, pref_element*cur_elt )
{
  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
  dt_conf_set_int(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}
static void callback_float(GtkWidget *widget, pref_element*cur_elt )
{
  char pref_name[1024];
  get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
  dt_conf_set_float(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
}

static void response_callback_string(GtkDialog *dialog, gint response_id, pref_element* cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    dt_conf_set_string(pref_name,gtk_entry_get_text(GTK_ENTRY(cur_elt->widget)));
  }
}
static void response_callback_bool(GtkDialog *dialog, gint response_id, pref_element* cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    dt_conf_set_bool(pref_name, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cur_elt->widget)));
  }
}
static void response_callback_int(GtkDialog *dialog, gint response_id, pref_element* cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    dt_conf_set_int(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
  }
}
static void response_callback_float(GtkDialog *dialog, gint response_id, pref_element* cur_elt)
{
  if(response_id == GTK_RESPONSE_ACCEPT)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    dt_conf_set_float(pref_name, gtk_spin_button_get_value(GTK_SPIN_BUTTON(cur_elt->widget)));
  }
}

static gboolean reset_widget_string (GtkWidget *label, GdkEventButton *event, pref_element*cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), cur_elt->type_data.string_data.default_value);
    return TRUE;
  }
  return FALSE;
}
static gboolean reset_widget_bool (GtkWidget *label, GdkEventButton *event, pref_element*cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), cur_elt->type_data.string_data.default_value);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cur_elt->widget), cur_elt->type_data.bool_data.default_value);
    return TRUE;
  }
  return FALSE;
}
static gboolean reset_widget_int (GtkWidget *label, GdkEventButton *event, pref_element*cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), cur_elt->type_data.int_data.default_value);
    return TRUE;
  }
  return FALSE;
}
static gboolean reset_widget_float (GtkWidget *label, GdkEventButton *event, pref_element*cur_elt)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), cur_elt->type_data.float_data.default_value);
    return TRUE;
  }
  return FALSE;
}
void init_tab_lua (GtkWidget *dialog, GtkWidget *tab)
{
  if(!pref_list) return; // no option registered => don't create the tab
  char tooltip[1024];
  GtkWidget  *label, *labelev;
  GtkWidget *hbox = gtk_hbox_new(5, FALSE);
  GtkWidget *vbox1 = gtk_vbox_new(5, TRUE);
  GtkWidget *vbox2 = gtk_vbox_new(5, TRUE);
  gtk_box_pack_start(GTK_BOX(hbox), vbox1, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), vbox2, FALSE, FALSE, 0);
  GtkWidget *alignment = gtk_alignment_new(0.5, 0.0, 1.0, 0.0);
  gtk_alignment_set_padding(GTK_ALIGNMENT(alignment), 20, 20, 20, 20);
  gtk_container_add(GTK_CONTAINER(alignment), hbox);
  gtk_notebook_append_page(GTK_NOTEBOOK(tab), alignment, gtk_label_new(_("lua options")));

  pref_element* cur_elt = pref_list;
  while(cur_elt)
  {
    char pref_name[1024];
    get_pref_name(pref_name,sizeof(pref_name),cur_elt->script,cur_elt->name);
    label = gtk_label_new(cur_elt->label);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
    labelev = gtk_event_box_new();
    gtk_widget_add_events(labelev, GDK_BUTTON_PRESS_MASK);
    gtk_container_add(GTK_CONTAINER(labelev), label);
    switch(cur_elt->type)
    {
      case pref_string:
        cur_elt->widget = gtk_entry_new();
        gchar *str = dt_conf_get_string(pref_name);
        gtk_entry_set_text(GTK_ENTRY(cur_elt->widget), str);
        g_free(str);
        g_signal_connect(G_OBJECT(cur_elt->widget), "activate", G_CALLBACK(callback_string), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"), cur_elt->type_data.string_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_string), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_string), cur_elt);
        break;
      case pref_bool:
        cur_elt->widget = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cur_elt->widget), dt_conf_get_bool(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "toggled", G_CALLBACK(callback_bool), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%s'"), cur_elt->type_data.bool_data.default_value?"true":"false");
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_bool), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_bool), cur_elt);
        break;
      case pref_int:
        cur_elt->widget = gtk_spin_button_new_with_range(cur_elt->type_data.int_data.min, cur_elt->type_data.int_data.max, 1);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(cur_elt->widget), 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_int(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "value-changed", G_CALLBACK(callback_int), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%d'"), cur_elt->type_data.int_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_int), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_int), cur_elt);
        break;
      case pref_float:
        cur_elt->widget = gtk_spin_button_new_with_range(cur_elt->type_data.float_data.min, cur_elt->type_data.float_data.max, cur_elt->type_data.float_data.step);
        //gtk_spin_button_set_digits(GTK_SPIN_BUTTON(cur_elt->widget), 0);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cur_elt->widget), dt_conf_get_float(pref_name));
        g_signal_connect(G_OBJECT(cur_elt->widget), "value-changed", G_CALLBACK(callback_float), cur_elt);
        snprintf(tooltip, sizeof(tooltip), _("double click to reset to `%f'"), cur_elt->type_data.float_data.default_value);
        g_signal_connect(G_OBJECT(labelev), "button-press-event", G_CALLBACK(reset_widget_float), cur_elt);
        g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(response_callback_float), cur_elt);
        break;
    }
    g_object_set(labelev,  "tooltip-text", tooltip, (char *)NULL);
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(labelev), FALSE);
    g_object_set(cur_elt->widget, "tooltip-text", cur_elt->tooltip, (char *)NULL);
    gtk_box_pack_start(GTK_BOX(vbox1), labelev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox2), cur_elt->widget, FALSE, FALSE, 0);
    cur_elt = cur_elt->next;
  }
}

int dt_lua_init_preferences(lua_State * L)
{

  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"preferences");

  lua_pushcfunction(L,register_pref);
  lua_setfield(L,-2,"register");

  lua_pushcfunction(L,read_pref);
  lua_setfield(L,-2,"read");

  lua_pushcfunction(L,write_pref);
  lua_setfield(L,-2,"write");

  lua_pop(L,1);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

