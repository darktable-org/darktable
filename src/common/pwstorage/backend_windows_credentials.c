// This file is part of darktable
// Copyright (C) 2024 darktable developers.

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

#include "backend_windows_credentials.h"
#include "common/darktable.h"

#include <json-glib/json-glib.h>

#include <wincred.h>

const backend_windows_credentials_context_t *dt_pwstorage_windows_credentials_new()
{
  // no context needed for windows credentials manager
  return NULL;
}

void dt_pwstorage_windows_credentials_destroy(const backend_windows_credentials_context_t *context)
{
  // nothing to do here
}

gboolean dt_pwstorage_windows_credentials_set(const backend_windows_credentials_context_t *context,
                                              const gchar *slot,
                                              GHashTable *table)
{
  gboolean result = TRUE;
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, table);
  gpointer key, value;

  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_windows_credentials_set] storing (%s, %s)\n", (gchar *) key, (gchar *) value);

    gchar *target_name = g_strconcat("darktable - ", slot, NULL);

    // Parse server, username and password from JSON value
    // {"server":"www.example.com","username":"myuser","password":"mypassword"}
    JsonParser *json_parser = json_parser_new();

    if(json_parser_load_from_data(json_parser, value, -1, NULL) == FALSE)
    {
      dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_windows_credentials_set] unable to parse JSON from value (%s)\n", (gchar *) value);
      g_object_unref(json_parser);
      g_free(target_name);
      return FALSE;
    }

    // Read JSON
    JsonNode *json_root = json_parser_get_root(json_parser);
    JsonReader *json_reader = json_reader_new(json_root);

    GHashTable *v_attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    // Save each element as a key/value pair
    gint n_attributes = json_reader_count_members(json_reader);
    for(gint i = 0; i < n_attributes; i++)
    {
      if(json_reader_read_element(json_reader, i) == FALSE)
      {
        continue;
      }

      const gchar *k = json_reader_get_member_name(json_reader);
      const gchar *v = json_reader_get_string_value(json_reader);

      g_hash_table_insert(v_attributes, g_strdup(k), g_strdup(v));

      json_reader_end_element(json_reader);
    }

    g_object_unref(json_reader);
    g_object_unref(json_parser);

    gchar *server = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "server"));
    gchar *username = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "username"));
    gchar *password = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "password"));

    // delete existing entry
    CredDeleteW((LPWSTR) target_name, CRED_TYPE_GENERIC, 0);

    // create new entry
    const DWORD cbCreds = 1 + strlen(password);

    CREDENTIALW cred = { 0 };
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = (LPWSTR) target_name;
    cred.CredentialBlobSize = cbCreds;
    cred.CredentialBlob = (LPBYTE) password;
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    cred.Comment = (LPWSTR) server;
    cred.UserName = (LPWSTR) username;

    result = CredWriteW (&cred, 0);

    g_free(server);
    g_free(username);
    g_free(password);
    g_free(target_name);
  }

  return result;
}

GHashTable *dt_pwstorage_windows_credentials_get(const backend_windows_credentials_context_t *context, const gchar *slot)
{
  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  gchar *target_name = g_strconcat("darktable - ", slot, NULL);

  PCREDENTIALW pcred;
  gboolean ok = CredReadW((LPWSTR) target_name, CRED_TYPE_GENERIC, 0, &pcred);

  if(ok)
  {
    // build JSON
    JsonBuilder *json_builder = json_builder_new();
    json_builder_begin_object(json_builder);
    json_builder_set_member_name(json_builder, "server");
    json_builder_add_string_value(json_builder, (gchar*) pcred->Comment);
    json_builder_set_member_name(json_builder, "username");
    json_builder_add_string_value(json_builder, (gchar*) pcred->UserName);
    json_builder_set_member_name(json_builder, "password");
    json_builder_add_string_value(json_builder, (char*) pcred->CredentialBlob);
    json_builder_end_object(json_builder);

    // generate JSON
    JsonGenerator *json_generator = json_generator_new();
    json_generator_set_root(json_generator, json_builder_get_root(json_builder));
    gchar *json_data = json_generator_to_data(json_generator, 0);

    g_object_unref(json_generator);
    g_object_unref(json_builder);

    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_windows_credentials_get] reading (%s, %s)\n", (gchar*) pcred->Comment, json_data);

    g_hash_table_insert(table, g_strdup((gchar*) pcred->Comment), g_strdup(json_data));
  }

  CredFree (pcred);
  g_free(target_name);

  return table;
}
