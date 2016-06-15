// This file is part of darktable
//
// Copyright (c) 2014 Moritz Lipp <mlq@pwmt.org>.
// Copyright (c) 2016 tobias ellinghaus <me@houz.org>.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "backend_libsecret.h"
#include "control/conf.h"

#include "version.h"

#include <libsecret/secret.h>
#include <glib.h>
#include <json-glib/json-glib.h>

// change this to SECRET_COLLECTION_SESSION for non-permanent storage
#define SECRET_COLLECTION_DARKTABLE SECRET_COLLECTION_DEFAULT

#define EMPTY_STRING(string) !*(string)

static const SecretSchema *secret_darktable_get_schema(void) G_GNUC_CONST;
#define SECRET_SCHEMA_DARKTABLE secret_darktable_get_schema()

static GHashTable *secret_to_attributes(gchar *value);
static gchar *attributes_to_secret(GHashTable *attributes);

static const SecretSchema *secret_darktable_get_schema(void)
{
  static const SecretSchema darktable_schema = {
    "org.darktable.Password",
    SECRET_SCHEMA_NONE,
    {
      { "slot", SECRET_SCHEMA_ATTRIBUTE_STRING }, { "magic", SECRET_SCHEMA_ATTRIBUTE_STRING }, { "NULL", 0 },
    }
  };

  return &darktable_schema;
}

const backend_libsecret_context_t *dt_pwstorage_libsecret_new()
{
  GError *error = NULL;
  backend_libsecret_context_t *context = calloc(1, sizeof(backend_libsecret_context_t));
  if(context == NULL)
  {
    return NULL;
  }

  SecretService *secret_service = secret_service_get_sync(SECRET_SERVICE_LOAD_COLLECTIONS, NULL, &error);
  if(error)
  {
    fprintf(stderr, "[pwstorage_libsecret] error connecting to Secret Service: %s\n", error->message);
    g_error_free(error);
    if(secret_service) g_object_unref(secret_service);
    dt_pwstorage_libsecret_destroy(context);
    return NULL;
  }

  if(secret_service)
    g_object_unref(secret_service);

  return context;
}

void dt_pwstorage_libsecret_destroy(const backend_libsecret_context_t *context)
{
  free((backend_libsecret_context_t *)context);
}

gboolean dt_pwstorage_libsecret_set(const backend_libsecret_context_t *context, const gchar *slot,
                                    GHashTable *attributes)
{
  GError *error = NULL;
  if(context == NULL || slot == NULL || EMPTY_STRING(slot) || attributes == NULL)
  {
    return FALSE;
  }

  /* Convert attributes to secret */
  char *secret_value = attributes_to_secret(attributes);
  if(secret_value == NULL)
  {
    return FALSE;
  }

  gchar *label = g_strdup_printf("darktable@%s", slot);
  if(!label)
  {
    g_free(secret_value);
    return FALSE;
  }

  gboolean res = secret_password_store_sync(SECRET_SCHEMA_DARKTABLE,
                                            SECRET_COLLECTION_DARKTABLE,
                                            label,
                                            secret_value,
                                            NULL,
                                            &error,
                                            "slot", slot,
                                            "magic", PACKAGE_NAME,
                                            NULL);
  if(!res)
  {
    fprintf(stderr, "[pwstorage_libsecret] error storing password: %s\n", error->message);
    g_error_free(error);
  }

  g_free(secret_value);
  g_free(label);

  return res;
}

GHashTable *dt_pwstorage_libsecret_get(const backend_libsecret_context_t *context, const gchar *slot)
{
  GError *error = NULL;
  GHashTable *attributes;
  gchar *secret_value = NULL;

  if(context == NULL || slot == NULL || EMPTY_STRING(slot))
  {
    goto error;
  }

  secret_value = secret_password_lookup_sync(SECRET_SCHEMA_DARKTABLE,
                                             NULL,
                                             &error,
                                             "slot", slot,
                                             "magic", PACKAGE_NAME,
                                             NULL);
  if(error)
  {
    fprintf(stderr, "[pwstorage_libsecret] error retrieving password: %s\n", error->message);
    g_error_free(error);
    goto error;
  }

  attributes = secret_to_attributes(secret_value);

  if(attributes == NULL)
  {
    goto error;
  }

  goto end;

error:
  attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

end:
  g_free(secret_value);
  return attributes;
}

static void append_pair_to_json(gpointer key, gpointer value, gpointer data)
{
  JsonBuilder *json_builder = (JsonBuilder *)data;

  json_builder_set_member_name(json_builder, (char *)key);
  json_builder_add_string_value(json_builder, (char *)value);
}

static gchar *attributes_to_secret(GHashTable *attributes)
{
  /* Build JSON */
  JsonBuilder *json_builder = json_builder_new();
  json_builder_begin_object(json_builder);
  g_hash_table_foreach(attributes, append_pair_to_json, json_builder);
  json_builder_end_object(json_builder);

  /* Generate JSON */
  JsonGenerator *json_generator = json_generator_new();
  json_generator_set_root(json_generator, json_builder_get_root(json_builder));
  gchar *json_data = json_generator_to_data(json_generator, 0);

  g_object_unref(json_generator);
  g_object_unref(json_builder);

  return json_data;
}

static GHashTable *secret_to_attributes(gchar *secret)
{
  if(secret == NULL || EMPTY_STRING(secret))
  {
    return NULL;
  }

  /* Parse JSON from data */
  JsonParser *json_parser = json_parser_new();

  if(json_parser_load_from_data(json_parser, secret, -1, NULL) == FALSE)
  {
    g_object_unref(json_parser);
    return NULL;
  }

  /* Read JSON */
  JsonNode *json_root = json_parser_get_root(json_parser);
  JsonReader *json_reader = json_reader_new(json_root);

  GHashTable *attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  /* Save each element as an attribute pair */
  gint n_attributes = json_reader_count_members(json_reader);
  for(gint i = 0; i < n_attributes; i++)
  {
    if(json_reader_read_element(json_reader, i) == FALSE)
    {
      continue;
    }

    const gchar *key = json_reader_get_member_name(json_reader);
    const gchar *value = json_reader_get_string_value(json_reader);

    g_hash_table_insert(attributes, (gpointer)g_strdup(key), (gpointer)g_strdup(value));

    json_reader_end_element(json_reader);
  }

  g_object_unref(json_reader);
  g_object_unref(json_parser);

  return attributes;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
