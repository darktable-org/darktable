/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "gui/theme_variants.h"

#include "common/darktable.h"
#include "common/file_location.h"
#include "control/conf.h"

#include <string.h>

// reserved group: describes the manifest itself, not a variant
#define DT_VARIANTS_GROUP_THEME "theme"

// bounded so a circular set of @imports cannot recurse forever
#define DT_VARIANTS_MAX_DEPTH 8

static GList *_variants_load(const char *theme, int depth);

static void _variant_free(void *data)
{
  dt_theme_variant_t *v = (dt_theme_variant_t *)data;
  if(!v) return;

  g_free(v->owner);
  g_free(v->key);
  g_free(v->label);
  g_free(v->tooltip);
  g_free(v->css);
  g_free(v->def);
  g_list_free_full(v->values, g_free);
  g_list_free_full(v->labels, g_free);
  if(v->css_override) g_hash_table_destroy(v->css_override);
  g_free(v);
}

void dt_theme_variants_free(GList *variants)
{
  g_list_free_full(variants, _variant_free);
}

// a theme file in the user config dir, else in the data dir. same order
// dt_gui_load_theme uses, so a user theme shadows a shipped one
static char *_theme_file(const char *theme, const char *ext)
{
  if(!theme || !*theme) return NULL;

  char configdir[PATH_MAX] = { 0 }, datadir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  char *name = g_strconcat(theme, ext, NULL);
  char *path = g_build_filename(configdir, "themes", name, NULL);

  if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    g_free(path);
    path = g_build_filename(datadir, "themes", name, NULL);
    if(!g_file_test(path, G_FILE_TEST_EXISTS))
    {
      g_free(path);
      path = NULL;
    }
  }

  g_free(name);
  return path;
}

// the themes `theme` builds on, from its @import lines and in that order.
// chunks are partial CSS, never themes, so they are skipped
static GList *_css_imports(const char *theme)
{
  char *path = _theme_file(theme, ".css");
  if(!path) return NULL;

  char *css = NULL;
  if(!g_file_get_contents(path, &css, NULL, NULL))
  {
    g_free(path);
    return NULL;
  }
  g_free(path);

  GList *imports = NULL;
  GRegex *re = g_regex_new("@import\\s+url\\(\\s*[\"']?([^\"')]+)[\"']?\\s*\\)",
                           0, 0, NULL);
  GMatchInfo *match = NULL;
  g_regex_match(re, css, 0, &match);

  while(g_match_info_matches(match))
  {
    char *name = g_match_info_fetch(match, 1);
    if(name && g_str_has_suffix(name, ".css")
       && !g_str_has_prefix(name, "chunk-"))
    {
      // imports name the file, variants are looked up by theme name
      name[strlen(name) - strlen(".css")] = '\0';
      imports = g_list_append(imports, name);
    }
    else
      g_free(name);

    g_match_info_next(match, NULL);
  }

  g_match_info_free(match);
  g_regex_unref(re);
  g_free(css);

  return imports;
}

// split "a;b;c" as GKeyFile writes it, dropping the empty trailing field
static GList *_string_list(char **raw, gsize length)
{
  GList *list = NULL;
  for(gsize i = 0; i < length; i++)
    if(raw[i] && *raw[i])
      list = g_list_append(list, g_strdup(raw[i]));
  return list;
}

// the option `value` names, or NULL when the theme does not offer it.
// returns the list's own string so callers can borrow it
static const char *_offered(const dt_theme_variant_t *variant,
                            const char *value)
{
  for(GList *l = variant->values; l; l = g_list_next(l))
    if(!g_strcmp0((const char *)l->data, value)) return (const char *)l->data;
  return NULL;
}

static dt_theme_variant_t *_parse_group(GKeyFile *kf,
                                        const char *group,
                                        const char *owner)
{
  dt_theme_variant_t *v = g_malloc0(sizeof(dt_theme_variant_t));
  v->owner = g_strdup(owner);
  v->key = g_strdup(group);

  char *type = g_key_file_get_string(kf, group, "type", NULL);
  v->type = (type && !g_strcmp0(type, "enum"))
    ? DT_THEME_VARIANT_ENUM
    : DT_THEME_VARIANT_BOOL;
  g_free(type);

  // a theme ships its own translations; the key stands in without them
  v->label = g_key_file_get_locale_string(kf, group, "label", NULL, NULL);
  if(!v->label) v->label = g_strdup(group);
  v->tooltip = g_key_file_get_locale_string(kf, group, "tooltip", NULL, NULL);

  v->css = g_key_file_get_string(kf, group, "css", NULL);
  v->def = g_key_file_get_string(kf, group, "default", NULL);

  if(v->type == DT_THEME_VARIANT_ENUM)
  {
    gsize n = 0;
    char **values = g_key_file_get_string_list(kf, group, "values", &n, NULL);
    v->values = values ? _string_list(values, n) : NULL;
    g_strfreev(values);

    // a [<group>/labels] group names the options – one bare key each,
    // which is all intltool will translate. css_<value> overrides the
    // template for one option; empty means it adds no CSS at all, which
    // is how "none" switches a variant off
    char *labels_group = g_strconcat(group, "/labels", NULL);

    for(GList *o = v->values; o; o = g_list_next(o))
    {
      const char *value = (const char *)o->data;

      char *label = g_key_file_get_locale_string(kf, labels_group, value,
                                                 NULL, NULL);
      v->labels = g_list_append(v->labels, label ? label : g_strdup(value));

      char *key = g_strconcat("css_", value, NULL);
      if(g_key_file_has_key(kf, group, key, NULL))
      {
        if(!v->css_override)
          v->css_override = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_free);
        g_hash_table_insert(v->css_override, g_strdup(value),
                            g_key_file_get_string(kf, group, key, NULL));
      }
      g_free(key);
    }
    g_free(labels_group);

    // a default the theme does not offer would show one option in
    // preferences and apply another; an absent default already means the
    // first one, so use that
    if(v->def && !_offered(v, v->def))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[theme_variants] %s: default \"%s\" is not one of its values",
               group, v->def);
      g_free(v->def);
      v->def = NULL;
    }
    if(!v->def && v->values) v->def = g_strdup(v->values->data);
  }
  else if(!v->def)
    v->def = g_strdup("false");

  return v;
}

