/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gtk/gtk.h>
#include <gmodule.h>
#ifndef __ANDROID__
#error This is an Android ABI probe, not a host GTK test
#endif

/* Safe before gtk_init(): no display, window, Broadway server or browser is
 * created. This entry point is for a future Android instrumentation runner. */
G_MODULE_EXPORT int dt_mobile_gtk_probe(void)
{
  if(gtk_get_major_version() != GTK_MAJOR_VERSION) return -1;
  if(gtk_check_version(3, 24, 0) != NULL) return -2;
  if(gtk_widget_get_type() == G_TYPE_INVALID) return -3;
  return (int)gtk_get_major_version();
}
