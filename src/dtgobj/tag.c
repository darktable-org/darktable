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

#include "tag.h"

G_DEFINE_TYPE (DtTagObj, dt_tag_obj, G_TYPE_OBJECT)

static void _dt_tag_obj_dispose(GObject *gobject)
{
  G_OBJECT_CLASS(dt_tag_obj_parent_class)->dispose(gobject);
}

static void _dt_tag_obj_finalize(GObject *gobject)
{
  DtTagObj *self = DT_TAG_OBJ(gobject);

  g_free(self->tag.tag);
  self->tag.tag = NULL;
  g_free(self->tag.leave);
  self->tag.leave = NULL;
  g_free(self->tag.synonym);
  self->tag.synonym = NULL;

  G_OBJECT_CLASS(dt_tag_obj_parent_class)->finalize (gobject);  
}

static void dt_tag_obj_class_init(DtTagObjClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->dispose = _dt_tag_obj_dispose;
  object_class->finalize = _dt_tag_obj_finalize;
}

static void dt_tag_obj_init(DtTagObj *self)
{
}

GObject *dt_tag_obj_new(const dt_tag_t *tag)
{
  DtTagObj *self = g_object_new(dt_tag_obj_get_type(), NULL);
  memcpy(&self->tag, tag, sizeof(dt_tag_t));
  self->tag.tag = g_strdup(tag->tag);
  self->tag.leave = g_strdup(tag->leave);
  self->tag.synonym = g_strdup(tag->synonym);
  return (GObject *)self;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

