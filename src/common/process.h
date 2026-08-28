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

G_BEGIN_DECLS

/* run a shell command synchronously and return its exit-code integer

   On Linux/macOS this is a thin wrapper around system(). On Windows it runs the
   command through the interpreter configured by COMSPEC, with a system-directory
   cmd.exe fallback, but uses CREATE_NO_WINDOW so a console child spawned from a
   console-less GUI parent no longer flashes a console window (issue #17193)

   Returns the exit-code integer, or -1 if the command could not be spawned or
   cmd is NULL */
int dt_exec_command_sync(const char *cmd);

G_END_DECLS
