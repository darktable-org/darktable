/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/calculator.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

typedef struct dt_conf_dreggn_t
{
  GSList *result;
  const char *match;
} dt_conf_dreggn_t;

static void _free_confgen_value(void *value)
{
  dt_confgen_value_t *s = (dt_confgen_value_t *)value;
  g_free(s->def);
  g_free(s->min);
  g_free(s->max);
  g_free(s->enum_values);
  g_free(s->shortdesc);
  g_free(s->longdesc);
  g_free(s);
}

/** return slot for this variable or newly allocated slot. */
static inline char *dt_conf_get_var(const char *name)
{
  char *str;

  dt_pthread_mutex_lock(&darktable.conf->mutex);

  str = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  if(str) goto fin;

  str = (char *)g_hash_table_lookup(darktable.conf->table, name);
  if(str) goto fin;

  // not found, try defaults
  str = (char *)dt_confgen_get(name, DT_DEFAULT);
  if(str)
  {
    char *str_new = g_strdup(str);
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str_new);
    str = str_new;
    goto fin;
  }

  // FIXME: why insert garbage?
  // still no luck? insert garbage:
  str = (char *)g_malloc0(sizeof(int32_t));
  g_hash_table_insert(darktable.conf->table, g_strdup(name), str);

fin:
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return str;
}

/* set the value only if it hasn't been overridden from commandline
 * return 1 if key/value is still the one passed on commandline. */
static int dt_conf_set_if_not_overridden(const char *name, char *str)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);

  char *over = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  const int is_overridden = (over && !strcmp(str, over));
  if(!is_overridden)
  {
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  }

  dt_pthread_mutex_unlock(&darktable.conf->mutex);

  return is_overridden;
}

