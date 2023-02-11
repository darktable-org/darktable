// This file is part of darktable
//
// darktable is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// darktable is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with darktable.  If not, see <http://www.gnu.org/licenses/>.

// This file contains code taken from
// http://src.chromium.org/viewvc/chrome/trunk/src/chrome/browser/password_manager/native_backend_kwallet_x.cc?revision=50034&view=markup

// The original copyright notice was as follows:

// Copyright (c) 2010 The Chromium Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Since the BSD license permits I hereby release this stuff under GPL.
// See the top of this file.

// Connect do dbus interface of KWallet
// http://websvn.kde.org/trunk/KDE/kdelibs/kdeui/util/org.kde.KWallet.xml?revision=1054210&view=markup
// http://websvn.kde.org/trunk/KDE/kdelibs/kdeui/util/kwallet.cpp?revision=1107541&view=markup

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "backend_kwallet.h"
#include "control/conf.h"

#include <string.h>

static const gchar *app_id = "darktable";
static const gchar *kwallet_folder = "darktable credentials";

static const gchar *kwallet_service_name = "org.kde.kwalletd";
static const gchar *kwallet_path = "/modules/kwalletd";
static const gchar *kwallet_interface = "org.kde.KWallet";
static const gchar *klauncher_service_name = "org.kde.klauncher";
static const gchar *klauncher_path = "/KLauncher";
static const gchar *klauncher_interface = "org.kde.KLauncher";

// Invalid handle returned by get_wallet_handle().
static const gint invalid_kwallet_handle = -1;

// http://doc.qt.nokia.com/4.6/datastreamformat.html
// A QString has the length in the first 4 bytes, then the string in UTF-16 encoding
// Has to be stored as big endian!
static gchar *char2qstring(const gchar *in, gsize *size)
{
  glong read, written;
  GError *error = NULL;
  gunichar2 *out = g_utf8_to_utf16(in, -1, &read, &written, &error);

  if(error)
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet] ERROR: error converting string: %s\n", error->message);
    g_free(out);
    g_error_free(error);
    return NULL;
  }

  glong i;
  for(i = 0; i < written; ++i)
  {
    out[i] = g_htons(out[i]);
  }

  guint bytes = sizeof(gunichar2) * written;
  guint BE_bytes = GUINT_TO_BE(bytes);
  *size = sizeof(guint) + bytes;
  gchar *result = g_malloc(*size);

  memcpy(result, &BE_bytes, sizeof(guint));
  memcpy(result + sizeof(guint), out, bytes);

  g_free(out);
  return result;
}

// For cleaner code ...
static gboolean check_error(GError *error)
{
  if(error)
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet] ERROR: failed to complete kwallet call: %s\n",
             error->message);
    g_error_free(error);
    return TRUE;
  }
  return FALSE;
}

// If kwalletd isn't running: try to start it
static gboolean start_kwallet(backend_kwallet_context_t *context)
{
  GError *error = NULL;

  // Sadly kwalletd doesn't use DBUS activation, so we have to make a call to
  // klauncher to start it.
  /*
   * signature:
   *
   * in  s serviceName,
   * in  as urls,
   * in  as envs,
   * in  s startup_id,
   * in  b blind,
   *
   * out i arg_0,
   * out s dbusServiceName,
   * out s error,
   * out i pid
  */
  GVariant *ret = g_dbus_connection_call_sync(context->connection, klauncher_service_name, klauncher_path,
                                              klauncher_interface, "start_service_by_desktop_name",
                                              g_variant_new("(sasassb)", "kwalletd", NULL, NULL, "", FALSE),
                                              NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(check_error(error))
  {
    return FALSE;
  }

  GVariant *child = g_variant_get_child_value(ret, 2);
  gchar *error_string = g_variant_dup_string(child, NULL);
  g_variant_unref(child);
  g_variant_unref(ret);

  if(error_string && error_string[0] != '\0')
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet] ERROR: error launching kwalletd: %s\n", error_string);
    g_free(error_string);
    return FALSE;
  }

  g_free(error_string);

  return TRUE;
}

