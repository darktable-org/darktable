/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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

#include <gtk/gtk.h>

// check whether the given container has any user-added children
gboolean dtgtk_container_has_children(GtkContainer *container);
// return a count of the user-added children in the given container
int dtgtk_container_num_children(GtkContainer *container);
// return the first child of the given container
GtkWidget *dtgtk_container_first_child(GtkContainer *container);
// return the requested child of the given container, or NULL if it has fewer children
GtkWidget *dtgtk_container_nth_child(GtkContainer *container, int which);

// remove all of the children we've added to the container.  Any which no longer have any references will
// be destroyed.
void dtgtk_container_remove_children(GtkContainer *container);

// delete all of the children we've added to the container.  Use this function only if you are SURE
// there are no other references to any of the children (if in doubt, use dtgtk_container_remove_children
// instead; it's a bit slower but safer).
void dtgtk_container_destroy_children(GtkContainer *container);