void dt_conf_set_int(const char *name, int val)
{
  char *str = g_strdup_printf("%d", val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_int64(const char *name, int64_t val)
{
  char *str = g_strdup_printf("%" PRId64, val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_float(const char *name, float val)
{
  char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);
  g_ascii_dtostr(str, G_ASCII_DTOSTR_BUF_SIZE, val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_bool(const char *name, int val)
{
  char *str = g_strdup(val ? "TRUE" : "FALSE");
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_string(const char *name, const char *val)
{
  char *str = g_strdup(val);
  if(dt_conf_set_if_not_overridden(name, str)) g_free(str);
}

void dt_conf_set_folder_from_file_chooser(const char *name, GtkFileChooser *chooser)
{

#ifdef WIN32
  // for Windows native file chooser, gtk_file_chooser_get_current_folder()
  // does not work, so we workaround
  if(GTK_IS_FILE_CHOOSER_NATIVE(chooser))
  {
    gchar *pathname = gtk_file_chooser_get_filename(chooser);
    if(pathname)
    {
      gchar *folder = g_path_get_dirname(pathname);
      if(dt_conf_set_if_not_overridden(name, folder)) g_free(folder);
      g_free(pathname);
    }
    return;
  }
#endif

  gchar *folder = gtk_file_chooser_get_current_folder(chooser);
  if(dt_conf_set_if_not_overridden(name, folder)) g_free(folder);
}

int dt_conf_get_int_fast(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }

  int val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  return val;
}

int dt_conf_get_int(const char *name)
{
  const int min = dt_confgen_get_int(name, DT_MIN);
  const int max = dt_confgen_get_int(name, DT_MAX);
  const int val = dt_conf_get_int_fast(name);
  const int ret = CLAMP(val, min, max);
  return ret;
}

int64_t dt_conf_get_int64_fast(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }

  int64_t val;
  if(new_value > 0)
    val = new_value + 0.5;
  else
    val = new_value - 0.5;
  return val;
}

int64_t dt_conf_get_int64(const char *name)
{
  const int64_t min = dt_confgen_get_int64(name, DT_MIN);
  const int64_t max = dt_confgen_get_int64(name, DT_MAX);
  const int64_t val = dt_conf_get_int64_fast(name);
  const int64_t ret = CLAMP(val, min, max);
  return ret;
}

float dt_conf_get_float_fast(const char *name)
{
  const char *str = dt_conf_get_var(name);
  float new_value = dt_calculator_solve(1, str);
  if(isnan(new_value))
  {
    //we've got garbage, check default
    const char *def_val = dt_confgen_get(name, DT_DEFAULT);
    if(def_val)
    {
      new_value = dt_calculator_solve(1, def_val);
      if(isnan(new_value))
        new_value = 0.0;
      else
      {
        char *fix_badval = g_strdup(def_val);
        if(dt_conf_set_if_not_overridden(name, fix_badval))
          g_free(fix_badval);
      }
    }
    else
    {
      new_value = 0.0;
    }
  }
  return new_value;
}

float dt_conf_get_float(const char *name)
{
  const float min = dt_confgen_get_float(name, DT_MIN);
  const float max = dt_confgen_get_float(name, DT_MAX);
  const float val = dt_conf_get_float_fast(name);
  const float ret = CLAMP(val, min, max);
  return ret;
}

int dt_conf_get_and_sanitize_int(const char *name, int min, int max)
{
  const int cmin = dt_confgen_get_int(name, DT_MIN);
  const int cmax = dt_confgen_get_int(name, DT_MAX);
  const int val = dt_conf_get_int_fast(name);
  const int ret = CLAMPS(val, MAX(min, cmin), MIN(max, cmax));
  dt_conf_set_int(name, ret);
  return ret;
}

int64_t dt_conf_get_and_sanitize_int64(const char *name, int64_t min, int64_t max)
{
  const int64_t cmin = dt_confgen_get_int64(name, DT_MIN);
  const int64_t cmax = dt_confgen_get_int64(name, DT_MAX);
  const int64_t val = dt_conf_get_int64_fast(name);
  const int64_t ret = CLAMPS(val, MAX(min, cmin), MIN(max, cmax));
  dt_conf_set_int64(name, ret);
  return ret;
}

float dt_conf_get_and_sanitize_float(const char *name, float min, float max)
{
  const float cmin = dt_confgen_get_float(name, DT_MIN);
  const float cmax = dt_confgen_get_float(name, DT_MAX);
  const float val = dt_conf_get_float_fast(name);
  const float ret = CLAMPS(val, MAX(min, cmin), MIN(max, cmax));
  dt_conf_set_float(name, ret);
  return ret;
}

int dt_conf_get_bool(const char *name)
{
  const char *str = dt_conf_get_var(name);
  const int val = (str[0] == 'T') || (str[0] == 't');
  return val;
}

gchar *dt_conf_get_string(const char *name)
{
  const char *str = dt_conf_get_var(name);
  return g_strdup(str);
}

const char *dt_conf_get_string_const(const char *name)
{
  return dt_conf_get_var(name);
}

gboolean dt_conf_key_not_empty(const char *name)
{
  const char *val = dt_conf_get_string_const(name);
  if(val == NULL)      return FALSE;
  if(strlen(val) == 0) return FALSE;
  return TRUE;
}

gboolean dt_conf_get_folder_to_file_chooser(const char *name, GtkFileChooser *chooser)
{
  const gchar *folder = dt_conf_get_string_const(name);
  if(folder)
  {
    gtk_file_chooser_set_current_folder(chooser, folder);
    return TRUE;
  }
  return FALSE;
}

gboolean dt_conf_is_equal(const char *name, const char *value)
{
  const char *str = dt_conf_get_var(name);
  return g_strcmp0(str, value) == 0;
}

static char *_sanitize_confgen(const char *name, const char *value)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(!item) return g_strdup(value);

  char *result = NULL;

  switch(item->type)
  {
    case DT_INT:
    {
      float v = dt_calculator_solve(1, value);

      const int min = item->min ? (int)dt_calculator_solve(1, item->min) : INT_MIN;
      const int max = item->max ? (int)dt_calculator_solve(1, item->max) : INT_MAX;
      // if garbage, use default
      const int val = isnan(v) ? dt_confgen_get_int(name, DT_DEFAULT) : (int)v;
      result = g_strdup_printf("%d", CLAMP(val, min, max));
    }
    break;
    case DT_INT64:
    {
      float v = dt_calculator_solve(1, value);

      const int64_t min = item->min ? (int64_t)dt_calculator_solve(1, item->min) : INT64_MIN;
      const int64_t max = item->max ? (int64_t)dt_calculator_solve(1, item->max) : INT64_MAX;
      // if garbage, use default
      const int64_t val = isnan(v) ? dt_confgen_get_int64(name, DT_DEFAULT) : (int64_t)v;
      result = g_strdup_printf("%"PRId64, CLAMP(val, min, max));
    }
    break;
    case DT_FLOAT:
    {
      float v = dt_calculator_solve(1, value);

      const float min = item->min ? (float)dt_calculator_solve(1, item->min) : -FLT_MAX;
      const float max = item->max ? (float)dt_calculator_solve(1, item->max) : FLT_MAX;
      // if garbage, use default
      const float val = isnan(v) ? dt_confgen_get_float(name, DT_DEFAULT) : v;
      result = g_strdup_printf("%f", CLAMP(val, min, max));
    }
    break;
    case DT_BOOL:
    {
      if(strcasecmp(value, "true") && strcasecmp(value, "false"))
        result = g_strdup(dt_confgen_get(name, DT_DEFAULT));
      else
        result = g_strdup(value);
    }
    break;
    case DT_ENUM:
    {
      char *v = g_strdup_printf("[%s]", value);
      if(!strstr(item->enum_values, v))
        result = g_strdup(dt_confgen_get(name, DT_DEFAULT));
      else
        result = g_strdup(value);
      g_free(v);
    }
    break;
    default:
      result = g_strdup(value);
      break;
  }

  return result;
}

void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries)
{
  cf->x_confgen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _free_confgen_value);

  cf->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->override_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_pthread_mutex_init(&darktable.conf->mutex, NULL);

  // init conf filename
  g_strlcpy(darktable.conf->filename, filename, sizeof(darktable.conf->filename));

#define LINE_SIZE 1023

  char line[LINE_SIZE + 1];

  FILE *f = NULL;

  // check for user config
  f = g_fopen(filename, "rb");

  // if file has been found, parse it

  if(f)
  {
    while(!feof(f))
    {
      const int read = fscanf(f, "%" STR(LINE_SIZE) "[^\r\n]\r\n", line);
      if(read > 0)
      {
        char *c = line;
        char *end = line + strlen(line);
        // check for '=' which is separator between the conf name and value
        while(*c != '=' && c < end) c++;

        if(*c == '=')
        {
          *c = '\0';

          char *name = g_strdup(line);
          // ensure that numbers are properly clamped if min/max
          // defined and if not and garbage is read then the default
          // value is returned.
          char *value = _sanitize_confgen(name, (const char *)(c + 1));

          g_hash_table_insert(darktable.conf->table, name, value);
        }
      }
    }
    fclose(f);
  }
  else
  {
    // we initialize the conf table with default values
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, darktable.conf->x_confgen);
    while(g_hash_table_iter_next (&iter, &key, &value))
    {
      const char *name = (const char *)key;
      const dt_confgen_value_t *entry = (dt_confgen_value_t *)value;
      g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(entry->def));
    }
  }

  if(override_entries)
  {
    for(GSList *p = override_entries; p; p = g_slist_next(p))
    {
      dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)p->data;
      g_hash_table_insert(darktable.conf->override_entries, entry->key, entry->value);
    }
  }

