// This file is part of darktable
//
// Copyright (c) 2014 Moritz Lipp <mlq@pwmt.org>.
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

#include <libsecret/secret.h>
#include <glib.h>
#include <json-glib/json-glib.h>

#define DARKTABLE_KEYRING	PACKAGE_NAME

#define GFOREACH(item, list) for(GList *__glist = list; __glist && (item = __glist->data, true); __glist = __glist->next)

#define EMPTY_STRING(string) !*(string)

const SecretSchema * secret_darktable_get_schema (void) G_GNUC_CONST;
#define SECRET_SCHEMA_DARKTABLE  secret_darktable_get_schema ()

static GHashTable* secret_to_attributes(SecretValue* value);
static SecretValue* attributes_to_secret(GHashTable* attributes);

const SecretSchema *
secret_darktable_get_schema (void)
{
  static const SecretSchema darktable_schema = {
    "org.darktable.Password", SECRET_SCHEMA_NONE,
    {
      { "slot", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "magic", SECRET_SCHEMA_ATTRIBUTE_STRING },
      { "NULL", 0 },
    }
  };

  return &darktable_schema;
}

const backend_libsecret_context_t*
dt_pwstorage_libsecret_new()
{
  backend_libsecret_context_t* context = g_malloc(sizeof(backend_libsecret_context_t));

  context->secret_service = secret_service_get_sync(SECRET_SERVICE_NONE, NULL, NULL);
  if (context->secret_service == NULL) {
    return NULL;
  }

  /* Ensure to load all collections */
  if (secret_service_load_collections_sync(context->secret_service, NULL, NULL) == FALSE) {
    dt_pwstorage_libsecret_destroy(context);
    return NULL;
  }

  GList* collections = secret_service_get_collections(context->secret_service);
  SecretCollection* item = NULL;

  gboolean collection_exists = FALSE;
  GFOREACH(item, collections) {
    if (g_strcmp0(secret_collection_get_label(item), DARKTABLE_KEYRING)) {
      context->secret_collection = item;
      collection_exists = TRUE;
      break;
    }
  }

  if (collection_exists == FALSE) {
    context->secret_collection =
      secret_collection_create_sync(context->secret_service, DARKTABLE_KEYRING,
          NULL, SECRET_COLLECTION_CREATE_NONE, NULL, NULL);

    if (context->secret_collection == NULL) {
      dt_pwstorage_libsecret_destroy(context);
      return NULL;
    }
  }

  return context;
}

void
dt_pwstorage_libsecret_destroy(const backend_libsecret_context_t *context)
{
  if (context == NULL) {
    return;
  }

  if (context->secret_service != NULL) {
    g_object_unref(context->secret_service);
  }

  if (context->secret_collection != NULL) {
    g_object_unref(context->secret_collection);
  }

  g_free((backend_libsecret_context_t*) context);
}

gboolean dt_pwstorage_libsecret_set(const backend_libsecret_context_t* context,
    const gchar* slot, GHashTable* attributes)
{
  if (context == NULL || slot == NULL || EMPTY_STRING(slot) || attributes == NULL) {
    return FALSE;
  }

  /* Convert attributes to secret */
  SecretValue* secret_value = attributes_to_secret(attributes);

  if (secret_value == NULL) {
    return FALSE;
  }

  /* Insert slot as a attribute */
  GHashTable* secret_attributes = secret_attributes_build(SECRET_SCHEMA_DARKTABLE,
      "slot", slot, "magic", PACKAGE_NAME, NULL);

  /* Save the item */
  gchar* label = g_strdup_printf("darktable@%s", slot);

  GError* error = NULL;
  SecretItem* item = secret_item_create_sync(
      context->secret_collection,
      SECRET_SCHEMA_DARKTABLE,
      secret_attributes,
      label,
      secret_value,
      SECRET_ITEM_CREATE_REPLACE,
      NULL,
      &error);

  if (item == NULL) {
    return FALSE;
  } else {
    return TRUE;
  }
}

