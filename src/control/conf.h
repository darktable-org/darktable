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

#include "common/calculator.h"
#include "common/darktable.h"
#include "common/file_location.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

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

typedef struct dt_conf_dreggn_t
{
  GSList *result;
  const char *match;
} dt_conf_dreggn_t;

/** return slot for this variable or newly allocated slot. */
static inline char *dt_conf_get_var(const char *name)
{
  char *str = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  if(str) return str;

  str = (char *)g_hash_table_lookup(darktable.conf->table, name);
  if(str) return str;

  // not found, try defaults
  str = (char *)g_hash_table_lookup(darktable.conf->defaults, name);
  if(str)
  {
    g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(str));
    // and try again:
    return dt_conf_get_var(name);
  }

  // still no luck? insert garbage:
  char *garbage = (char *)g_malloc0(sizeof(int32_t));
  g_hash_table_insert(darktable.conf->table, g_strdup(name), garbage);
  return garbage;
}

/** return if key/value is still the one passed on commandline. */
static inline int dt_conf_is_still_overridden(const char *name, const char *value)
{
  char *over = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  return (over && !strcmp(value, over));
}

static inline void dt_conf_set_int(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%d", val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_int64(const char *name, int64_t val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%" PRId64, val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_float(const char *name, float val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);
  g_ascii_dtostr(str, G_ASCII_DTOSTR_BUF_SIZE, val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_bool(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%s", val ? "TRUE" : "FALSE");
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_string(const char *name, const char *val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  if(!dt_conf_is_still_overridden(name, val))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(val));
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline int dt_conf_get_int(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value)) new_value = 0.0;
  int val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline int64_t dt_conf_get_int64(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value)) new_value = 0.0;
  int64_t val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline float dt_conf_get_float(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  float val = dt_calculator_solve(1, str);
  if(isnan(val)) val = 0.0;
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline int dt_conf_get_bool(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  const int val = (str[0] == 'T') || (str[0] == 't');
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline gchar *dt_conf_get_string(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return g_strdup(str);
}

static inline void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries)
{
  cf->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->defaults = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->override_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_pthread_mutex_init(&darktable.conf->mutex, NULL);
  FILE *f = 0;

#define LINE_SIZE 1023

  char line[LINE_SIZE + 1];

  int read = 0;
  int defaults = 0;
  for(int i = 0; i < 2; i++)
  {
    // TODO: read default darktablerc into ->defaults and other into ->table!
    if(!i)
    {
      snprintf(darktable.conf->filename, sizeof(darktable.conf->filename), "%s", filename);
      f = g_fopen(filename, "rb");
      if(!f)
      {
        // remember we init to default rc and try again
        defaults = 1;
        continue;
      }
    }
    if(i)
    {
      char buf[PATH_MAX] = { 0 }, defaultrc[PATH_MAX] = { 0 };
      dt_loc_get_datadir(buf, sizeof(buf));
      snprintf(defaultrc, sizeof(defaultrc), "%s/darktablerc", buf);
      f = g_fopen(defaultrc, "rb");
    }
    if(!f) return;
    while(!feof(f))
    {
      read = fscanf(f, "%" STR(LINE_SIZE) "[^\r\n]\r\n", line);
      if(read > 0)
      {
        char *c = line;
        char *end = line + strlen(line);
        while(*c != '=' && c < end) c++;
        if(*c == '=')
        {
          *c = '\0';
          if(i) g_hash_table_insert(darktable.conf->defaults, g_strdup(line), g_strdup(c + 1));
          if(!i || defaults) g_hash_table_insert(darktable.conf->table, g_strdup(line), g_strdup(c + 1));
        }
      }
    }
    fclose(f);
  }

  // for the very first time after a fresh install
  // execute performance configuration no matter what
  if(defaults)
    dt_configure_performance();

  if(override_entries)
  {
    GSList *p = override_entries;
    while(p)
    {
      dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)p->data;
      g_hash_table_insert(darktable.conf->override_entries, entry->key, entry->value);
      p = g_slist_next(p);
    }
  }

#undef LINE_SIZE

  return;
}

static void dt_conf_print(const gchar *key, const gchar *val, FILE *f)
{
  fprintf(f, "%s=%s\n", key, val);
}

static inline void dt_conf_cleanup(dt_conf_t *cf)
{
  FILE *f = g_fopen(cf->filename, "wb");
  if(f)
  {
    GList *keys = g_hash_table_get_keys(cf->table);
    GList *sorted = g_list_sort(keys, (GCompareFunc)g_strcmp0);

    GList *iter = sorted;

    while(iter)
    {
      const gchar *key = (const gchar *)iter->data;
      const gchar *val = (const gchar *)g_hash_table_lookup(cf->table, key);
      dt_conf_print(key, val, f);
      iter = g_list_next(iter);
    }

    g_list_free(sorted);
    fclose(f);
  }
  g_hash_table_unref(cf->table);
  g_hash_table_unref(cf->defaults);
  g_hash_table_unref(cf->override_entries);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
}

/** check if key exists, return 1 if lookup successed, 0 if failed..*/
static inline int dt_conf_key_exists(const char *key)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int res = (g_hash_table_lookup(darktable.conf->table, key) != NULL)
                  || (g_hash_table_lookup(darktable.conf->override_entries, key) != NULL);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return res;
}

static void _conf_add(char *key, char *val, dt_conf_dreggn_t *d)
{
  if(strncmp(key, d->match, strlen(d->match)) == 0)
  {
    dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)g_malloc(sizeof(dt_conf_string_entry_t));
    nv->key = g_strdup(key + strlen(d->match) + 1);
    nv->value = g_strdup(val);
    d->result = g_slist_append(d->result, nv);
  }
}

/** get all strings in */
static inline GSList *dt_conf_all_string_entries(const char *dir)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  dt_conf_dreggn_t d;
  d.result = NULL;
  d.match = dir;
  g_hash_table_foreach(darktable.conf->table, (GHFunc)_conf_add, &d);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return d.result;
}

static inline void dt_conf_string_entry_free(gpointer data)
{
  dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)data;
  g_free(nv->key);
  g_free(nv->value);
  nv->key = NULL;
  nv->value = NULL;
  g_free(nv);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
