#ifndef DARKTABLE_IOP_GAMMA_H
#define DARKTABLE_IOP_GAMMA_H

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gegl.h>

typedef struct dt_iop_gamma_params_t
{
  float gamma, linear;
}
dt_iop_gamma_params_t;

typedef struct dt_iop_gamma_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2;
  GtkHScale *scale1, *scale2;
}
dt_iop_gamma_gui_data_t;

typedef struct dt_iop_gamma_data_t
{
  // not needed.
}
dt_iop_gamma_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_reset     (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void gamma_callback  (GtkRange *range, gpointer user_data);
void linear_callback (GtkRange *range, gpointer user_data);

#endif
