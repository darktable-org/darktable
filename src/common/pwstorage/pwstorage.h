// This file is part of darktable
// Copyright (c) 2010-2016 Tobias Ellinghaus <me@houz.org>.

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

#pragma once

#include "common/darktable.h"

typedef enum pw_storage_backend_t
{
  PW_STORAGE_BACKEND_NONE = 0,
  PW_STORAGE_BACKEND_KWALLET,
  PW_STORAGE_BACKEND_LIBSECRET
} pw_storage_backend_t;

/** pwstorage context */
typedef struct dt_pwstorage_t
{
  pw_storage_backend_t pw_storage_backend;
  void *backend_context;
} dt_pwstorage_t;

/** Initializes a new pwstorage context. */
const dt_pwstorage_t *dt_pwstorage_new();
/** Cleanup and destroy pwstorage context. \remarks After this point pointer at pwstorage is invalid. */
void dt_pwstorage_destroy(const dt_pwstorage_t *pwstorage);
/** Store (key,value) pairs. */
gboolean dt_pwstorage_set(const gchar *slot, GHashTable *table);
/** Load (key,value) pairs. */
GHashTable *dt_pwstorage_get(const gchar *slot);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
