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
/*
 * cmocka unit tests for dt_lua_metadata_script_tagname(), the rule that
 * turns the script and key names a Lua script passes to
 * darktable.metadata.register() into the metadata tag name that stores its
 * per-image values. Scripts never choose the tag name themselves, so this
 * function is the only place the naming scheme is enforced.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cmocka.h>

#include "lua/metadata.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// lua entry point around the function under test, so that the argument
// errors it raises unwind through lua_pcall instead of aborting the test
static int _tagname(lua_State *L)
{
  char *tagname = dt_lua_metadata_script_tagname(L, 1, 2);
  lua_pushstring(L, tagname);
  g_free(tagname);
  return 1;
}

// call the function with two strings (nil for NULL); the result or the
// error message is left on top of the stack
static int _call(lua_State *L, const char *script, const char *key)
{
  lua_settop(L, 0);
  lua_pushcfunction(L, _tagname);
  if(script)
    lua_pushstring(L, script);
  else
    lua_pushnil(L);
  if(key)
    lua_pushstring(L, key);
  else
    lua_pushnil(L);
  return lua_pcall(L, 2, 1, 0);
}

static int _setup(void **state)
{
  *state = luaL_newstate();
  return *state ? 0 : -1;
}

static int _teardown(void **state)
{
  lua_close(*state);
  return 0;
}

static void test_valid_names_compose_prefixed_tagname(void **state)
{
  lua_State *L = *state;

  assert_int_equal(_call(L, "myscript", "state"), LUA_OK);
  assert_string_equal(lua_tostring(L, -1), "Xmp.darktable.lua_myscript_state");

  assert_int_equal(_call(L, "Script_2", "Key_9"), LUA_OK);
  assert_string_equal(lua_tostring(L, -1), "Xmp.darktable.lua_Script_2_Key_9");

  assert_int_equal(_call(L, "_", "_"), LUA_OK);
  assert_string_equal(lua_tostring(L, -1), "Xmp.darktable.lua____");
}

static void test_invalid_names_raise(void **state)
{
  lua_State *L = *state;
  // anything that is not a letter, digit or underscore, including the
  // separators a script could use to escape the prefix
  const char *bad[] = { "", "my-script", "my.script", "my script", "Xmp.dc",
                        "a/b", "state\"", "\xc3\xbc", "a\nb", NULL };

  for(int i = 0; bad[i]; i++)
  {
    assert_int_not_equal(_call(L, bad[i], "key"), LUA_OK);
    assert_non_null(strstr(lua_tostring(L, -1), "script name"));

    assert_int_not_equal(_call(L, "script", bad[i]), LUA_OK);
    assert_non_null(strstr(lua_tostring(L, -1), "key"));
  }
}

static void test_missing_arguments_raise(void **state)
{
  lua_State *L = *state;

  assert_int_not_equal(_call(L, NULL, "key"), LUA_OK);
  assert_int_not_equal(_call(L, "script", NULL), LUA_OK);
  assert_int_not_equal(_call(L, NULL, NULL), LUA_OK);
}

static void test_subkey_carries_script_name(void **state)
{
  lua_State *L = *state;
  // darktable addresses metadata by the part after the last dot, so two
  // scripts using the same key must still get different subkeys
  char one[64];
  char two[64];

  assert_int_equal(_call(L, "one", "state"), LUA_OK);
  g_strlcpy(one, strrchr(lua_tostring(L, -1), '.') + 1, sizeof(one));
  assert_int_equal(_call(L, "two", "state"), LUA_OK);
  g_strlcpy(two, strrchr(lua_tostring(L, -1), '.') + 1, sizeof(two));

  assert_string_not_equal(one, two);
  assert_string_equal(one, "lua_one_state");
  assert_string_equal(two, "lua_two_state");
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_valid_names_compose_prefixed_tagname, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_invalid_names_raise, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_missing_arguments_raise, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_subkey_carries_script_name, _setup, _teardown),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
