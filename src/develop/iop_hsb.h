#ifndef IOP_HSB_H
#define IOP_HSB_H

#include "develop/develop.h"

void dt_iop_gui_reset_hsb ();
void dt_iop_gui_init_hsb ();
void dt_iop_gui_callback_hsb (GtkRange *range, gpointer user_data);
void dt_iop_execute_hsb (float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                 dt_dev_operation_t operation, dt_dev_operation_params_t *params);

#endif
