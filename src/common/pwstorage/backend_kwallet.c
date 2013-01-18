// This file is part of darktable
// Copyright (c) 2010 Tobias Ellinghaus <houz@gmx.de>.

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

static const gchar* app_id = "darktable";
static const gchar* kwallet_folder = "darktable credentials";

static const gchar* kwallet_service_name = "org.kde.kwalletd";
static const gchar* kwallet_path = "/modules/kwalletd";
static const gchar* kwallet_interface = "org.kde.KWallet";
static const gchar* klauncher_service_name = "org.kde.klauncher";
static const gchar* klauncher_path = "/KLauncher";
static const gchar* klauncher_interface = "org.kde.KLauncher";

// Invalid handle returned by get_wallet_handle().
static const gint invalid_kwallet_handle = -1;

// Unfortunately I need this one for init: we call functions which need the context, yet the global context doesn't exist before we return.
backend_kwallet_context_t* _context;

// http://doc.qt.nokia.com/4.6/datastreamformat.html
// A QString has the length in the first 4 bytes, then the string in UTF-16 encoding
// Has to be stored as big endian!
static gchar* char2qstring(const gchar* in, gsize* size)
{
  glong read, written;
  GError* error = NULL;
  gunichar2* out = g_utf8_to_utf16(in, -1, &read, &written, &error);

  if(error)
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet] ERROR: error converting string: %s\n", error->message);
    g_error_free(error);
    return NULL;
  }

  glong i;
  for(i=0; i<written; ++i)
  {
    out[i] = g_htons(out[i]);
  }

  guint bytes = sizeof(gunichar2)*written;
  guint BE_bytes = GUINT_TO_BE(bytes);
  *size = sizeof(guint)+bytes;
  gchar* result = g_malloc(*size);

  memcpy(result, &BE_bytes, sizeof(guint));
  memcpy(result+sizeof(guint), out, bytes);

  return result;
}

// For cleaner code ...
static gboolean CheckError(GError* error)
{
  if (error)
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet] ERROR: failed to complete kwallet call: %s\n", error->message);
    g_error_free(error);
    error = NULL;
    return TRUE;
  }
  return FALSE;
}

// If kwalletd isn't running: try to start it
static gboolean start_kwallet()
{
  // Sadly kwalletd doesn't use DBUS activation, so we have to make a call to
  // klauncher to start it.
  DBusGProxy* klauncher_proxy =
    dbus_g_proxy_new_for_name(_context->connection, klauncher_service_name,
                              klauncher_path, klauncher_interface);

  gchar* empty_string_list = NULL;
  gint ret = 1;
  gchar* error_string = NULL;
  GError* error = NULL;
  dbus_g_proxy_call(klauncher_proxy, "start_service_by_desktop_name", &error,
                    G_TYPE_STRING,  "kwalletd",          // serviceName
                    G_TYPE_STRV,    &empty_string_list,  // urls
                    G_TYPE_STRV,    &empty_string_list,  // envs
                    G_TYPE_STRING,  "",                  // startup_id
                    G_TYPE_BOOLEAN, FALSE,               // blind
                    G_TYPE_INVALID,
                    G_TYPE_INT,     &ret,                // result
                    G_TYPE_STRING,  NULL,                // dubsName
                    G_TYPE_STRING,  &error_string,              // error
                    G_TYPE_INT,     NULL,                // pid
                    G_TYPE_INVALID);

  if (error_string && *error_string)
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet] ERROR: error launching kwalletd: %s\n", error);
    ret = 1;  // Make sure we return false after freeing.
  }

  g_free(error_string);
  g_object_unref(klauncher_proxy);

  if (CheckError(error) || ret != 0)
    return FALSE;
  return TRUE;
}

