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
#ifndef DT_USER_CONFIG_H
#define DT_USER_CONFIG_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <glib.h>
#include <glib/gprintf.h>

// silly gconf replacement for all of us now. (: very sucky altogether.

#define DT_CONF_MAX_VARS 512
#define DT_CONF_MAX_VAR_BUF 512

typedef struct dt_conf_t
{
  dt_pthread_mutex_t mutex;
  char filename[1024];
  int  num;
  char varname[DT_CONF_MAX_VARS][DT_CONF_MAX_VAR_BUF];
  char varval [DT_CONF_MAX_VARS][DT_CONF_MAX_VAR_BUF];
}
dt_conf_t;

/** return slot for this variable or newly allocated slot. */
static inline int dt_conf_get_var_pos(const char *name)
{
  for(int i=0; i<darktable.conf->num; i++)
  {
    if(!strncmp(name, darktable.conf->varname[i], DT_CONF_MAX_VAR_BUF)) return i;
  }
  // not found, give it a new slot:
  int num = darktable.conf->num++;
  snprintf(darktable.conf->varname[num], DT_CONF_MAX_VAR_BUF, "%s", name);
  memset(darktable.conf->varval[num], 0, DT_CONF_MAX_VAR_BUF);

  // and get the default value from default darktablerc:
  char buf[1024], defaultrc[1024];
  dt_util_get_datadir(buf, 1024);
  snprintf(defaultrc, 1024, "%s/darktablerc", buf);
  FILE *f = fopen(defaultrc, "rb");
  char line[1024];
  int read = 0;
  if(!f) return num;
  while(!feof(f))
  {
    read = fscanf(f, "%[^\n]\n", line);
    if(read > 0)
    {
      char *c = line;
      while(*c != '=' && c < line + strlen(line)) c++;
      if(*c == '=')
      {
        *c = '\0';
        if(!strncmp(line, name, DT_CONF_MAX_VAR_BUF))
        {
          strncpy(darktable.conf->varval[num], c+1, DT_CONF_MAX_VAR_BUF);
          break;
        }
      }
    }
  }
  fclose(f);
  return num;
}

static inline void dt_conf_set_int(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  snprintf(darktable.conf->varval[num], DT_CONF_MAX_VAR_BUF, "%d", val);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_float(const char *name, float val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  g_ascii_dtostr(darktable.conf->varval[num], DT_CONF_MAX_VAR_BUF, val);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_bool(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  snprintf(darktable.conf->varval[num], DT_CONF_MAX_VAR_BUF, "%s", val ? "TRUE" : "FALSE");
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_string(const char *name, const char *val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  snprintf(darktable.conf->varval[num], DT_CONF_MAX_VAR_BUF, "%s", val);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline int dt_conf_get_int(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  const int val = atol(darktable.conf->varval[num]);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline float dt_conf_get_float(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  const float val = g_ascii_strtod(darktable.conf->varval[num], NULL);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline int dt_conf_get_bool(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  const int val = darktable.conf->varval[num][0] == 'T';
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline gchar *dt_conf_get_string(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const int num = dt_conf_get_var_pos(name);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return g_strdup(darktable.conf->varval[num]);
}

typedef struct dt_conf_string_entry_t
{
  char *key;
  char *value;
} dt_conf_string_entry_t;

/** get all strings in */
static inline GSList *dt_conf_all_string_entries (const char *dir)
{
  GSList *result = NULL;

  dt_pthread_mutex_lock(&darktable.conf->mutex);
  for (int i=0; i<DT_CONF_MAX_VARS; i++)
  {
    if (strcmp(darktable.conf->varname[i],dir)==0)
    {
      dt_conf_string_entry_t *nv = (dt_conf_string_entry_t*)g_malloc (sizeof(dt_conf_string_entry_t));
      gchar *key = g_strdup (darktable.conf->varname[i]);

      /* get the key name from path/key */
      gchar *p = key+strlen (key);
      while (*--p!='/');
      nv->key = g_strdup (++p);

      /* get the value */
      nv->value = g_strdup(darktable.conf->varval[i]);

      result = g_slist_append (result,nv);
    }
  }

  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return result;
}

static inline void dt_conf_init(dt_conf_t *cf, const char *filename)
{
  dt_pthread_mutex_init(&darktable.conf->mutex, NULL);
  memset(cf->varname,0, DT_CONF_MAX_VARS*DT_CONF_MAX_VAR_BUF);
  memset(cf->varval, 0, DT_CONF_MAX_VARS*DT_CONF_MAX_VAR_BUF);
  snprintf(darktable.conf->filename, 1024, "%s", filename);
  darktable.conf->num = 0;
  FILE *f = fopen(filename, "rb");
  char line[1024];
  int read = 0;
  int defaults = 0;
  if(!f)
  {
    char buf[1024], defaultrc[1024];
    dt_util_get_datadir(buf, 1024);
    snprintf(defaultrc, 1024, "%s/darktablerc", buf);
    f = fopen(defaultrc, "rb");
    defaults = 1;
  }
  if(!f) return;
  while(!feof(f))
  {
    read = fscanf(f, "%[^\n]\n", line);
    if(read > 0)
    {
      char *c = line;
      while(*c != '=' && c < line + strlen(line)) c++;
      if(*c == '=')
      {
        *c = '\0';
        dt_conf_set_string(line, c+1);
      }
    }
  }
  fclose(f);
  if(defaults) dt_configure_defaults();
  return;
}

static inline void dt_conf_cleanup(dt_conf_t *cf)
{
  FILE *f = fopen(cf->filename, "wb");
  if(!f) return;
  for(int i=0; i<cf->num; i++)
  {
    fprintf(f, "%s=%s\n", cf->varname[i], cf->varval[i]);
  }
  fclose(f);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
}

/** check if key exists, return 1 if lookup successed, 0 if failed..*/
static inline int dt_conf_key_exists (const char *key)
{
  dt_pthread_mutex_lock (&darktable.conf->mutex);
  int res = 0;
  /* lookup in stringtable for match of key name */
  for (int i=0; i<darktable.conf->num; i++)
  {
    if (!strncmp (key, darktable.conf->varname[i], DT_CONF_MAX_VAR_BUF))
    {
      res=1;
      break;
    }
  }
  dt_pthread_mutex_unlock (&darktable.conf->mutex);
  return res;
}

#endif