// Initialize the connection to KWallet
static gboolean init_kwallet(backend_kwallet_context_t *context)
{
  GError *error = NULL;

  // Make a proxy to KWallet.
  if(context->proxy) g_object_unref(context->proxy);

  context->proxy = g_dbus_proxy_new_sync(context->connection, G_DBUS_PROXY_FLAGS_NONE, NULL,
                                         kwallet_service_name, kwallet_path, kwallet_interface, NULL, &error);

  if(check_error(error))
  {
    context->proxy = NULL;
    return FALSE;
  }

  // Check KWallet is enabled.
  GVariant *ret
      = g_dbus_proxy_call_sync(context->proxy, "isEnabled", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(!ret) return FALSE;
  GVariant *child = g_variant_get_child_value(ret, 0);
  gboolean is_enabled = g_variant_get_boolean(child);
  g_variant_unref(child);
  g_variant_unref(ret);

  if(check_error(error) || !is_enabled) return FALSE;

  // Get the wallet name.
  g_free(context->wallet_name);

  ret = g_dbus_proxy_call_sync(context->proxy, "networkWallet", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
                               &error);

  child = g_variant_get_child_value(ret, 0);
  context->wallet_name = g_variant_dup_string(child, NULL);
  g_variant_unref(child);
  g_variant_unref(ret);

  if(check_error(error) || !context->wallet_name)
  {
    context->wallet_name = NULL; // yes, it's stupid. go figure.
    return FALSE;
  }

  return TRUE;
}

// General initialization. Takes care of all the other stuff.
const backend_kwallet_context_t *dt_pwstorage_kwallet_new()
{
  backend_kwallet_context_t *context = g_malloc0(sizeof(backend_kwallet_context_t));

  GError *error = NULL;
  context->connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if(check_error(error))
  {
    g_free(context);
    return NULL;
  }

  if(!init_kwallet(context))
  {
    // kwalletd may not be running. Try to start it and try again.
    if(!start_kwallet(context) || !init_kwallet(context))
    {
      g_object_unref(context->connection);
      g_free(context);
      return NULL;
    }
  }

  return context;
}

/** Cleanup and destroy kwallet backend context. */
void dt_pwstorage_kwallet_destroy(const backend_kwallet_context_t *context)
{
  backend_kwallet_context_t *c = (backend_kwallet_context_t *)context;
  g_object_unref(c->connection);
  g_object_unref(c->proxy);
  g_free(c->wallet_name);
  g_free(c);
}

// get the handle for connections to KWallet
static int get_wallet_handle(const backend_kwallet_context_t *context)
{
  // Open the wallet.
  int handle = invalid_kwallet_handle;
  GError *error = NULL;

  /* signature:
   *
   * in  s wallet,
   * in  x wId,
   * in  s appid,
   *
   * out i arg_0
   */
  GVariant *ret = g_dbus_proxy_call_sync(context->proxy, "open",
                                         g_variant_new("(sxs)", context->wallet_name, 0LL, app_id),
                                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(check_error(error))
  {
    g_variant_unref(ret);
    return invalid_kwallet_handle;
  }

  GVariant *child = g_variant_get_child_value(ret, 0);
  handle = g_variant_get_int32(child);
  g_variant_unref(child);
  g_variant_unref(ret);

  // Check if our folder exists.
  gboolean has_folder = FALSE;

  /* signature:
   *
   * in  i handle,
   * in  s folder,
   * in  s appid,
   *
   * out b arg_0
   */
  ret = g_dbus_proxy_call_sync(context->proxy, "hasFolder",
                               g_variant_new("(iss)", handle, kwallet_folder, app_id), G_DBUS_CALL_FLAGS_NONE,
                               -1, NULL, &error);

  if(check_error(error))
  {
    g_variant_unref(ret);
    return invalid_kwallet_handle;
  }

  child = g_variant_get_child_value(ret, 0);
  has_folder = g_variant_get_boolean(child);
  g_variant_unref(child);
  g_variant_unref(ret);

  // Create it if it didn't.
  if(!has_folder)
  {

    gboolean success = FALSE;

    /* signature:
     *
     * in  i handle,
     * in  s folder,
     * in  s appid,
     *
     * out b arg_0
     */
    ret = g_dbus_proxy_call_sync(context->proxy, "createFolder",
                                 g_variant_new("(iss)", handle, kwallet_folder, app_id),
                                 G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

    if(check_error(error))
    {
      g_variant_unref(ret);
      return invalid_kwallet_handle;
    }

    child = g_variant_get_child_value(ret, 0);
    success = g_variant_get_boolean(child);
    g_variant_unref(child);
    g_variant_unref(ret);

    if(!success) return invalid_kwallet_handle;
  }

  return handle;
}

// Store (key,value) pairs from a GHashTable in the kwallet.
// Every 'slot' has to take care of it's own data.
gboolean dt_pwstorage_kwallet_set(const backend_kwallet_context_t *context, const gchar *slot,
                                  GHashTable *table)
{
  printf("slot %s\n", slot);

  GArray *byte_array = g_array_new(FALSE, FALSE, sizeof(gchar));

  GHashTableIter iter;
  g_hash_table_iter_init(&iter, table);
  gpointer key, value;

  guint size = g_hash_table_size(table);

  size = GINT_TO_BE(size);

  g_array_append_vals(byte_array, &size, sizeof(guint) / sizeof(gchar));

  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet_set] storing (%s, %s)\n", (gchar *)key, (gchar *)value);
    gsize length;
    gchar *new_key = char2qstring(key, &length);
    if(new_key == NULL)
    {
      g_free(g_array_free(byte_array, FALSE));
      return FALSE;
    }
    g_array_append_vals(byte_array, new_key, length);
    g_free(new_key);

    gchar *new_value = char2qstring(value, &length);
    if(new_value == NULL)
    {
      g_free(g_array_free(byte_array, FALSE));
      return FALSE;
    }
    g_array_append_vals(byte_array, new_value, length);
    g_free(new_value);
  }

  int wallet_handle = get_wallet_handle(context);
  GError *error = NULL;

  /* signature:
   *
   * in  i handle,
   * in  s folder,
   * in  s key,
   * in  ay value,
   * in  s appid,
   *
   * out i arg_0
   */
  GVariant *ret = g_dbus_proxy_call_sync(
      context->proxy, "writeMap",
      g_variant_new("(iss@ays)", wallet_handle, kwallet_folder, slot,
                    g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING, byte_array->data, byte_array->len,
                                            TRUE, g_free, byte_array->data),
                    app_id),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  g_array_free(byte_array, FALSE);

  if(check_error(error))
  {
    g_variant_unref(ret);
    return FALSE;
  }

  GVariant *child = g_variant_get_child_value(ret, 0);
  int return_code = g_variant_get_int32(child);
  g_variant_unref(child);
  g_variant_unref(ret);

  if(return_code != 0)
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet_set] Warning: bad return code %d from kwallet\n",
             return_code);

  return return_code == 0;
}