// Initialize the connection to KWallet
static gboolean init_kwallet()
{
  GError* error = NULL;
  // Make a proxy to KWallet.
  _context->proxy = dbus_g_proxy_new_for_name(_context->connection, kwallet_service_name,
                    kwallet_path, kwallet_interface);

  // Check KWallet is enabled.
  gboolean is_enabled = FALSE;
  dbus_g_proxy_call(_context->proxy, "isEnabled", &error,
                    G_TYPE_INVALID,
                    G_TYPE_BOOLEAN, &is_enabled,
                    G_TYPE_INVALID);
  if (CheckError(error) || !is_enabled)
    return FALSE;

  // Get the wallet name.
  dbus_g_proxy_call(_context->proxy, "networkWallet", &error,
                    G_TYPE_INVALID,
                    G_TYPE_STRING, &(_context->wallet_name),
                    G_TYPE_INVALID);
  if (CheckError(error) || !_context->wallet_name)
    return FALSE;

  return TRUE;
}

// General initialization. Takes care of all the other stuff.
const backend_kwallet_context_t* dt_pwstorage_kwallet_new()
{
  _context = g_malloc(sizeof(backend_kwallet_context_t));

  // NULL the context
  memset(_context, 0, sizeof(backend_kwallet_context_t));

#if !GLIB_CHECK_VERSION(2, 32, 0)
  // Initialize threading in dbus-glib - it should be fine for
  // dbus_g_thread_init to be called multiple times.
  if (!g_thread_supported())
    g_thread_init(NULL);
#endif
  dbus_g_thread_init();

  GError* error = NULL;
  // Get a connection to the session bus.
  _context->connection = dbus_g_bus_get(DBUS_BUS_SESSION, &error);

  if (CheckError(error))
    return NULL;

  if (!init_kwallet())
  {
    // kwalletd may not be running. Try to start it and try again.
    if (!start_kwallet() || !init_kwallet())
      return NULL;
  }

  return _context;
}

/** Cleanup and destroy kwallet backend context. */
// void dt_pwstorage_kwallet_destroy(const backend_kwallet_context_t *context){
// 	if(context->error != NULL){
// 		g_error_free(context->error);
// 	}
// }

// get the handle for connections to KWallet
static int get_wallet_handle()
{
  // Open the wallet.
  int handle = invalid_kwallet_handle;
  GError* error = NULL;
  dbus_g_proxy_call(_context->proxy, "open", &error,
                    G_TYPE_STRING, _context->wallet_name,  // wallet
                    G_TYPE_INT64,  0LL,                   // wid
                    G_TYPE_STRING, app_id,                // appid
                    G_TYPE_INVALID,
                    G_TYPE_INT,    &handle,
                    G_TYPE_INVALID);
  if (CheckError(error) || handle == invalid_kwallet_handle)
    return invalid_kwallet_handle;

  // Check if our folder exists.
  gboolean has_folder = FALSE;
  dbus_g_proxy_call(_context->proxy, "hasFolder", &error,
                    G_TYPE_INT,    handle,          // handle
                    G_TYPE_STRING, kwallet_folder,  // folder
                    G_TYPE_STRING, app_id,          // appid
                    G_TYPE_INVALID,
                    G_TYPE_BOOLEAN, &has_folder,
                    G_TYPE_INVALID);
  if (CheckError(error))
    return invalid_kwallet_handle;

  // Create it if it didn't.
  if (!has_folder)
  {
    gboolean success = FALSE;
    dbus_g_proxy_call(_context->proxy, "createFolder", &error,
                      G_TYPE_INT,    handle,          // handle
                      G_TYPE_STRING, kwallet_folder,  // folder
                      G_TYPE_STRING, app_id,          // appid
                      G_TYPE_INVALID,
                      G_TYPE_BOOLEAN, &success,
                      G_TYPE_INVALID);
    if (CheckError(error) || !success)
      return invalid_kwallet_handle;
  }

  return handle;
}

