/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include "common/printing.h"

int dt_printing_get_image_box(dt_images_box *imgs, const int x, const int y)
{
  int box = -1;

  for(int k=0; k<imgs->count; k++)
  {
    dt_image_box *b = &imgs->box[k];

    // check if over a box
    if(x > b->screen.x && x < (b->screen.x + b->screen.width)
       && y > b->screen.y && y < (b->screen.y + b->screen.height))
    {
      box = k;
      break;
    }
  }

  return box;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
