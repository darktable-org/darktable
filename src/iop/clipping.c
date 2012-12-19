/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2012 henrik andersson.

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

/*NOTE
 * 
 * old keystone values (version 2 and 3):
 *  to avoid too many slider, these values are not settable anymore
 *  but if some have been set, a entry "old keystone" is added to the keystone type combobox
 *  so you can always revert back to those values
 *  unfortunately, there is no possibility to "translate" old values in the new mechanism (it would mean drawing points outside the image)
 * 
 * 
 * Corrected bug from old version
 *  croping is not correct if the image is flipped
 */
 
 /*TODO
  * 
  * Verifications needed :
  *   - in "gui_draw_sym"
  * 
  */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/debug.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/guides.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>
#include <assert.h>

DT_MODULE(4)

// number of gui ratios in combo box
#define NUM_RATIOS 13

/** flip H/V, rotate an image, then clip the buffer. */
typedef enum dt_iop_clipping_flags_t
{
  FLAG_FLIP_HORIZONTAL = 1,
  FLAG_FLIP_VERTICAL = 2
}
dt_iop_clipping_flags_t;

typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, k_h, k_v;
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
  int k_type, k_sym;
  int k_apply, crop_auto;
}
dt_iop_clipping_params_t;

/* calculate the aspect ratios for current image */
static void _iop_clipping_update_ratios(dt_iop_module_t *self);
static void keystone_type_populate(struct dt_iop_module_t *self,gboolean with_applied,int select);

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if (new_version == old_version) return 1;
  if(new_version == 4)
  {
    int old = old_version;
    typedef struct old_params_t
    {
      float angle, cx, cy, cw, ch, k_h, k_v;
    }
    old_params_t;
    old_params_t *tmp = malloc(sizeof(old_params_t));
    memcpy(tmp,old_params,sizeof(old_params_t));
    //version 2 => version 3
    if (old == 2)
    {
      uint32_t intk = *(uint32_t *)&tmp->k_h;
      int is_horizontal;
      if(intk & 0x40000000u) is_horizontal = 1;
      else                   is_horizontal = 0;
      intk &= ~0x40000000;
      float floatk = *(float *)&intk;
      if(is_horizontal)
      {
        tmp->k_h = floatk;
        tmp->k_v = 0.0;
      }
      else
      {
        tmp->k_h = 0.0;
        tmp->k_v = floatk;
      }
      old = 3;
    }
    //version 3 => version 4
    if (old == 3)
    {
      dt_iop_clipping_params_t *n = (dt_iop_clipping_params_t *)new_params;
      //we copy all old values
      n->angle=tmp->angle, n->cx=tmp->cx, n->cy=tmp->cy, n->cw=tmp->cw, n->ch=tmp->ch;
      n->k_h=tmp->k_h, n->k_v=tmp->k_v;
      //we set all value to default
      n->kxa = n->kxd = 0.2f;
      n->kxc = n->kxb = 0.8f;
      n->kya = n->kyb = 0.2f;
      n->kyc = n->kyd = 0.8f;
      if (n->k_h ==0 && n->k_v==0) n->k_type = 0;
      else n->k_type = 4;
      n->k_sym = 0;
      n->k_apply = 0;
      n->crop_auto = 1;
    }
    free(tmp);
    return 0;
  }
  return 1;
}
typedef struct dt_iop_clipping_gui_data_t
{
  GtkWidget *angle;
  GtkWidget *hvflip;
  GtkWidget *aspect_presets;
  GtkWidget *guide_lines;
  GtkWidget *flip_guides;
  GtkWidget *golden_extras;
  GtkWidget *keystone_type;
  GtkWidget *crop_auto;

  float button_down_x, button_down_y;
  float button_down_zoom_x, button_down_zoom_y, button_down_angle; // position in image where the button has been pressed.
  /* current clip box */
  float clip_x, clip_y, clip_w, clip_h, handle_x, handle_y;
  /* last committed clip box */
  float old_clip_x, old_clip_y, old_clip_w, old_clip_h;
  /* last box before change */
  float prev_clip_x, prev_clip_y, prev_clip_w, prev_clip_h;

  int k_selected, k_show, k_selected_segment;
  gboolean k_drag;
  
  int cropping, straightening, applied, center_lock;
  float aspect_ratios[NUM_RATIOS];
  float current_aspect;
}
dt_iop_clipping_gui_data_t;

typedef struct dt_iop_clipping_data_t
{
  float angle;              // rotation angle
  float aspect;             // forced aspect ratio
  float m[4];               // rot matrix
  float ki_h, k_h;          // keystone correction, ki and corrected k
  float ki_v, k_v;          // keystone correction, ki and corrected k
  float tx, ty;             // rotation center
  float cx, cy, cw, ch;     // crop window
  float cix, ciy, ciw, cih; // crop window on roi_out 1.0 scale
  uint32_t all_off;         // 1: v and h off, else one of them is used
  uint32_t flags;           // flipping flags
  uint32_t flip;            // flipped output buffer so more area would fit.
  
  float k_space[4];         //space for the "destination" rectangle of the keystone quadrilatere
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd; //point of the "source" quadrilatere (modified if keystone is not "full")
  float a,b,d,e,g,h; //value of the transformation matrix (c=f=0 && i=1)
  int k_apply;
  int crop_auto;
  float enlarge_x, enlarge_y;
}
dt_iop_clipping_data_t;

typedef struct dt_iop_clipping_global_data_t
{
  int kernel_clip_rotate_bilinear;
  int kernel_clip_rotate_bicubic;
  int kernel_clip_rotate_lanczos2;
  int kernel_clip_rotate_lanczos3;
}
dt_iop_clipping_global_data_t;

static void commit_box(dt_iop_module_t *self, dt_iop_clipping_gui_data_t *g,
                       dt_iop_clipping_params_t *p);

static void mul_mat_vec_2(const float *m, const float *p, float *o)
{
  o[0] = p[0]*m[0] + p[1]*m[1];
  o[1] = p[0]*m[2] + p[1]*m[3];
}

// helper to count corners in for loops:
static void get_corner(const float *aabb, const int i, float *p)
{
  for(int k=0; k<2; k++) p[k] = aabb[2*((i>>k)&1) + k];
}

static void adjust_aabb(const float *p, float *aabb)
{
  aabb[0] = fminf(aabb[0], p[0]);
  aabb[1] = fminf(aabb[1], p[1]);
  aabb[2] = fmaxf(aabb[2], p[0]);
  aabb[3] = fmaxf(aabb[3], p[1]);
}

const char *name()
{
  return _("crop and rotate");
}

int
groups ()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE;
}

int
operation_tags ()
{
  return IOP_TAG_DISTORT;
}

int
operation_tags_filter ()
{
  // switch off watermark, it gets confused.
  return IOP_TAG_DECORATION;
}


static int
gui_has_focus(struct dt_iop_module_t *self)
{
  return self->dev->gui_module == self;
}

static void
keystone_get_matrix(float *k_space, float kxa, float kxb, float kxc, float kxd, float kya, float kyb, float kyc, float kyd,
                    float *a, float *b, float *d, float *e, float *g, float *h)
{
  *a=-((kxb*(kyd*kyd-kyc*kyd)-kxc*kyd*kyd+kyb*(kxc*kyd-kxd*kyd)+kxd*kyc*kyd)*k_space[2])/(kxb*(kxc*kyd*kyd-kxd*kyc*kyd)+kyb*(kxd*kxd*kyc-kxc*kxd*kyd));
  *b=((kxb*(kxd*kyd-kxd*kyc)-kxc*kxd*kyd+kxd*kxd*kyc+(kxc*kxd-kxd*kxd)*kyb)*k_space[2])/(kxb*(kxc*kyd*kyd-kxd*kyc*kyd)+kyb*(kxd*kxd*kyc-kxc*kxd*kyd));
  *d=(kyb*(kxb*(kyd*k_space[3]-kyc*k_space[3])-kxc*kyd*k_space[3]+kxd*kyc*k_space[3])+kyb*kyb*(kxc*k_space[3]-kxd*k_space[3]))/(kxb*kyb*(-kxc*kyd-kxd*kyc)+kxb*kxb*kyc*kyd+kxc*kxd*kyb*kyb);
  *e=-(kxb*(kxd*kyc*k_space[3]-kxc*kyd*k_space[3])+kxb*kxb*(kyd*k_space[3]-kyc*k_space[3])+kxb*kyb*(kxc*k_space[3]-kxd*k_space[3]))/(kxb*kyb*(-kxc*kyd-kxd*kyc)+kxb*kxb*kyc*kyd+kxc*kxd*kyb*kyb);
  *g=-(kyb*(kxb*(2.0f*kxc*kyd*kyd-2.0f*kxc*kyc*kyd)-kxc*kxc*kyd*kyd+2.0f*kxc*kxd*kyc*kyd-kxd*kxd*kyc*kyc)+kxb*kxb*(kyc*kyc*kyd-kyc*kyd*kyd)+kyb*kyb*(-2.0f*kxc*kxd*kyd+kxc*kxc*kyd+kxd*kxd*kyc))/(kxb*kxb*(kxd*kyc*kyc*kyd-kxc*kyc*kyd*kyd)+kxb*kyb*(kxc*kxc*kyd*kyd-kxd*kxd*kyc*kyc)+kyb*kyb*(kxc*kxd*kxd*kyc-kxc*kxc*kxd*kyd));
  *h=(kxb*(-kxc*kxc*kyd*kyd+2.0f*kxc*kxd*kyc*kyd-kxd*kxd*kyc*kyc)+kxb*kxb*(kxc*kyd*kyd-2.0f*kxd*kyc*kyd+kxd*kyc*kyc)+kxb*(2.0f*kxd*kxd-2.0f*kxc*kxd)*kyb*kyc+(kxc*kxc*kxd-kxc*kxd*kxd)*kyb*kyb)/(kxb*kxb*(kxd*kyc*kyc*kyd-kxc*kyc*kyd*kyd)+kxb*kyb*(kxc*kxc*kyd*kyd-kxd*kxd*kyc*kyc)+kyb*kyb*(kxc*kxd*kxd*kyc-kxc*kxc*kxd*kyd));
}

static void
keystone_backtransform(float *i, float *k_space, float a, float b, float d, float e, float g, float h, float kxa, float kya)
{
  float xx = i[0] - k_space[0];
  float yy = i[1] - k_space[1];
  
  float div = ((d*xx-a*yy)*h+(b*yy-e*xx)*g+a*e-b*d);
  
  i[0]= (e*xx-b*yy)/div + kxa;
  i[1]=-(d*xx-a*yy)/div + kya;
}

static int
keystone_transform(float *i, float *k_space, float a, float b, float d, float e, float g, float h, float kxa, float kya)
{
  float xx = i[0] - kxa;
  float yy = i[1] - kya;
  
  float div = g*xx+h*yy+1;
  i[0] = (a*xx+b*yy)/div + k_space[0];
  i[1] = (d*xx+e*yy)/div + k_space[1];
  return 1;
}

static void
backtransform(float *x, float *o, const float *m, const float t_h, const float t_v)
{
  x[1] /= (1.0f + x[0]*t_h);
  x[0] /= (1.0f + x[1]*t_v);
  mul_mat_vec_2(m, x, o);
}

