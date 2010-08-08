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
#include "../config.h"
#endif

#include "pwstorage.h"
#include "backend_gconf.h"
#include "backend_gkeyring.h"
#include "backend_kwallet.h"
#include "control/conf.h"

#include <glib.h>
// #include <dbus/dbus-glib.h>
#include <string.h>

/** Initializes a new pwstorage context. */
const dt_pwstorage_t* dt_pwstorage_new(){
	dt_pwstorage_t *pwstorage = g_malloc(sizeof(dt_pwstorage_t));
	dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] Creating new context %lx\n",(unsigned long int)pwstorage);

	if(pwstorage == NULL)
		return NULL;

	gint _backend = dt_conf_get_int( "plugins/pwstorage/pwstorage_backend" );

	switch(_backend){
		default:
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] unknown storage backend. Using none.\n");
		case PW_STORAGE_BACKEND_NONE:
			pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_NONE;
			pwstorage->backend_context = NULL;
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] no storage backend. not storing username/password. please change in gconf: \"plugins/pwstorage/pwstorage_backend\".\n");
			break;
		case PW_STORAGE_BACKEND_GCONF:
			// this is so important that I want it to be printed in any case. so g_printerr() instead of dt_print()
			g_printerr("[pwstorage_new] WARNING: you are using gconf for username/password storage! they are being stored unencrypted on your hard disk.\n");
			pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_GCONF;
			pwstorage->backend_context = NULL;
			break;
		case PW_STORAGE_BACKEND_KWALLET:
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] using kwallet backend for username/password storage");
			pwstorage->backend_context = (void*)dt_pwstorage_kwallet_new();
			if(pwstorage->backend_context == NULL){
				dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] error starting kwallet. using no storage backend.\n");
				pwstorage->backend_context = NULL;
				pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_NONE;
			} else {
				pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_KWALLET;
			}
			dt_print(DT_DEBUG_PWSTORAGE,"  done.\n");
			break;
		case PW_STORAGE_BACKEND_GNOME_KEYRING:
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] using gnome keyring backend for usersname/password storage.\n");
			pwstorage->backend_context = (void*)dt_pwstorage_gkeyring_new();
			if(pwstorage->backend_context == NULL){
				dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] error starting gnome keyring. using no storage backend.\n");
				pwstorage->backend_context = NULL;
				pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_NONE;
			} else {
				pwstorage->pw_storage_backend = PW_STORAGE_BACKEND_GNOME_KEYRING;
			}
			break;
	}

	dt_conf_set_int( "plugins/pwstorage/pwstorage_backend", pwstorage->pw_storage_backend );

	return pwstorage;
}

/** Cleanup and destroy pwstorage context. \remarks After this point pointer at pwstorage is invalid. */
void dt_pwstorage_destroy(const dt_pwstorage_t *pwstorage){
	dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_new] Destroying context %lx\n",(unsigned long int)pwstorage);
	switch(darktable.pwstorage->pw_storage_backend){
		case PW_STORAGE_BACKEND_NONE:
			// nothing to be done
		case PW_STORAGE_BACKEND_GCONF:
			// nothing to be done
			break;
		case PW_STORAGE_BACKEND_KWALLET:
// 			dt_pwstorage_kwallet_destroy(pwstorage->backend_context); // doesn't do anything.
			g_free(pwstorage->backend_context);
			break;
		case PW_STORAGE_BACKEND_GNOME_KEYRING:
			g_print("[pwstorage_destroy] gnome keyring not implemented yet. not storing anything.\n");
			break;
	}
}

/** Store (key,value) pairs. */
gboolean dt_pwstorage_set(const gchar* slot, GHashTable* table){
	switch(darktable.pwstorage->pw_storage_backend){
		case PW_STORAGE_BACKEND_NONE:
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_set] no backend. not storing anything.\n");
			break;
		case PW_STORAGE_BACKEND_GCONF:
			return dt_pwstorage_gconf_set(slot, table);
			break;
		case PW_STORAGE_BACKEND_KWALLET:
			return dt_pwstorage_kwallet_set(slot, table);
			break;
		case PW_STORAGE_BACKEND_GNOME_KEYRING:
			g_print("[pwstorage_set] gnome keyring not implemented yet. not storing anything.\n");
			break;
	}
	return FALSE;
}

/** Load (key,value) pairs. */
GHashTable* dt_pwstorage_get(const gchar* slot){
	switch(darktable.pwstorage->pw_storage_backend){
		case PW_STORAGE_BACKEND_NONE:
			dt_print(DT_DEBUG_PWSTORAGE,"[pwstorage_get] no backend. not reading anything.\n");
			break;
		case PW_STORAGE_BACKEND_GCONF:
			return dt_pwstorage_gconf_get(slot);
			break;
		case PW_STORAGE_BACKEND_KWALLET:
			return dt_pwstorage_kwallet_get(slot);
			break;
		case PW_STORAGE_BACKEND_GNOME_KEYRING:
			g_print("[pwstorage_get] gnome keyring not implemented yet. not reading anything.\n");
			break;
	}

	return g_hash_table_new(g_str_hash, g_str_equal);
}
