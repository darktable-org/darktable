#ifndef DT_IOP_LENS_H
#define DT_IOP_LENS_H

#include "develop/imageop.h"
#include <lensfun.h>
#include <gtk/gtk.h>
#include <inttypes.h>
  
typedef struct dt_iop_lensfun_params_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
  char camera[52];
  char lens[52];
}
dt_iop_lensfun_params_t;

typedef struct dt_iop_lensfun_gui_data_t
{
  const lfCamera *camera;
  GtkEntry *camera_model;
  GtkMenu *camera_menu;
  GtkEntry *lens_model;
  GtkMenu *lens_menu;
}
dt_iop_lensfun_gui_data_t;

typedef struct dt_iop_lensfun_data_t
{
  lfLens *lens;
  float *tmpbuf;
  float *tmpbuf2;
  size_t tmpbuf_len;
  size_t tmpbuf2_len;
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  lfLensType target_geom;
}
dt_iop_lensfun_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

const char *name();
void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void reset_params  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in);
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

#endif