static void
transform(float *x, float *o, const float *m, const float t_h, const float t_v)
{
  float rt[] = { m[0], -m[1], -m[2], m[3]};
  mul_mat_vec_2(rt, x, o);
  o[1] *= (1.0f + o[0]*t_h);
  o[0] *= (1.0f + o[1]*t_v);
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in_orig)
{  
  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  dt_iop_roi_t roi_in_d = *roi_in_orig;
  dt_iop_roi_t* roi_in = &roi_in_d;

  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle), sinf(d->angle), -sinf(d->angle), cosf(d->angle) };
  if(d->angle == 0.0f)
  {
    rt[0] = rt[3] = 1.0;
    rt[1] = rt[2] = 0.0f;
  }

  for(int k=0; k<4; k++) d->m[k] = rt[k];
  if(d->flags & FLAG_FLIP_HORIZONTAL)
  {
    d->m[0] = - rt[0];
    d->m[2] = - rt[2];
  }
  if(d->flags & FLAG_FLIP_VERTICAL)
  {
    d->m[1] = - rt[1];
    d->m[3] = - rt[3];
  }
  
  if (d->k_apply==0 && d->crop_auto==1)  //this is the old solution.
  {
    /* Account for interpolation constraints right now, so when doing the
     * backtransform in modify_roi_in all nicely fits */
    roi_in->x += interpolation->width;
    roi_in->y += interpolation->width;
    roi_in->width -= 2*interpolation->width;
    roi_in->height -= 2*interpolation->width;
    *roi_out = *roi_in;
    
    // correct keystone correction factors by resolution of this buffer
    const float kc = 1.0f/fminf(roi_in->width, roi_in->height);
    d->k_h = d->ki_h * kc;
    d->k_v = d->ki_v * kc;
    
    float cropscale = -1.0f;
    // check portrait/landscape orientation, whichever fits more area:
    const float oaabb[4] = {-.5f*roi_in->width, -.5f*roi_in->height, .5f*roi_in->width, .5f*roi_in->height};
    for(int flip=0; flip<2; flip++)
    {
      const float roi_in_width  = flip ? roi_in->height : roi_in->width;
      const float roi_in_height = flip ? roi_in->width  : roi_in->height;
      float newcropscale = 1.0f;
      // fwd transform rotated points on corners and scale back inside roi_in bounds.
      float p[2], o[2], aabb[4] = {-.5f*roi_in_width, -.5f*roi_in_height, .5f*roi_in_width, .5f*roi_in_height};
      for(int c=0; c<4; c++)
      {
        get_corner(oaabb, c, p);
        transform(p, o, rt, d->k_h, d->k_v);
        for(int k=0; k<2; k++) if(fabsf(o[k]) > 0.001f) newcropscale = fminf(newcropscale, aabb[(o[k] > 0 ? 2 : 0) + k]/o[k]);
      }
      if(newcropscale >= cropscale)
      {
        cropscale = newcropscale;
        // remember rotation center in whole-buffer coordinates:
        d->tx = roi_in->width  * .5f;
        d->ty = roi_in->height * .5f;
        d->flip = flip;
  
        float ach = d->ch-d->cy, acw = d->cw-d->cx;
        // rotate and clip to max extent
        if(flip)
        {
          roi_out->y      = d->tx - (.5f - d->cy)*cropscale*roi_in->width;
          roi_out->x      = d->ty - (.5f - d->cx)*cropscale*roi_in->height;
          roi_out->height = ach*cropscale*roi_in->width;
          roi_out->width  = acw*cropscale*roi_in->height;
        }
        else
        {
          roi_out->x      = d->tx - (.5f - d->cx)*cropscale*roi_in->width;
          roi_out->y      = d->ty - (.5f - d->cy)*cropscale*roi_in->height;
          roi_out->width  = acw*cropscale*roi_in->width;
          roi_out->height = ach*cropscale*roi_in->height;
        }
      }
    }
  }
  else
  {
    *roi_out = *roi_in;
    //set roi_out values with rotation and keystone
    //initial corners pos
    float corn_x[4] = {0.0f,roi_in->width,roi_in->width,0.0f};
    float corn_y[4] = {0.0f,0.0f,roi_in->height,roi_in->height};
    //destination corner points
    float corn_out_x[4] = {0.0f};
    float corn_out_y[4] = {0.0f};
    
    //we don't test image flip as autocrop is not completly ok...
    d->flip = 0;
    
    //we apply rotation and keystone to all those points
    float p[2],o[2];
    for (int c=0; c<4; c++)
    {
      //keystone
      o[0] = corn_x[c];
      o[1] = corn_y[c];
      if (d->k_apply==1)
      {
        o[0] /= (float)roi_in->width, o[1] /= (float)roi_in->height;
        if (keystone_transform(o,d->k_space,d->a,d->b,d->d,d->e,d->g,d->h,d->kxa,d->kya) != 1)
        {
          //we set the point to maximum possible
          if (o[0]<0.5f) o[0]=-1.0f;
          else o[0]=2.0f;
          if (o[1]<0.5f) o[1]=-1.0f;
          else o[1]=2.0f;
        }
        o[0] *= roi_in->width, o[1] *= roi_in->height;
      }
      //rotation
      p[0] = o[0] - .5f*roi_in->width;
      p[1] = o[1] - .5f*roi_in->height;    
      transform(p, o, d->m, d->k_h, d->k_v);
      o[0] += .5f*roi_in->width;
      o[1] += .5f*roi_in->height;
      
      //and we set the values
      corn_out_x[c] = o[0];
      corn_out_y[c] = o[1];
    }
    
    float new_x,new_y,new_sc_x,new_sc_y;
    new_x = fminf(fminf(fminf(corn_out_x[0],corn_out_x[1]),corn_out_x[2]),corn_out_x[3]);
    if (new_x + roi_in->width < 0) new_x = -roi_in->width;
    new_y = fminf(fminf(fminf(corn_out_y[0],corn_out_y[1]),corn_out_y[2]),corn_out_y[3]);
    if (new_y + roi_in->height < 0) new_y = -roi_in->height;
  
    new_sc_x = fmaxf(fmaxf(fmaxf(corn_out_x[0],corn_out_x[1]),corn_out_x[2]),corn_out_x[3]);
    if (new_sc_x > 2.0f*roi_in->width) new_sc_x = 2.0f*roi_in->width;
    new_sc_y = fmaxf(fmaxf(fmaxf(corn_out_y[0],corn_out_y[1]),corn_out_y[2]),corn_out_y[3]);
    if (new_sc_y > 2.0f*roi_in->height) new_sc_y = 2.0f*roi_in->height;
    
    //be carefull, we don't want too small area here !
    if (new_sc_x - new_x < roi_in->width/8.0f)
    {
      float f = (new_sc_x+new_x)/2.0f;
      if (f < roi_in->width/16.0f) f = roi_in->width/16.0f;
      if (f >= roi_in->width*15.0f/16.0f) f = roi_in->width*15.0f/16.0f -1.0f;
      new_x = f-roi_in->width/16.0f, new_sc_x = f+roi_in->width/16.0f; 
    }
    if (new_sc_y - new_y < roi_in->height/8.0f)
    {
      float f = (new_sc_y+new_y)/2.0f;
      if (f < roi_in->height/16.0f) f = roi_in->height/16.0f;
      if (f >= roi_in->height*15.0f/16.0f) f = roi_in->height*15.0f/16.0f -1.0f;
      new_y = f-roi_in->height/16.0f, new_sc_y = f+roi_in->height/16.0f; 
    }
  
    new_sc_y = new_sc_y - new_y;
    new_sc_x = new_sc_x - new_x;
    
    //now we apply the clipping
    new_x += d->cx*new_sc_x;
    new_y += d->cy*new_sc_y;
    new_sc_x *= d->cw-d->cx;
    new_sc_y *= d->ch-d->cy;
    
    d->enlarge_x = fmaxf(-new_x,0.0f);
    roi_out->x = fmaxf(new_x,0.0f);
    d->enlarge_y = fmaxf(-new_y,0.0f);
    roi_out->y = fmaxf(new_y,0.0f);

    roi_out->width = new_sc_x;
    roi_out->height = new_sc_y;
    d->tx = roi_in->width  * .5f;
    d->ty = roi_in->height * .5f;
  }
  
  // sanity check.
  if(roi_out->x < 0) roi_out->x = 0;
  if(roi_out->y < 0) roi_out->y = 0;
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;

  // save rotation crop on output buffer in world scale:
  d->cix = roi_out->x;
  d->ciy = roi_out->y;
  d->ciw = roi_out->width;
  d->cih = roi_out->height;

}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  *roi_in = *roi_out;
  // modify_roi_out took care of bounds checking for us. we hopefully do not get requests outside the clipping area.
  // transform aabb back to roi_in

  // this aabb is set off by cx/cy
  const float so = roi_out->scale;
  const float kw = piece->buf_in.width*so, kh = piece->buf_in.height*so;
  const float roi_out_x = roi_out->x - d->enlarge_x*so, roi_out_y = roi_out->y - d->enlarge_y*so;
  float p[2], o[2], aabb[4] = {roi_out_x+d->cix*so, roi_out_y+d->ciy*so, roi_out_x+d->cix*so+roi_out->width, roi_out_y+d->ciy*so+roi_out->height};
  float aabb_in[4] = {INFINITY, INFINITY, -INFINITY, -INFINITY};
  for(int c=0; c<4; c++)
  {
    // get corner points of roi_out
    get_corner(aabb, c, p);
    
    // backtransform aabb using m
    if(d->flip)
    {
      p[1] -= d->tx*so;
      p[0] -= d->ty*so;
    }
    else
    {
      p[0] -= d->tx*so;
      p[1] -= d->ty*so;
    }
    p[0] *= 1.0/so;
    p[1] *= 1.0/so;
    backtransform(p, o, d->m, d->k_h, d->k_v);
    o[0] *= so;
    o[1] *= so;
    o[0] += d->tx*so;
    o[1] += d->ty*so;
    o[0] /= kw;
    o[1] /= kh;
    if (d->k_apply==1) keystone_backtransform(o,d->k_space,d->a,d->b,d->d,d->e,d->g,d->h,d->kxa,d->kya);
    o[0] *= kw;
    o[1] *= kh;
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  // adjust roi_in to minimally needed region
  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  roi_in->x      = aabb_in[0] - interpolation->width;
  roi_in->y      = aabb_in[1] - interpolation->width;
  roi_in->width  = aabb_in[2]-aabb_in[0]+2*interpolation->width;
  roi_in->height = aabb_in[3]-aabb_in[1]+2*interpolation->width;

  if(d->angle == 0.0f && d->all_off)
  {
    // just crop: make sure everything is precise.
    roi_in->x      = aabb_in[0];
    roi_in->y      = aabb_in[1];
    roi_in->width  = roi_out->width;
    roi_in->height = roi_out->height;
  }

  // sanity check.
  const int scwidth = (piece->pipe->iflipped ? piece->pipe->iheight : piece->pipe->iwidth)*so;
  const int scheight = (piece->pipe->iflipped ? piece->pipe->iwidth : piece->pipe->iheight)*so;
  roi_in->x = CLAMP(roi_in->x, 0, scwidth);
  roi_in->y = CLAMP(roi_in->y, 0, scheight);
  roi_in->width = CLAMP(roi_in->width, 1, scwidth - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, scheight - roi_in->y);
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the ouput region!
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  const int ch = piece->colors;
  const int ch_width = ch*roi_in->width;

  assert(ch == 4);
     
  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width && roi_in->height == roi_out->height)
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(d,ovoid,ivoid,roi_in,roi_out)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in  = ((float *)ivoid)+ch*roi_out->width*j;
      float *out = ((float *)ovoid)+ch*roi_out->width*j;
      for(int i=0; i<roi_out->width; i++)
      {
        for(int c=0; c<4; c++) out[c] = in[c];
        out += ch;
        in += ch;
      }
    }
  }
  else
  {
    const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
    const float rx = piece->buf_in.width*roi_in->scale;
    const float ry = piece->buf_in.height*roi_in->scale;
    float k_space[4] = {d->k_space[0]*rx,d->k_space[1]*ry,d->k_space[2]*rx,d->k_space[3]*ry};
    const float kxa = d->kxa*rx, kxb = d->kxb*rx, kxc = d->kxc*rx, kxd = d->kxd*rx;
    const float kya = d->kya*ry, kyb = d->kyb*ry, kyc = d->kyc*ry, kyd = d->kyd*ry;
    float ma,mb,md,me,mg,mh;
    keystone_get_matrix(k_space,kxa,kxb,kxc,kxd,kya,kyb,kyc,kyd,&ma,&mb,&md,&me,&mg,&mh);
    
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(d,ivoid,ovoid,roi_in,roi_out,interpolation,k_space,ma,mb,md,me,mg,mh)
#endif
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j=0; j<roi_out->height; j++)
    {
      float *out = ((float *)ovoid)+ch*j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out+=ch)
      {
        float pi[2], po[2];
  
        pi[0] = roi_out->x - roi_out->scale*d->enlarge_x + roi_out->scale*d->cix + i + .5;
        pi[1] = roi_out->y - roi_out->scale*d->enlarge_y + roi_out->scale*d->ciy + j + .5;
        
        // transform this point using matrix m
        if(d->flip)
        {
          pi[1] -= d->tx*roi_out->scale;
          pi[0] -= d->ty*roi_out->scale;
        }
        else
        {
          pi[0] -= d->tx*roi_out->scale;
          pi[1] -= d->ty*roi_out->scale;
        }
        pi[0] /= roi_out->scale;
        pi[1] /= roi_out->scale;
        backtransform(pi, po, d->m, d->k_h, d->k_v);
        po[0] *= roi_in->scale;
        po[1] *= roi_in->scale;
        po[0] += d->tx*roi_in->scale;
        po[1] += d->ty*roi_in->scale;
        if (d->k_apply==1) keystone_backtransform(po,k_space,ma,mb,md,me,mg,mh,kxa,kya);
        po[0] -= roi_in->x;
        po[1] -= roi_in->y;

        dt_interpolation_compute_pixel4c(interpolation, (float *)ivoid, out, po[0], po[1], roi_in->width, roi_in->height, ch_width);
      }
    }
  }
}



