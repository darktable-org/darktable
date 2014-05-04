/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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
#include "common/dbus.h"
#include "control/control.h"
#include "control/conf.h"

#include <gio/gio.h>


typedef struct dt_dbus_t
{
  int connected;

  GDBusNodeInfo *introspection_data;
  guint owner_id;
  guint registration_id;
} dt_dbus_t;

/* Introspection data for the service we are exporting */
static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.darktable.service.Remote'>"
  "    <method name='Quit' />"
  "    <method name='Open'>"
  "      <arg type='s' name='FileName' direction='in'/>"
  "      <arg type='i' name='id' direction='out' />"
  "    </method>"
  "    <property type='s' name='DataDir' access='read'/>"
  "    <property type='s' name='ConfigDir' access='read'/>"
  "  </interface>"
  "</node>";


static void
_handle_method_call(GDBusConnection       *connection,
                    const gchar           *sender,
                    const gchar           *object_path,
                    const gchar           *interface_name,
                    const gchar           *method_name,
                    GVariant              *parameters,
                    GDBusMethodInvocation *invocation,
                    gpointer               user_data)
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
    int32_t id = dt_load_from_string(filename, TRUE);
    g_dbus_method_invocation_return_value(invocation, g_variant_new("(i)", id));
  }
}

// TODO: expose the conf? partly? completely?

static GVariant *
_handle_get_property(GDBusConnection  *connection,
                     const gchar      *sender,
                     const gchar      *object_path,
                     const gchar      *interface_name,
                     const gchar      *property_name,
                     GError          **error,
                     gpointer          user_data)
{
  GVariant *ret;

  ret = NULL;
  if(!g_strcmp0(property_name, "DataDir"))
  {
    gchar datadir[PATH_MAX];
    dt_loc_get_datadir(datadir, sizeof(datadir));
    ret = g_variant_new_string(datadir);
  }
  else if(!g_strcmp0(property_name, "ConfigDir"))
  {
    gchar configdir[PATH_MAX];
    dt_loc_get_user_config_dir(configdir, sizeof(configdir));
    ret = g_variant_new_string(configdir);
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

static const GDBusInterfaceVTable interface_vtable =
{
  _handle_method_call,
  _handle_get_property,
//   _handle_set_property
  NULL
};

static void _on_bus_acquired (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t*)user_data;

  dbus->registration_id = g_dbus_connection_register_object(connection,
                          "/darktable",
                          dbus->introspection_data->interfaces[0],
                          &interface_vtable,
                          dbus,  /* user_data */
                          NULL,  /* user_data_free_func */
                          NULL); /* GError** */

  if(dbus->registration_id == 0)
    dbus->connected = 0; // technically we are connected, but we are not exporting anything. or something like that
}

static void _on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t*)user_data;
  dbus->connected = 1;
}

static void _on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
  dt_dbus_t *dbus = (dt_dbus_t*)user_data;
  dbus->connected = 0;
}

struct dt_dbus_t *dt_dbus_init()
{
  dt_dbus_t *dbus = (dt_dbus_t *)g_malloc(sizeof(dt_dbus_t));
  if(!dbus) return NULL;
  memset(dbus, 0, sizeof(dt_dbus_t));

  dbus->introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);

  if(dbus->introspection_data == NULL)
    return dbus;

  dbus->owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                  "org.darktable.service", // FIXME
                                  G_BUS_NAME_OWNER_FLAGS_NONE,
                                  _on_bus_acquired,
                                  _on_name_acquired,
                                  _on_name_lost,
                                  dbus,
                                  NULL);

  return dbus;
}

void dt_dbus_destroy(const dt_dbus_t *dbus)
{
  g_bus_unown_name(dbus->owner_id);
  g_dbus_node_info_unref(dbus->introspection_data);

  g_free((dt_dbus_t*)dbus);
}

gboolean dt_dbus_connected(const dt_dbus_t *dbus)
{
  return dbus->connected;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
