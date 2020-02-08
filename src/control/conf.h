/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/dtpthread.h"

#include <glib.h>
#include <inttypes.h>

typedef struct dt_conf_t
{
  dt_pthread_mutex_t mutex;
  char filename[PATH_MAX];
  GHashTable *table;
  GHashTable *defaults;
  GHashTable *override_entries;
} dt_conf_t;

typedef struct dt_conf_string_entry_t
{
  char *key;
  char *value;
} dt_conf_string_entry_t;

void dt_conf_set_int(const char *name, int val);
void dt_conf_set_int64(const char *name, int64_t val);
void dt_conf_set_float(const char *name, float val);
void dt_conf_set_bool(const char *name, int val);
void dt_conf_set_string(const char *name, const char *val);
int dt_conf_get_int(const char *name);
int64_t dt_conf_get_int64(const char *name);
float dt_conf_get_float(const char *name);
int dt_conf_get_bool(const char *name);
gchar *dt_conf_get_string(const char *name);
void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries);
void dt_conf_cleanup(dt_conf_t *cf);
int dt_conf_key_exists(const char *key);
GSList *dt_conf_all_string_entries(const char *dir);
void dt_conf_string_entry_free(gpointer data);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
