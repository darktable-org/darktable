/*
    This file is part of darktable,
    copyright (c) 2011 robert bieber.

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

#ifndef DARKTABLE_ACCELERATORS_H
#define DARKTABLE_ACCELERATORS_H

#include <gtk/gtk.h>

#include "develop/imageop.h"
#include "views/view.h"
#include "libs/lib.h"

typedef struct dt_accel_t
{

  gchar path[256];
  gchar translated_path[256];
  gchar module[256];
  guint views;
  gboolean local;

} dt_accel_t;

// Accel path string building functions
void dt_accel_path_global(char *s, size_t n, const char* path);
void dt_accel_path_view(char *s, size_t n, char *module,
                        const char* path);
void dt_accel_path_iop(char *s, size_t n, char *module,
                       const char *path);
void dt_accel_path_lib(char *s, size_t n, char *module,
                       const char* path);
/**
  * Accepts an array of 4 char*, writes the following paths to them
  * 0 - Slider increase path
  * 1 - Slider decrease path
  * 2 - Slider reset path
  * 3 - Slider edit path
  */
void dt_accel_paths_slider_iop(char *s[], size_t n, char *module,
                               const char *path);

// Accelerator registration functions
void dt_accel_register_global(const gchar *path, guint accel_key,
                              GdkModifierType mods);
void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key,
                            GdkModifierType mods);
void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path, guint accel_key,
                           GdkModifierType mods);
void dt_accel_register_lib(dt_lib_module_t *self,const gchar *path,
                           guint accel_key, GdkModifierType mods);
void dt_accel_register_slider_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path);

// Accelerator connection functions
void dt_accel_connect_global(const gchar *path, GClosure *closure);
void dt_accel_connect_view(dt_view_t *self, const gchar *path,
                           GClosure *closure);
void dt_accel_connect_iop(dt_iop_module_t *module, const gchar *path,
                          GClosure *closure);
void dt_accel_connect_lib(dt_lib_module_t *module, const gchar *path,
                          GClosure *closure);
void dt_accel_connect_button_iop(dt_iop_module_t *module, const gchar *path,
                                 GtkWidget *button);
void dt_accel_connect_button_lib(dt_lib_module_t *module, const gchar *path,
                                 GtkWidget *button);
void dt_accel_connect_slider_iop(dt_iop_module_t *module, const gchar *path,
                                 GtkWidget *slider);
void dt_accel_connect_locals_iop(dt_iop_module_t *module);
void dt_accel_connect_preset_iop(dt_iop_module_t *so, const gchar *path);

// Disconnect function
void dt_accel_disconnect_list(GSList *accels);
void dt_accel_disconnect_locals_iop(dt_iop_module_t *module);
void dt_accel_cleanup_locals_iop(dt_iop_module_t *module);

// Deregister functions
void dt_accel_deregister_locals_iop(dt_iop_module_t *module,const gchar *path);

#endif