#undef LINE_SIZE

  return;
}

/** check if key exists, return 1 if lookup succeeded, 0 if failed..*/
int dt_conf_key_exists(const char *key)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int res = (g_hash_table_lookup(darktable.conf->table, key) != NULL)
                  || (g_hash_table_lookup(darktable.conf->override_entries, key) != NULL);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return (res || dt_confgen_value_exists(key, DT_DEFAULT));
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
GSList *dt_conf_all_string_entries(const char *dir)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  dt_conf_dreggn_t d;
  d.result = NULL;
  d.match = dir;
  g_hash_table_foreach(darktable.conf->table, (GHFunc)_conf_add, &d);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return d.result;
}

void dt_conf_string_entry_free(gpointer data)
{
  dt_conf_string_entry_t *nv = (dt_conf_string_entry_t *)data;
  g_free(nv->key);
  g_free(nv->value);
  nv->key = NULL;
  nv->value = NULL;
  g_free(nv);
}

gboolean dt_confgen_exists(const char *name)
{
  return g_hash_table_lookup(darktable.conf->x_confgen, name) != NULL;
}

dt_confgen_type_t dt_confgen_type(const char *name)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
    return item->type;
  else
    return DT_STRING;
}

gboolean dt_confgen_value_exists(const char *name, dt_confgen_value_kind_t kind)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);
  if(item == NULL)
    return FALSE;

  switch(kind)
  {
     case DT_DEFAULT:
       return item->def != NULL;
     case DT_MIN:
       return item->min != NULL;
     case DT_MAX:
       return item->max != NULL;
     case DT_VALUES:
       return item->enum_values != NULL;
  }
  return FALSE;
}

