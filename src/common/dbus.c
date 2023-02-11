/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#include "common/dbus.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "common/file_location.h"

#ifdef USE_LUA
#include "lua/call.h"
#endif

/* Introspection data for the service we are exporting */
static const gchar introspection_xml[] = "<node>"
                                         "  <interface name='org.darktable.service.Remote'>"
                                         "    <method name='Quit' />"
                                         "    <method name='Open'>"
                                         "      <arg type='s' name='FileName' direction='in'/>"
                                         "      <arg type='i' name='id' direction='out' />"
                                         "    </method>"
#ifdef USE_LUA
                                         "    <method name='Lua'>"
                                         "      <arg type='s' name='Command' direction='in'/>"
                                         "      <arg type='s' name='Result' direction='out' />"
                                         "    </method>"
#endif
                                         "    <property type='s' name='DataDir' access='read'/>"
                                         "    <property type='s' name='ConfigDir' access='read'/>"
                                         "    <property type='b' name='LuaEnabled' access='read'/>"
                                         "  </interface>"
                                         "</node>";


#ifdef USE_LUA
static void dbus_lua_call_finished(lua_State* L,int result,void* data)
{
  GDBusMethodInvocation *invocation = (GDBusMethodInvocation*)data;
  if(result == LUA_OK)
  {
    if(lua_isnil(L, -1))
    {
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", ""));
    }
    else
    {
      const char *checkres = luaL_checkstring(L, -1);
      g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", checkres));
    }
  }
  else
  {
    const char *msg = luaL_checkstring(L, -1);
    g_dbus_method_invocation_return_dbus_error(invocation, "org.darktable.Error.LuaError", msg);
    dt_lua_check_print_error(L,result);
  }
}
#endif

static void _handle_method_call(GDBusConnection *connection, const gchar *sender, const gchar *object_path,
                                const gchar *interface_name, const gchar *method_name, GVariant *parameters,
                                GDBusMethodInvocation *invocation, gpointer user_data)
{
  if(!g_strcmp0(method_name, "Quit"))
  {
    g_dbus_method_invocation_return_value(invocation, NULL);
    dt_control_quit();
  }
  else if(!g_strcmp0(method_name, "Open"))
  {
    const gchar *filename;
    g_variant_get(parameters, "(&s)", &filename);
    int32_t id = dt_load_from_string(filename, TRUE, NULL);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", id));
  }
#ifdef USE_LUA
  else if(!g_strcmp0(method_name, "Lua"))
  {
    const gchar *command;
    g_variant_get(parameters, "(&s)", &command);
    dt_lua_async_call_string(command, 1,dbus_lua_call_finished,invocation);
    // we don't finish the invocation, the async task will do this for us
  }
#endif
}

// TODO: expose the conf? partly? completely?

static GVariant *_handle_get_property(GDBusConnection *connection, const gchar *sender,
                                      const gchar *object_path, const gchar *interface_name,
                                      const gchar *property_name, GError **error, gpointer user_data)
{
  GVariant *ret;

  ret = NULL;
  if(!g_strcmp0(property_name, "DataDir"))
  {
    gchar datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));
    ret = g_variant_new_string(datadir);
  }
  else if(!g_strcmp0(property_name, "ConfigDir"))
  {
    gchar configdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(configdir, sizeof(configdir));
    ret = g_variant_new_string(configdir);
  }
  else if(!g_strcmp0(property_name, "LuaEnabled"))
  {
#ifdef USE_LUA
    ret = g_variant_new_boolean(TRUE);
#else
    ret = g_variant_new_boolean(FALSE);
#endif
  }
  return ret;
}

// static gboolean
// _handle_set_property(GDBusConnection  *connection,
//                      const gchar      *sender,
//                      const gchar      *object_path,
//                      const gchar      *interface_name,
//                      const gchar      *property_name,
//                      GVariant         *value,
//                      GError          **error,
//                      gpointer          user_data)
// {
//   return *error == NULL;
// }

static const GDBusInterfaceVTable interface_vtable = { _handle_method_call, _handle_get_property,
                                                       //   _handle_set_property
                                                       NULL };

static void _on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t *)user_data;

  dbus->registration_id
      = g_dbus_connection_register_object(connection, "/darktable", dbus->introspection_data->interfaces[0],
                                          &interface_vtable, dbus, /* user_data */
                                          NULL,                    /* user_data_free_func */
                                          NULL);                   /* GError** */

  if(dbus->registration_id == 0)
    dbus->connected
        = 0; // technically we are connected, but we are not exporting anything. or something like that
}

static void _on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t *)user_data;
  dbus->connected = 1;
}

static void _on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t *)user_data;
  dbus->connected = 0;
}

struct dt_dbus_t *dt_dbus_init()
{
  dt_dbus_t *dbus = (dt_dbus_t *)g_malloc0(sizeof(dt_dbus_t));
  if(!dbus) return NULL;

  dbus->introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);

  if(dbus->introspection_data == NULL) return dbus;

  dbus->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  "org.darktable.service", // FIXME
                                  G_BUS_NAME_OWNER_FLAGS_NONE, _on_bus_acquired, _on_name_acquired,
                                  _on_name_lost, dbus, NULL);

  dbus->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
  g_object_set(G_OBJECT(dbus->dbus_connection), "exit-on-close", FALSE, (gchar *)0);

  return dbus;
}

void dt_dbus_destroy(const dt_dbus_t *dbus)
{
  g_bus_unown_name(dbus->owner_id);
  g_dbus_node_info_unref(dbus->introspection_data);
  if(dbus->dbus_connection)
    g_object_unref(G_OBJECT(dbus->dbus_connection));

  g_free((dt_dbus_t *)dbus);
}

gboolean dt_dbus_connected(const dt_dbus_t *dbus)
{
  return dbus->connected;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
