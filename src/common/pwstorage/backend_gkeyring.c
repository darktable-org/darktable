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

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include "backend_gkeyring.h"
#include "control/conf.h"
#include <gnome-keyring.h>
#include <glib.h>

const backend_gkeyring_context_t* 
dt_pwstorage_gkeyring_new() 
{
		backend_gkeyring_context_t *context = g_slice_new0 (backend_gkeyring_context_t);
	
		gnome_keyring_unlock_sync(NULL,NULL);
		return context;
}

gboolean 
dt_pwstorage_gkeyring_set(const gchar* slot, GHashTable* table)
{

}

GHashTable* 
dt_pwstorage_gconf_get(const gchar* slot)
{
	GHashTable* table = g_hash_table_new (g_str_hash,g_str_equal);
	//g_hash_table_insert (table,g_strdup (key),value);
	return table;
}
