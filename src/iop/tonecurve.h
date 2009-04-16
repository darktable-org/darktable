#ifndef DARKTABLE_IOP_CURVE_EDITOR_H
#define DARKTABLE_IOP_CURVE_EDITOR_H

#include <gtk/gtk.h>
#include <inttypes.h>
#include <gegl.h>

#include "common/nikon_curve.h"
#include "control/settings.h"

#define DT_IOP_TONECURVE_RES 64

typedef struct dt_iop_tonecurve_params_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
}
dt_iop_tonecurve_params_t;

typedef struct dt_iop_tonecurve_gui_data_t
{
  GtkVBox *vbox;
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkLabel *label;
  GtkComboBox *presets;
  double mouse_x, mouse_y;
  int selected, dragging;
  double selected_offset, selected_y, selected_min, selected_max;
  gdouble xs[DT_IOP_TONECURVE_RES], ys[DT_IOP_TONECURVE_RES];
}
dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  GeglNode *node;
  const gchar *const input_pad  = "input";
  const gchar *const output_pad = "output";
}
dt_iop_tonecurve_data_t;

void dt_iop_tonecurve_init(dt_iop_module_t *module);
void dt_iop_tonecurve_cleanup(dt_iop_module_t *module);

void dt_iop_tonecurve_gui_reset   (struct dt_iop_module_t *self);
void dt_iop_tonecurve_gui_init    (struct dt_iop_module_t *self);
void dt_iop_tonecurve_gui_cleanup (struct dt_iop_module_t *self);
gboolean dt_iop_tonecurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);

void dt_iop_tonecurve_get_output_pad(struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);
void dt_iop_tonecurve_get_input_pad (struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);


void dt_gui_curve_editor_init(dt_gui_curve_editor_t *c, GtkWidget *widget);
void dt_gui_curve_editor_cleanup(dt_gui_curve_editor_t *c);
gboolean dt_gui_curve_editor_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_gui_curve_editor_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_gui_curve_editor_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_curve_editor_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_gui_curve_editor_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
void dt_gui_curve_editor_get_curve(dt_gui_curve_editor_t *c, uint16_t *curve, dt_ctl_image_settings_t *settings);

#endif