#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  dt_iop_clipping_global_data_t *gd = (dt_iop_clipping_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;


  // only crop, no rot fast and sharp path:
  if(!d->flags && d->angle == 0.0 && d->all_off && roi_in->width == roi_out->width && roi_in->height == roi_out->height)
  {
    size_t origin[] = {0, 0, 0};
    size_t region[] = {width, height, 1};
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    int crkernel = -1;

    const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

    switch(interpolation->id)
    {
      case DT_INTERPOLATION_BILINEAR:
        crkernel = gd->kernel_clip_rotate_bilinear;
        break;
      case DT_INTERPOLATION_BICUBIC:
        crkernel = gd->kernel_clip_rotate_bicubic;
        break;
      case DT_INTERPOLATION_LANCZOS2:
        crkernel = gd->kernel_clip_rotate_lanczos2;
        break;
      case DT_INTERPOLATION_LANCZOS3:
        crkernel = gd->kernel_clip_rotate_lanczos3;
        break;
      default:
        return FALSE;
    }

    int roi[2]  = { roi_in->x, roi_in->y };
    float roo[2]  = { roi_out->x - roi_out->scale*d->enlarge_x + roi_out->scale*d->cix + .5, roi_out->y - roi_out->scale*d->enlarge_y + roi_out->scale*d->ciy + .5 };
    float t[2]  = { d->tx, d->ty };
    float k[2]  = { d->k_h, d->k_v };
    float m[4]  = { d->m[0], d->m[1], d->m[2], d->m[3] };

    float k_sizes[2] = {piece->buf_in.width*roi_in->scale, piece->buf_in.height*roi_in->scale};
    float k_space[4] = { d->k_space[0]*k_sizes[0], d->k_space[1]*k_sizes[1], d->k_space[2]*k_sizes[0], d->k_space[3]*k_sizes[1]};
    if (d->k_apply==0) k_space[2] = 0.0f;
    float ma,mb,md,me,mg,mh;
    keystone_get_matrix(k_space,d->kxa*k_sizes[0],d->kxb*k_sizes[0],d->kxc*k_sizes[0],d->kxd*k_sizes[0],
                        d->kya*k_sizes[1],d->kyb*k_sizes[1],d->kyc*k_sizes[1],d->kyd*k_sizes[1],&ma,&mb,&md,&me,&mg,&mh);
    float ka[2] = { d->kxa*k_sizes[0], d->kya*k_sizes[1]};
    float maa[4] = { ma,mb,md,me };
    float mbb[2] = { mg,mh };
    
    size_t sizes[3];

    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, crkernel, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, crkernel, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, crkernel, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, crkernel, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, crkernel, 4, sizeof(int), &roi_in->width);
    dt_opencl_set_kernel_arg(devid, crkernel, 5, sizeof(int), &roi_in->height);
    dt_opencl_set_kernel_arg(devid, crkernel, 6, 2*sizeof(int), &roi);
    dt_opencl_set_kernel_arg(devid, crkernel, 7, 2*sizeof(float), &roo);
    dt_opencl_set_kernel_arg(devid, crkernel, 8, sizeof(float), &roi_in->scale);
    dt_opencl_set_kernel_arg(devid, crkernel, 9, sizeof(float), &roi_out->scale);
    dt_opencl_set_kernel_arg(devid, crkernel, 10, sizeof(int), &d->flip);
    dt_opencl_set_kernel_arg(devid, crkernel, 11, 2*sizeof(float), &t);
    dt_opencl_set_kernel_arg(devid, crkernel, 12, 2*sizeof(float), &k);
    dt_opencl_set_kernel_arg(devid, crkernel, 13, 4*sizeof(float), &m);
    dt_opencl_set_kernel_arg(devid, crkernel, 14, 4*sizeof(float), &k_space);
    dt_opencl_set_kernel_arg(devid, crkernel, 15, 2*sizeof(float), &ka);
    dt_opencl_set_kernel_arg(devid, crkernel, 16, 4*sizeof(float), &maa);
    dt_opencl_set_kernel_arg(devid, crkernel, 17, 2*sizeof(float), &mbb);
    err = dt_opencl_enqueue_kernel_2d(devid, crkernel, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_clipping] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  float ioratio = (float)roi_out->width*roi_out->height/((float)roi_in->width*roi_in->height);

  tiling->factor = 1.0f + ioratio; // in + out, no temp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_clipping_global_data_t *gd = (dt_iop_clipping_global_data_t *)malloc(sizeof(dt_iop_clipping_global_data_t));
  module->data = gd;
  gd->kernel_clip_rotate_bilinear = dt_opencl_create_kernel(program, "clip_rotate_bilinear");
  gd->kernel_clip_rotate_bicubic = dt_opencl_create_kernel(program, "clip_rotate_bicubic");
  gd->kernel_clip_rotate_lanczos2 = dt_opencl_create_kernel(program, "clip_rotate_lanczos2");
  gd->kernel_clip_rotate_lanczos3 = dt_opencl_create_kernel(program, "clip_rotate_lanczos3");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_clipping_global_data_t *gd = (dt_iop_clipping_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bilinear);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_bicubic);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos2);
  dt_opencl_free_kernel(gd->kernel_clip_rotate_lanczos3);
  free(module->data);
  module->data = NULL;
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)p1;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  //reset all values to be sure evreything is initialized
  d->m[0] = d->m[3] = 1.0f;
  d->m[1] = d->m[2] = 0.0f;
  d->ki_h = d->ki_v = d->k_h = d->k_v = 0.0f;
  d->tx = d->ty = 0.0f;
  d->cix = d->ciy = 0.0f;
  d->cih = d->ciw = 1.0f;
  d->kxa = d->kxd = d->kya = d->kyb = 0.0f;
  d->kxb = d->kxc = d->kyc = d->kyd = 0.6f;
  d->k_space[0] = d->k_space[1] = 0.2f;
  d->k_space[2] = d->k_space[3] = 0.6f;
  d->k_apply = 0;
  d->enlarge_x = d->enlarge_y = 0.0f;
  d->flip = 0;
  d->angle = M_PI/180.0 * p->angle;
  
  //image flip
  d->flags = (p->ch < 0 ? FLAG_FLIP_VERTICAL : 0) | (p->cw < 0 ? FLAG_FLIP_HORIZONTAL : 0);
  d->crop_auto = p->crop_auto;
  
  //keystones values computation
  if (p->k_type == 4)
  {
    //this is for old keystoning
    d->k_apply = 0;
    d->all_off = 1;
    if(fabsf(p->k_h) >= .0001) d->all_off = 0;
    if(p->k_h >= -1.0 && p->k_h <= 1.0) d->ki_h = p->k_h;
    else d->ki_h = 0.0f;
    if(fabsf(p->k_v) >= .0001) d->all_off = 0;
    if(p->k_v >= -1.0 && p->k_v <= 1.0) d->ki_v = p->k_v;
    else d->ki_v = 0.0f;
  }
  else if (p->k_type >= 0 && p->k_apply==1)
  {
    //we reset old keystoning values
    d->ki_h = d->ki_v = 0;
    d->kxa = p->kxa;
    d->kxb = p->kxb;
    d->kxc = p->kxc;
    d->kxd = p->kxd;
    d->kya = p->kya;
    d->kyb = p->kyb;
    d->kyc = p->kyc;
    d->kyd = p->kyd;
    //we adjust the points if the keystoning is not in "full" mode
    if (p->k_type == 1) //we want horizontal points to be aligned
    {
      //line equations parameters
      float a1 = (d->kxd-d->kxa)/(float)(d->kyd-d->kya);
      float b1 = d->kxa-a1*d->kya;
      float a2 = (d->kxc-d->kxb)/(float)(d->kyc-d->kyb);
      float b2 = d->kxb-a2*d->kyb;
      
      if (d->kya > d->kyb)
      {
        //we move kya to the level of kyb
        d->kya = d->kyb;
        d->kxa = a1*d->kya + b1;
      }
      else
      {
        //we move kyb to the level of kya
        d->kyb = d->kya;
        d->kxb = a2*d->kyb + b2;
      }
      
      if (d->kyc > d->kyd)
      {
        //we move kyd to the level of kyc
        d->kyd = d->kyc;
        d->kxd = a1*d->kyd + b1;
      }
      else
      {
        //we move kyc to the level of kyd
        d->kyc = d->kyd;
        d->kxc = a2*d->kyc + b2;
      }
    }
    else if (p->k_type == 2)  //we want vertical points to be aligned
    {
      //line equations parameters
      float a1 = (d->kyb-d->kya)/(float)(d->kxb-d->kxa);
      float b1 = d->kya-a1*d->kxa;
      float a2 = (d->kyc-d->kyd)/(float)(d->kxc-d->kxd);
      float b2 = d->kyd-a2*d->kxd;
      
      if (d->kxa > d->kxd)
      {
        //we move kxa to the level of kxd
        d->kxa = d->kxd;
        d->kya = a1*d->kxa + b1;
      }
      else
      {
        //we move kyb to the level of kya
        d->kxd = d->kxa;
        d->kyd = a2*d->kxd + b2;
      }
      
      if (d->kxc > d->kxb)
      {
        //we move kyd to the level of kyc
        d->kxb = d->kxc;
        d->kyb = a1*d->kxb + b1;
      }
      else
      {
        //we move kyc to the level of kyd
        d->kxc = d->kxb;
        d->kyc = a2*d->kxc + b2;
      }
    }
    d->k_space[0]=fabsf((d->kxa+d->kxd)/2.0f);
    d->k_space[1]=fabsf((d->kya+d->kyb)/2.0f);
    d->k_space[2]=fabsf((d->kxb+d->kxc)/2.0f)-d->k_space[0];
    d->k_space[3]=fabsf((d->kyc+d->kyd)/2.0f)-d->k_space[1];
    //d->kxa = d->kxa -d->kxa; //- d->k_space[0];
    d->kxb = d->kxb -d->kxa; //- d->k_space[0];
    d->kxc = d->kxc -d->kxa; //- d->k_space[0];
    d->kxd = d->kxd -d->kxa; //- d->k_space[0];
    //d->kya = d->kya -d->kya; //- d->k_space[1];
    d->kyb = d->kyb -d->kya; //- d->k_space[1];
    d->kyc = d->kyc -d->kya; //- d->k_space[1];
    d->kyd = d->kyd -d->kya; //- d->k_space[1]; 
    keystone_get_matrix(d->k_space,d->kxa,d->kxb,d->kxc,d->kxd,d->kya,d->kyb,d->kyc,d->kyd,&d->a,&d->b,&d->d,&d->e,&d->g,&d->h);
    
    d->k_apply = 1;
    d->all_off = 0;
    d->crop_auto = 0;
  }
  else if (p->k_type == 0)
  {
    d->all_off = 1;
    d->k_apply = 0;
  }
  else
  {
    d->all_off = 1;
    d->k_apply = 0;
    //we are setting the keystone points, so we disable flip and rotate
    d->angle = 0.0f;
    d->flags = 0;
  }
  
  
  if(gui_has_focus(self))
  {
    d->cx = 0.0f;
    d->cy = 0.0f;
    d->cw = 1.0f;
    d->ch = 1.0f;
  }
  else
  {
    d->cx = p->cx;
    d->cy = p->cy;
    d->cw = fabsf(p->cw);
    d->ch = fabsf(p->ch);
  }
}

