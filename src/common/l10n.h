/*
 *    This file is part of darktable,
 *    copyright (c) 2017--2018 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <glib.h>

typedef struct dt_l10n_language_t
{
  char *code, *base_code;
  char *name;
  gboolean is_default;
} dt_l10n_language_t;

typedef struct dt_l10n_t
{
  GList *languages;
  int selected;
  int sys_default;
} dt_l10n_t;

dt_l10n_t *dt_l10n_init(gboolean init_list);

const char *dt_l10n_get_name(const dt_l10n_language_t *language);
