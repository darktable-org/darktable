/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "control/control.h"
#include "blend.h"

#define CLAMP_RANGE(x,y,z) (CLAMP(x,y,z))

typedef void (_blend_row_func)(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride);

static inline void _blend_colorspace_channel_range(dt_iop_colorspace_type_t cst,float *min,float *max)
{
  switch(cst)
  {
    case iop_cs_Lab:
      min[0] = 0.0; max[0] = 100.0;
      min[1] = -128.0; max[1] = 128.0;
      min[2] = -128.0; max[2] = 128.0;
      min[3] = -128.0; max[3] = 128.0;
    break;
    default:
      min[0] = 0; max[0] = 1.0;
      min[1] = 0; max[1] = 1.0;
      min[2] = 0; max[2] = 1.0;
      min[3] = 0; max[3] = 1.0;
    break;
  }
}

static inline int _blend_colorspace_channels(dt_iop_colorspace_type_t cst)
{
  switch(cst)
  {
    case iop_cs_RAW:
      return 4;
    
    case iop_cs_Lab:
    default:
      return 3;
  }
}

/* normal blend */
static void _blend_normal(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
      b[j+k] =  (a[j+k] * (1.0 - opacity)) + ((b[j+k]) * opacity);
}

/* lighten */
static void _blend_lighten(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  for(int j=0;j<stride;j+=4)
  {
    if(cst==iop_cs_Lab)
    {
        /* blend all channels but base blend on L channel */
        b[j+0] =  (a[j+0] * (1.0 - opacity)) + ((a[j+0]>b[j+0]?a[j+0]:b[j+0]) * opacity);
        b[j+1] =  (a[j+1] * (1.0 - opacity)) + ((a[j+0]>b[j+0]?a[j+1]:b[j+1]) * opacity);
        b[j+2] =  (a[j+2] * (1.0 - opacity)) + ((a[j+0]>b[j+0]?a[j+2]:b[j+2]) * opacity);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  (a[j+k] * (1.0 - opacity)) + ((fmax(a[j+k],b[j+k])) * opacity);
      
  }
}

/* darken */
static void _blend_darken(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  for(int j=0;j<stride;j+=4)
  {
    if(cst==iop_cs_Lab)
    {
        /* blend all channels but base blend on L channel */
        b[j+0] =  (a[j+0] * (1.0 - opacity)) + ((a[j+0]<b[j+0]?a[j+0]:b[j+0]) * opacity);
        b[j+1] =  (a[j+1] * (1.0 - opacity)) + ((a[j+0]<b[j+0]?a[j+1]:b[j+1]) * opacity);
        b[j+2] =  (a[j+2] * (1.0 - opacity)) + ((a[j+0]<b[j+0]?a[j+2]:b[j+2]) * opacity);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  (a[j+k] * (1.0 - opacity)) + ((fmin(a[j+k],b[j+k])) * opacity);
      
  }
  // return fmin(a,b);
}

/* multiply */
static void _blend_multiply(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);

  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);

  for(int j=0;j<stride;j+=4)
      for(int k=0;k<channels;k++)
        b[j+k] = CLAMP_RANGE( ((a[j+k] * (1.0 - opacity)) + ((a[j+k] * b[j+k]) * opacity)), min[k], max[k]);
  
  // return (a*b);
}

/* average */
static void _blend_average(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  for(int j=0;j<stride;j+=4)
      for(int k=0;k<channels;k++)
        b[j+k] =  (a[j+k] * (1.0 - opacity)) + ( ((a[j+k] + b[j+k])/2.0) * opacity);
  
  // return (a+b)/2.0;
}
/* add */
static void _blend_add(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE( (a[j+k] * (1.0 - opacity)) + ( ((a[j+k] + b[j+k])) * opacity), min[k], max[k]);
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return CLAMP_RANGE(a+b,min,max);
  */
}
/* substract */
static void _blend_substract(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
      b[j+k] =  CLAMP_RANGE( ((a[j+k] * (1.0 - opacity)) + ( ((b[j+k] + a[j+k]) - (fabs(min[k]+max[k]))) * opacity)), min[k], max[k]);
      
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return ((a+b<max) ? 0:(b+a-max));
  */
}
/* difference */
static void _blend_difference(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      
      b[j+k] =  CLAMP_RANGE( (la * (1.0 - opacity)) + ( fabs(la - lb) * opacity), lmin, lmax)-fabs(min[k]);
    }
  // return fabs(a-b);
}

/* screen */
static void _blend_screen(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      
      b[j+k] =  CLAMP_RANGE( (la * (1.0 - opacity)) + (( (lmax - (lmax-la) * (lmax-lb)) ) * opacity), lmin, lmax)-fabs(min[k]);
    }
    
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return max - (max-a) * (max-b);
  */
}

/* overlay */
static void _blend_overlay(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      const float doublemax=lmax*2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax) ? ( lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) ) : ( ( doublemax*la) * lb )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
  
    
/*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (a>halfmax) ? max - (max - doublemax*(a-halfmax)) * (max-b) : (doublemax*a) * b;
  */
}

