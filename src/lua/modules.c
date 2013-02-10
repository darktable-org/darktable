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
#include "lua/modules.h"
#include "lua/types.h"
typedef enum {
  GET_EXTENSION,
  LAST_FORMAT_FIELD
} style_fields;
const char *style_fields_name[] = {
  "extension",
  NULL
};
luaA_Type dt_lua_init_format_internal(lua_State* L, char*type_name,size_t size){
  luaA_type_add(type_name,size);
  luaA_Type my_type = dt_lua_init_type_internal(L,type_name,NULL,NULL,NULL,size);
  luaA_struct_typeid(L,my_type);
  return my_type;
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
