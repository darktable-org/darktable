// This file is part of darktable
// Copyright (c) 2010 Henrik Andersson <hean01@users.sourceforge.net>.

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

#include "version.h"

#include "backend_gkeyring.h"
#include "control/conf.h"
#include <gnome-keyring.h>
#include <glib.h>
#define DARKTABLE_KEYRING PACKAGE_NAME
#undef DARKTABLE_KEYRING

const backend_gkeyring_context_t *dt_pwstorage_gkeyring_new()
{
  backend_gkeyring_context_t *context
      = (backend_gkeyring_context_t *)g_malloc(sizeof(backend_gkeyring_context_t));

#ifdef DARKTABLE_KEYRING
  /* Check if darktable keyring exists, if not create it */
  gboolean keyring_exists = FALSE;
  GList *keyrings = NULL, *item = NULL;
  gnome_keyring_list_keyring_names_sync(&keyrings);
  if(keyrings && (item = keyrings))
  {
    do
    {
      if(strcmp((gchar *)item->data, DARKTABLE_KEYRING) == 0)
      {
        keyring_exists = TRUE;
        break;
      }
    } while((item = g_list_next(item)) != NULL);
    gnome_keyring_string_list_free(keyrings);
  }

  if(keyring_exists == FALSE) gnome_keyring_create_sync(DARKTABLE_KEYRING, NULL);
#else
#define DARKTABLE_KEYRING NULL
#endif
  /* unlock darktable keyring */
  // Keep this locked until accessed..
  // gnome_keyring_lock_sync(DARKTABLE_KEYRING);
  return context;
}

gboolean dt_pwstorage_gkeyring_set(const gchar *slot, GHashTable *table)
{
  GnomeKeyringResult result = 0;
  GnomeKeyringAttributeList *attributes;
  gchar name[256] = "Darktable account information for ";
  /* build up attributes for slot */
  attributes = g_array_new(FALSE, FALSE, sizeof(GnomeKeyringAttribute));
  gnome_keyring_attribute_list_append_string(attributes, "magic", PACKAGE_NAME);
  gnome_keyring_attribute_list_append_string(attributes, "slot", slot);

  /* search for existing item for slot */
  GList *items = NULL;
  gnome_keyring_find_items_sync(GNOME_KEYRING_ITEM_GENERIC_SECRET, attributes, &items);
  guint item_id;

  /* add attributes from hash table*/
  GHashTableIter iter;
  g_hash_table_iter_init(&iter, table);
  gpointer key, value;
  while(g_hash_table_iter_next(&iter, &key, &value))
    gnome_keyring_attribute_list_append_string(attributes, key, value);

  if(items)
  {
    GnomeKeyringFound *f = (GnomeKeyringFound *)items->data;
    gnome_keyring_item_set_attributes_sync(DARKTABLE_KEYRING, f->item_id, attributes);
  }
  else
  {
    g_strlcat(name, slot, sizeof(name));
    /* create/update item with attributes */
    result = gnome_keyring_item_create_sync(DARKTABLE_KEYRING, GNOME_KEYRING_ITEM_GENERIC_SECRET, name,
                                            attributes, NULL, TRUE, &item_id);
  }

  gnome_keyring_attribute_list_free(attributes);

  return (result == GNOME_KEYRING_RESULT_OK);
}

GHashTable *dt_pwstorage_gkeyring_get(const gchar *slot)
{

  GHashTable *table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  /* find item for slot */
  GList *items = NULL;
  GnomeKeyringAttributeList *attributes;
  attributes = g_array_new(FALSE, FALSE, sizeof(GnomeKeyringAttribute));
  gnome_keyring_attribute_list_append_string(attributes, "magic", PACKAGE_NAME);
  gnome_keyring_attribute_list_append_string(attributes, "slot", slot);
  gnome_keyring_find_items_sync(GNOME_KEYRING_ITEM_GENERIC_SECRET, attributes, &items);
  gnome_keyring_attribute_list_free(attributes);

  /* if item found get the attributes into result table and return */
  if(items)
  {
    GnomeKeyringFound *f = (GnomeKeyringFound *)items->data;

    /* get all attributes of found item */
    gnome_keyring_item_get_attributes_sync(DARKTABLE_KEYRING, f->item_id, &attributes);

    /* build hash table result */
    for(int i = 0; i < attributes->len; i++)
    {
      GnomeKeyringAttribute *attribute = &gnome_keyring_attribute_list_index(attributes, i);
      if(attribute != NULL)
      {
        if(strcmp(attribute->name, "slot") != 0 && strcmp(attribute->name, "magic") != 0)
          g_hash_table_insert(table, g_strdup(attribute->name), g_strdup(attribute->value.string));
      }
      else
        break;
    }
    gnome_keyring_attribute_list_free(attributes);
    gnome_keyring_found_free(items->data);
  }
  return table;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
