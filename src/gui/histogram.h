#ifndef DARKTABLE_GUI_HISTOGRAM_H
#define DARKTABLE_GUI_HISTOGRAM_H

#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_gui_histogram_t
{
  int32_t dragging;
}
dt_gui_histogram_t;

void dt_gui_histogram_init(dt_gui_histogram_t *n, GtkWidget *widget);
void dt_gui_histogram_cleanup(dt_gui_histogram_t *n);
gboolean dt_gui_histogram_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_gui_histogram_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_gui_histogram_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_histogram_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_histogram_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
void dt_gui_histogram_draw_8(cairo_t *cr, float *hist, int32_t channel);

#endif
