/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include <gtk/gtk.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef enum dt_confgen_type_t
{
  DT_INT,
  DT_INT64,
  DT_FLOAT,
  DT_BOOL,
  DT_PATH,
  DT_STRING,
  DT_ENUM
} dt_confgen_type_t;

typedef struct dt_confgen_value_t
{
  dt_confgen_type_t type;
  char *def;
  char *min;
  char *max;
  char *enum_values;
  char *shortdesc;
  char *longdesc;
} dt_confgen_value_t;

typedef struct dt_conf_t
{
  dt_pthread_mutex_t mutex;
  char filename[PATH_MAX];
  GHashTable *table;
  GHashTable *x_confgen;
  GHashTable *override_entries;
} dt_conf_t;

typedef struct dt_conf_string_entry_t
{
  char *key;
  char *value;
} dt_conf_string_entry_t;

typedef enum dt_confgen_value_kind_t
{
  DT_DEFAULT,
  DT_MIN,
  DT_MAX,
  DT_VALUES
} dt_confgen_value_kind_t;

void dt_conf_set_int(const char *name, int val);
void dt_conf_set_int64(const char *name, int64_t val);
void dt_conf_set_float(const char *name, float val);
void dt_conf_set_bool(const char *name, int val);
void dt_conf_set_string(const char *name, const char *val);
void dt_conf_set_folder_from_file_chooser(const char *name, GtkFileChooser *chooser);
int dt_conf_get_int_fast(const char *name);
int dt_conf_get_int(const char *name);
int64_t dt_conf_get_int64_fast(const char *name);
int64_t dt_conf_get_int64(const char *name);
float dt_conf_get_float_fast(const char *name);
float dt_conf_get_float(const char *name);
int dt_conf_get_and_sanitize_int(const char *name, int min, int max);
int64_t dt_conf_get_and_sanitize_int64(const char *name, int64_t min, int64_t max);
float dt_conf_get_and_sanitize_float(const char *name, float min, float max);
int dt_conf_get_bool(const char *name);
// get the configuration string without duplicating it; the returned string will be invalidated by any
// subsequent dt_conf_set_string call
const char *dt_conf_get_string_const(const char *name);
// get a freshly-allocated duplicate of the configuration string; safe to use even if calling dt_conf_set_string
gchar *dt_conf_get_string(const char *name);
gboolean dt_conf_get_folder_to_file_chooser(const char *name, GtkFileChooser *chooser);
gboolean dt_conf_is_equal(const char *name, const char *value);
void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries);
void dt_conf_cleanup(dt_conf_t *cf);
void dt_conf_save(dt_conf_t *cf);
int dt_conf_key_exists(const char *key);
gboolean dt_conf_key_not_empty(const char *key);
GSList *dt_conf_all_string_entries(const char *dir);
void dt_conf_string_entry_free(gpointer data);

#define DT_CONF_SET_SANITIZED_INT(name, val, min, max) dt_conf_set_int(name, CLAMPS(val, min,max));
#define DT_CONF_SET_SANITIZED_INT6464(name, val, min, max) dt_conf_set_int(name, CLAMPS(val, min,max));
#define DT_CONF_SET_SANITIZED_FLOAT(name, val, min, max) dt_conf_set_float(name, CLAMPS(val, min,max));

// conf generated from darktable config XML

gboolean dt_confgen_exists(const char *name);
dt_confgen_type_t dt_confgen_type(const char *name);

gboolean dt_confgen_value_exists(const char *name, dt_confgen_value_kind_t kind);

int dt_confgen_get_int(const char *name, dt_confgen_value_kind_t kind);
int64_t dt_confgen_get_int64(const char *name, dt_confgen_value_kind_t kind);
gboolean dt_confgen_get_bool(const char *name, dt_confgen_value_kind_t kind);
float dt_confgen_get_float(const char *name, dt_confgen_value_kind_t kind);
const char *dt_confgen_get(const char *name, dt_confgen_value_kind_t kind);

const char *dt_confgen_get_label(const char *name);
const char *dt_confgen_get_tooltip(const char *name);

gboolean dt_conf_is_default(const char *name);
gchar* dt_conf_expand_default_dir(const char *dir);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