void gui_focus (struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  if(self->enabled)
  {
    if(in)
    {
      // got focus. make it redraw in full and grab stuff to gui:
      // need to get gui stuff for the first time for this image,
      // and advice the pipe to redraw in full:
      g->clip_x = p->cx;
      g->clip_w = fabsf(p->cw) - p->cx;
      g->clip_y = p->cy;
      g->clip_h = fabsf(p->ch) - p->cy;
      // flip one bit to trigger the cache:
      uint32_t hack = *(uint32_t*)&p->cy;
      hack ^= 1;
      p->cy = *(float *)&hack;
      if(!darktable.gui->reset)
        dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else
    {
      // lost focus, commit current params:
      //if the keystone setting is not finished, we discard it
      if (p->k_apply == 0 && p->k_type < 4 && p->k_type > 0)
      {
        keystone_type_populate(self,FALSE,0);
      }      
      commit_box (self, g, p);
    }
  }
}


void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_clipping_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
}

static void
apply_box_aspect(dt_iop_module_t *self, int grab)
{

  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int iwd, iht;
  dt_dev_get_processed_size(darktable.develop, &iwd, &iht);
  float wd = iwd, ht = iht;
  // enforce aspect ratio.
  const float aspect = g->current_aspect;
  // const float aspect = gtk_spin_button_get_value(g->aspect);
  // if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->aspect_on)))
  if(aspect > 0)
  {
    // if only one side changed, force aspect by two adjacent in equal parts
    // 1 2 4 8 : x y w h
    double clip_x = g->clip_x, clip_y = g->clip_y, clip_w = g->clip_w, clip_h = g->clip_h;

    // aspect = wd*w/ht*h
    // if we only modified one dim, respectively, we wanted these values:
    const double target_h = (double)wd*g->clip_w/(double)(ht*aspect);
    const double target_w = (double)ht*g->clip_h*aspect/(double)wd;
    // i.e. target_w/h = w/target_h = aspect
    // first fix aspect ratio:

    // corners: move two adjacent
    if     (grab == 1+2)
    {
      // move x y
      clip_x = clip_x + clip_w - (target_w + clip_w)*.5;
      clip_y = clip_y + clip_h - (target_h + clip_h)*.5;
      clip_w = (target_w + clip_w)*.5;
      clip_h = (target_h + clip_h)*.5;
    }
    else if(grab == 2+4) // move y w
    {
      clip_y = clip_y + clip_h - (target_h + clip_h)*.5;
      clip_w = (target_w + clip_w)*.5;
      clip_h = (target_h + clip_h)*.5;
    }
    else if(grab == 4+8) // move w h
    {
      clip_w = (target_w + clip_w)*.5;
      clip_h = (target_h + clip_h)*.5;
    }
    else if(grab == 8+1) // move h x
    {
      clip_h = (target_h + clip_h)*.5;
      clip_x = clip_x + clip_w - (target_w + clip_w)*.5;
      clip_w = (target_w + clip_w)*.5;
    }
    else if(grab & 5) // dragged either x or w (1 4)
    {
      // change h and move y, h equally
      const double off = target_h - clip_h;
      clip_h = clip_h + off;
      clip_y = clip_y - .5*off;
    }
    else if(grab & 10) // dragged either y or h (2 8)
    {
      // channge w and move x, w equally
      const double off = target_w - clip_w;
      clip_w = clip_w + off;
      clip_x = clip_x - .5*off;
    }

    // now fix outside boxes:
    if(clip_x < 0)
    {
      double prev_clip_h = clip_h;
      clip_h *= (clip_w + clip_x)/clip_w;
      clip_w  =  clip_w + clip_x;
      clip_x  = 0;
      if (grab & 2) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y < 0)
    {
      double prev_clip_w = clip_w;
      clip_w *= (clip_h + clip_y)/clip_h;
      clip_h  =  clip_h + clip_y;
      clip_y  =  0;
      if (grab & 1) clip_x += prev_clip_w - clip_w;
    }
    if(clip_x + clip_w > 1.0)
    {
      double prev_clip_h = clip_h;
      clip_h *= (1.0 - clip_x)/clip_w;
      clip_w  =  1.0 - clip_x;
      if (grab & 2) clip_y += prev_clip_h - clip_h;
    }
    if(clip_y + clip_h > 1.0)
    {
      double prev_clip_w = clip_w;
      clip_w *= (1.0 - clip_y)/clip_h;
      clip_h  =  1.0 - clip_y;
      if (grab & 1) clip_x += prev_clip_w - clip_w;
    }
    g->clip_x = clip_x;
    g->clip_y = clip_y;
    g->clip_w = clip_w;
    g->clip_h = clip_h;
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_clipping_params_t tmp = (dt_iop_clipping_params_t)
  {
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.2f, 0.2f, 0.8f, 0.2f, 0.8f, 0.8f, 0.2f, 0.8f, 0, 0,FALSE,TRUE
  };
  memcpy(self->params, &tmp, sizeof(dt_iop_clipping_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_clipping_params_t));
  self->default_enabled = 0;
}

static void
aspect_presets_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = dt_bauhaus_combobox_get(combo);
  if (which < 0)
  {
    // parse config param:
    if(g->current_aspect == -1.0f)
    {
      g->current_aspect = dt_conf_get_float("plugins/darkroom/clipping/custom_aspect");
      if(g->current_aspect <= 0.0f) g->current_aspect = 1.5f;
      char text[128];
      snprintf(text, 128, "%.3f:1", g->current_aspect);
      dt_bauhaus_combobox_set_text(combo, text);
      if(self->dt->gui->reset) return;
      apply_box_aspect(self, 5);
      dt_control_queue_redraw_center();
    }
    // user is typing, don't overwrite it.
    g->current_aspect = -2.0f;
    // reset to free aspect ratio:
    dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", -1);

    const char* text = dt_bauhaus_combobox_get_text(combo);
    if(text)
    {
      const char *c = text;
      while(*c != ':' && *c != '/' && c < text + strlen(text)) c++;
      if(c < text + strlen(text) - 1)
      {
        // *c = '\0'; // not needed, atof will stop there.
        c++;
        g->current_aspect = atof(text) / atof(c);
        if(self->dt->gui->reset) return;
        apply_box_aspect(self, 5);
        dt_control_queue_redraw_center();
      }
    }
  }
  else if (which < NUM_RATIOS)
  {
    dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", which);

    g->current_aspect = g->aspect_ratios[which];

    if(self->dt->gui->reset) return;
    apply_box_aspect(self, 5);
    dt_control_queue_redraw_center();
  }
}

static void
angle_callback (GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->angle = - dt_bauhaus_slider_get(slider);
  commit_box (self, g, p);
}

void gui_reset(struct dt_iop_module_t *self)
{
  /* reset aspect preset to default */
  dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", 1);

}

static void
keystone_type_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  int which = dt_bauhaus_combobox_get(combo);
  if ((which == 5) || (which == 4 && p->k_h==0 && p->k_v==0))
  {
     //if the keystone is applied,autocrop must be disabled !
     gtk_widget_set_sensitive(g->crop_auto,FALSE);
     //and we can enable flip and rotate
     gtk_widget_set_sensitive(g->hvflip, TRUE);
     gtk_widget_set_sensitive(g->angle, TRUE);
     gtk_widget_set_sensitive(g->aspect_presets, TRUE);
     return; 
  }
  //we recreate the list to be sure that the "already applied" entry is not display
  if (g->k_show == 2)
  {
    if (which == 0 || which == 4) g->k_show = 0;
    else g->k_show = 1;
    keystone_type_populate(self,FALSE,which);   
  }
  
  //we set the params
  p->k_apply = 0;
  p->k_type = which;
  if (which == 0 || which == 4) g->k_show = 0;
  else g->k_show = 1;  
  
  //we can enable autocrop
  gtk_widget_set_sensitive(g->crop_auto,(g->k_show == 0));
  
  //if we are not setting keystone, we can enable flip and rotate
  gtk_widget_set_sensitive(g->hvflip, (g->k_show == 0));
  gtk_widget_set_sensitive(g->angle, (g->k_show == 0));
  gtk_widget_set_sensitive(g->aspect_presets, (g->k_show == 0));
     
  commit_box(self,g,p);
  dt_iop_request_focus(self);
  dt_control_queue_redraw_center();
}

static void keystone_type_populate(struct dt_iop_module_t *self,gboolean with_applied,int select)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  dt_bauhaus_combobox_clear(g->keystone_type);
  dt_bauhaus_combobox_add(g->keystone_type, _("none"));
  dt_bauhaus_combobox_add(g->keystone_type, _("vertical"));
  dt_bauhaus_combobox_add(g->keystone_type, _("horizontal"));
  dt_bauhaus_combobox_add(g->keystone_type, _("full"));
  if (p->k_h != 0 || p->k_v != 0) dt_bauhaus_combobox_add(g->keystone_type, _("old system"));
  if (with_applied) dt_bauhaus_combobox_add(g->keystone_type, _("correction applied"));
  
  if (select < 0) return;
  int sel = 0;
  if (select > 10 && p->k_h == 0 && p->k_v == 0) sel = 4;
  else if (select > 10) sel = 5;
  else sel = select;
  
  dt_bauhaus_combobox_set(g->keystone_type, sel);
  //we have to be sure that the event is called...
  keystone_type_changed(g->keystone_type, self);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  /* recalc aspect ratios for image */
  _iop_clipping_update_ratios(self);

  /* update ui elements */
  dt_bauhaus_slider_set(g->angle, -p->angle);
  int hvflip = 0;
  if(p->cw < 0)
  {
    if(p->ch < 0) hvflip = 3;
    else          hvflip = 1;
  }
  else
  {
    if(p->ch < 0) hvflip = 2;
    else          hvflip = 0;
  }
  dt_bauhaus_combobox_set(g->hvflip, hvflip);

  int act = dt_conf_get_int("plugins/darkroom/clipping/aspect_preset");
  if (act < -1 || act >= NUM_RATIOS)
    act = 0;

  //keystone :
  if (p->k_apply==1) g->k_show = 2; //needed to initialise correctly the combobox
  if (g->k_show == 2)
  {
    keystone_type_populate(self,TRUE,99);
  }
  else if (g->k_show == -1)
  {
    keystone_type_populate(self,FALSE,p->k_type);
  }


  /* special handling the combobox when current act is already selected
     callback is not called, let do it our self then..
   */
  if (dt_bauhaus_combobox_get(g->aspect_presets) == act)
    aspect_presets_changed(g->aspect_presets, self);
  else
    dt_bauhaus_combobox_set(g->aspect_presets, act);

  // reset gui draw box to what we have in the parameters:
  g->applied = 1;
  g->clip_x = p->cx;
  g->clip_w = fabsf(p->cw) - p->cx;
  g->clip_y = p->cy;
  g->clip_h = fabsf(p->ch) - p->cy;
  
  dt_bauhaus_combobox_set(g->crop_auto, p->crop_auto);
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_clipping_data_t));
  module->params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_clipping_params_t);
  module->gui_data = NULL;
  module->priority = 384; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