static gchar *array2string(const gchar *pos, guint *length)
{
  memcpy(length, pos, sizeof(gint));
  *length = GUINT_FROM_BE(*length);
  pos += sizeof(gint);
  guint j;

  gunichar2 *tmp_string = (gunichar2 *)malloc(*length);
  memcpy(tmp_string, pos, *length);

  for(j = 0; j < ((*length) / sizeof(gunichar2)); j++)
  {
    tmp_string[j] = g_ntohs(tmp_string[j]);
  }

  glong read, written;
  GError *error = NULL;
  gchar *out = g_utf16_to_utf8(tmp_string, *length / sizeof(gunichar2), &read, &written, &error);

  free(tmp_string);

  if(error)
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet] ERROR: Error converting string: %s\n", error->message);
    g_error_free(error);
    return NULL;
  }

  *length += sizeof(gint);
  return out;
}

// Get the (key,value) pairs back from KWallet.
GHashTable *dt_pwstorage_kwallet_get(const backend_kwallet_context_t *context, const gchar *slot)
{
  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GError *error = NULL;

  // Is there an entry in the wallet?
  gboolean has_entry = FALSE;
  int wallet_handle = get_wallet_handle(context);

  /* signature:
   *
   * in  i handle,
   * in  s folder,
   * in  s key,
   * in  s appid,
   *
   * out b arg_0
   */
  GVariant *ret = g_dbus_proxy_call_sync(context->proxy, "hasEntry",
                                         g_variant_new("(isss)", wallet_handle, kwallet_folder, slot, app_id),
                                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(check_error(error))
  {
    g_variant_unref(ret);
    return table;
  }

  GVariant *child = g_variant_get_child_value(ret, 0);
  has_entry = g_variant_get_boolean(child);
  g_variant_unref(child);
  g_variant_unref(ret);

  if(!has_entry) return table;

  /* signature:
   *
   * in  i handle,
   * in  s folder,
   * in  s key,
   * in  s appid,
   *
   * out a{sv} arg_0)
   */
  ret = g_dbus_proxy_call_sync(context->proxy, "readMapList",
                               g_variant_new("(isss)", wallet_handle, kwallet_folder, slot, app_id),
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if(check_error(error))
  {
    g_variant_unref(ret);
    return table;
  }

  child = g_variant_get_child_value(ret, 0);

  // we are only interested in the first child. i am not even sure that there can legally be more than one
  if(g_variant_n_children(child) < 1)
  {
    g_variant_unref(child);
    g_variant_unref(ret);
    return table;
  }

  GVariant *element = g_variant_get_child_value(child, 0);
  GVariant *v = NULL;
  g_variant_get(element, "{sv}", NULL, &v);

  const gchar *byte_array = g_variant_get_data(v);
  if(!byte_array)
  {
    g_variant_unref(v);
    g_variant_unref(element);
    g_variant_unref(child);
    g_variant_unref(ret);
    return table;
  }

  int entries = GINT_FROM_BE(*((int *)byte_array));
  byte_array += sizeof(gint);

  for(int i = 0; i < entries; i++)
  {
    guint length;
    gchar *key = array2string(byte_array, &length);

    byte_array += length;

    gchar *value = array2string(byte_array, &length);

    byte_array += length;

    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_kwallet_get] reading (%s, %s)\n", (gchar *)key, (gchar *)value);

    g_hash_table_insert(table, key, value);
  }

  g_variant_unref(v);
  g_variant_unref(element);
  g_variant_unref(child);
  g_variant_unref(ret);

  return table;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
