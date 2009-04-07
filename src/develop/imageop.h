#ifndef DT_DEVELOP_IMAGEOP_H
#define DT_DEVELOP_IMAGEOP_H

#include "develop/develop.h"
#include "develop/iop_hsb.h"

#define dt_red 2
#define dt_green 1
#define dt_blue 0

typedef struct dt_iop_module_t
{
  dt_dev_operation_t op;  // TODO: typedef char[20]
  void (*gui_reset) ();   // TODO: darktable as param
  void (*gui_init) ();    // TODO: return container
  void (*gui_cleanup) (); // TODO: pass this container
  void (*execute) (float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                 dt_dev_operation_t operation, dt_dev_operation_params_t *params);
}
dt_iop_module_t;

typedef struct dt_iop_t
{
  int32_t num_modules;
  dt_iop_module_t *module;
}
dt_iop_t;


// job that transforms in pixel block to out pixel block, 4x supersampling
void dt_iop_clip_and_zoom(const float *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                float *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh);

uint32_t dt_iop_create_histogram_final_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist, uint8_t *gamma, uint16_t *tonecurve);
uint32_t dt_iop_create_histogram_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist);
uint32_t dt_iop_create_histogram_8 (const uint8_t  *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist);

void dt_iop_execute(float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                    dt_dev_operation_t operation, dt_dev_operation_params_t *params);

void dt_iop_gui_reset();
void dt_iop_gui_init();

#endif