/* softlight */
static void _blend_softlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax)? ( lmax - (lmax-la)  * (lmax - (lb-halfmax))) : ( la * (lb+halfmax) )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  return (a>halfmax) ? max - (max-a) * (max - (b-halfmax)) : a * (b+halfmax);
  */
}

/* hardlight */
static void _blend_hardlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      const float doublemax=lmax*2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax) ? ( lmax - (lmax-la)  * (lmax - doublemax*(lb-halfmax))) : ( la * (lb+halfmax) )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (a>halfmax) ? max - (max-a) * (max - doublemax*(b-halfmax)) : a * (b+halfmax);
  */
}

/* vividlight */
static void _blend_vividlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      const float doublemax=lmax*2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax) ? ( lmax - (lmax-la) / (doublemax*(lb-halfmax))) : ( la / (lmax - doublemax * lb) )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (a>halfmax) ? max - (max-a) / (doublemax*(b-halfmax)) : a / (max-doublemax*b);
  */
}

/* linearlight */
static void _blend_linearlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);

  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      const float doublemax=lmax*2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax) ? ( la + doublemax*(lb-halfmax) ) : ( la + doublemax*lb-lmax )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
  
    
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (a>halfmax) ? a + doublemax*(b-halfmax) : a +doublemax*b-max;
  */
}

/* pinlight */
static void _blend_pinlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride)
{
  int channels = _blend_colorspace_channels(cst);
  
  /* force using only L channel in Lab space for this blend op */
  if(cst == iop_cs_Lab)
    channels = 1;
  
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    for(int k=0;k<channels;k++)
    {
      const float lmin =  0.0;
      const float lmax = max[k]+fabs(min[k]);
      const float la = a[j+k]+fabs(min[k]), lb = b[j+k]+fabs(min[k]);
      const float halfmax=lmax/2.0;
      const float doublemax=lmax*2.0;
      
      b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity)) + (
          (la>halfmax) ? ( fmax(la,doublemax*(lb-halfmax)) ) : ( fmin(la,doublemax*lb) )
        ) * opacity), lmin, lmax)-fabs(min[k]);
    }
    
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (a>halfmax) ? fmax(a,doublemax*(b-halfmax)) : fmin(a,doublemax*b);
  */
}

void dt_develop_blend_process (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
  float *in =(float *)i;
  float *out =(float *)o;
  int ch = piece->colors;
  _blend_row_func *blend = NULL;
  dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;
  
  /* check if blend is disabled */
  if (!d || d->mode==0) return;

  /* select the blend operator */
  switch (d->mode)
  {
    case DEVELOP_BLEND_LIGHTEN:
      blend = _blend_lighten;
      break;

    case DEVELOP_BLEND_DARKEN:
      blend = _blend_darken;
      break;

    case DEVELOP_BLEND_MULTIPLY:
      blend = _blend_multiply;
      break;

    case DEVELOP_BLEND_AVERAGE:
      blend = _blend_average;
      break;
    case DEVELOP_BLEND_ADD:
      blend = _blend_add;
      break;
    case DEVELOP_BLEND_SUBSTRACT:
      blend = _blend_substract;
      break;
    case DEVELOP_BLEND_DIFFERENCE:
      blend = _blend_difference;
      break;
    case DEVELOP_BLEND_SCREEN:
      blend = _blend_screen;
      break;
    case DEVELOP_BLEND_OVERLAY:
      blend = _blend_overlay;
      break;
    case DEVELOP_BLEND_SOFTLIGHT:
      blend = _blend_softlight;
      break;
    case DEVELOP_BLEND_HARDLIGHT:
      blend = _blend_hardlight;
      break;
    case DEVELOP_BLEND_VIVIDLIGHT:
      blend = _blend_vividlight;
      break;
    case DEVELOP_BLEND_LINEARLIGHT:
      blend = _blend_linearlight;
      break;
    case DEVELOP_BLEND_PINLIGHT:
      blend = _blend_pinlight;
      break;

      /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL:
    default:
      blend = _blend_normal;
      break;
  }

  if (!(d->mode & DEVELOP_BLEND_MASK_FLAG))
  {
    /* get the clipped opacity value  0 - 1 */
    const float opacity = fmin(fmax(0,(d->opacity/100.0)),1.0);


    /* get channel max values depending on colorspace */
    const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);
    
    /* correct bpp per pixel for raw 
        \TODO actually invest why channels per pixel is 4 in raw..  
    */
    if(cst==iop_cs_RAW)
      ch = 1;
    
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(in,roi_out,out,blend,d,stderr,ch)
#endif
    for (int y=0; y<roi_out->height; y++) {
        int index = (ch*y*roi_out->width);
        blend(cst, opacity, in+index, out+index, roi_out->width*ch);
    }
  }
  else
  {
    /* blending with mask */
    dt_control_log("blending using masks is not yet implemented.");

  }
}