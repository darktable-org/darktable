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
#include "develop/imageop.h"
#include "blend.h"

#define CLAMP_RANGE(x,y,z) (CLAMP(x,y,z))

typedef void (_blend_row_func)(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag);

static inline void _blend_colorspace_channel_range(dt_iop_colorspace_type_t cst,float *min,float *max)
{
  switch(cst)
  {
    case iop_cs_Lab:		// after scaling !!!
      min[0] = 0.0; max[0] = 1.0;
      min[1] = -1.0; max[1] = 1.0;
      min[2] = -1.0; max[2] = 1.0;
      min[3] = -1.0; max[3] = 1.0;
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


static inline void _blend_Lab_scale(const float *i, float *o)
{
  o[0] = i[0]/100.0f;
  o[1] = i[1]/128.0f;
  o[2] = i[2]/128.0f;
}


static inline void _blend_Lab_rescale(const float *i, float *o)
{
  o[0] = i[0]*100.0f;
  o[1] = i[1]*128.0f;
  o[2] = i[2]*128.0f;
}


/* normal blend */
static void _blend_normal(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  for(int j=0;j<stride;j+=4)
  {
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tb[0] =  (ta[0] * (1.0 - opacity)) + tb[0] * opacity;

       if (flag == 0)
       {
         tb[1] =  (ta[1] * (1.0 - opacity)) + tb[1] * opacity;
         tb[2] =  (ta[2] * (1.0 - opacity)) + tb[2] * opacity;
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  (a[j+k] * (1.0 - opacity)) + b[j+k] * opacity;
  }
}

/* lighten */
static void _blend_lighten(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  int channels = _blend_colorspace_channels(cst);
  float ta[3], tb[3], tbo;
  float max[4]={0},min[4]={0};

  _blend_colorspace_channel_range(cst,min,max);

  for(int j=0;j<stride;j+=4)
  {
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tbo = tb[0];
       tb[0] =  CLAMP_RANGE(ta[0] * (1.0 - opacity) + (ta[0]>tb[0]?ta[0]:tb[0]) * opacity, min[0], max[0]);

       if (flag == 0)
       {
         tb[1] = CLAMP_RANGE(ta[1] * (1.0f - fabs(tbo - tb[0])) + 0.5f * (ta[1] + tb[1]) * fabs(tbo - tb[0]), min[1], max[1]);
         tb[2] = CLAMP_RANGE(ta[2] * (1.0f - fabs(tbo - tb[0])) + 0.5f * (ta[2] + tb[2]) * fabs(tbo - tb[0]), min[2], max[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE(a[j+k] * (1.0 - opacity) + fmax(a[j+k],b[j+k]) * opacity, min[k], max[k]);
      
  }
}

/* darken */
static void _blend_darken(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  int channels = _blend_colorspace_channels(cst);
  float ta[3], tb[3], tbo;
  float max[4]={0},min[4]={0};

  _blend_colorspace_channel_range(cst,min,max);

  for(int j=0;j<stride;j+=4)
  {
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tbo = tb[0];
       tb[0] =  CLAMP_RANGE(ta[0] * (1.0 - opacity) + (ta[0]<tb[0]?ta[0]:tb[0]) * opacity, min[0], max[0]);

       if (flag == 0)
       {
         tb[1] = CLAMP_RANGE(ta[1] * (1.0f - fabs(tbo - tb[0])) + 0.5f * (ta[1] + tb[1]) * fabs(tbo - tb[0]), min[1], max[1]);
         tb[2] = CLAMP_RANGE(ta[2] * (1.0f - fabs(tbo - tb[0])) + 0.5f * (ta[2] + tb[2]) * fabs(tbo - tb[0]), min[2], max[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE(a[j+k] * (1.0 - opacity) + fmin(a[j+k],b[j+k]) * opacity, min[k], max[k]);
      
  }
  // return fmin(a,b);
}


/* multiply */
static void _blend_multiply(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb;

  _blend_colorspace_channel_range(cst,min,max);

  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]); 
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity)) + ((la * lb) * opacity)), min[0], max[0]) - fabs(min[0]);

       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]);
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);

        b[j+k] = CLAMP_RANGE( ((a[j+k] * (1.0 - opacity)) + ((a[j+k] * b[j+k]) * opacity)), min[k], max[k]);
      }

  
  // return (a*b);
}