const char *dt_confgen_get(const char *name, dt_confgen_value_kind_t kind)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
    switch(kind)
    {
       case DT_DEFAULT:
         return item->def;
       case DT_MIN:
         return item->min;
       case DT_MAX:
         return item->max;
       case DT_VALUES:
         return item->enum_values;
    }
  }

  return "";
}

const char *dt_confgen_get_label(const char *name)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
    return item->shortdesc;
  }

  return "";
}

const char *dt_confgen_get_tooltip(const char *name)
{
  const dt_confgen_value_t *item = g_hash_table_lookup(darktable.conf->x_confgen, name);

  if(item)
  {
    return item->longdesc;
  }

  return "";
}

int dt_confgen_get_int(const char *name, dt_confgen_value_kind_t kind)
{
  if(!dt_confgen_value_exists(name, kind))
  {
    //early bail
    switch(kind)
    {
    case DT_MIN:
      return INT_MIN;
      break;
    case DT_MAX:
      return INT_MAX;
      break;
    default:
      return 0;
      break;
    }
  }
  const char *str = dt_confgen_get(name, kind);

  //if str is NULL or empty, dt_calculator_solve will return NAN
  const float value = dt_calculator_solve(1, str);

  switch(kind)
  {
  case DT_MIN:
    return isnan(value) ? INT_MIN : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  case DT_MAX:
    return isnan(value) ? INT_MAX : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  default:
    return isnan(value) ? 0.0f : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  }
  return (int)value;
}

int64_t dt_confgen_get_int64(const char *name, dt_confgen_value_kind_t kind)
{
  if(!dt_confgen_value_exists(name, kind))
  {
    //early bail
    switch(kind)
    {
    case DT_MIN:
      return INT64_MIN;
      break;
    case DT_MAX:
      return INT64_MAX;
      break;
    default:
      return 0;
      break;
    }
  }
  const char *str = dt_confgen_get(name, kind);

  //if str is NULL or empty, dt_calculator_solve will return NAN
  const float value = dt_calculator_solve(1, str);

  switch(kind)
  {
  case DT_MIN:
    return isnan(value) ? INT64_MIN : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  case DT_MAX:
    return isnan(value) ? INT64_MAX : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  default:
    return isnan(value) ? 0.0f : (value > 0 ? value + 0.5f : value - 0.5f);
    break;
  }
  return (int64_t)value;
}

gboolean dt_confgen_get_bool(const char *name, dt_confgen_value_kind_t kind)
{
  const char *str = dt_confgen_get(name, kind);
  return !strcmp(str, "true");
}

