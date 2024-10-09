/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "common/tags.h"

G_BEGIN_DECLS

// GObject of dt_tag_t
#define DT_TYPE_TAG_OBJ dt_tag_obj_get_type()
G_DECLARE_FINAL_TYPE(DtTagObj, dt_tag_obj, DT, TAG_OBJ, GObject)

struct _DtTagObj
{
  GObject parent_instance;
  dt_tag_t tag;
};

GObject *dt_tag_obj_new(const dt_tag_t *tag);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