/* average */
static void _blend_average(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};

  _blend_colorspace_channel_range(cst,min,max);

  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tb[0] = CLAMP_RANGE(ta[0] * (1.0 - opacity) + (ta[0] + tb[0])/2.0 * opacity, min[0], max[0]);

       if (flag == 0)
       {
         tb[1] = CLAMP_RANGE(ta[1] * (1.0 - opacity) +  (ta[1] + tb[1])/2.0 * opacity, min[1], max[1]);
         tb[2] = CLAMP_RANGE(ta[2] * (1.0 - opacity) +  (ta[2] + tb[2])/2.0 * opacity, min[2], max[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE(a[j+k] * (1.0 - opacity) + (a[j+k] + b[j+k])/2.0 * opacity, min[k], max[k]);
  
  // return (a+b)/2.0;
}


/* add */
static void _blend_add(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tb[0] = CLAMP_RANGE((ta[0] * (1.0 - opacity)) + ( ((ta[0] + tb[0])) * opacity), min[0], max[0]);

       if (flag == 0)
       {
         tb[1] = CLAMP_RANGE( (ta[1] * (1.0 - opacity)) + ( ((ta[1] + tb[1])) * opacity), min[1], max[1]);
         tb[2] = CLAMP_RANGE( (ta[2] * (1.0 - opacity)) + ( ((ta[2] + tb[2])) * opacity), min[2], max[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE( (a[j+k] * (1.0 - opacity)) + ( ((a[j+k] + b[j+k])) * opacity), min[k], max[k]);
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return CLAMP_RANGE(a+b,min,max);
  */
}


/* substract */
static void _blend_substract(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);

       tb[0] = CLAMP_RANGE( ((ta[0] * (1.0 - opacity)) + ( ((tb[0] + ta[0]) - (fabs(min[0]+max[0]))) * opacity)), min[0], max[0]);

       if (flag == 0)
       {
         tb[1] = CLAMP_RANGE( ((ta[1] * (1.0 - opacity)) + ( ((tb[1] + ta[1]) - (fabs(min[1]+max[1]))) * opacity)), min[1], max[1]);
         tb[2] = CLAMP_RANGE( ((ta[2] * (1.0 - opacity)) + ( ((tb[2] + ta[2]) - (fabs(min[2]+max[2]))) * opacity)), min[2], max[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
      for(int k=0;k<channels;k++)
        b[j+k] =  CLAMP_RANGE( ((a[j+k] * (1.0 - opacity)) + ( ((b[j+k] + a[j+k]) - (fabs(min[k]+max[k]))) * opacity)), min[k], max[k]);
      
  
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return ((a+b<max) ? 0:(b+a-max));
  */
}



/* difference */
static void _blend_difference(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]); 
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax); 
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);

       tb[0] = CLAMP_RANGE( (la * (1.0 - opacity)) + ( fabs(la - lb) * opacity), lmin, lmax)-fabs(min[0]);

       if (flag == 0)
       {
         lmax = max[1]+fabs(min[1]); la = CLAMP_RANGE(ta[1]+fabs(min[1]), lmin, lmax); lb = CLAMP_RANGE(tb[1]+fabs(min[1]), lmin, lmax);
         tb[1] = CLAMP_RANGE( (la * (1.0 - opacity)) + ( fabs(la - lb) * opacity), lmin, lmax)-fabs(min[1]);
         lmax = max[2]+fabs(min[2]); la = CLAMP_RANGE(ta[2]+fabs(min[2]), lmin, lmax); lb = CLAMP_RANGE(tb[2]+fabs(min[2]), lmin, lmax);
         tb[2] = CLAMP_RANGE( (la * (1.0 - opacity)) + ( fabs(la - lb) * opacity), lmin, lmax)-fabs(min[2]);
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]); la = a[j+k]+fabs(min[k]); lb = b[j+k]+fabs(min[k]);

        b[j+k] =  CLAMP_RANGE( (la * (1.0 - opacity)) + ( fabs(la - lb) * opacity), lmin, lmax)-fabs(min[k]);
      }
    }
  // return fabs(a-b);
}


/* screen */
static void _blend_screen(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]); 
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);

       tb[0] = CLAMP_RANGE( (la * (1.0 - opacity)) + (( (lmax - (lmax-la) * (lmax-lb)) ) * opacity), lmin, lmax)-fabs(min[0]);

       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity) + 0.5f * (ta[1] + tb[1]) * tb[0]/ta[0] * opacity, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity) + 0.5f * (ta[2] + tb[2]) * tb[0]/ta[0] * opacity, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity) + 0.5f * (ta[1] + tb[1]) * tb[0]/0.01f * opacity, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity) + 0.5f * (ta[2] + tb[2]) * tb[0]/0.01f * opacity, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]);
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);

        b[j+k] =  CLAMP_RANGE( (la * (1.0 - opacity)) + (( (lmax - (lmax-la) * (lmax-lb)) ) * opacity), lmin, lmax)-fabs(min[k]);
      }
    }
    
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  return max - (max-a) * (max-b);
  */
}

