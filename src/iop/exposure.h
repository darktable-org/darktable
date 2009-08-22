#ifndef DARKTABLE_IOP_EXPOSURE_H
#define DARKTABLE_IOP_EXPOSURE_H

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>

typedef struct dt_iop_exposure_params_t
{
  float black, white, gain;
}
dt_iop_exposure_params_t;

typedef struct dt_iop_exposure_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2, *label3;
  GtkHScale *scale1, *scale2, *scale3;
}
dt_iop_exposure_gui_data_t;

typedef struct dt_iop_exposure_data_t
{
  float black, scale, gain;
}
dt_iop_exposure_data_t;

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

void black_callback  (GtkRange *range, gpointer user_data);
void white_callback (GtkRange *range, gpointer user_data);
void gain_callback (GtkRange *range, gpointer user_data);

#endif
