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
#include "libintl.h"
#include "lua/lua.h"
// function used by the lua interpreter to load darktable

static int lua_gettext(lua_State*L)
{
  const char* msgid = luaL_checkstring(L,1);
  lua_pushstring(L,gettext(msgid));
  return 1;
}

static int lua_dgettext(lua_State*L)
{
  const char* domainname = luaL_checkstring(L,1);
  const char* msgid = luaL_checkstring(L,2);
  lua_pushstring(L,dgettext(domainname,msgid));
  return 1;
}

static int lua_ngettext(lua_State*L)
{
  const char* msgid = luaL_checkstring(L,1);
  const char* msgid_plural = luaL_checkstring(L,2);
  int n = luaL_checkinteger(L,3);
  lua_pushstring(L,ngettext(msgid,msgid_plural,n));
  return 1;
}

static int lua_dngettext(lua_State*L)
{
  const char* domainname = luaL_checkstring(L,1);
  const char* msgid = luaL_checkstring(L,2);
  const char* msgid_plural = luaL_checkstring(L,3);
  int n = luaL_checkinteger(L,4);
  lua_pushstring(L,dngettext(domainname,msgid,msgid_plural,n));
  return 1;
}

static int lua_bindtextdomain(lua_State*L)
{
  const char* domainname = luaL_checkstring(L,1);
  const char* dirname = luaL_checkstring(L,2);
  bindtextdomain(domainname,dirname);
  return 0;
}


int dt_lua_init_gettext(lua_State *L)
{

  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"gettext");

  lua_pushcfunction(L,lua_gettext);
  lua_setfield(L,-2,"gettext");
  lua_pushcfunction(L,lua_dgettext);
  lua_setfield(L,-2,"dgettext");
  //lua_pushcfunction(L,lua_dcgettext);
  //lua_setfield(L,-2,"dcgettext");
  lua_pushcfunction(L,lua_ngettext);
  lua_setfield(L,-2,"ngettext");
  lua_pushcfunction(L,lua_dngettext);
  lua_setfield(L,-2,"dngettext");
  //lua_pushcfunction(L,lua_dcngettext);
  //lua_setfield(L,-2,"dcngettext");
  lua_pushcfunction(L,lua_bindtextdomain);
  lua_setfield(L,-2,"bindtextdomain");

  lua_pop(L,1);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