/* overlay */
static void _blend_overlay(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb, halfmax, doublemax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);
       halfmax = lmax/2.0;
       doublemax = lmax*2.0;

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (la>halfmax) ? ( lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) ) : ( ( doublemax*la) * lb )
          ) * opacity2), lmin, lmax)-fabs(min[0]);


       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity2, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity2, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]);
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);
        halfmax = lmax/2.0;
        doublemax = lmax*2.0;

        b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (la>halfmax) ? ( lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) ) : ( ( doublemax*la) * lb )
          ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
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
static void _blend_softlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb, halfmax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);
       halfmax = lmax/2.0;

       tb[0] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax)? ( lmax - (lmax-la)  * (lmax - (lb-halfmax))) : ( la * (lb+halfmax) )
        ) * opacity2), lmin, lmax)-fabs(min[0]);

       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity2, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity2, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]);
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);
        halfmax = lmax/2.0;

        b[j+k] =   CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax)? ( lmax - (lmax-la)  * (lmax - (lb-halfmax))) : ( la * (lb+halfmax) )
        ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
    }
 
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  return (b>halfmax) ? max - (max-a) * (max - (b-halfmax)) : a * (b+halfmax);
  */
}

/* hardlight */
static void _blend_hardlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb, halfmax, doublemax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);
       halfmax = lmax/2.0;
       doublemax = lmax*2.0;

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) ) : ( ( doublemax*la) * lb )
          ) * opacity2), lmin, lmax)-fabs(min[0]);


       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity2, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity2, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]);
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);
        halfmax = lmax/2.0;
        doublemax = lmax*2.0;

        b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( lmax - (lmax - doublemax*(la-halfmax)) * (lmax-lb) ) : ( ( doublemax*la) * lb )
          ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
    }
    
/*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (b>halfmax) ? max - (max - doublemax*(a-halfmax)) * (max-b) : (doublemax*a) * b;
  */
}


/* vividlight */
static void _blend_vividlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0}, min[4]={0};
  float lmin = 0.0, lmax, la, lb, halfmax, doublemax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax);
       halfmax = lmax/2.0;
       doublemax = lmax*2.0;

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( la / (doublemax*(lmax - lb))) : ( lmax - (lmax - la)/(doublemax * lb) )
        ) * opacity2), lmin, lmax)-fabs(min[0]);

       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity2, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity2, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]); 
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax);
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax);
        halfmax = lmax/2.0;
        doublemax = lmax*2.0;

        b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( la / (doublemax*(lmax - lb))) : ( lmax - (lmax - la)/(doublemax * lb) )
        ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
    }
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (b>halfmax) ? a / (doublemax*(max-b)) : max - (max-a) / (doublemax*b);
  */
}

/* linearlight */
static void _blend_linearlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb, doublemax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax); 
       doublemax = lmax*2.0;

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity2)) + ( la + doublemax*lb-lmax ) * opacity2), lmin, lmax)-fabs(min[0]);

       if (flag == 0)
       {
         if (ta[0] > 0.01f)
         {
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/ta[0] * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/ta[0] * opacity2, min[2], max[2]);
         }
         else
         { 
           tb[1] = CLAMP_RANGE(ta[1] * (1.0f - opacity2) + (ta[1] + tb[1]) * tb[0]/0.01f * opacity2, min[1], max[1]);
           tb[2] = CLAMP_RANGE(ta[2] * (1.0f - opacity2) + (ta[2] + tb[2]) * tb[0]/0.01f * opacity2, min[2], max[2]);
         }
       }
       else
       {
         tb[1] = ta[1];
         tb[2] = ta[2];
       }

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]); 
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax); 
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax); 
        doublemax = lmax*2.0;

        b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + ( la + doublemax*lb-lmax ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
    }
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return a +doublemax*b-max;
  */
}