static GList *_variants_load(const char *theme, int depth)
{
  if(depth > DT_VARIANTS_MAX_DEPTH) return NULL;

  char *path = _theme_file(theme, ".variants");

  if(!path)
  {
    // no manifest of its own: inherit from whatever this theme builds on,
    // which is the same relationship its CSS already states
    GList *result = NULL;
    GList *imports = _css_imports(theme);
    for(GList *l = imports; l && !result; l = g_list_next(l))
      result = _variants_load((const char *)l->data, depth + 1);
    g_list_free_full(imports, g_free);
    return result;
  }

  GKeyFile *kf = g_key_file_new();
  GError *error = NULL;
  if(!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &error))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[theme_variants] cannot read %s: %s",
             path, error ? error->message : "unknown error");
    if(error) g_error_free(error);
    g_key_file_free(kf);
    g_free(path);
    return NULL;
  }
  g_free(path);

  GList *variants = NULL;

  // a manifest that names a parent extends it rather than replacing it,
  // so a theme can add one variant without restating the family's
  char *inherits = g_key_file_get_string(kf, DT_VARIANTS_GROUP_THEME,
                                         "inherits", NULL);
  if(inherits && *inherits)
    variants = _variants_load(inherits, depth + 1);
  g_free(inherits);

  gsize n = 0;
  char **groups = g_key_file_get_groups(kf, &n);
  for(gsize i = 0; groups && i < n; i++)
  {
    if(!g_strcmp0(groups[i], DT_VARIANTS_GROUP_THEME)) continue;
    if(strchr(groups[i], '/')) continue;  // [<group>/labels], read below

    dt_theme_variant_t *v = _parse_group(kf, groups[i], theme);

    // a theme redeclaring an inherited variant replaces it in place, so
    // the controls keep the order the parent gave them
    GList *prev = NULL;
    for(GList *l = variants; l && !prev; l = g_list_next(l))
      if(!g_strcmp0(((dt_theme_variant_t *)l->data)->key, v->key)) prev = l;

    if(prev)
    {
      _variant_free(prev->data);
      prev->data = v;
    }
    else
      variants = g_list_append(variants, v);
  }
  g_strfreev(groups);
  g_key_file_free(kf);

  return variants;
}

// themes/condensed shipped before variants were per theme. carry it over
// once, or everyone who enabled it silently gets it switched off
static void _migrate_legacy_keys(void)
{
  static gboolean done = FALSE;
  if(done) return;
  done = TRUE;

  if(dt_conf_key_exists("themes/darktable/condensed")
     || !dt_conf_key_exists("themes/condensed"))
    return;

  dt_conf_set_string("themes/darktable/condensed",
                     dt_conf_get_bool("themes/condensed") ? "true" : "false");
}

GList *dt_theme_variants_load(const char *theme)
{
  _migrate_legacy_keys();
  return _variants_load(theme, 0);
}

char *dt_theme_variant_conf_key(const dt_theme_variant_t *variant)
{
  if(!variant) return NULL;
  return g_strdup_printf("themes/%s/%s", variant->owner, variant->key);
}

const char *dt_theme_variant_get(const dt_theme_variant_t *variant)
{
  if(!variant) return "";

  char *key = dt_theme_variant_conf_key(variant);
  char *stored = dt_conf_key_exists(key) ? dt_conf_get_string(key) : NULL;
  g_free(key);

  const char *result = variant->def ? variant->def : "";

  if(stored && *stored)
  {
    if(variant->type == DT_THEME_VARIANT_BOOL)
    {
      result = !g_strcmp0(stored, "true") ? "true" : "false";
    }
    else
    {
      // a value the theme no longer offers falls back to the default
      const char *offered = _offered(variant, stored);
      if(offered) result = offered;
    }
  }
  g_free(stored);

  return result;
}

char *dt_theme_variant_css(const dt_theme_variant_t *variant,
                           const char *value)
{
  if(!variant || !value) return NULL;

  if(variant->type == DT_THEME_VARIANT_BOOL)
    return g_strcmp0(value, "true") ? NULL : g_strdup(variant->css);

  if(variant->css_override
     && g_hash_table_contains(variant->css_override, value))
  {
    const char *css = g_hash_table_lookup(variant->css_override, value);
    return (css && *css) ? g_strdup(css) : NULL;
  }

  if(!variant->css) return NULL;

  char *css = dt_util_str_replace(variant->css, "$value", value);
  if(css && !*css)
  {
    g_free(css);
    css = NULL;
  }
  return css;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files
// by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on;
// clang-format on
