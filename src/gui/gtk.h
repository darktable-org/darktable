#ifndef DT_GUI_GTK_H
#define DT_GUI_GTK_H

#include <gtk/gtk.h>
#include <glade/glade.h>
#include "gui/navigation.h"
#include "gui/histogram.h"

typedef struct dt_gui_key_accel_t
{
  guint   state;
  guint   keyval;
  guint16 hardware_keycode;
  void (*callback)(void *);
  void *data;
}
dt_gui_key_accel_t;

typedef struct dt_gui_gtk_t
{
  GladeXML *main_window;
  GdkPixmap *pixmap;
  GList *redraw_widgets;
  GList *key_accels;
  dt_gui_navigation_t navigation;
  dt_gui_histogram_t histogram;
  int32_t reset;
  float bgcolor[3];
}
dt_gui_gtk_t;

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[]);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);

/** register an accel callback for the whole window. data is not freed on unregister. */
void dt_gui_key_accel_register(guint state, guint keyval, void (*callback)(void *), void *data);
void dt_gui_key_accel_unregister(void (*callback)(void *));

#endif
