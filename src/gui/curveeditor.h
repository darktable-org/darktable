#ifndef DARKTABLE_CURVE_EDITOR_H
#define DARKTABLE_CURVE_EDITOR_H

#include <gtk/gtk.h>
#include <inttypes.h>

#include "common/nikon_curve.h"
#include "control/settings.h"

typedef struct dt_gui_curve_editor_t
{
  double mouse_x, mouse_y;
  int selected, dragging;
  double selected_offset, selected_y, selected_min, selected_max;
  CurveData curve;
  CurveSample draw, draw_max, draw_min;
  CurveSample convert;
}
dt_gui_curve_editor_t;

void dt_gui_curve_editor_init(dt_gui_curve_editor_t *c, GtkWidget *widget);
void dt_gui_curve_editor_cleanup(dt_gui_curve_editor_t *c);
gboolean dt_gui_curve_editor_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_gui_curve_editor_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_gui_curve_editor_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_curve_editor_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_curve_editor_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
void dt_gui_curve_editor_get_curve(dt_gui_curve_editor_t *c, uint16_t *curve, dt_ctl_image_settings_t *settings);

#endif
