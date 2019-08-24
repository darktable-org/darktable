/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

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

#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

// this module could appear in the panels, but to save space I've found better
// to hide it behing the preference button in export images panel.
// so here the minimum to get the presets work.

const char *name(dt_lib_module_t *self)
{
  return _("export_metadata");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position()
{
  return 1;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

void gui_reset(dt_lib_module_t *self)
{
}

void gui_init(dt_lib_module_t *self)
{
  self->gui_reset(self);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

void init_presets(dt_lib_module_t *self)
{
}

void *get_params(dt_lib_module_t *self, int *size)
{
  return NULL;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  return 0;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
