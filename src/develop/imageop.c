#include "control/control.h"
#include "develop/imageop.h"
#include <strings.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void dt_iop_init(dt_iop_t *iop)
{
  iop->module = NULL;
  // TODO: add dummy module entries for fixed function pipeline with empty execute etc.
  // TODO: query modules directory (-ies)
  // TODO: read libmodule.so
  // TODO: init dt_iop_module_t:
  //       name, functions, execute gui_init method:
  // TODO: gui system: add expander to right panel, gui_init return: a container
  // TODO: develop read history: check if operation is available in this distro!
  // TODO: qsort module by name!
}

void dt_iop_cleanup(dt_iop_t *iop)
{
  // TODO:
  // for(int k=0;k<iop->num_modules) iop->module[k].gui_cleanup();
  free(iop->module);
}

void dt_iop_gui_reset()
{
  // TODO: reset all modules
  dt_iop_gui_reset_hsb();
}

void dt_iop_gui_init()
{
  // TODO: init all modules' guis?
  dt_iop_gui_init_hsb();
}

void dt_iop_execute(float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
                    dt_dev_operation_t operation, dt_dev_operation_params_t *params)
{
  // TODO: bsearch module op => execute (else print some error)
  if(strncmp(operation, "hsb", 20) == 0)
  {
    dt_iop_execute_hsb(dst, src, wd, ht, bufwd, bufht, operation, params);
  }
  // fprintf(stderr, "[dt_iop_execute] unknown operation %d\n", operation);
}

void dt_iop_clip_and_zoom(const float *i, int32_t ix, int32_t iy, int32_t iw, int32_t ih, int32_t ibw, int32_t ibh,
                                float *o, int32_t ox, int32_t oy, int32_t ow, int32_t oh, int32_t obw, int32_t obh)
{
  const float scalex = iw/(float)ow;
  const float scaley = ih/(float)oh;
  int32_t ix2 = MAX(ix, 0);
  int32_t iy2 = MAX(iy, 0);
  int32_t ox2 = MAX(ox, 0);
  int32_t oy2 = MAX(oy, 0);
  int32_t oh2 = MIN(MIN(oh, (ibh - iy2)/scaley), obh - oy2);
  int32_t ow2 = MIN(MIN(ow, (ibw - ix2)/scalex), obw - ox2);
  assert((int)(ix2 + ow2*scalex) <= ibw);
  assert((int)(iy2 + oh2*scaley) <= ibh);
  assert(ox2 + ow2 <= obw);
  assert(oy2 + oh2 <= obh);
  assert(ix2 >= 0 && iy2 >= 0 && ox2 >= 0 && oy2 >= 0);
  float x = ix2, y = iy2;
  for(int s=0;s<oh2;s++)
  {
    int idx = ox2 + obw*(oy2+s);
    for(int t=0;t<ow2;t++)
    {
      for(int k=0;k<3;k++) o[3*idx + k] =  //i[3*(ibw* (int)y +             (int)x             ) + k)];
             (i[(3*(ibw*(int32_t) y +            (int32_t) (x + .5f*scalex)) + k)] +
              i[(3*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x + .5f*scalex)) + k)] +
              i[(3*(ibw*(int32_t)(y+.5f*scaley) +(int32_t) (x             )) + k)] +
              i[(3*(ibw*(int32_t) y +            (int32_t) (x             )) + k)])*0.25;
      x += scalex; idx++;
    }
    y += scaley; x = ix2;
  }
}

uint32_t dt_iop_create_histogram_final_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist, uint8_t *gamma, uint16_t *tonecurve)
{
  uint32_t max = 0;
  bzero(hist, sizeof(uint32_t)*4*0x100);
  for(int j=0;j<height;j+=4) for(int i=0;i<width;i+=4)
  {
    uint8_t rgb[3];
    for(int k=0;k<3;k++)
      rgb[k] = gamma[tonecurve[(int)CLAMP(0xffff*pixels[3*j*stride+3*i+k], 0, 0xffff)]];

    for(int k=0;k<3;k++)
      hist[4*rgb[k]+k] ++;
    uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);//rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
    hist[4*lum+3] ++;
  }
  // don't count <= 0 pixels
  for(int k=19;k<4*256;k+=4) max = max > hist[k] ? max : hist[k];
  return max;
}

uint32_t dt_iop_create_histogram_f(const float *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist)
{
  uint32_t max = 0;
  bzero(hist, sizeof(uint32_t)*4*0x100);
  for(int j=0;j<height;j+=4) for(int i=0;i<width;i+=4)
  {
    uint8_t rgb[3];
    for(int k=0;k<3;k++)
      rgb[k] = CLAMP(0xff*pixels[3*j*stride+3*i+k], 0, 0xff);

    for(int k=0;k<3;k++)
      hist[4*rgb[k]+k] ++;
    uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);//rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
    hist[4*lum+3] ++;
  }
  // don't count <= 0 pixels
  for(int k=19;k<4*256;k+=4) max = max > hist[k] ? max : hist[k];
  return max;
}

uint32_t dt_iop_create_histogram_8(const uint8_t *pixels, int32_t width, int32_t height, int32_t stride, uint32_t *hist)
{
  uint32_t max = 0;
  bzero(hist, sizeof(uint32_t)*4*0x100);
  for(int j=0;j<height;j++) for(int i=0;i<width;i++)
  {
    uint8_t rgb[3];
    for(int k=0;k<3;k++)
      rgb[k] = pixels[4*j*stride+4*i+k];

    for(int k=0;k<3;k++)
      hist[4*rgb[k]+k] ++;
    uint8_t lum = MAX(MAX(rgb[0], rgb[1]), rgb[2]);//rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2]) : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
    hist[4*lum+3] ++;
  }
  for(int k=19;k<4*256;k+=4) max = max > hist[k] ? max : hist[k];
  return max;
}

