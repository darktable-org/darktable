#ifndef DARKTABLE_IOP_COLORIN_H
#define DARKTABLE_IOP_COLORIN_H

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <lcms.h>

// max iccprofile file name length
#define DT_IOP_COLOR_ICC_LEN 100

// constants fit to the ones from lcms.h:
typedef enum dt_iop_color_intent_t
{
  DT_INTENT_PERCEPTUAL             = INTENT_PERCEPTUAL,            // 0
  DT_INTENT_RELATIVE_COLORIMETRIC  = INTENT_RELATIVE_COLORIMETRIC, // 1
  DT_INTENT_SATURATION             = INTENT_SATURATION,            // 2
  DT_INTENT_ABSOLUTE_COLORIMETRIC  = INTENT_ABSOLUTE_COLORIMETRIC  // 3
}
dt_iop_color_intent_t;

typedef struct dt_iop_color_profile_t
{
  char filename[512]; // icc file name
  char name[512];     // product name
  int  pos;           // position in combo box    
}
dt_iop_color_profile_t;

typedef struct dt_iop_colorin_params_t
{
  char iccprofile[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t intent;
  // TODO: store color matrix from image
  // TODO: store whether to apply color matrix
}
dt_iop_colorin_params_t;

typedef struct dt_iop_colorin_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2;
  GtkComboBox *cbox1, *cbox2;
  GList *profiles;
}
dt_iop_colorin_gui_data_t;

typedef struct dt_iop_colorin_data_t
{
  cmsHPROFILE input;
  cmsHPROFILE Lab;
  cmsHTRANSFORM xform;
}
dt_iop_colorin_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void reset_params  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

#endif
