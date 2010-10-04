/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#include "gui/gtk.h"
#include <stdlib.h>
pthread_mutex_t clarity_mutex; 

int main (int argc, char *argv[])
{
  if(dt_init(argc, argv)) exit(1);
  pthread_mutex_init(&clarity_mutex, NULL);
  dt_gui_gtk_run(darktable.gui);
  pthread_mutex_destroy(&clarity_mutex);
  exit(0);
}

