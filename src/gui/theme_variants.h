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

#pragma once

#include <glib.h>

// Options a theme offers on top of its CSS, declared by the theme rather
// than by darktable. A theme ships <theme>.variants beside <theme>.css:
//
//   [accent-color]
//   type=enum
//   label=accent color
//   label[de]=Akzentfarbe
//   values=none;green;blue
//   default=none
//   css=chunk-$value-accent-color.css
//   css.none=
//
// Each group is one control in preferences and one CSS chunk imported
// after the theme. A theme with no manifest inherits the one belonging to
// the theme it @imports, so a family declares its variants once.

typedef enum dt_theme_variant_type_t
{
  DT_THEME_VARIANT_BOOL = 0,
  DT_THEME_VARIANT_ENUM
} dt_theme_variant_type_t;

typedef struct dt_theme_variant_t
{
  char *owner;      // theme that declares this; namespaces the conf key
  char *key;        // group name, e.g. "accent-color"
  char *label;      // localized, for the preferences control
  char *tooltip;    // localized, may be NULL
  dt_theme_variant_type_t type;
  GList *values;    // char*, option ids; NULL for a bool
  GList *labels;    // char*, localized, same length as values
  char *css;        // chunk name, "$value" replaced by the option id
  GHashTable *css_override;  // option id -> chunk name, "" for none
  char *def;        // default option id, or "true"/"false" for a bool
} dt_theme_variant_t;

/**
 * @brief The variants offered by `theme`, in declaration order
 *
 * Resolves <theme>.variants from the user config dir, then the data dir.
 * A theme without one inherits through its CSS @import chain; a theme
 * with one may add `inherits=` to extend rather than replace.
 *
 * @param theme Theme name without extension, e.g. "darktable-elegant-grey"
 * @return List of dt_theme_variant_t*, empty when nothing is declared.
 *         Free with dt_theme_variants_free()
 */
GList *dt_theme_variants_load(const char *theme);

void dt_theme_variants_free(GList *variants);

/**
 * @brief Where this variant's value is stored, "themes/<owner>/<key>"
 * @return Newly allocated string, free with g_free()
 */
char *dt_theme_variant_conf_key(const dt_theme_variant_t *variant);

/**
 * @brief The option currently selected, falling back to the declared
 *        default. A value no longer offered by the theme is ignored.
 * @return Borrowed option id, valid while `variant` is; never NULL
 */
const char *dt_theme_variant_get(const dt_theme_variant_t *variant);

/**
 * @brief The CSS chunk `value` asks for
 * @return Newly allocated chunk filename, or NULL when this option adds
 *         no CSS. Free with g_free()
 */
char *dt_theme_variant_css(const dt_theme_variant_t *variant,
                           const char *value);

// clang-format off
// modelines: These editor modelines have been set for all relevant files
// by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on;
// clang-format on