hvflip_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  const int flip = dt_bauhaus_combobox_get(widget);
  p->cw = copysignf(p->cw, (flip & 1) ? -1.0 : 1.0);
  p->ch = copysignf(p->ch, (flip & 2) ? -1.0 : 1.0);
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  commit_box (self, g, p);
}

static void
key_swap_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                  guint keyval, GdkModifierType modifier, gpointer d)
{
  (void)accel_group;
  (void)acceleratable;
  (void)keyval;
  (void)modifier;
  dt_iop_module_t *self = (dt_iop_module_t *)d;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  g->current_aspect = 1.0/g->current_aspect;
  apply_box_aspect(self, 5);
  dt_control_queue_redraw_center();
}

static gboolean key_commit_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable,
                                    guint keyval, GdkModifierType modifier,
                                    gpointer data)
{
  dt_iop_module_t* self = (dt_iop_module_t*)data;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  commit_box(self, g, p);
  return TRUE;
}

static void
aspect_flip(GtkWidget *button, dt_iop_module_t *self)
{
  key_swap_callback(NULL, NULL, 0, 0, self);
}

#define GUIDE_NONE 0
#define GUIDE_GRID 1
#define GUIDE_THIRD 2
#define GUIDE_DIAGONAL 3
#define GUIDE_TRIANGL 4
#define GUIDE_GOLDEN 5

static void
guides_presets_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = dt_bauhaus_combobox_get(combo);
  if (which == GUIDE_TRIANGL || which == GUIDE_GOLDEN )
    gtk_widget_set_visible(g->flip_guides, TRUE);
  else
    gtk_widget_set_visible(g->flip_guides, FALSE);

  if (which == GUIDE_GOLDEN)
    gtk_widget_set_visible(g->golden_extras, TRUE);
  else
    gtk_widget_set_visible(g->golden_extras, FALSE);

  dt_iop_request_focus(self);
  dt_control_queue_redraw_center();
}

static void
guides_button_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  // redraw guides
  dt_control_queue_redraw_center();
}

