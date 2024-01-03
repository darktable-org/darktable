// This file is part of darktable
// Copyright (C) 2023 darktable developers.

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

#include "backend_apple_keychain.h"
#include "common/darktable.h"

#include <json-glib/json-glib.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>


gboolean dt_pwstorage_apple_keychain_set(const backend_apple_keychain_context_t *context,
                                         const gchar *slot,
                                         GHashTable *table)
{
  OSStatus result = errSecSuccess;

  GHashTableIter iter;
  g_hash_table_iter_init(&iter, table);
  gpointer key, value;

  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_apple_keychain_set] storing (%s, %s)\n", (gchar *) key, (gchar *) value);

    gchar *lbl = g_strconcat("darktable - ", slot, NULL);
    const CFStringRef label = CFStringCreateWithCString(NULL, lbl, kCFStringEncodingUTF8);
    g_free(lbl);

    // Parse server, username and password from JSON value
    // {"server":"www.example.com","username":"myuser","password":"mypassword"}
    JsonParser *json_parser = json_parser_new();

    if(json_parser_load_from_data(json_parser, value, -1, NULL) == FALSE)
    {
      g_object_unref(json_parser);
      continue;
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

    gchar *attr_server = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "server"));
    gchar *attr_username = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "username"));
    gchar *attr_password = g_strdup((gchar *) g_hash_table_lookup(v_attributes, "password"));

    const CFStringRef server = CFStringCreateWithCString(NULL, attr_server, kCFStringEncodingUTF8);
    const CFStringRef username = CFStringCreateWithCString(NULL, attr_username, kCFStringEncodingUTF8);
    const CFStringRef password = CFStringCreateWithCString(NULL, attr_password, kCFStringEncodingUTF8);

    g_free(attr_server);
    g_free(attr_username);
    g_free(attr_password);

    // encrypted password
    CFDataRef password_enc = CFStringCreateExternalRepresentation(NULL, password, kCFStringEncodingUTF8, 0);
    CFRelease(password);

    g_hash_table_destroy(v_attributes);

    // seach for an existing entry in the keychain
    CFMutableDictionaryRef search_query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, NULL);
    CFDictionaryAddValue(search_query, kSecClass, kSecClassInternetPassword);
    CFDictionaryAddValue(search_query, kSecAttrLabel, label);
    CFDictionaryAddValue(search_query, kSecAttrServer, server);
    CFDictionaryAddValue(search_query, kSecAttrAccount, username);
    CFDictionaryAddValue(search_query, kSecMatchLimit, kSecMatchLimitOne);

    OSStatus search_result = SecItemCopyMatching(search_query, NULL);

    if (search_result == errSecItemNotFound)
    {
      // create new entry
      CFMutableDictionaryRef query = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, NULL);
      CFDictionaryAddValue(query, kSecClass, kSecClassInternetPassword);
      CFDictionaryAddValue(query, kSecAttrLabel, label);
      CFDictionaryAddValue(query, kSecAttrServer, server);
      CFDictionaryAddValue(query, kSecAttrAccount, username);
      CFDictionaryAddValue(query, kSecValueData, password_enc);

      result = SecItemAdd(query, NULL);

      CFRelease(query);
    }
    else
    {
      // update the existing entry
      CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, NULL, NULL);
      CFDictionaryAddValue(attributes, kSecValueData, password_enc);

      result = SecItemUpdate(search_query, attributes);

      CFRelease(attributes);
    }

    CFRelease(label);
    CFRelease(search_query);
    CFRelease(server);
    CFRelease(username);
    CFRelease(password_enc);
  }

  return result == errSecSuccess;
}

GHashTable *dt_pwstorage_apple_keychain_get(const backend_apple_keychain_context_t *context, const gchar *slot)
{
  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  const CFStringRef keys[] = {
    kSecClass,
    kSecAttrLabel,
    kSecMatchLimit,
    kSecReturnAttributes,
    kSecReturnRef
  };

  gchar *label = g_strconcat("darktable - ", slot, NULL);

  const CFTypeRef values[] = {
    kSecClassGenericPassword,
    CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8),
    kSecMatchLimitAll,
    kCFBooleanTrue,
    kCFBooleanTrue
  };

  CFDictionaryRef query  = CFDictionaryCreate(kCFAllocatorDefault, 
                                              (const void**) keys, 
                                              (const void**) values, 
                                              5, NULL, NULL);

  CFArrayRef items = NULL;
  OSStatus res = SecItemCopyMatching(query, (CFTypeRef *) &items);

  g_free(label);
  CFRelease(query);
  CFRelease(values[1]);

  if (res == errSecSuccess) {
    for (int i = 0; i < CFArrayGetCount(items); i++) {
      CFDictionaryRef item = CFArrayGetValueAtIndex(items, i);

      CFStringRef description = CFDictionaryGetValue(item, kSecAttrDescription);
      const gchar *key = CFStringGetCStringPtr(description, kCFStringEncodingUTF8);
      
      CFDataRef pwdata = CFDictionaryGetValue(item, kSecValueRef);
      CFStringRef pw = CFStringCreateFromExternalRepresentation(NULL, pwdata, kCFStringEncodingUTF8);
      const gchar *value =  CFStringGetCStringPtr(pw, kCFStringEncodingUTF8);
      
      dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_apple_keychain_get] reading (%s, %s)\n", key, value);

      g_hash_table_insert(table, g_strdup(key), g_strdup(value));

      CFRelease(pw);
    }

    CFRelease(items);
  }

  return table;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