float dt_confgen_get_float(const char *name, dt_confgen_value_kind_t kind)
{
  if(!dt_confgen_value_exists(name, kind))
  {
    //early bail
    switch(kind)
    {
    case DT_MIN:
      return -FLT_MAX;
      break;
    case DT_MAX:
      return FLT_MAX;
      break;
    default:
      break;
    }
    return 0.0f;
  }

  const char *str = dt_confgen_get(name, kind);

  //if str is NULL or empty, dt_calculator_solve will return NAN
  const float value = dt_calculator_solve(1, str);

  switch(kind)
  {
  case DT_MIN:
    // to anyone askig FLT_MIN is superclose to 0, not furthest value from 0 possible in float
    return isnan(value) ? -FLT_MAX : value;
    break;
  case DT_MAX:
    return isnan(value) ? FLT_MAX : value;
    break;
  default:
    break;
  }
  return isnan(value) ? 0.0f : value;
}

gboolean dt_conf_is_default(const char *name)
{
  if(!dt_confgen_exists(name))
    return TRUE; // well if default doesn't know about it, it's default

  switch(dt_confgen_type(name))
  {
  case DT_INT:
    return dt_conf_get_int(name) == dt_confgen_get_int(name, DT_DEFAULT);
    break;
  case DT_INT64:
    return dt_conf_get_int64(name) == dt_confgen_get_int64(name, DT_DEFAULT);
    break;
  case DT_FLOAT:
    return dt_conf_get_float(name) == dt_confgen_get_float(name, DT_DEFAULT);
    break;
  case DT_BOOL:
    return dt_conf_get_bool(name) == dt_confgen_get_bool(name, DT_DEFAULT);
    break;
  case DT_PATH:
  case DT_STRING:
  case DT_ENUM:
  default:
    {
      const char *def_val = dt_confgen_get(name, DT_DEFAULT);
      const char *cur_val = dt_conf_get_var(name);
      return g_strcmp0(def_val, cur_val) == 0;
      break;
    }
  }
}

gchar* dt_conf_expand_default_dir(const char *dir)
{
  // expand special dirs
#define CONFIG_DIR "$(config)"
#define HOME_DIR   "$(home)"

  gchar *path = NULL;
  if(g_str_has_prefix(dir, CONFIG_DIR))
  {
    gchar configdir[PATH_MAX] = { 0 };
    dt_loc_get_user_config_dir(configdir, sizeof(configdir));
    path = g_strdup_printf("%s%s", configdir, dir + strlen(CONFIG_DIR));
  }
  else if(g_str_has_prefix(dir, HOME_DIR))
  {
    gchar *homedir = dt_loc_get_home_dir(NULL);
    path = g_strdup_printf("%s%s", homedir, dir + strlen(HOME_DIR));
    g_free(homedir);
  }
  else path = g_strdup(dir);

  gchar *normalized_path = dt_util_normalize_path(path);
  g_free(path);

  return normalized_path;
}

static void dt_conf_print(const gchar *key, const gchar *val, FILE *f)
{
  fprintf(f, "%s=%s\n", key, val);
}

void dt_conf_save(dt_conf_t *cf)
{
  FILE *f = g_fopen(cf->filename, "wb");
  if(f)
  {
    GList *keys = g_hash_table_get_keys(cf->table);
    GList *sorted = g_list_sort(keys, (GCompareFunc)g_strcmp0);

    for(GList *iter = sorted; iter; iter = g_list_next(iter))
    {
      const gchar *key = (const gchar *)iter->data;
      const gchar *val = (const gchar *)g_hash_table_lookup(cf->table, key);
      dt_conf_print(key, val, f);
    }

    g_list_free(sorted);
    fclose(f);
  }
}
void dt_conf_cleanup(dt_conf_t *cf)
{
  dt_conf_save(cf);
  g_hash_table_unref(cf->table);
  g_hash_table_unref(cf->override_entries);
  g_hash_table_unref(cf->x_confgen);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

