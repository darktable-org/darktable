#ifndef DT_IOP_COLORCORRECTION_H
#define DT_IOP_COLORCORRECTION_H

// this wraps gegl:whitebalance

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_iop_colorcorrection_params_t
{
  float hia, hib, loa, lob, saturation;
}
dt_iop_colorcorrection_params_t;

typedef struct dt_iop_colorcorrection_gui_data_t
{
  GtkDrawingArea *area;
  GtkHBox *hbox;
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2, *label3, *label4, *label5;
  GtkHScale *scale1, *scale2, *scale3, *scale4, *scale5;
  float press_x, press_y, mouse_x, mouse_y;
  int selected, dragging;
  dt_iop_colorcorrection_params_t press_params;
  cmsHPROFILE hsRGB;
  cmsHPROFILE hLab;
  cmsHTRANSFORM xform;
}
dt_iop_colorcorrection_gui_data_t;

typedef struct dt_iop_colorcorrection_data_t
{
  float a_scale, a_base, b_scale, b_base, saturation;
}
dt_iop_colorcorrection_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void reset_params  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, int x, int y, float scale, int width, int height);

void loa_callback (GtkRange *range, gpointer user_data);
void hia_callback (GtkRange *range, gpointer user_data);
void lob_callback (GtkRange *range, gpointer user_data);
void hib_callback (GtkRange *range, gpointer user_data);
void sat_callback (GtkRange *range, gpointer user_data);
gboolean dt_iop_colorcorrection_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_iop_colorcorrection_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_iop_colorcorrection_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_colorcorrection_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_colorcorrection_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
gboolean dt_iop_colorcorrection_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);

#endif
