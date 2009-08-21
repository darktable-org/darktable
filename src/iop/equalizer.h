#ifndef DARKTABLE_IOP_EQUALIZER_H
#define DARKTABLE_IOP_EQUALIZER_H

#include "develop/imageop.h"
#include "gui/draw.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_IOP_EQUALIZER_RES 64
#define DT_IOP_EQUALIZER_BANDS 5
#define DT_IOP_EQUALIZER_MAX_LEVEL 6

typedef struct dt_iop_equalizer_params_t
{
  float equalizer_x[3][DT_IOP_EQUALIZER_BANDS], equalizer_y[3][DT_IOP_EQUALIZER_BANDS];
}
dt_iop_equalizer_params_t;

typedef enum dt_iop_equalizer_channel_t
{
  DT_IOP_EQUALIZER_L = 0,
  DT_IOP_EQUALIZER_a = 1,
  DT_IOP_EQUALIZER_b = 2
}
dt_iop_equalizer_channel_t;

typedef struct dt_iop_equalizer_gui_data_t
{
  dt_draw_curve_t *minmax_curve;        // curve for gui to draw
  GtkHBox *hbox;
  GtkDrawingArea *area;
  GtkRadioButton *channel_button[3];
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_equalizer_params_t drag_params;
  int dragging;
  dt_iop_equalizer_channel_t channel;
  double draw_xs[DT_IOP_EQUALIZER_RES], draw_ys[DT_IOP_EQUALIZER_RES];
  double draw_min_xs[DT_IOP_EQUALIZER_RES], draw_min_ys[DT_IOP_EQUALIZER_RES];
  double draw_max_xs[DT_IOP_EQUALIZER_RES], draw_max_ys[DT_IOP_EQUALIZER_RES];
  float band_hist[DT_IOP_EQUALIZER_BANDS];
  float band_max;
}
dt_iop_equalizer_gui_data_t;

typedef struct dt_iop_equalizer_data_t
{
  dt_draw_curve_t *curve[3];
  int num_levels;
}
dt_iop_equalizer_data_t;

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

gboolean dt_iop_equalizer_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
gboolean dt_iop_equalizer_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
gboolean dt_iop_equalizer_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_equalizer_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
gboolean dt_iop_equalizer_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
gboolean dt_iop_equalizer_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data);
void dt_iop_equalizer_button_toggled(GtkToggleButton *togglebutton, gpointer user_data);

#endif
