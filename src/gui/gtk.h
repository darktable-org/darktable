#ifndef DT_GUI_GTK_H
#define DT_GUI_GTK_H

#include <gtk/gtk.h>
#include <glade/glade.h>
#include "gui/navigation.h"
#include "gui/histogram.h"


typedef struct dt_gui_gtk_t
{
  GladeXML *main_window;
  GdkPixmap *pixmap;
  dt_gui_navigation_t navigation;
  dt_gui_histogram_t histogram;
  int32_t reset;
  float bgcolor[3];
}
dt_gui_gtk_t;

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[]);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);

#endif
