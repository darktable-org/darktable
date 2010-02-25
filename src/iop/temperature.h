
#ifndef DT_IOP_COLOR_TEMPERATURE
#define DT_IOP_COLOR_TEMPERATURE
// plug-in frontend for gegl:color-temperature: (in sRGB)

#include "develop/imageop.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define DT_IOP_LOWEST_TEMPERATURE     3000
#define DT_IOP_HIGHEST_TEMPERATURE   12000


static const float dt_iop_temperature_rgb_r55[][12];

typedef struct dt_iop_temperature_params_t
{
  float temp_out;
  float coeffs[3];
}
dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkVBox *vbox1, *vbox2;
  GtkLabel *label1, *label2;
  GtkHScale *scale_k, *scale_tint, *scale_k_out, *scale_r, *scale_g, *scale_b;
  GtkComboBox *presets;
  GtkSpinButton *finetune;
  int preset_cnt;
  int preset_num[50];
}
dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[3];
}
dt_iop_temperature_data_t;

void init(dt_iop_module_t *module);
void cleanup(dt_iop_module_t *module);

void gui_update    (struct dt_iop_module_t *self);
void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void init_pipe     (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);
void cleanup_pipe  (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece);

void gui_init     (struct dt_iop_module_t *self);
void gui_cleanup  (struct dt_iop_module_t *self);

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out);

static void tint_callback     (GtkRange *range, gpointer user_data);
static void temp_callback     (GtkRange *range, gpointer user_data);
static void temp_out_callback (GtkRange *range, gpointer user_data);
static void rgb_callback      (GtkRange *range, gpointer user_data);
static void presets_changed   (GtkComboBox *widget, gpointer user_data);
static void finetune_changed  (GtkSpinButton *widget, gpointer user_data);

#endif