// Store (key,value) pairs from a GHashTable in the kwallet.
// Every 'slot' has to take care of it's own data.
gboolean dt_pwstorage_kwallet_set(const gchar* slot, GHashTable* table)
{
  _context = (backend_kwallet_context_t*)(darktable.pwstorage->backend_context);
  GArray* byte_array = g_array_new(FALSE, FALSE, sizeof(gchar));

  GHashTableIter iter;
  g_hash_table_iter_init (&iter, table);
  gpointer key, value;

  guint size = g_hash_table_size(table);

  size = GINT_TO_BE(size);

  g_array_append_vals(byte_array, &size, sizeof(guint)/sizeof(gchar));

  while (g_hash_table_iter_next (&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet_set] storing (%s, %s)\n",(gchar*)key, (gchar*)value);
    gsize length;
    gchar* new_key = char2qstring(key, &length);
    if(new_key == NULL)
      return FALSE;
    g_array_append_vals(byte_array, new_key, length);
    g_free(new_key);
    new_key = NULL;

    gchar* new_value = char2qstring(value, &length);
    if(new_value == NULL)
      return FALSE;
    g_array_append_vals(byte_array, new_value, length);
    g_free(new_value);
    new_value = NULL;
  }

  int wallet_handle = get_wallet_handle();
  int ret = 0;
  GError* error = NULL;

  dbus_g_proxy_call(_context->proxy, "writeMap", &error,
                    G_TYPE_INT,     wallet_handle,         // handle
                    G_TYPE_STRING,  kwallet_folder,        // folder
                    G_TYPE_STRING,  slot,                  // key
                    DBUS_TYPE_G_UCHAR_ARRAY, byte_array,   // value
                    G_TYPE_STRING,  app_id,                // appid
                    G_TYPE_INVALID,
                    G_TYPE_INT,     &ret,
                    G_TYPE_INVALID);

  g_array_free(byte_array, TRUE);

  if (CheckError(error))
    return FALSE;

  if (ret != 0)
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet_set] Warning: bad return code %d from kwallet\n", ret);

  return ret == 0;
}

static gchar* array2string(gchar* pos, guint* length)
{
  memcpy(length, pos, sizeof(gint));
  *length = GUINT_FROM_BE(*length);
  pos += sizeof(gint);
  guint j;

  for(j=0; j<((*length)/sizeof(gunichar2)); j++)
  {
    ((gunichar2*)pos)[j] = g_ntohs(((gunichar2*)pos)[j]);
  }

  glong read, written;
  GError* error = NULL;
  gchar* out = g_utf16_to_utf8((gunichar2*)pos, *length/sizeof(gunichar2), &read, &written, &error);

  if(error)
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet] ERROR: Error converting string: %s\n", error->message);
    g_error_free(error);
    return NULL;
  }

  *length += sizeof(gint);
  return out;
}

// Get the (key,value) pairs back from KWallet.
GHashTable* dt_pwstorage_kwallet_get(const gchar* slot)
{
  _context = (backend_kwallet_context_t*)(darktable.pwstorage->backend_context);
  GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);
  GError* error = NULL;

  // Is there an entry in the wallet?
  gboolean has_entry = FALSE;
  int wallet_handle = get_wallet_handle();
  dbus_g_proxy_call(_context->proxy, "hasEntry", &error,
                    G_TYPE_INT,     wallet_handle,         // handle
                    G_TYPE_STRING,  kwallet_folder,        // folder
                    G_TYPE_STRING,  slot,                  // key
                    G_TYPE_STRING,  app_id,                // appid
                    G_TYPE_INVALID,
                    G_TYPE_BOOLEAN, &has_entry,
                    G_TYPE_INVALID);

  if (CheckError(error) || !has_entry)
    return table;

  GArray* byte_array = NULL;

  dbus_g_proxy_call(_context->proxy, "readMap", &error,
                    G_TYPE_INT,     wallet_handle,         // handle
                    G_TYPE_STRING,  kwallet_folder,        // folder
                    G_TYPE_STRING,  slot,                  // key
                    G_TYPE_STRING,  app_id,                // appid
                    G_TYPE_INVALID,
                    DBUS_TYPE_G_UCHAR_ARRAY, &byte_array,
                    G_TYPE_INVALID);

  if (CheckError(error) || !byte_array || !byte_array->len)
    return table;

  gint entries;
  memcpy(&entries, byte_array->data, sizeof(gint));
  entries = GINT_FROM_BE(entries);

  gchar* pos = byte_array->data + sizeof(gint);

  gint i;
  for(i=0; i<entries; ++i)
  {
    guint length;
    gchar* key = array2string(pos, &length);

    pos += length;

    gchar* value = array2string(pos, &length);

    pos += length;

    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_kwallet_get] reading (%s, %s)\n",(gchar*)key, (gchar*)value);

    g_hash_table_insert(table, key, value);
  }

  g_array_free(byte_array, TRUE);

  return table;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
