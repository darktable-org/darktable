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
  OSStatus result;

  GHashTableIter iter;
  g_hash_table_iter_init(&iter, table);
  gpointer key, value;

  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[pwstorage_apple_keychain_set] storing (%s, %s)\n", (gchar *) key, (gchar *) value);

    gchar *label = g_strconcat("darktable@", slot, NULL);
    const CFStringRef pw = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);

    // seach for an existing entry in the keychain
    const CFStringRef search_keys[] = {
      kSecClass,
      kSecAttrLabel,
      kSecAttrAccount,
      kSecMatchLimit
    };

    const CFTypeRef search_values[] = {
      kSecClassGenericPassword,
      CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8),
      CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8),
      kSecMatchLimitOne
    };

    CFDictionaryRef search_query  = CFDictionaryCreate(kCFAllocatorDefault, 
                                                       (const void**) search_keys, 
                                                       (const void**) search_values, 
                                                       4, NULL, NULL);

    OSStatus search_result = SecItemCopyMatching(search_query, NULL);

    if (search_result == errSecItemNotFound)
    {
      // create new entry
      const CFStringRef keys[] = {
        kSecClass,
        kSecAttrAccount,
        kSecValueData,
        kSecAttrDescription,
        kSecAttrLabel
      };

      const CFTypeRef values[] = {
        kSecClassGenericPassword,
        CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8),
        CFStringCreateExternalRepresentation(NULL, pw, kCFStringEncodingUTF8, 0),
        CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8),
        CFStringCreateWithCString(NULL, label, kCFStringEncodingUTF8)
      };

      CFDictionaryRef query  = CFDictionaryCreate(kCFAllocatorDefault, 
                                                  (const void**) keys, 
                                                  (const void**) values, 
                                                  5, NULL, NULL);

      result = SecItemAdd(query, NULL);

      CFRelease(query);
      CFRelease(values[1]);
      CFRelease(values[2]);
      CFRelease(values[3]);
      CFRelease(values[4]);
    }
    else
    {
      // update the existing entry
      const CFStringRef attribute_keys[] = {
        kSecValueData
      };

      const CFTypeRef attribute_values[] = {
        CFStringCreateExternalRepresentation(NULL, pw, kCFStringEncodingUTF8, 0)
      };

      CFDictionaryRef attributes = CFDictionaryCreate(kCFAllocatorDefault, 
                                                      (const void**) attribute_keys, 
                                                      (const void**) attribute_values, 
                                                      1, NULL, NULL);


      result = SecItemUpdate(search_query, attributes);

      CFRelease(attributes);
      CFRelease(attribute_values[0]);
    }
    
    g_free(label);
    CFRelease(pw);
    CFRelease(search_query);
    CFRelease(search_values[1]);
    CFRelease(search_values[2]);
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

  gchar *label = g_strconcat("darktable@", slot, NULL);

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

