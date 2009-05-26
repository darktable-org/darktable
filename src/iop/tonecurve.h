#ifndef DARKTABLE_IOP_CURVE_EDITOR_H
#define DARKTABLE_IOP_CURVE_EDITOR_H

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gegl.h>

#define DT_IOP_TONECURVE_RES 64

typedef struct dt_iop_tonecurve_params_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
}
dt_iop_tonecurve_params_t;

typedef struct dt_iop_tonecurve_gui_data_t
{
  GeglCurve *minmax_curve;        // curve for gui to draw
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkLabel *label;
  GtkComboBox *presets;
  double mouse_x, mouse_y;
  int selected, dragging;
  double selected_offset, selected_y, selected_min, selected_max;
  gdouble draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  gdouble draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  gdouble draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
}
dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  GeglCurve *curve;               // curve for gegl nodes and pixel processing
  GeglNode *node, *node_preview;  // dual pixel pipeline
  gchar input_pad[20];
  gchar output_pad[20];
}
dt_iop_tonecurve_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_reset    (struct dt_iop_module_t *self);
void commit_params(struct dt_iop_module_t *self);
void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void get_output_pad(struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);
void get_input_pad (struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);
void get_preview_output_pad(struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);
void get_preview_input_pad (struct dt_iop_module_t *self, GeglNode **node, const gchar **pad);

gboolean dt_iop_tonecurve_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_tonecurve_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);

#endif
