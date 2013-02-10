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
#include "lua/database.h"
#include "lua/stmt.h"
#include "lua/image.h"
#include "common/debug.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/film.h"

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

static int duplicate_image(lua_State *L) {
	int imgid = dt_lua_image_get(L,-1);
  dt_lua_image_push(L,dt_image_duplicate(imgid));
  return 1;
}


static int import_images(lua_State *L){
  char* full_name= realpath(luaL_checkstring(L,-1), NULL);
  int result;

  if (g_file_test(full_name, G_FILE_TEST_IS_DIR)) {
    result =dt_film_import_blocking(full_name);
    if(result == 0) {
      free(full_name);
      return luaL_error(L,"error while importing");
    }
  } else {
    dt_film_t new_film;
    dt_film_init(&new_film);
    char* dirname =g_path_get_dirname(full_name);
    result = dt_film_new(&new_film,dirname);
    if(result == 0) {
      free(full_name);
      dt_film_cleanup(&new_film);
      free(dirname);
      return luaL_error(L,"error while importing");
    }

    result =dt_image_import(new_film.id,full_name,TRUE);
    free(dirname);
    dt_film_cleanup(&new_film);
    if(result == 0) {
      free(full_name);
      return luaL_error(L,"error while importing");
    }
  }
  free(full_name);
  return 0;
}



int dt_lua_init_database(lua_State * L) {
	
  /* images */
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"database");
  lua_pushcfunction(L,duplicate_image);
  lua_setfield(L,-2,"duplicate");
  lua_pushcfunction(L,import_images);
  lua_setfield(L,-2,"import");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