/* pinlight */
static void _blend_pinlight(dt_iop_colorspace_type_t cst,const float opacity,const float *a, float *b,int stride, int flag)
{
  float ta[3], tb[3];
  int channels = _blend_colorspace_channels(cst);
  float max[4]={0},min[4]={0};
  float lmin = 0.0, lmax, la, lb, halfmax, doublemax;
  float opacity2 = opacity*opacity;

  _blend_colorspace_channel_range(cst,min,max);
  
  for(int j=0;j<stride;j+=4)
    if(cst==iop_cs_Lab)
    {
       _blend_Lab_scale(&a[j], ta); _blend_Lab_scale(&b[j], tb);
       lmax = max[0]+fabs(min[0]);
       la = CLAMP_RANGE(ta[0]+fabs(min[0]), lmin, lmax);
       lb = CLAMP_RANGE(tb[0]+fabs(min[0]), lmin, lmax); 
       halfmax = lmax/2.0; 
       doublemax = lmax*2.0;

       tb[0] = CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( fmax(la,doublemax*(lb-halfmax)) ) : ( fmin(la,doublemax*lb) )
        ) * opacity2), lmin, lmax)-fabs(min[0]);

       tb[1] = ta[1];
       tb[2] = ta[2];

       _blend_Lab_rescale(tb, &b[j]);
    }
    else
    {
      for(int k=0;k<channels;k++)
      {
        lmax = max[k]+fabs(min[k]); 
        la = CLAMP_RANGE(a[j+k]+fabs(min[k]), lmin, lmax); 
        lb = CLAMP_RANGE(b[j+k]+fabs(min[k]), lmin, lmax); 
        halfmax = lmax/2.0; 
        doublemax = lmax*2.0;

        b[j+k] =  CLAMP_RANGE( ((la * (1.0 - opacity2)) + (
          (lb>halfmax) ? ( fmax(la,doublemax*(lb-halfmax)) ) : ( fmin(la,doublemax*lb) )
        ) * opacity2), lmin, lmax)-fabs(min[k]);
      }
    }
  /*
  float max,min;
  _blend_colorspace_channel_range(cst,channel,&min,&max);
  const float halfmax=max/2.0;
  const float doublemax=max*2.0;
  return (b>halfmax) ? fmax(a,doublemax*(b-halfmax)) : fmin(a,doublemax*b);
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

    /* check if we only should blend lightness channel. will affect only Lab space */
    const int blendflag = self->flags() & IOP_FLAGS_BLEND_ONLY_LIGHTNESS;    

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
        blend(cst, opacity, in+index, out+index, roi_out->width*ch, blendflag);
    }
  }
  else
  {
    /* blending with mask */
    dt_control_log("blending using masks is not yet implemented.");

  }
}


#ifdef HAVE_OPENCL
int 
dt_develop_blend_process_cl (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
  dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;
  cl_int err = -999;
  cl_mem dev_m = NULL;

  // fprintf(stderr, "dt_develop_blend_process_cl: mode %d\n", d->mode);

  /* check if blend is disabled: just return, output is already in dev_out */
  if (!d || d->mode==0) return TRUE;

  const dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);
  int kernel = darktable.blendop->kernel_blendop_Lab;

  switch (cst)
  {
    case iop_cs_RAW:
      kernel = darktable.blendop->kernel_blendop_RAW;
      break;

    case iop_cs_rgb:
      kernel = darktable.blendop->kernel_blendop_rgb;
      break;

    case iop_cs_Lab:
    default:
      kernel = darktable.blendop->kernel_blendop_Lab;
      break;
  }

  const int devid = piece->pipe->devid;
  const float opacity = fmin(fmax(0,(d->opacity/100.0)),1.0);
  const int blendflag = self->flags() & IOP_FLAGS_BLEND_ONLY_LIGHTNESS;

  size_t sizes[] = {roi_in->width, roi_in->height, 1};
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&(d->mode));
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&opacity);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), (void *)&blendflag);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  return TRUE;

error:
  if (dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  dt_print(DT_DEBUG_OPENCL, "[opencl_blendop] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/** global init of blendops */
void dt_develop_blend_init(dt_blendop_t *gd)
{
#ifdef HAVE_OPENCL
  const int program = 3; // blendop.cl, from programs.conf
  gd->kernel_blendop_Lab = dt_opencl_create_kernel(program, "blendop_Lab");
  gd->kernel_blendop_RAW = dt_opencl_create_kernel(program, "blendop_RAW");
  gd->kernel_blendop_rgb = dt_opencl_create_kernel(program, "blendop_rgb");
#else
  gd->kernel_blendop_Lab = gd->kernel_blendop_RAW = gd->kernel_blendop_rgb = -1;
#endif
}

