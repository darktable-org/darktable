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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "backend_gconf.h"
#include "control/conf.h"

#include <glib.h>


static const gchar* gconf_path = "plugins/pwstorage/";

/** Store (key,value) pairs. */
gboolean dt_pwstorage_gconf_set(const gchar* slot, GHashTable* table)
{

  GHashTableIter iter;
  g_hash_table_iter_init (&iter, table);
  gpointer key, value;

  while (g_hash_table_iter_next (&iter, &key, &value))
  {
    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_gconf_set] storing (%s, %s)\n",(gchar*)key, (gchar*)value);
    gsize size = strlen(gconf_path) + strlen(slot) + 1 + strlen(key);
    gchar* _path = g_malloc(size+1);
    gchar* _tmp = _path;
    if(_path == NULL)
      return FALSE;
    _tmp = g_stpcpy(_tmp, gconf_path);
    _tmp = g_stpcpy(_tmp, slot);
    _tmp[0] = '/';
    _tmp++;
    g_stpcpy(_tmp, key);

    // This would be the place to do manual encryption of the data.
    // I know enough about cryptography to not implement this.
    // If you don't like plain text password just use one of the other backends.

    dt_conf_set_string( _path, value );
    g_free(_path);
  }

  return TRUE;
}

/** Load (key,value) pairs. */
GHashTable* dt_pwstorage_gconf_get(const gchar* slot)
{
  GHashTable* table = g_hash_table_new(g_str_hash, g_str_equal);

  gsize size = strlen(gconf_path) + strlen(slot);
  gchar* _path = g_malloc(size+1);
  gchar* _tmp = _path;
  if(_path == NULL)
    return table;
  _tmp = g_stpcpy(_tmp, gconf_path);
  g_stpcpy(_tmp, slot);

  GSList* list;
  list = dt_conf_all_string_entries(_path);

  g_free(_path);

  GSList* next = list;
  while(next)
  {
    gchar* key = ((dt_conf_string_entry_t*)next->data)->key;

    gsize size = strlen(gconf_path) + strlen(slot) + 1 + strlen(key);
    gchar* _path = g_malloc(size+1);
    gchar* _tmp = _path;
    if(_path == NULL)
      return table;
    _tmp = g_stpcpy(_tmp, gconf_path);
    _tmp = g_stpcpy(_tmp, slot);
    _tmp[0] = '/';
    _tmp++;
    g_stpcpy(_tmp, key);

    gchar* value = ((dt_conf_string_entry_t*)next->data)->value;
    g_free(_path);

    dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_gconf_get] reading (%s, %s)\n",(gchar*)key, (gchar*)value);

    // This would be the place for manual decryption.
    // See above.

    g_hash_table_insert(table, g_strdup(key), g_strdup(value));

    next = next->next;
  }

  g_slist_free(list);

  return table;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
