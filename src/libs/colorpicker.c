/*
    This file is part of darktable,
    copyright (c) 2011 Robert Bieber.

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

#include "common/darktable.h"
#include "control/control.h"
#include "control/conf.h"
#include "libs/lib.h"
#include "gui/gtk.h"

DT_MODULE(1);

typedef struct dt_lib_colorpicker_t
{
  int nothing_to_store_yet;
} dt_lib_colorpicker_t;

const char *name()
{
  return _("color picker");
}

uint32_t views()
{
  return DT_VIEW_DARKROOM;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable()
{
  return 1;
}

int position()
{
  return 800;
}

void gui_init(dt_lib_module_t *self)
{
  // Initializing self data structure
  dt_lib_colorpicker_t *data =
      (dt_lib_colorpicker_t*)malloc(sizeof(dt_lib_colorpicker_t));
  self->data = (void*)data;
  memset(data, 0, sizeof(dt_lib_colorpicker_t));

  self->widget = gtk_button_new_with_label("color picker");
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}
