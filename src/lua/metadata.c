/*
   This file is part of darktable,
   Copyright (C) 2026 darktable developers.

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
#include "common/metadata.h"
#include "control/control.h"
#include "control/signal.h"
#include "gui/gtkentry.h"
#include "lua/metadata.h"
#include "lua/glist.h"
#include "lua/types.h"

static int exists(lua_State *L)
{
  gboolean result = false;
  dt_metadata_t *md;

  const char *tagname = luaL_checkstring(L, 1);

  md = dt_metadata_get_metadata_by_tagname(tagname);

  if(md)
    result = true;

  lua_pushboolean(L, result);
  return 1;
}

// the names end up in an XMP tag name and in darktablerc keys, so only
// characters that are safe in both are accepted
static gboolean _valid_name(const char *name)
{
  if(!name || !*name) return FALSE;
  for(const char *p = name; *p; p++)
    if(!g_ascii_isalnum(*p) && *p != '_') return FALSE;
  return TRUE;
}

char *dt_lua_metadata_script_tagname(lua_State *L,
                                     const int script_index,
                                     const int key_index)
{
  const char *script = luaL_checkstring(L, script_index);
  const char *key = luaL_checkstring(L, key_index);
  if(!_valid_name(script))
    luaL_argerror(L, script_index,
                  "script name may only contain letters, digits and underscores");
  if(!_valid_name(key))
    luaL_argerror(L, key_index,
                  "key may only contain letters, digits and underscores");
  // the part after the last dot must be unique across all metadata (see
  // dt_metadata_get_tag_subkey), so the script name is folded into it
  return g_strdup_printf("Xmp.darktable.lua_%s_%s", script, key);
}

static gboolean _option(lua_State *L,
                        const int index,
                        const char *field,
                        const gboolean fallback)
{
  gboolean result = fallback;
  lua_getfield(L, index, field);
  if(!lua_isnil(L, -1)) result = lua_toboolean(L, -1);
  lua_pop(L, 1);
  return result;
}

static int _register(lua_State *L)
{
  gboolean visible = FALSE;
  gboolean priv = TRUE;
  if(!lua_isnoneornil(L, 3))
  {
    luaL_checktype(L, 3, LUA_TTABLE);
    visible = _option(L, 3, "visible", visible);
    priv = _option(L, 3, "private", priv);
  }
  char *tagname = dt_lua_metadata_script_tagname(L, 1, 2);

  gboolean added = FALSE;
  gboolean failed = FALSE;
  dt_pthread_mutex_lock(&darktable.metadata_threadsafe);
  if(!dt_metadata_get_metadata_by_tagname(tagname))
  {
    dt_metadata_t *md = calloc(1, sizeof(dt_metadata_t));
    md->tagname = tagname;
    // the display name is unique in the database too; the subkey is
    // unique by construction so it doubles as the name
    md->name = g_strdup(dt_metadata_get_tag_subkey(tagname));
    md->internal = FALSE;
    md->visible = visible;
    md->priv = priv;
    md->display_order = g_list_length(dt_metadata_get_list());
    added = dt_metadata_add_metadata(md);
    if(added)
    {
      tagname = NULL; // owned by the metadata list now
      dt_metadata_sort();
      // the completion model is a gtk list store, so only touch it from
      // the gui thread; scripts run from jobs pick it up at next start
      if(darktable.gui && pthread_equal(darktable.control->gui_thread, pthread_self()))
        dt_gtkentry_variables_add_metadata(md);
    }
    else
    {
      g_free(md->name);
      free(md);
      failed = TRUE;
    }
  }
  dt_pthread_mutex_unlock(&darktable.metadata_threadsafe);

  if(added)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_METADATA_CHANGED, DT_METADATA_SIGNAL_PREF_CHANGED);
  if(failed)
  {
    lua_pushfstring(L, "could not register metadata key '%s'", tagname);
    g_free(tagname);
    return lua_error(L);
  }
  g_free(tagname);
  return 0;
}

int dt_lua_init_metadata(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L, "metadata");

  lua_pushcfunction(L, exists);
  lua_setfield(L, -2, "exists");
  lua_pushcfunction(L, _register);
  lua_setfield(L, -2, "register");

  lua_pop(L, 1);

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
