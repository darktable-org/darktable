/*
   This file is part of darktable,
   Copyright (C) 2015-2020 darktable developers.

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
#include "gui/gtk.h"
#include "lua/types.h"
#include "lua/widget/common.h"

static void entry_init(lua_State* L);
static void entry_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t entry_type = {
  .name = "entry",
  .gui_init = entry_init,
  .gui_cleanup = entry_cleanup,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};

static gboolean completion_match_func(GtkEntryCompletion *completion,
                                      const gchar *key,
                                      GtkTreeIter *iter,
                                      gpointer user_data)
{
  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  if(!model)
    return FALSE;

  const int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING)
    return FALSE;

  gchar *text = NULL;
  gtk_tree_model_get(model, iter, column, &text, -1);

  gboolean match = FALSE;

  if(text)
  {
    gchar *normalized_text = g_utf8_normalize(text, -1, G_NORMALIZE_ALL);
    gchar *normalized_key = g_utf8_normalize(key, -1, G_NORMALIZE_ALL);

    if(normalized_text && normalized_key)
    {
      gchar *casefold_text = g_utf8_casefold(normalized_text, -1);
      gchar *casefold_key = g_utf8_casefold(normalized_key, -1);

      if(casefold_text && casefold_key)
        match = g_strstr_len(casefold_text, -1, casefold_key) != NULL;

      g_free(casefold_text);
      g_free(casefold_key);
    }

    g_free(normalized_text);
    g_free(normalized_key);
    g_free(text);
  }

  return match;
}

static void entry_init(lua_State* L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);

  GtkEntryCompletion *completion = gtk_entry_completion_new();

  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, FALSE);
  gtk_entry_completion_set_match_func(completion, completion_match_func, NULL, NULL);
  gtk_entry_set_completion(GTK_ENTRY(entry->widget), completion);

  g_object_unref(completion);
}

static void entry_cleanup(lua_State* L,lua_widget widget)
{
}


static int text_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_entry_set_text(GTK_ENTRY(entry->widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_text(GTK_ENTRY(entry->widget)));
  return 1;
}

static int is_password_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const gboolean visibility = lua_toboolean(L,3);
    gtk_entry_set_visibility(GTK_ENTRY(entry->widget),!visibility);
    return 0;
  }
  lua_pushboolean(L,gtk_entry_get_visibility(GTK_ENTRY(entry->widget)));
  return 1;
}

static int placeholder_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * placeholder = luaL_checkstring(L,3);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->widget),placeholder);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_placeholder_text(GTK_ENTRY(entry->widget)));
  return 1;
}

static int editable_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  gboolean editable;
  if(lua_gettop(L) > 2) {
    editable = lua_toboolean(L,3);
    g_object_set(G_OBJECT(entry->widget), "editable", editable, (gchar *)0);
    return 0;
  }
  g_object_get(G_OBJECT(entry->widget),"editable",&editable,NULL);
  lua_pushboolean(L,editable);
  return 1;
}

static void changed_callback(GtkEntry *widget, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","changed",
      LUA_ASYNC_DONE);
}

static void activate_callback(GtkEntry *widget, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","activate",
      LUA_ASYNC_DONE);
}

static int completion_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L, lua_entry, &entry, 1);

  GtkEntryCompletion *completion = gtk_entry_get_completion(GTK_ENTRY(entry->widget));

  if(lua_gettop(L) > 2)
  {
    luaL_checktype(L, 3, LUA_TTABLE);

    GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);

    const lua_Integer len = luaL_len(L, 3);
    for(lua_Integer i = 1; i <= len; i++)
    {
      lua_geti(L, 3, i);
      const char *text = luaL_checkstring(L, -1);

      gtk_list_store_insert_with_values(model, NULL, -1, 0, text, -1);

      lua_pop(L, 1);
    }

    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));

    g_object_unref(model);
    return 0;
  }

  lua_newtable(L);

  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  if(!model)
    return 1;

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  lua_Integer index = 1;

  while(valid)
  {
    gchar *text = NULL;
    gtk_tree_model_get(model, &iter, 0, &text, -1);

    if(text)
    {
      lua_pushstring(L, text);
      lua_seti(L, -2, index++);
      g_free(text);
    }

    valid = gtk_tree_model_iter_next(model, &iter);
  }

  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_entry widget;
  luaA_to(L, lua_entry, &widget, 1);
  const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget->widget));
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_entry(lua_State* L)
{
  dt_lua_init_widget_type(L,&entry_type,lua_entry,GTK_TYPE_ENTRY);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_entry, "__tostring");

  dt_lua_widget_register_gtk_callback(L,lua_entry,"changed","changed_callback",G_CALLBACK(changed_callback));
  dt_lua_widget_register_gtk_callback(L,lua_entry,"activate","activate_callback",G_CALLBACK(activate_callback));

  lua_pushcfunction(L, completion_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "completion");

  lua_pushcfunction(L,text_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "text");

  lua_pushcfunction(L,is_password_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "is_password");

  lua_pushcfunction(L,placeholder_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "placeholder");

  lua_pushcfunction(L,editable_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "editable");

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