static void
crop_auto_changed (GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  
  if (dt_bauhaus_combobox_get(combo) == p->crop_auto) return; //no change
  p->crop_auto = dt_bauhaus_combobox_get(combo);
  commit_box(self,g,p);
  dt_iop_request_focus(self);
  dt_control_queue_redraw_center();
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_clipping_gui_data_t));
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  g->current_aspect = -1.0f;
  g->clip_x = g->clip_y = g->handle_x = g->handle_y = 0.0;
  g->clip_w = g->clip_h = 1.0;
  g->old_clip_x = g->old_clip_y = 0.0;
  g->old_clip_w = g->old_clip_h = 1.0;
  g->cropping = 0;
  g->straightening = 0;
  g->applied = 1;
  g->center_lock = 0;
  g->k_drag = FALSE;
  g->k_show = -1;
  g->k_selected = -1;

  self->widget = gtk_vbox_new(FALSE, DT_BAUHAUS_SPACE);
  g->hvflip = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->hvflip, _("flip"));
  dt_bauhaus_combobox_add(g->hvflip, _("none"));
  dt_bauhaus_combobox_add(g->hvflip, _("horizontal"));
  dt_bauhaus_combobox_add(g->hvflip, _("vertical"));
  dt_bauhaus_combobox_add(g->hvflip, _("both"));
  g_signal_connect (G_OBJECT (g->hvflip), "value-changed", G_CALLBACK (hvflip_callback), self);
  g_object_set(G_OBJECT(g->hvflip), "tooltip-text", _("mirror image horizontally and/or vertically"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->hvflip, TRUE, TRUE, 0);


  g->angle= dt_bauhaus_slider_new_with_range(self, -180.0, 180.0, 0.25, p->angle, 2);
  dt_bauhaus_widget_set_label(g->angle, _("angle"));
  dt_bauhaus_slider_set_format(g->angle, "%.02f°");
  g_signal_connect (G_OBJECT (g->angle), "value-changed", G_CALLBACK (angle_callback), self);
  g_object_set(G_OBJECT(g->angle), "tooltip-text", _("right-click and drag a line on the image to drag a straight line"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->angle, TRUE, TRUE, 0);

  g->keystone_type = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->keystone_type, _("keystone"));
  dt_bauhaus_combobox_add(g->keystone_type, _("none"));
  dt_bauhaus_combobox_add(g->keystone_type, _("vertical"));
  dt_bauhaus_combobox_add(g->keystone_type, _("horizontal"));
  dt_bauhaus_combobox_add(g->keystone_type, _("full"));
  g_object_set(G_OBJECT(g->keystone_type), "tooltip-text", _("set perspective correction for your image"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->keystone_type), "value-changed", G_CALLBACK (keystone_type_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->keystone_type, TRUE, TRUE, 0);
  
  g->crop_auto = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->crop_auto, _("automatic cropping"));
  dt_bauhaus_combobox_add(g->crop_auto, _("no"));
  dt_bauhaus_combobox_add(g->crop_auto, _("yes"));
  g_object_set(G_OBJECT(g->crop_auto), "tooltip-text", _("automatically crop to avoid black edges"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->crop_auto), "value-changed", G_CALLBACK (crop_auto_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->crop_auto, TRUE, TRUE, 0);
  
  g->aspect_presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_set_editable(g->aspect_presets, 1);
  dt_bauhaus_widget_set_label(g->aspect_presets, _("aspect"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("free"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("image"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("golden cut"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("1:2"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("3:2"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("7:5"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("4:3"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("5:4"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("square"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("DIN"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("16:9"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("16:10"));
  dt_bauhaus_combobox_add(g->aspect_presets, _("10:8 in print"));

  int act = dt_conf_get_int("plugins/darkroom/clipping/aspect_preset");
  if(act < 0 || act >= NUM_RATIOS) act = 0;
  dt_bauhaus_combobox_set(g->aspect_presets, act);
  g_signal_connect (G_OBJECT (g->aspect_presets), "value-changed", G_CALLBACK (aspect_presets_changed), self);
  g_object_set(G_OBJECT(g->aspect_presets), "tooltip-text", _("set the aspect ratio (w:h)"), (char *)NULL);
  dt_bauhaus_widget_set_quad_paint(g->aspect_presets, dtgtk_cairo_paint_aspectflip, 0);
  g_signal_connect (G_OBJECT (g->aspect_presets), "quad-pressed", G_CALLBACK (aspect_flip), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->aspect_presets, TRUE, TRUE, 0);

  g->guide_lines = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->guide_lines, _("guides"));
  dt_bauhaus_combobox_add(g->guide_lines, _("none"));
  dt_bauhaus_combobox_add(g->guide_lines, _("grid"));
  dt_bauhaus_combobox_add(g->guide_lines, _("rules of thirds"));
  dt_bauhaus_combobox_add(g->guide_lines, _("diagonal method"));
  dt_bauhaus_combobox_add(g->guide_lines, _("harmonious triangles"));
  dt_bauhaus_combobox_add(g->guide_lines, _("golden mean"));
  g_object_set(G_OBJECT(g->guide_lines), "tooltip-text", _("display guide lines to help compose your photograph"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->guide_lines), "value-changed", G_CALLBACK (guides_presets_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->guide_lines, TRUE, TRUE, 0);

  g->flip_guides = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->flip_guides, _("flip"));
  dt_bauhaus_combobox_add(g->flip_guides, _("none"));
  dt_bauhaus_combobox_add(g->flip_guides, _("horizontally"));
  dt_bauhaus_combobox_add(g->flip_guides, _("vertically"));
  dt_bauhaus_combobox_add(g->flip_guides, _("both"));
  g_object_set(G_OBJECT(g->flip_guides), "tooltip-text", _("flip guides"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->flip_guides), "value-changed", G_CALLBACK (guides_button_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->flip_guides, TRUE, TRUE, 0);

  g->golden_extras = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->golden_extras, _("extra"));
  dt_bauhaus_combobox_add(g->golden_extras, _("golden sections"));
  dt_bauhaus_combobox_add(g->golden_extras, _("golden spiral sections"));
  dt_bauhaus_combobox_add(g->golden_extras, _("golden spiral"));
  dt_bauhaus_combobox_add(g->golden_extras, _("all"));
  g_object_set(G_OBJECT(g->golden_extras), "tooltip-text", _("show some extra guides"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->golden_extras), "value-changed", G_CALLBACK (guides_button_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->golden_extras, TRUE, TRUE, 0);

  gtk_widget_set_visible(g->flip_guides, FALSE);
  gtk_widget_set_visible(g->golden_extras, FALSE);
  gtk_widget_set_no_show_all(g->flip_guides, TRUE);
  gtk_widget_set_no_show_all(g->golden_extras, TRUE);

  _iop_clipping_update_ratios(self);

  /* set default aspect ratio */
  g->current_aspect = g->aspect_ratios[act];
}

void _iop_clipping_update_ratios(dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = self->gui_data;

  g->aspect_ratios[0] = -1;
  g->aspect_ratios[1] = self->dev->image_storage.width / (float)self->dev->image_storage.height;
  g->aspect_ratios[2] = PHI;
  g->aspect_ratios[3] = 2.0/1.0;
  g->aspect_ratios[4] = 3.0/2.0;
  g->aspect_ratios[5] = 7.0/5.0;
  g->aspect_ratios[6] = 4.0/3.0;
  g->aspect_ratios[7] = 5.0f/4.0f;
  g->aspect_ratios[8] = 1.0;
  g->aspect_ratios[9] = sqrtf(2.0);
  g->aspect_ratios[10] = 16.0f/9.0f;
  g->aspect_ratios[11] = 16.0f/10.0f;
  g->aspect_ratios[12] = 244.5f/203.2f;

  // if adding new presets, make sure to change this as well:
  assert(NUM_RATIOS == 13);

  /* swap default fixed ratios for portraits */
  if (g->aspect_ratios[1] < 1.0)
  {
    for (int k=2; k<NUM_RATIOS; k++)
      g->aspect_ratios[k] = 1.0 / g->aspect_ratios[k];
  }

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

static int
get_grab (float pzx, float pzy, dt_iop_clipping_gui_data_t *g, const float border, const float wd, const float ht)
{
  int grab = 0;
  if(pzx >= g->clip_x && pzx*wd < g->clip_x*wd + border) grab |= 1; // left border
  if(pzy >= g->clip_y && pzy*ht < g->clip_y*ht + border) grab |= 2; // top border
  if(pzx <= g->clip_x+g->clip_w && pzx*wd > (g->clip_w+g->clip_x)*wd - border) grab |= 4; // right border
  if(pzy <= g->clip_y+g->clip_h && pzy*ht > (g->clip_h+g->clip_y)*ht - border) grab |= 8; // bottom border
  return grab;
}

//draw rounded rectangle
void gui_draw_rounded_rectangle(cairo_t *cr,int width,int height,int x,int y)
{
  float radius = height/5.0f;
  float degrees = M_PI / 180.0;
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc (cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc (cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc (cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path (cr);
  cairo_fill(cr);
}
// draw symetry signs
void gui_draw_sym(cairo_t *cr, float x, float y, gboolean active)
{
  cairo_text_extents_t extents;
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, 16);
  //VERIF : is there any pb to use such special char here ?
  cairo_text_extents (cr, "ꝏ", &extents);
  cairo_set_source_rgba(cr, .5,.5,.5, .7);
  gui_draw_rounded_rectangle(cr,extents.width+4,extents.height+8,x-extents.width/2.0f-2,y-extents.height/2.0f-4);
  cairo_move_to(cr,x-extents.width/2.0f-1,y+extents.height/2.0f-1);
  if (active) cairo_set_source_rgba(cr, 1.0,0.0,0.0, .9);
  else cairo_set_source_rgba(cr, .2,.2,.2, .9);
  cairo_show_text(cr, "ꝏ");
}

// draw guides and handles over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  //reapply box aspect to be sure that the ratio has not been modified by the keystone transform
  apply_box_aspect(self,5);
  
  int32_t zoom, closeup;
  float zoom_x, zoom_y;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  //printf("expose %f,%f %f,%f %f\n",wd,ht,zoom_x,zoom_y,zoom_scale);
  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  double dashes = 5.0/zoom_scale;

  // draw cropping window
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  cairo_set_dash (cr, &dashes, 0, 0);
  cairo_set_source_rgba(cr, .2, .2, .2, .8);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle (cr, -1, -1, wd+2, ht+2);
  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, g->clip_h*ht);
  cairo_fill (cr);

  if(g->clip_x > .0f || g->clip_y > .0f || g->clip_w < 1.0f || g->clip_h < 1.0f)
  {
    cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, g->clip_h*ht);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_stroke (cr);
  }

  //draw cropping window dimensions if first mouse button is pressed
  if(darktable.control->button_down && darktable.control->button_down_which == 1 && g->k_show != 1)
  {
    char dimensions[16];
    dimensions[0] = '\0';
    cairo_text_extents_t extents;

    int procw, proch;
    dt_dev_get_processed_size(dev, &procw, &proch);
    sprintf(dimensions, "%.0fx%.0f",
            (float)procw * g->clip_w, (float)proch * g->clip_h);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);

    cairo_text_extents (cr, dimensions, &extents);
    cairo_move_to(cr, (g->clip_x + g->clip_w / 2) * wd - extents.width * .5f, (g->clip_y + g->clip_h/2) * ht);
    cairo_show_text(cr, dimensions);
  }

  // draw crop area guides
  float left, top, right, bottom, xThird, yThird;
  left = g->clip_x*wd;
  top = g->clip_y*ht;
  right = g->clip_x*wd + g->clip_w*wd;
  bottom = g->clip_y*ht + g->clip_h*ht;
  float cwidth = g->clip_w*wd;
  float cheight = g->clip_h*ht;
  xThird = cwidth  / 3;
  yThird = cheight / 3;

  // save context
  cairo_save(cr);
  cairo_rectangle (cr, left, top, cwidth, cheight);
  cairo_clip(cr);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);

  // draw guides
  cairo_set_dash(cr, &dashes, 1, 0);

  int guide_flip = dt_bauhaus_combobox_get(g->flip_guides);
  int which = dt_bauhaus_combobox_get(g->guide_lines);
  if (which == GUIDE_GRID)
  {
    dt_guides_draw_simple_grid(cr, left, top, right, bottom, zoom_scale);
  }
  else if (which == GUIDE_DIAGONAL)
  {
    dt_guides_draw_diagonal_method(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    dt_guides_draw_diagonal_method(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_THIRD)
  {
    dt_guides_draw_rules_of_thirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    dt_guides_draw_rules_of_thirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_TRIANGL)
  {
    int dst = (int)((cheight*cos(atan(cwidth/cheight)) / (cos(atan(cheight/cwidth)))));
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (guide_flip & 1)
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (guide_flip & 2)
      cairo_scale(cr, 1, -1);

    dt_guides_draw_harmonious_triangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);
    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    dt_guides_draw_harmonious_triangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_GOLDEN)
  {
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (guide_flip & 1)
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (guide_flip & 2)
      cairo_scale(cr, 1, -1);

    float w = cwidth;
    float h = cheight;

    // lengths for the golden mean and half the sizes of the region:
    float w_g = w*INVPHI;
    float h_g = h*INVPHI;
    float w_2 = w/2;
    float h_2 = h/2;

    dt_QRect_t R1, R2, R3, R4, R5, R6, R7;
    dt_guides_q_rect (&R1, -w_2, -h_2, w_g, h);

    // w - 2*w_2 corrects for one-pixel difference
    // so that R2.right() is really at the right end of the region
    dt_guides_q_rect (&R2, w_g-w_2, h_2-h_g, w-w_g+1-(w - 2*w_2), h_g);
    dt_guides_q_rect (&R3, w_2 - R2.width*INVPHI, -h_2, R2.width*INVPHI, h - R2.height);
    dt_guides_q_rect (&R4, R2.left, R1.top, R3.left - R2.left, R3.height*INVPHI);
    dt_guides_q_rect (&R5, R4.left, R4.bottom, R4.width*INVPHI, R3.height - R4.height);
    dt_guides_q_rect (&R6, R5.left + R5.width, R5.bottom - R5.height*INVPHI, R3.left - R5.right, R5.height*INVPHI);
    dt_guides_q_rect (&R7, R6.right - R6.width*INVPHI, R4.bottom, R6.width*INVPHI, R5.height - R6.height);

    const int extras = dt_bauhaus_combobox_get(g->golden_extras);
    dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7,
                               extras == 0 || extras == 3,
                               0,
                               extras == 1 || extras == 3,
                               extras == 2 || extras == 3);
    cairo_stroke (cr);

    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    dt_guides_draw_golden_mean(cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7,
                               extras == 0 || extras == 3,
                               0,
                               extras == 1 || extras == 3,
                               extras == 2 || extras == 3);
    cairo_stroke (cr);
  }
  cairo_restore(cr);

  cairo_set_line_width(cr, 2.0/zoom_scale);
  cairo_set_source_rgb(cr, .3, .3, .3);
  const int border = 30.0/zoom_scale;
  if(g->straightening)
  {
    float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    cairo_arc (cr, bzx*wd, bzy*ht, 3, 0, 2.0*M_PI);
    cairo_stroke (cr);
    cairo_arc (cr, pzx*wd, pzy*ht, 3, 0, 2.0*M_PI);
    cairo_stroke (cr);
    cairo_move_to (cr, bzx*wd, bzy*ht);
    cairo_line_to (cr, pzx*wd, pzy*ht);
    cairo_stroke (cr);

    //show rotation angle
    float dx = pzx*wd - bzx*wd, dy = pzy*ht - bzy*ht ;
    if(dx < 0)
    {
      dx = -dx;
      dy = -dy;
    }
    float angle = atan2f(dy, dx);
    angle = angle * 180 / M_PI;
    if (angle > 45.0) angle -= 90;
    if (angle < -45.0) angle += 90;

    char view_angle[16];
    view_angle[0] = '\0';
    sprintf(view_angle, "%.2f °", angle);
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_move_to (cr, pzx*wd + 20, pzy*ht);
    cairo_show_text(cr, view_angle);
  }
  else if (g->k_show != 1)
  {
    int grab = g->cropping ? g->cropping : get_grab (pzx, pzy, g, border, wd, ht);
    if(grab == 1)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, border, g->clip_h*ht);
    if(grab == 2)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, border);
    if(grab == 3)  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, border, border);
    if(grab == 4)  cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, g->clip_y*ht, border, g->clip_h*ht);
    if(grab == 8)  cairo_rectangle (cr, g->clip_x*wd, (g->clip_y+g->clip_h)*ht-border, g->clip_w*wd, border);
    if(grab == 12) cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, (g->clip_y+g->clip_h)*ht-border, border, border);
    if(grab == 6)  cairo_rectangle (cr, (g->clip_x+g->clip_w)*wd-border, g->clip_y*ht, border, border);
    if(grab == 9)  cairo_rectangle (cr, g->clip_x*wd, (g->clip_y+g->clip_h)*ht-border, border, border);
    cairo_stroke (cr);
  }
  
  //draw keystone points and lines
  if (g->k_show == 1 && p->k_type > 0)
  {
    //points in screen space
    float p1[2] = {p->kxa*wd,p->kya*ht};
    float p2[2] = {p->kxb*wd,p->kyb*ht};
    float p3[2] = {p->kxc*wd,p->kyc*ht};
    float p4[2] = {p->kxd*wd,p->kyd*ht};

    if (p->k_type == 3)
    {
      //determine extremity of the lines
      int v1t = p1[0] - (p4[0]-p1[0]) * p1[1] / (float)(p4[1]-p1[1]);
      int v1b = (p4[0]-p1[0]) * ht / (float)(p4[1]-p1[1]) + v1t;
      int v2t = p2[0] - (p3[0]-p2[0]) * p2[1] / (float)(p3[1]-p2[1]);
      int v2b = (p3[0]-p2[0]) * ht / (float)(p3[1]-p2[1]) + v2t;
      int h1l = p1[1] - (p2[1]-p1[1]) * p1[0] / (float)(p2[0]-p1[0]);
      int h1r = (p2[1]-p1[1]) * wd / (float)(p2[0]-p1[0]) + h1l;
      int h2l = p4[1] - (p3[1]-p4[1]) * p4[0] / (float)(p3[0]-p4[0]);
      int h2r = (p3[1]-p4[1]) * wd / (float)(p3[0]-p4[0]) + h2l;
      
      //draw the lines
      cairo_move_to(cr,v1t,0);
      cairo_line_to(cr,v1b,ht);
      cairo_stroke(cr);
      cairo_move_to(cr,v2t,0);
      cairo_line_to(cr,v2b,ht);
      cairo_stroke(cr);
      cairo_move_to(cr,0,h1l);
      cairo_line_to(cr,wd,h1r);
      cairo_stroke(cr);
      cairo_move_to(cr,0,h2l);
      cairo_line_to(cr,wd,h2r);
      cairo_stroke(cr);
      //redraw selected one
      cairo_set_line_width(cr, 4.0/zoom_scale);
      if (g->k_selected_segment == 0)
      {
        cairo_move_to(cr,p->kxa*wd,p->kya*ht);
        cairo_line_to(cr,p->kxb*wd,p->kyb*ht);
        cairo_stroke(cr);
      }
      else if (g->k_selected_segment == 1)
      {
        cairo_move_to(cr,p->kxc*wd,p->kyc*ht);
        cairo_line_to(cr,p->kxb*wd,p->kyb*ht);
        cairo_stroke(cr);
      }
      else if (g->k_selected_segment == 2)
      {
        cairo_move_to(cr,p->kxc*wd,p->kyc*ht);
        cairo_line_to(cr,p->kxd*wd,p->kyd*ht);
        cairo_stroke(cr);
      }
      else if (g->k_selected_segment == 3)
      {
        cairo_move_to(cr,p->kxa*wd,p->kya*ht);
        cairo_line_to(cr,p->kxd*wd,p->kyd*ht);
        cairo_stroke(cr);
      }
    }
    else if (p->k_type == 2)
    {
      //determine extremity of the lines
      int h1l = p1[1] - (p2[1]-p1[1]) * p1[0] / (float)(p2[0]-p1[0]);
      int h1r = (p2[1]-p1[1]) * wd / (float)(p2[0]-p1[0]) + h1l;
      int h2l = p4[1] - (p3[1]-p4[1]) * p4[0] / (float)(p3[0]-p4[0]);
      int h2r = (p3[1]-p4[1]) * wd / (float)(p3[0]-p4[0]) + h2l;
      
      //draw the lines
      cairo_move_to(cr,0,h1l);
      cairo_line_to(cr,wd,h1r);
      cairo_stroke(cr);
      cairo_move_to(cr,0,h2l);
      cairo_line_to(cr,wd,h2r);
      cairo_stroke(cr);
      //redraw selected one
      cairo_set_line_width(cr, 4.0/zoom_scale);
      if (g->k_selected_segment == 1)
      {
        cairo_move_to(cr,p->kxc*wd,p->kyc*ht);
        cairo_line_to(cr,p->kxb*wd,p->kyb*ht);
        cairo_stroke(cr);
      }
      else if (g->k_selected_segment == 3)
      {
        cairo_move_to(cr,p->kxa*wd,p->kya*ht);
        cairo_line_to(cr,p->kxd*wd,p->kyd*ht);
        cairo_stroke(cr);
      }
    }
    else if (p->k_type == 1)
    {
      //determine extremity of the lines
      int v1t = p1[0] - (p4[0]-p1[0]) * p1[1] / (float)(p4[1]-p1[1]);
      int v1b = (p4[0]-p1[0]) * ht / (float)(p4[1]-p1[1]) + v1t;
      int v2t = p2[0] - (p3[0]-p2[0]) * p2[1] / (float)(p3[1]-p2[1]);
      int v2b = (p3[0]-p2[0]) * ht / (float)(p3[1]-p2[1]) + v2t;
      
      //draw the lines
      cairo_move_to(cr,v1t,0);
      cairo_line_to(cr,v1b,ht);
      cairo_stroke(cr);
      cairo_move_to(cr,v2t,0);
      cairo_line_to(cr,v2b,ht);
      cairo_stroke(cr);
      //redraw selected one
      cairo_set_line_width(cr, 4.0/zoom_scale);
      if (g->k_selected_segment == 0)
      {
        cairo_move_to(cr,p->kxa*wd,p->kya*ht);
        cairo_line_to(cr,p->kxb*wd,p->kyb*ht);
        cairo_stroke(cr);
      }
      else if (g->k_selected_segment == 2)
      {
        cairo_move_to(cr,p->kxc*wd,p->kyc*ht);
        cairo_line_to(cr,p->kxd*wd,p->kyd*ht);
        cairo_stroke(cr);
      }
    }
      
    //draw the points
    if (g->k_selected == 0) //point 1
    {
      cairo_set_line_width(cr, 4.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
    }
    else
    {
      cairo_set_line_width(cr, 2.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
    }
    cairo_arc (cr, p1[0], p1[1], 5.0/zoom_scale, 0, 2.0*M_PI);
    cairo_stroke (cr);
    if (g->k_selected == 1) //point 2
    {
      cairo_set_line_width(cr, 4.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
    }
    else
    {
      cairo_set_line_width(cr, 2.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
    }
    cairo_arc (cr, p2[0], p2[1], 5.0/zoom_scale, 0, 2.0*M_PI);
    cairo_stroke (cr);
    if (g->k_selected == 2) //point 3
    {
      cairo_set_line_width(cr, 4.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
    }
    else
    {
      cairo_set_line_width(cr, 2.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
    }
    cairo_arc (cr, p3[0], p3[1], 5.0/zoom_scale, 0, 2.0*M_PI);
    cairo_stroke (cr);
    if (g->k_selected == 3) //point 4
    {
      cairo_set_line_width(cr, 4.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .8);
    }
    else
    {
      cairo_set_line_width(cr, 2.0/zoom_scale);
      cairo_set_source_rgba(cr, 1.0, 0, 0, .5);
    }
    cairo_arc (cr, p4[0], p4[1], 5.0/zoom_scale, 0, 2.0*M_PI);
    cairo_stroke (cr);
    
    //draw the apply "button"
    cairo_text_extents_t extents;
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 16);
    cairo_text_extents (cr, "ok", &extents);
    int c[2] = {(MIN(p3[0],p2[0])+MAX(p1[0],p4[0]))/2.0f, (MIN(p3[1],p4[1])+MAX(p1[1],p2[1]))/2.0f};
    cairo_set_source_rgba(cr, .5,.5,.5, .9);
    gui_draw_rounded_rectangle(cr,extents.width+8,extents.height+12,c[0]-extents.width/2.0f-4,c[1]-extents.height/2.0f-6);
    cairo_move_to(cr,c[0]-extents.width/2.0f,c[1]+extents.height/2.0f);
    cairo_set_source_rgba(cr, .2,.2,.2, .9);
    cairo_show_text(cr, "ok");
    
    //draw the symetry buttons
    gboolean sym = FALSE;
    if (p->k_type == 1 || p->k_type == 3)
    {
      if (p->k_sym == 1 || p->k_sym == 3) sym = TRUE;
      gui_draw_sym(cr,(p1[0]+p4[0])/2.0f,(p1[1]+p4[1])/2.0f,sym);
      gui_draw_sym(cr,(p2[0]+p3[0])/2.0f,(p2[1]+p3[1])/2.0f,sym);
    }
    if (p->k_type == 2 || p->k_type == 3)
    {
      sym = (p->k_sym >=2);
      gui_draw_sym(cr,(p1[0]+p2[0])/2.0f,(p1[1]+p2[1])/2.0f,sym);
      gui_draw_sym(cr,(p4[0]+p3[0])/2.0f,(p4[1]+p3[1])/2.0f,sym);
    }
  }
}

//determine the distance between the segment [(xa,ya)(xb,yb)] and the point (xc,yc)
float dist_seg (float xa,float ya,float xb,float yb,float xc,float yc)
{
  if (xa==xb && ya==yb) return (xc-xa)*(xc-xa)+(yc-ya)*(yc-ya);
 
  float sx=xb-xa;
  float sy=yb-ya;
   
  float ux=xc-xa;
  float uy=yc-ya;
   
  float dp=sx*ux+sy*uy;
  if (dp<0) return (xc-xa)*(xc-xa)+(yc-ya)*(yc-ya);
   
  float sn2 = sx*sx+sy*sy;
  if (dp>sn2) return (xc-xb)*(xc-xb)+(yc-yb)*(yc-yb);

  float ah2 = dp*dp / sn2;
  float un2=ux*ux+uy*uy;
  return un2-ah2;
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  
  int32_t zoom, closeup;
  float wd = self->dev->preview_pipe->backbuf_width;
  float ht = self->dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  static int old_grab = -1;
  int grab = get_grab (pzx, pzy, g, 30.0/zoom_scale, wd, ht);

  if(darktable.control->button_down && darktable.control->button_down_which == 3 && g->k_show != 1)
  {
    // second mouse button, straighten activated:
    g->straightening = 1;
    dt_control_change_cursor(GDK_CROSSHAIR);
    dt_control_queue_redraw_center();
  }
  else if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    //case when we drag a point for keystone
    if (g->k_drag == TRUE && g->k_selected >= 0)
    {
      float xx=pzx, yy=pzy;
      if (g->k_selected == 0)
      {
        if (p->k_sym == 1 || p->k_sym == 3) p->kxa = fminf(xx,(p->kxc+p->kxd-0.01f)/2.0f), p->kxb = p->kxc-p->kxa+p->kxd;
        else p->kxa=fminf(xx,p->kxb-0.01f);
        if (p->k_sym > 1) p->kya = fminf(yy,(p->kyc+p->kyb-0.01f)/2.0f), p->kyd = p->kyc-p->kya+p->kyb;
        else p->kya=fminf(yy,p->kyd-0.01f);
      }
      else if (g->k_selected == 1) 
      {
        if (p->k_sym == 1 || p->k_sym == 3) p->kxb = fmaxf(xx,(p->kxc+p->kxd+0.01f)/2.0f), p->kxa = p->kxc-p->kxb+p->kxd;
        else p->kxb=fmaxf(xx,p->kxa+0.01f);
        if (p->k_sym > 1) p->kyb = fminf(yy,(p->kya+p->kyd-0.01f)/2.0f), p->kyc = p->kya-p->kyb+p->kyd;
        else p->kyb=fminf(yy,p->kyc-0.01f);
      }
      else if (g->k_selected == 2)
      {
        if (p->k_sym == 1 || p->k_sym == 3) p->kxc = fmaxf(xx,(p->kxa+p->kxb+0.01f)/2.0f), p->kxd = p->kxa-p->kxc+p->kxb;
        else p->kxc=fmaxf(xx,p->kxd+0.01f);
        if (p->k_sym > 1) p->kyc = fmaxf(yy,(p->kya+p->kyd+0.01f)/2.0f), p->kyb = p->kya-p->kyc+p->kyd;
        else p->kyc=fmaxf(yy,p->kyb+0.01f);
      }
      else if (g->k_selected == 3)
      {
        if (p->k_sym == 1 || p->k_sym == 3) p->kxd = fminf(xx,(p->kxa+p->kxb-0.01f)/2.0f), p->kxc = p->kxa-p->kxd+p->kxb;
        else p->kxd=fminf(xx,p->kxc-0.01f);
        if (p->k_sym > 1) p->kyd = fmaxf(yy,(p->kyc+p->kyb+0.01f)/2.0f), p->kya = p->kyc-p->kyd+p->kyb;
        else p->kyd=fmaxf(yy,p->kya+0.01f);
      }
      dt_control_queue_redraw_center();
      return 1;
    }
    //case when we drag a segment for keystone
    if (g->k_drag == TRUE && g->k_selected_segment >= 0)
    {
      float decalx = pzx - g->button_down_zoom_x;
      float decaly = pzy - g->button_down_zoom_y;
      if (g->k_selected_segment == 0 && (p->k_type == 1 || p->k_type == 3))
      {
        decaly = fminf(decaly,p->kyd-p->kya);
        decaly = fminf(decaly,p->kyc-p->kyb);
        p->kxa += decalx;
        p->kya += decaly;
        p->kxb += decalx;
        p->kyb += decaly;
      }
      else if (g->k_selected_segment == 1 && (p->k_type == 2 || p->k_type == 3)) 
      {
        decalx = fmaxf(decalx,p->kxa-p->kxb);
        decalx = fmaxf(decalx,p->kxd-p->kxc);
        p->kxc += decalx;
        p->kyc += decaly;
        p->kxb += decalx;
        p->kyb += decaly;
      }
      else if (g->k_selected_segment == 2 && (p->k_type == 1 || p->k_type == 3)) 
      {
        decaly = fmaxf(decaly,p->kya-p->kyd);
        decaly = fmaxf(decaly,p->kyb-p->kyc);
        p->kxc += decalx;
        p->kyc += decaly;
        p->kxd += decalx;
        p->kyd += decaly;
      }
      else if (g->k_selected_segment == 3 && (p->k_type == 2 || p->k_type == 3)) 
      {
        decalx = fminf(decalx,p->kxb-p->kxa);
        decalx = fminf(decalx,p->kxc-p->kxd);
        p->kxa += decalx;
        p->kya += decaly;
        p->kxd += decalx;
        p->kyd += decaly;
      }
      g->button_down_zoom_x = pzx;
      g->button_down_zoom_y = pzy;
      dt_control_queue_redraw_center();
      return 1;
    }
    // draw a light gray frame, to show it's not stored yet:
    g->applied = 0;
    // first mouse button, adjust cropping frame, but what do we do?
    float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    if(!g->cropping && !g->straightening && g->k_show != 1)
    {
      g->cropping = grab;
      if(!grab)
      {
        g->cropping = 15;
        g->handle_x = g->clip_x;
        g->handle_y = g->clip_y;
      }
      if(grab & 1) g->handle_x = bzx-g->clip_x;
      if(grab & 2) g->handle_y = bzy-g->clip_y;
      if(grab & 4) g->handle_x = bzx-(g->clip_w + g->clip_x);
      if(grab & 8) g->handle_y = bzy-(g->clip_h + g->clip_y);
      if(!grab && darktable.control->button_down_which == 3) g->straightening = 1;
    }
    if(!g->straightening && darktable.control->button_down_which == 1 && g->k_show != 1)
    {
      grab = g->cropping;

      if(grab == 15)
      {
        /* moving the crop window */
        g->clip_x = fminf(1.0 - g->clip_w, fmaxf(0.0, g->handle_x + pzx - bzx));
        g->clip_y = fminf(1.0 - g->clip_h, fmaxf(0.0, g->handle_y + pzy - bzy));
      }
      else
      {
        /* changing the crop window */
        if (g->center_lock)
        {
          /* the center is locked, scale crop radial with locked ratio */
          gboolean flag = FALSE;
          float length = 0.0;
          float xx = 0.0;
          float yy = 0.0;

          if (grab & 1 || grab & 4)
            xx = (grab & 1) ? (pzx-bzx) : (bzx-pzx);
          if (grab & 2 || grab & 8)
            yy = (grab & 2) ? (pzy-bzy) : (bzy-pzy);

          length = (fabs(xx) > fabs(yy)) ? xx : yy;

          if ((g->prev_clip_w - (length+length)) < 0.1 ||
              (g->prev_clip_h - (length+length)) < 0.1)
            flag = TRUE;

          g->clip_x = flag ? g->clip_x : g->prev_clip_x + length;
          g->clip_y = flag ? g->clip_y : g->prev_clip_y + length;
          g->clip_w = fmax(0.1, g->prev_clip_w - (length+length));
          g->clip_h = fmax(0.1, g->prev_clip_h - (length+length));

        }
        else
        {

          if(grab & 1)
          {
            const float old_clip_x = g->clip_x;
            g->clip_x = fmaxf(0.0, pzx - g->handle_x);
            g->clip_w = fmaxf(0.1, old_clip_x + g->clip_w - g->clip_x);
          }
          if(grab & 2)
          {
            const float old_clip_y = g->clip_y;
            g->clip_y = fmaxf(0.0, pzy - g->handle_y);
            g->clip_h = fmaxf(0.1, old_clip_y + g->clip_h - g->clip_y);
          }
          if(grab & 4) g->clip_w = fmaxf(0.1, fminf(1.0, pzx - g->clip_x - g->handle_x));
          if(grab & 8) g->clip_h = fmaxf(0.1, fminf(1.0, pzy - g->clip_y - g->handle_y));
        }

        if(g->clip_x + g->clip_w > 1.0) g->clip_w = 1.0 - g->clip_x;
        if(g->clip_y + g->clip_h > 1.0) g->clip_h = 1.0 - g->clip_y;
      }
      apply_box_aspect(self, grab);
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (grab && g->k_show != 1)
  {
    // hover over active borders, no button pressed
    if(old_grab != grab)
    {
      // change mouse pointer
      if     (grab == 1)  dt_control_change_cursor(GDK_LEFT_SIDE);
      else if(grab == 2)  dt_control_change_cursor(GDK_TOP_SIDE);
      else if(grab == 4)  dt_control_change_cursor(GDK_RIGHT_SIDE);
      else if(grab == 8)  dt_control_change_cursor(GDK_BOTTOM_SIDE);
      else if(grab == 3)  dt_control_change_cursor(GDK_TOP_LEFT_CORNER);
      else if(grab == 6)  dt_control_change_cursor(GDK_TOP_RIGHT_CORNER);
      else if(grab == 12) dt_control_change_cursor(GDK_BOTTOM_RIGHT_CORNER);
      else if(grab == 9)  dt_control_change_cursor(GDK_BOTTOM_LEFT_CORNER);
    }
    dt_control_queue_redraw_center();
  }
  else
  {
    // somewhere besides borders. maybe rotate?
    if(old_grab != grab) dt_control_change_cursor(GDK_FLEUR);
    g->straightening = g->cropping = 0;
    //or maybe keystone
    float ext = 0.005f / zoom_scale;
    if (g->k_show == 1 && g->k_drag == FALSE)
    {
      float xx=pzx, yy=pzy;
      //are we near a keystone point ?
      g->k_selected = -1;
      g->k_selected_segment = -1;
      if (xx<p->kxa+ext && xx>p->kxa-ext && yy<p->kya+ext && yy>p->kya-ext) g->k_selected = 0;
      if (xx<p->kxb+ext && xx>p->kxb-ext && yy<p->kyb+ext && yy>p->kyb-ext) g->k_selected = 1;
      if (xx<p->kxc+ext && xx>p->kxc-ext && yy<p->kyc+ext && yy>p->kyc-ext) g->k_selected = 2;
      if (xx<p->kxd+ext && xx>p->kxd-ext && yy<p->kyd+ext && yy>p->kyd-ext) g->k_selected = 3;
      //or near a keystone segment
      if (g->k_selected < 0)
      {
        if (p->k_type == 1 || p->k_type == 3)
        {
          if (dist_seg(p->kxa,p->kya,p->kxb,p->kyb,pzx,pzy) < ext*ext) g->k_selected_segment = 0;
          else if (dist_seg(p->kxd,p->kyd,p->kxc,p->kyc,pzx,pzy) < ext*ext) g->k_selected_segment = 2;
        }
        if (p->k_type == 1 || p->k_type == 3)
        {
          if (dist_seg(p->kxb,p->kyb,p->kxc,p->kyc,pzx,pzy) < ext*ext) g->k_selected_segment = 1;
          else if (dist_seg(p->kxd,p->kyd,p->kxa,p->kya,pzx,pzy) < ext*ext) g->k_selected_segment = 3;
        }
      }
      if (g->k_selected >=0) dt_control_change_cursor(GDK_CROSS);
      else dt_control_change_cursor(GDK_FLEUR);
    }
    dt_control_queue_redraw_center();
  }
  old_grab = grab;
  return 0;
}

static void
commit_box (dt_iop_module_t *self, dt_iop_clipping_gui_data_t *g, dt_iop_clipping_params_t *p)
{
  if(darktable.gui->reset) return;
  g->old_clip_x = g->clip_x;
  g->old_clip_y = g->clip_y;
  g->old_clip_w = g->clip_w;
  g->old_clip_h = g->clip_h;
  g->cropping = 0;
  if(!self->enabled)
  {
    // first time crop, if any data is stored in p, it's obsolete:
    p->cx = p->cy = 0.0f;
    p->cw = p->ch = 1.0f;
  }
  p->cx = g->clip_x;
  p->cy = g->clip_y;
  p->cw = copysignf(p->cx + g->clip_w, p->cw);
  p->ch = copysignf(p->cy + g->clip_h, p->ch);
  
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  g->applied = 1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  if(g->straightening)
  {
    float dx = x - g->button_down_x, dy = y - g->button_down_y;
    if(dx < 0)
    {
      dx = -dx;
      dy = - dy;
    }
    float angle = atan2f(dy, dx);
    if(!(angle >= - M_PI/2.0 && angle <= M_PI/2.0)) angle = 0.0f;
    float close = angle;
    if     (close >  M_PI/4.0) close =  M_PI/2.0 - close;
    else if(close < -M_PI/4.0) close = -M_PI/2.0 - close;
    else close = - close;
    float a = 180.0/M_PI*close + g->button_down_angle;
    if(a < -180.0) a += 360.0;
    if(a >  180.0) a -= 360.0;
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
    dt_bauhaus_slider_set(g->angle, -a);
    dt_control_change_cursor(GDK_LEFT_PTR);
  }
  if (g->k_drag) g->k_drag = FALSE;

  /* reset internal ui states*/
  g->center_lock = g->straightening = g->cropping = 0;
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  // avoid unexpected back to lt mode:
  if(type == GDK_2BUTTON_PRESS && which == 1)
  {
    dt_iop_request_focus(NULL);
    return 1;
  }
  if(which == 3 || which == 1)
  {
    if (self->off)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

    if (g->k_show == 1)
    {
      if (g->k_selected >= 0) g->k_drag = TRUE; //if a keystone point is selected then we start to drag it
      else//if we click to the apply button
      {
        int32_t zoom, closeup;
        float wd = self->dev->preview_pipe->backbuf_width;
        //float ht = self->dev->preview_pipe->backbuf_height;
        DT_CTL_GET_GLOBAL(zoom, dev_zoom);
        DT_CTL_GET_GLOBAL(closeup, dev_closeup);
        float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
        float pzx, pzy;
        dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
        pzx += 0.5f;
        pzy += 0.5f;
        float c[2] = {(MIN(p->kxc,p->kxb)+MAX(p->kxa,p->kxd))/2.0f, (MIN(p->kyc,p->kyd)+MAX(p->kya,p->kyb))/2.0f};
        float ext = 10.0/(wd*zoom_scale);
        float xx=pzx, yy=pzy;
        //Apply button
        if (pzx>c[0]-ext && pzx<c[0]+ext && pzy>c[1]-ext && pzy<c[1]+ext)
        {
          //add an entry to the combo box and select it
          keystone_type_populate(self,TRUE,99);
          //reset gui settings
          g->k_show = 2;
          g->k_selected = -1;
          g->k_drag = FALSE;
          //do the changes
          p->k_apply = 1;
          commit_box(self,g,p);
        }
        else
        {
          //Horizontal symetry button (1)
          c[0] = (p->kxa+p->kxd)/2.0f, c[1] = (p->kya+p->kyd)/2.0f;
          if (xx>c[0]-ext && xx<c[0]+ext && yy>c[1]-ext && yy<c[1]+ext && (p->k_type==1 || p->k_type==3))
          {
            if (p->k_sym == 0) p->k_sym = 1;
            else if(p->k_sym == 1) p->k_sym = 0;
            else if(p->k_sym == 2) p->k_sym = 3;
            else p->k_sym = 2;
          }
          else
          {
            //Horizontal symetry button (2)
            c[0] = (p->kxb+p->kxc)/2.0f, c[1] = (p->kyb+p->kyc)/2.0f;
            if (xx>c[0]-ext && xx<c[0]+ext && yy>c[1]-ext && yy<c[1]+ext && (p->k_type==1 || p->k_type==3))
            {
              if (p->k_sym == 0) p->k_sym = 1;
              else if(p->k_sym == 1) p->k_sym = 0;
              else if(p->k_sym == 2) p->k_sym = 3;
              else p->k_sym = 2;
            }
            else
            {
              //vertical symetry button (1)
              c[0] = (p->kxb+p->kxa)/2.0f, c[1] = (p->kyb+p->kya)/2.0f;
              if (xx>c[0]-ext && xx<c[0]+ext && yy>c[1]-ext && yy<c[1]+ext && (p->k_type==2 || p->k_type==3))
              {
                if (p->k_sym == 0) p->k_sym = 2;
                else if(p->k_sym == 1) p->k_sym = 3;
                else if(p->k_sym == 2) p->k_sym = 0;
                else p->k_sym = 1;
              }
              else
              {
                //vertical symetry button (2)
                c[0] = (p->kxc+p->kxd)/2.0f, c[1] = (p->kyc+p->kyd)/2.0f;
                if (xx>c[0]-ext && xx<c[0]+ext && yy>c[1]-ext && yy<c[1]+ext && (p->k_type==2 || p->k_type==3))
                {
                  if (p->k_sym == 0) p->k_sym = 2;
                  else if(p->k_sym == 1) p->k_sym = 3;
                  else if(p->k_sym == 2) p->k_sym = 0;
                  else p->k_sym = 1;
                }
                else
                {
                  //dragging a border ?
                  if (g->k_selected_segment >=0)
                  {
                    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
                    g->button_down_zoom_x += 0.5;
                    g->button_down_zoom_y += 0.5;
                    g->k_drag = TRUE;
                  }
                }
              }
            }
          }
        }
      }
    }
    else
    {
      g->button_down_x = x;
      g->button_down_y = y;
      dt_dev_get_pointer_zoom_pos(self->dev, x, y, &g->button_down_zoom_x, &g->button_down_zoom_y);
      g->button_down_angle = p->angle;
  
      /* update prev clip box with current */
      g->prev_clip_x = g->clip_x;
      g->prev_clip_y = g->clip_y;
      g->prev_clip_w = g->clip_w;
      g->prev_clip_h = g->clip_h;
  
      /* if shift is pressed, then lock crop on center */
      if ((state&GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
        g->center_lock = 1;
    }  
    
    return 1;
  }
  else return 0;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, TRUE, NC_("accel", "commit"),
                        GDK_Return, 0);
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "angle"));
  //dt_accel_register_slider_iop(self, FALSE, NC_("accel", "keystone h"));
  //dt_accel_register_slider_iop(self, FALSE, NC_("accel", "keystone v"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t*)self->gui_data;
  GClosure *closure;

  closure = g_cclosure_new(G_CALLBACK(key_commit_callback),
                           (gpointer)self, NULL);
  dt_accel_connect_iop(self, "commit", closure);

  dt_accel_connect_slider_iop(self, "angle", GTK_WIDGET(g->angle));
  //dt_accel_connect_slider_iop(self, "keystone h", GTK_WIDGET(g->keystone_h));
  //dt_accel_connect_slider_iop(self, "keystone v", GTK_WIDGET(g->keystone_v));
}

#undef PHI
#undef INVPHI
#undef GUIDE_NONE
#undef GUIDE_GRID
#undef GUIDE_THIRD
#undef GUIDE_DIAGONAL
#undef GUIDE_TRIANGL
#undef GUIDE_GOLDEN

#undef NUM_RATIOS

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
