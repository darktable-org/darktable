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

// Accelerator registration functions
void dt_accel_register_global(const gchar *path, guint accel_key,
                              GdkModifierType mods);
void dt_accel_register_view(dt_view_t *self, const gchar *path, guint accel_key,
                            GdkModifierType mods);
void dt_accel_register_iop(dt_iop_module_so_t *so, gboolean local,
                           const gchar *path, guint accel_key,
                           GdkModifierType mods);
void dt_accel_register_lib(dt_lib_module_t *self, gboolean local,
                           const gchar *path, guint accel_key,
                           GdkModifierType mods);
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

// Disconnect function
void dt_accel_disconnect_list(GSList *accels);

#endif