GHashTable* dt_pwstorage_libsecret_get(const backend_libsecret_context_t*
    context, const gchar* slot)
{
  if (context == NULL || slot == NULL || EMPTY_STRING(slot)) {
    goto error_out;
  }

  /* Setup search attributes */
  GHashTable* secret_attributes = secret_attributes_build(SECRET_SCHEMA_DARKTABLE,
      "slot", slot, "magic", PACKAGE_NAME, NULL);

  /* Search for item */
  GError* error = NULL;
  GList* items = secret_collection_search_sync(
      context->secret_collection,
      SECRET_SCHEMA_DARKTABLE,
      secret_attributes,
      SECRET_SEARCH_NONE,
      NULL,
      &error);

  /* Since the search flag is set to SECRET_SEARCH_NONE only one
   * matching item is returned. */
  if (items == NULL || g_list_length(items) != 1) {
    goto error_out;
  }

  SecretItem* item = (SecretItem*) g_list_nth_data(items, 0);

  if (item == NULL) {
    goto error_out;
  }

  /* Load secret */
  secret_item_load_secret_sync(item, NULL, NULL);

  SecretValue* value = secret_item_get_secret(item);

  if (value == NULL) {
    goto error_out;
  }

  GHashTable* attributes = secret_to_attributes(value);

  if (attributes == NULL) {
    secret_value_unref(value);
    goto error_out;
  }

  secret_value_unref(value);

  return attributes;

error_out:

  return g_hash_table_new(g_str_hash, g_str_equal);
}

static void append_pair_to_json(gpointer key, gpointer value, gpointer data)
{
  JsonBuilder* json_builder = (JsonBuilder*) data;

  json_builder_set_member_name(json_builder, (char*) key);
  json_builder_add_string_value(json_builder, (char*) value);
}

static SecretValue* attributes_to_secret(GHashTable* attributes)
{
  /* Build JSON */
  JsonBuilder* json_builder = json_builder_new();
  json_builder_begin_object(json_builder);
  g_hash_table_foreach(attributes, append_pair_to_json, json_builder);
  json_builder_end_object(json_builder);

  /* Generate JSON */
  JsonGenerator* json_generator = json_generator_new();
  json_generator_set_root(json_generator, json_builder_get_root(json_builder));
  gchar *json_data = json_generator_to_data(json_generator, 0);

  /* Create secret */
  SecretValue* secret = secret_value_new(json_data, -1, "text/plain");

  g_object_unref(json_generator);
  g_object_unref(json_builder);

  return secret;
}

static GHashTable* secret_to_attributes(SecretValue* secret)
{
  if (secret == NULL) {
    return NULL;
  }

  /* Parse JSON from data */
  JsonParser* json_parser = json_parser_new();

  if (json_parser_load_from_data(json_parser, secret_value_get_text(secret), -1, NULL) == FALSE) {
    g_object_unref(json_parser);
    return NULL;
  }

  /* Read JSON */
  JsonNode* json_root = json_parser_get_root(json_parser);
  JsonReader* json_reader = json_reader_new(json_root);

  GHashTable* attributes = g_hash_table_new(g_str_hash, g_str_equal);

  /* Save each element as an attribute pair */
  gint n_attributes = json_reader_count_members(json_reader);
  for (gint i = 0; i < n_attributes; i++) {
    if (json_reader_read_element(json_reader, i) == FALSE) {
      continue;
    }

    const gchar* key = json_reader_get_member_name(json_reader);
    const gchar* value = json_reader_get_string_value(json_reader);

    g_hash_table_insert(attributes, (gpointer) g_strdup(key), (gpointer) g_strdup(value));

    json_reader_end_element(json_reader);
  }

  g_object_unref(json_reader);
  g_object_unref(json_parser);

  return attributes;
}
