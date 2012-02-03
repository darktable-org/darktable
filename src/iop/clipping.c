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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <gdk/gdkkeysyms.h>
#include <assert.h>

#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/debug.h"
#include "dtgtk/label.h"
#include "dtgtk/slider.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"

DT_MODULE(3)

// number of gui ratios in combo box
#define NUM_RATIOS 10

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
}
dt_iop_clipping_params_t;

/* calculate the aspect ratios for current image */
static void _iop_clipping_update_ratios(dt_iop_module_t *self);

int
legacy_params (dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    dt_iop_clipping_params_t *o = (dt_iop_clipping_params_t *)old_params;
    dt_iop_clipping_params_t *n = (dt_iop_clipping_params_t *)new_params;
    *n = *o; // only the old k field was split to k_h and k_v, everything else is copied as is
    uint32_t intk = *(uint32_t *)&o->k_h;
    int is_horizontal;
    if(intk & 0x40000000u) is_horizontal = 1;
    else                   is_horizontal = 0;
    intk &= ~0x40000000;
    float floatk = *(float *)&intk;
    if(is_horizontal)
    {
      n->k_h = floatk;
      n->k_v = 0.0;
    }
    else
    {
      n->k_h = 0.0;
      n->k_v = floatk;
    }
    return 0;
  }
  return 1;
}
typedef struct dt_iop_clipping_gui_data_t
{
  GtkDarktableSlider *scale5, *keystone_h,*keystone_v;
  GtkWidget *swap_button;
  GtkDarktableToggleButton *hflip,*vflip;
  GtkComboBoxEntry *aspect_presets;
  GtkComboBox *guide_lines;
  GtkLabel *label7;
  GtkDarktableToggleButton *flipHorGoldenGuide, *flipVerGoldenGuide;
  GtkCheckButton *goldenSectionBox, *goldenSpiralSectionBox, *goldenSpiralBox, *goldenTriangleBox;

  float button_down_x, button_down_y;
  float button_down_zoom_x, button_down_zoom_y, button_down_angle; // position in image where the button has been pressed.
  /* current clip box */
  float clip_x, clip_y, clip_w, clip_h, handle_x, handle_y;
  /* last committed clip box */
  float old_clip_x, old_clip_y, old_clip_w, old_clip_h;
  /* last box before change */
  float prev_clip_x, prev_clip_y, prev_clip_w, prev_clip_h;

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
}
dt_iop_clipping_data_t;

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
void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  // use whole-buffer roi information to create matrix and inverse.
  float rt[] = { cosf(d->angle), sinf(d->angle),
                 -sinf(d->angle), cosf(d->angle)
               };
  if(d->angle == 0.0f)
  {
    rt[0] = rt[3] = 1.0;
    rt[1] = rt[2] = 0.0f;
  }

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
  // sanity check.
  if(roi_out->width  < 1) roi_out->width  = 1;
  if(roi_out->height < 1) roi_out->height = 1;
  // save rotation crop on output buffer in world scale:
  d->cix = roi_out->x;
  d->ciy = roi_out->y;
  d->ciw = roi_out->width;
  d->cih = roi_out->height;

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
  float p[2], o[2], aabb[4] = {roi_out->x+d->cix*so, roi_out->y+d->ciy*so, roi_out->x+d->cix*so+roi_out->width, roi_out->y+d->ciy*so+roi_out->height};
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
    // mul_mat_vec_2(d->m, p, o);
    backtransform(p, o, d->m, d->k_h, d->k_v);
    o[0] *= so;
    o[1] *= so;
    o[0] += d->tx*so;
    o[1] += d->ty*so;
    // transform to roi_in space, get aabb.
    adjust_aabb(o, aabb_in);
  }

  // adjust roi_in to minimally needed region
  roi_in->x      = aabb_in[0]-2;
  roi_in->y      = aabb_in[1]-2;
  roi_in->width  = aabb_in[2]-aabb_in[0]+4;
  roi_in->height = aabb_in[3]-aabb_in[1]+4;

  if(d->angle == 0.0f && d->all_off)
  {
    // just crop: make sure everything is precise.
    roi_in->x      = aabb_in[0];
    roi_in->y      = aabb_in[1];
    roi_in->width  = roi_out->width;
    roi_in->height = roi_out->height;
  }
}

// 3rd (final) pass: you get this input region (may be different from what was requested above),
// do your best to fill the ouput region!
void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;

  const int ch = piece->colors;
  const int ch_width = ch*roi_in->width;

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
        for(int c=0; c<3; c++) out[c] = in[c];
        out += ch;
        in += ch;
      }
    }
  }
  else
  {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(d,ivoid,ovoid,roi_in,roi_out)
#endif
    // (slow) point-by-point transformation.
    // TODO: optimize with scanlines and linear steps between?
    for(int j=0; j<roi_out->height; j++)
    {
      float *out = ((float *)ovoid)+ch*j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out+=ch)
      {
        float pi[2], po[2];

        pi[0] = roi_out->x + roi_out->scale*d->cix + i + .5;
        pi[1] = roi_out->y + roi_out->scale*d->ciy + j + .5;
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
        // transform this point to roi_in
        po[0] -= roi_in->x;
        po[1] -= roi_in->y;

        const int ii = (int)po[0], jj = (int)po[1];
        if(ii >= 0 && jj >= 0 && ii <= roi_in->width-2 && jj <= roi_in->height-2)
        {
          const float *in = ((float *)ivoid) + ch*(roi_in->width*jj+ii);
          const float fi = po[0] - ii, fj = po[1] - jj;
          for(int c=0; c<3; c++,in++)
            out[c] =
              ((1.0f-fj)*(1.0f-fi)*in[0] +
               (1.0f-fj)*(     fi)*in[ch] +
               (     fj)*(     fi)*in[ch_width+ch] +
               (     fj)*(1.0f-fi)*in[ch_width]);
        }
        else for(int c=0; c<3; c++) out[c] = 0.0f;
      }
    }
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)p1;
  dt_iop_clipping_data_t *d = (dt_iop_clipping_data_t *)piece->data;
  // pull in bit from weird p->k => d->keystone = 1
  d->all_off = 1;
  if(fabsf(p->k_h) >= .0001) d->all_off = 0;
  if(p->k_h >= -1.0 && p->k_h <= 1.0) d->ki_h = p->k_h;
  else d->ki_h = 0.0f;
  if(fabsf(p->k_v) >= .0001) d->all_off = 0;
  if(p->k_v >= -1.0 && p->k_v <= 1.0) d->ki_v = p->k_v;
  else d->ki_v = 0.0f;
  d->angle = M_PI/180.0 * p->angle;
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
  d->flags = (p->ch < 0 ? FLAG_FLIP_VERTICAL : 0) | (p->cw < 0 ? FLAG_FLIP_HORIZONTAL : 0);
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
      g->clip_w = p->cw - p->cx;
      g->clip_y = p->cy;
      g->clip_h = p->ch - p->cy;
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

    // aspect = wd*w/ht*h
    // if we only modified one dim, respectively, we wanted these values:
    const float target_h = wd*g->clip_w/(ht*aspect);
    const float target_w = ht*g->clip_h*aspect/wd;
    // i.e. target_w/h = w/target_h = aspect


    // first fix aspect ratio:

    // corners: move two adjacent
    if     (grab == 1+2)
    {
      // move x y
      g->clip_x = g->clip_x + g->clip_w - (target_w + g->clip_w)*.5;
      g->clip_y = g->clip_y + g->clip_h - (target_h + g->clip_h)*.5;
      g->clip_w = (target_w + g->clip_w)*.5f;
      g->clip_h = (target_h + g->clip_h)*.5f;
    }
    else if(grab == 2+4) // move y w
    {
      g->clip_y = g->clip_y + g->clip_h - (target_h + g->clip_h)*.5;
      g->clip_w = (target_w + g->clip_w)*.5;
      g->clip_h = (target_h + g->clip_h)*.5;
    }
    else if(grab == 4+8) // move w h
    {
      g->clip_w = (target_w + g->clip_w)*.5;
      g->clip_h = (target_h + g->clip_h)*.5;
    }
    else if(grab == 8+1) // move h x
    {
      g->clip_h = (target_h + g->clip_h)*.5;
      g->clip_x = g->clip_x + g->clip_w - (target_w + g->clip_w)*.5;
      g->clip_w = (target_w + g->clip_w)*.5;
    }
    else if(grab & 5) // dragged either x or w (1 4)
    {
      // change h and move y, h equally
      const float off = target_h - g->clip_h;
      g->clip_h = g->clip_h + off;
      g->clip_y = g->clip_y - .5f*off;
    }
    else if(grab & 10) // dragged either y or h (2 8)
    {
      // channge w and move x, w equally
      const float off = target_w - g->clip_w;
      g->clip_w = g->clip_w + off;
      g->clip_x = g->clip_x - .5f*off;
    }

    // now fix outside boxes:
    if(g->clip_x < 0)
    {
      g->clip_h *= (g->clip_w + g->clip_x)/g->clip_w;
      g->clip_w  =  g->clip_w + g->clip_x;
      g->clip_x  = 0;
    }
    if(g->clip_y < 0)
    {
      g->clip_w *= (g->clip_h + g->clip_y)/g->clip_h;
      g->clip_h  =  g->clip_h + g->clip_y;
      g->clip_y  =  0;
    }
    if(g->clip_x + g->clip_w > 1.0)
    {
      g->clip_h *= (1.0 - g->clip_x)/g->clip_w;
      g->clip_w  =  1.0 - g->clip_x;
    }
    if(g->clip_y + g->clip_h > 1.0)
    {
      g->clip_w *= (1.0 - g->clip_y)/g->clip_h;
      g->clip_h  =  1.0 - g->clip_y;
    }
  }
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_clipping_params_t tmp = (dt_iop_clipping_params_t)
  {
    0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f
  };
  memcpy(self->params, &tmp, sizeof(dt_iop_clipping_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_clipping_params_t));
}

static void
aspect_free_activated (GtkEntry *entry, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  gchar *text = g_strdup(gtk_entry_get_text(entry));
  if(text)
  {
    gchar *c = text;
    while(*c != ':' && *c != '/' && c < text + strlen(text)) c++;
    if(c < text + strlen(text) - 1)
    {
      *c = '\0';
      c++;
      g->current_aspect = atof(text) / atof(c);
      dt_conf_set_int("plugins/darkroom/clipping/custom_aspect", g->current_aspect);
      apply_box_aspect(self, 5);
      dt_control_queue_redraw_center();
      dt_iop_request_focus(self);
    }
    g_free(text);
  }
}

static void
aspect_presets_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = gtk_combo_box_get_active(combo);
  if (which < 0)
  {
    // parse config param:
    if(g->current_aspect == -1.0f)
    {
      g->current_aspect = dt_conf_get_float("plugins/darkroom/clipping/custom_aspect");
      if(g->current_aspect <= 0.0f) g->current_aspect = 1.5f;
      char text[128];
      snprintf(text, 128, "%.3f:1", g->current_aspect);
      gtk_entry_set_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo))), text);
      apply_box_aspect(self, 5);
      dt_control_queue_redraw_center();
    }
    // user is typing, don't overwrite it.
    g->current_aspect = -2.0f;
    // reset to free aspect ratio:
    dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", -1);
  }
  else if (which < NUM_RATIOS)
  {
    dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", which);

    g->current_aspect = g->aspect_ratios[which];

    apply_box_aspect(self, 5);
    dt_control_queue_redraw_center();
    dt_iop_request_focus(self);
  }
}

static void
angle_callback (GtkDarktableSlider *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  p->angle = - dtgtk_slider_get_value(slider);
  commit_box (self, g, p);
}

static void
keystone_callback_h (GtkWidget *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  // we need k to be abs(k) < 2, so the second bit will always be zero (except we set it:).
  p->k_h = fmaxf(-1.9, fminf(1.9, dtgtk_slider_get_value(g->keystone_h)));
  commit_box (self, g, p);
}
static void
keystone_callback_v (GtkWidget *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  // we need k to be abs(k) < 2, so the second bit will always be zero (except we set it:).
  p->k_v = fmaxf(-1.9, fminf(1.9, dtgtk_slider_get_value(g->keystone_v)));
  commit_box (self, g, p);
}

void gui_reset(struct dt_iop_module_t *self)
{
  /* reset aspect preset to default */
  dt_conf_set_int("plugins/darkroom/clipping/aspect_preset", 1);

}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;

  /* recalc aspect ratios for image */
  _iop_clipping_update_ratios(self);

  /* update ui elements */
  dtgtk_slider_set_value(g->scale5, -p->angle);
  dtgtk_slider_set_value(g->keystone_h, p->k_h);
  dtgtk_slider_set_value(g->keystone_v, p->k_v);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->hflip), p->cw < 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->vflip), p->ch < 0);
  
  int act = dt_conf_get_int("plugins/darkroom/clipping/aspect_preset");
  if (act < -1 || act >= NUM_RATIOS) 
    act = 0;



  /* special handling the combobox when current act is already selected
     callback is not called, let do it our self then..
   */
  if (gtk_combo_box_get_active(GTK_COMBO_BOX(g->aspect_presets)) == act)
    aspect_presets_changed(GTK_COMBO_BOX(g->aspect_presets), self);
  else
    gtk_combo_box_set_active(GTK_COMBO_BOX(g->aspect_presets), act);

  // reset gui draw box to what we have in the parameters:
  g->applied = 1;
  g->clip_x = p->cx;
  g->clip_w = p->cw - p->cx;
  g->clip_y = p->cy;
  g->clip_h = p->ch - p->cy;

}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_clipping_data_t));
  module->params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_params = malloc(sizeof(dt_iop_clipping_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_clipping_params_t);
  module->gui_data = NULL;
  module->priority = 380; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void
toggled_callback(GtkDarktableToggleButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t *p = (dt_iop_clipping_params_t *)self->params;
  if(widget==g->hflip)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) p->cw = copysignf(p->cw, -1.0);
    else                                     p->cw = copysignf(p->cw,  1.0);
  }
  else if(widget==g->vflip)
  {
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))) p->ch = copysignf(p->ch, -1.0);
    else                                     p->ch = copysignf(p->ch,  1.0);
  }
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

static void key_commit_callback(GtkAccelGroup *accel_group,
                                GObject *acceleratable,
                                guint keyval, GdkModifierType modifier,
                                gpointer data)
{
  dt_iop_module_t* self = (dt_iop_module_t*)data;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  commit_box(self, g, p);
}

static void
aspect_flip(GtkWidget *button, dt_iop_module_t *self)
{
  key_swap_callback(NULL, NULL, 0, 0, self);
}

// Golden number (1+sqrt(5))/2
#define PHI      1.61803398874989479F
// 1/PHI
#define INVPHI   0.61803398874989479F

#define GUIDE_NONE 0
#define GUIDE_GRID 1
#define GUIDE_THIRD 2
#define GUIDE_DIAGONAL 3
#define GUIDE_TRIANGL 4
#define GUIDE_GOLDEN 5

static void
guides_presets_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  int which = gtk_combo_box_get_active(combo);
  if (which == GUIDE_TRIANGL || which == GUIDE_GOLDEN )
  {
    gtk_widget_set_visible(GTK_WIDGET(g->label7), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), TRUE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(g->label7), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), FALSE);
  }

  if (which == GUIDE_GOLDEN)
  {
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), TRUE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), FALSE);
  }

  dt_iop_request_focus(self);
  dt_control_queue_redraw_center();
}

static void
guides_button_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  // redraw guides
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

  self->widget = gtk_table_new(10, 6, FALSE);
  gtk_table_set_row_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);
  gtk_table_set_col_spacings(GTK_TABLE(self->widget), DT_GUI_IOP_MODULE_CONTROL_SPACING);
  g->hflip = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,CPF_DIRECTION_UP));
  g->vflip = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,0));
  gtk_widget_set_size_request(GTK_WIDGET(g->hflip),0,22);
  gtk_widget_set_size_request(GTK_WIDGET(g->vflip),0,22);

  GtkWidget *label = gtk_label_new(_("flip"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 2, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->hflip), 2, 4, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->vflip), 4, 6, 0, 1, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_signal_connect (G_OBJECT (g->hflip), "toggled", G_CALLBACK(toggled_callback), self);
  g_signal_connect (G_OBJECT (g->vflip), "toggled", G_CALLBACK(toggled_callback), self);
  g_object_set(G_OBJECT(g->hflip), "tooltip-text", _("flip image horizontally"), (char *)NULL);
  g_object_set(G_OBJECT(g->vflip), "tooltip-text", _("flip image vertically"), (char *)NULL);

  g->scale5 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, -180.0, 180.0, 0.25, p->angle, 2));
  dtgtk_slider_set_label(g->scale5, _("angle"));
  dtgtk_slider_set_unit(g->scale5, "°");
  g_signal_connect (G_OBJECT (g->scale5), "value-changed",
                    G_CALLBACK (angle_callback), self);
  g_object_set(G_OBJECT(g->scale5), "tooltip-text", _("right-click and drag a line on the image to drag a straight line"), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->scale5), 0, 6, 1, 2, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  g->keystone_h = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, -1.0, 1.0, 0.01, 0.0, 2));
  dtgtk_slider_set_label(g->keystone_h, _("keystone h"));
  g_object_set(G_OBJECT(g->keystone_h), "tooltip-text", _("adjust perspective for horizontal keystone distortion"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->keystone_h), "value-changed",
                    G_CALLBACK (keystone_callback_h), self);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->keystone_h), 0, 6, 2, 3, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->keystone_v = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, -1.0, 1.0, 0.01, 0.0, 2));
  dtgtk_slider_set_label(g->keystone_v, _("keystone v"));
  g_object_set(G_OBJECT(g->keystone_v), "tooltip-text", _("adjust perspective for vertical keystone distortion"), (char *)NULL);
  g_signal_connect (G_OBJECT (g->keystone_v), "value-changed",
                    G_CALLBACK (keystone_callback_v), self);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->keystone_v), 0, 6, 3, 4, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  label = gtk_label_new(_("aspect"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(label), 0, 2, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->aspect_presets = GTK_COMBO_BOX_ENTRY(gtk_combo_box_entry_new_text());
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("free"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("image"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("golden cut"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("1:2"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("3:2"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("4:3"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("square"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("DIN"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("16:9"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(g->aspect_presets), _("5:4"));
  
  dt_gui_key_accel_block_on_focus(gtk_bin_get_child(GTK_BIN(g->aspect_presets)));

  int act = dt_conf_get_int("plugins/darkroom/clipping/aspect_preset");
  if(act < 0 || act >= NUM_RATIOS) act = 0;
  gtk_combo_box_set_active(GTK_COMBO_BOX(g->aspect_presets), act);
  g_signal_connect (G_OBJECT (g->aspect_presets), "changed",
                    G_CALLBACK (aspect_presets_changed), self);
  g_signal_connect (G_OBJECT (gtk_bin_get_child(GTK_BIN(g->aspect_presets))), "activate",
		    G_CALLBACK (aspect_free_activated), self);
  g_object_set(G_OBJECT(g->aspect_presets), "tooltip-text", _("set the aspect ratio (w:h)\npress ctrl-x to swap sides"), (char *)NULL);

  GtkBox *hbox = GTK_BOX(gtk_hbox_new(FALSE, 5));
  gtk_box_pack_start(hbox, GTK_WIDGET(g->aspect_presets), TRUE, TRUE, 0);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_aspectflip, CPF_STYLE_FLAT);
  g->swap_button = GTK_WIDGET(button);
  g_signal_connect (G_OBJECT (button), "clicked", G_CALLBACK (aspect_flip), self);
  g_object_set(G_OBJECT(button), "tooltip-text", _("swap the aspect ratio (ctrl-x)"), (char *)NULL);
  gtk_box_pack_start(hbox, button, TRUE, FALSE, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(hbox), 2, 6, 4, 5, GTK_EXPAND|GTK_FILL, 0, 0, 0);


  /*-------------------------------------------*/
  gtk_table_set_row_spacing(GTK_TABLE(self->widget), 4, 10);
  label = GTK_WIDGET(dtgtk_label_new(_("guides"),DARKTABLE_LABEL_TAB|DARKTABLE_LABEL_ALIGN_RIGHT));
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 6, 5, 6, GTK_EXPAND|GTK_FILL, 0, 0, 5);
  g->guide_lines = GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->guide_lines, _("none"));
  gtk_combo_box_append_text(g->guide_lines, _("grid"));
  gtk_combo_box_append_text(g->guide_lines, _("rules of thirds"));
  gtk_combo_box_append_text(g->guide_lines, _("diagonal method"));
  gtk_combo_box_append_text(g->guide_lines, _("harmonious triangles"));
  gtk_combo_box_append_text(g->guide_lines, _("golden mean"));
  gtk_combo_box_set_active(g->guide_lines, GUIDE_NONE);
  g_object_set(G_OBJECT(g->guide_lines), "tooltip-text", _("with this option, you can display guide lines "
               "to help compose your photograph."), (char *)NULL);
  g_signal_connect (G_OBJECT (g->guide_lines), "changed",
                    G_CALLBACK (guides_presets_changed), self);
  label = gtk_label_new(_("type"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), label, 0, 2, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->guide_lines), 2, 6, 6, 7, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  /*-------------------------------------------*/
  g->label7 = GTK_LABEL(gtk_label_new(_("flip")));
  gtk_misc_set_alignment(GTK_MISC(g->label7), 0.0, 0.5);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->label7), 0, 2, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g->flipHorGoldenGuide = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,CPF_DIRECTION_UP));
  g->flipVerGoldenGuide = DTGTK_TOGGLEBUTTON(dtgtk_togglebutton_new(dtgtk_cairo_paint_flip,0));
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->flipHorGoldenGuide), 2, 4, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->flipVerGoldenGuide), 4, 6, 7, 8, GTK_EXPAND|GTK_FILL, 0, 0, 0);
  g_object_set(G_OBJECT(g->flipHorGoldenGuide), "tooltip-text", _("flip guides horizontally"), (char *)NULL);
  g_object_set(G_OBJECT(g->flipVerGoldenGuide), "tooltip-text", _("flip guides vertically"), (char *)NULL);
  /*-------------------------------------------*/
  g->goldenSectionBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden sections")));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->goldenSectionBox), TRUE);
  g_object_set(G_OBJECT(g->goldenSectionBox), "tooltip-text", _("enable this option to show golden sections."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->goldenSectionBox), 0, 3, 8, 9, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->goldenSpiralSectionBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("spiral sections")));
  g_object_set(G_OBJECT(g->goldenSpiralSectionBox), "tooltip-text", _("enable this option to show golden spiral sections."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->goldenSpiralSectionBox), 3, 6, 8, 9, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->goldenSpiralBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden spiral")));
  g_object_set(G_OBJECT(g->goldenSpiralBox), "tooltip-text", _("enable this option to show a golden spiral guide."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->goldenSpiralBox), 0, 3, 9, 10, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g->goldenTriangleBox = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("golden triangles")));
  g_object_set(G_OBJECT(g->goldenTriangleBox), "tooltip-text", _("enable this option to show golden triangles."), (char *)NULL);
  gtk_table_attach(GTK_TABLE(self->widget), GTK_WIDGET(g->goldenTriangleBox), 3, 6, 9, 10, GTK_EXPAND|GTK_FILL, 0, 0, 0);

  g_signal_connect (G_OBJECT (g->flipHorGoldenGuide), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->flipVerGoldenGuide), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSectionBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSpiralSectionBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenSpiralBox), "toggled", G_CALLBACK (guides_button_changed), self);
  g_signal_connect (G_OBJECT (g->goldenTriangleBox), "toggled", G_CALLBACK (guides_button_changed), self);

  gtk_widget_set_visible(GTK_WIDGET(g->label7), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->flipHorGoldenGuide), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->flipVerGoldenGuide), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSectionBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralSectionBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenSpiralBox), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->goldenTriangleBox), FALSE);

  gtk_widget_set_no_show_all(GTK_WIDGET(g->label7), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->flipHorGoldenGuide), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->flipVerGoldenGuide), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->goldenSectionBox), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->goldenSpiralSectionBox), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->goldenSpiralBox), TRUE);
  gtk_widget_set_no_show_all(GTK_WIDGET(g->goldenTriangleBox), TRUE);

  /*-------------------------------------------*/

  _iop_clipping_update_ratios(self);

  /* set default aspect ratio */
  g->current_aspect = g->aspect_ratios[act];
}

void _iop_clipping_update_ratios(dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = self->gui_data;

  g->aspect_ratios[0] = -1;
  g->aspect_ratios[1] = self->dev->image_storage.width / (float)self->dev->image_storage.height;
  g->aspect_ratios[2] = 1.6280;
  g->aspect_ratios[3] = 2.0/1.0;
  g->aspect_ratios[4] = 3.0/2.0;
  g->aspect_ratios[5] = 4.0/3.0;
  g->aspect_ratios[6] = 1.0;
  g->aspect_ratios[7] = sqrtf(2.0);
  g->aspect_ratios[8] = 16.0f/9.0f;
  g->aspect_ratios[9] = 5.0f/4.0f;

  // if adding new presets, make sure to change this as well:
  assert(NUM_RATIOS == 10);

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

static void
drawLine(cairo_t *cr, float left, float top, float right, float bottom)
{
  cairo_move_to(cr, left, top);
  cairo_line_to(cr, right,  bottom);
}

typedef struct QRect
{
  float left, top, right, bottom, width, height;
}
QRect;

static void
qRect(QRect *R1, float left, float top, float width, float height)
{
  R1->left=left;
  R1->top=top;
  R1->right=left+width;
  R1->bottom=top+height;
  R1->width=width;
  R1->height=height;
}

static void
drawDiagonalMethod(cairo_t *cr, const float x, const float y, const float w, const float h)
{
  if (w > h)
  {
    drawLine(cr, x, y, x+h, y+h);
    drawLine(cr, x, y+h, x+h, y);
    drawLine(cr, x+w-h, y, x+w, y+h);
    drawLine(cr, x+w-h, y+h, x+w, y);
  }
  else
  {
    drawLine(cr, x, y, x+w, y+w);
    drawLine(cr, x, y+w, x+w, y);
    drawLine(cr, x, y+h-w, x+w, y+h);
    drawLine(cr, x, y+h, x+w, y+h-w);
  }
}

static void
drawRulesOfThirds(cairo_t *cr, const float left, const float top,  const float right, const float bottom, const float xThird, const float yThird)
{
  drawLine(cr, left + xThird, top, left + xThird, bottom);
  drawLine(cr, left + 2*xThird, top, left + 2*xThird, bottom);

  drawLine(cr, left, top + yThird, right, top + yThird);
  drawLine(cr, left, top + 2*yThird, right, top + 2*yThird);
}

static void
drawHarmoniousTriangles(cairo_t *cr, const float left, const float top,  const float right, const float bottom, const float dst)
{
  float width, height;
  width = right - left;
  height = bottom - top;

  drawLine(cr, -width/2, -height/2, width/2,  height/2);
  drawLine(cr, -width/2+dst, -height/2, -width/2,  height/2);
  drawLine(cr, width/2, -height/2, width/2-dst,  height/2);
}

#define RADIANS(degrees) ((degrees) * (M_PI / 180.))
static void
drawGoldenMean(struct dt_iop_module_t *self, cairo_t *cr, QRect* R1, QRect* R2, QRect* R3, QRect* R4, QRect* R5, QRect* R6, QRect* R7)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;

  // Drawing Golden sections.
  if ( gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSectionBox)))
  {
    // horizontal lines:
    drawLine(cr, R1->left, R2->top, R2->right, R2->top);
    drawLine(cr, R1->left, R1->top + R2->height, R2->right, R1->top + R2->height);

    // vertical lines:
    drawLine(cr, R1->right, R1->top, R1->right, R1->bottom);
    drawLine(cr, R1->left+R2->width, R1->top, R1->left+R2->width, R1->bottom);
  }

  // Drawing Golden triangle guides.
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenTriangleBox)))
  {
    drawLine(cr, R1->left, R1->bottom, R2->right, R1->top);
    drawLine(cr, R1->left, R1->top, R2->right-R1->width, R1->bottom);
    drawLine(cr, R1->left + R1->width, R1->top, R2->right, R1->bottom);
  }

  // Drawing Golden spiral sections.
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSpiralSectionBox)))
  {
    drawLine(cr, R1->right, R1->top,    R1->right, R1->bottom);
    drawLine(cr, R2->left,  R2->top,    R2->right, R2->top);
    drawLine(cr, R3->left,  R3->top,    R3->left, R3->bottom);
    drawLine(cr, R4->left,  R4->bottom, R4->right, R4->bottom);
    drawLine(cr, R5->right, R5->top,    R5->right, R5->bottom);
    drawLine(cr, R6->left,  R6->top,    R6->right, R6->top);
    drawLine(cr, R7->left,  R7->top,    R7->left, R7->bottom);
  }

  // Drawing Golden Spiral.
  if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->goldenSpiralBox)))
  {
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R1->width/R1->height, 1);
    cairo_arc ( cr, R1->right/R1->width*R1->height, R1->top, R1->height, RADIANS(90), RADIANS(180) );
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R2->width/R2->height, 1);
    cairo_arc ( cr, R2->left/R2->width*R2->height, R2->top, R2->height, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R3->width/R3->height, 1);
    cairo_arc ( cr, R3->left/R3->width*R3->height, R3->bottom, R3->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R4->height/R4->width);
    cairo_arc ( cr, R4->right, R4->bottom/R4->height*R4->width, R4->width, RADIANS(180), RADIANS(270));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R5->height/R5->width);
    cairo_arc ( cr, R5->right, R5->top/R5->height*R5->width, R5->width, RADIANS(90), RADIANS(180));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R6->height/R6->width);
    cairo_arc ( cr, R6->left, R6->top/R6->height*R6->width, R6->width, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R7->width/R7->height, 1);
    cairo_arc ( cr, R7->left/R7->width*R7->height, R7->bottom, R7->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, (R6->width-R7->width)/R7->height, 1);
    cairo_arc ( cr, R7->left/(R6->width-R7->width)*R7->height, R7->bottom, R7->height, RADIANS(210), RADIANS(270));
    cairo_restore(cr);
  }
}
#undef RADIANS

static void
drawSimpleGrid(cairo_t *cr, const float left, const float top,  const float right, const float bottom, float zoom_scale)
{
  // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);
  dt_draw_grid(cr, 3, left, top, right, bottom);
  cairo_translate(cr, 1.0/zoom_scale, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  dt_draw_grid(cr, 3, left, top, right, bottom);
  cairo_set_source_rgba(cr, .8, .8, .8, 0.5);
  double dashes = 5.0/zoom_scale;
  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_grid(cr, 9, left, top, right, bottom);
}

// draw guides and handles over the image
void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;

  int32_t zoom, closeup;
  float zoom_x, zoom_y;
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f*wd-zoom_x*wd, -.5f*ht-zoom_y*ht);

  double dashes = 5.0/zoom_scale;

  // draw cropping window handles:
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, pointerx, pointery, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  cairo_set_dash (cr, &dashes, 0, 0);
  if(g->applied)
    cairo_set_source_rgba(cr, .0, .0, .0, .8);
  else
    cairo_set_source_rgba(cr, .0, .0, .0, .5);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle (cr, -1, -1, wd+2, ht+2);
  cairo_rectangle (cr, g->clip_x*wd, g->clip_y*ht, g->clip_w*wd, g->clip_h*ht);
  cairo_fill (cr);

  cairo_stroke (cr);

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

  // save context and draw guides
  cairo_save(cr);
  cairo_rectangle (cr, left, top, cwidth, cheight);
  cairo_clip(cr);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  cairo_set_dash(cr, &dashes, 1, 0);

  int which = gtk_combo_box_get_active(g->guide_lines);
  if (which == GUIDE_GRID)
  {
    drawSimpleGrid(cr, left, top, right, bottom, zoom_scale);
  }
  else if (which == GUIDE_DIAGONAL)
  {
    drawDiagonalMethod(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawDiagonalMethod(cr, left, top, cwidth, cheight);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_THIRD)
  {
    drawRulesOfThirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawRulesOfThirds(cr, left, top,  right, bottom, xThird, yThird);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_TRIANGL)
  {
    int dst = (int)((cheight*cos(atan(cwidth/cheight)) / (cos(atan(cheight/cwidth)))));
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipHorGoldenGuide)))
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipVerGoldenGuide)))
      cairo_scale(cr, 1, -1);

    drawHarmoniousTriangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);
    //p.setPen(QPen(d->guideColor, d->guideSize, Qt::DotLine));
    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawHarmoniousTriangles(cr, left, top,  right, bottom, dst);
    cairo_stroke (cr);
  }
  else if (which == GUIDE_GOLDEN)
  {
    // Move coordinates to local center selection.
    cairo_translate(cr, ((right - left)/2+left), ((bottom - top)/2+top));

    // Flip horizontal.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipHorGoldenGuide)))
      cairo_scale(cr, -1, 1);
    // Flip vertical.
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->flipVerGoldenGuide)))
      cairo_scale(cr, 1, -1);

    float w = cwidth;
    float h = cheight;

    // lengths for the golden mean and half the sizes of the region:
    float w_g = w*INVPHI;
    float h_g = h*INVPHI;
    float w_2 = w/2;
    float h_2 = h/2;

    QRect R1, R2, R3, R4, R5, R6, R7;
    qRect (&R1, -w_2, -h_2, w_g, h);

    // w - 2*w_2 corrects for one-pixel difference
    // so that R2.right() is really at the right end of the region
    qRect (&R2, w_g-w_2, h_2-h_g, w-w_g+1-(w - 2*w_2), h_g);
    qRect (&R3, w_2 - R2.width*INVPHI, -h_2, R2.width*INVPHI, h - R2.height);
    qRect (&R4, R2.left, R1.top, R3.left - R2.left, R3.height*INVPHI);
    qRect (&R5, R4.left, R4.bottom, R4.width*INVPHI, R3.height - R4.height);
    qRect (&R6, R5.left + R5.width, R5.bottom - R5.height*INVPHI, R3.left - R5.right, R5.height*INVPHI);
    qRect (&R7, R6.right - R6.width*INVPHI, R4.bottom, R6.width*INVPHI, R5.height - R6.height);

    drawGoldenMean(self, cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7);
    cairo_stroke (cr);

    cairo_set_dash (cr, &dashes, 0, 0);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    drawGoldenMean(self, cr, &R1, &R2, &R3, &R4, &R5, &R6, &R7);
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
  }
  else
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
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, int which)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
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

  if(darktable.control->button_down && darktable.control->button_down_which == 3)
  {
    // second mouse button, straighten activated:
    g->straightening = 1;
    dt_control_change_cursor(GDK_CROSSHAIR);
    dt_control_queue_redraw_center();
  }
  else if(darktable.control->button_down && darktable.control->button_down_which == 1)
  {
    // draw a light gray frame, to show it's not stored yet:
    g->applied = 0;
    // first mouse button, adjust cropping frame, but what do we do?
    float bzx = g->button_down_zoom_x + .5f, bzy = g->button_down_zoom_y + .5f;
    if(!g->cropping && !g->straightening)
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
    if(!g->straightening && darktable.control->button_down_which == 1)
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
  else if (grab)
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
    dtgtk_slider_set_value(g->scale5, -a);
    dt_control_change_cursor(GDK_LEFT_PTR);
  }

  /* reset internal ui states*/
  g->center_lock = g->straightening = g->cropping = 0;
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t *)self->gui_data;
  dt_iop_clipping_params_t   *p = (dt_iop_clipping_params_t   *)self->params;
  if(which == 3 || which == 1)
  {
    if (self->off) 
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

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

    return 1;
  }
  else return 0;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, TRUE, NC_("accel", "commit"),
                        GDK_Return, 0);
  dt_accel_register_iop(self, TRUE, NC_("accel", "swap the aspect ratio"),
                        GDK_x, 0);
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "angle"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "keystone h"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "keystone v"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_clipping_gui_data_t *g = (dt_iop_clipping_gui_data_t*)self->gui_data;
  GClosure *closure;

  closure = g_cclosure_new(G_CALLBACK(key_commit_callback),
                           (gpointer)self, NULL);
  dt_accel_connect_iop(self, "commit", closure);

  dt_accel_connect_button_iop(self, "swap the aspect ratio", g->swap_button);
  dt_accel_connect_slider_iop(self, "angle", GTK_WIDGET(g->scale5));
  dt_accel_connect_slider_iop(self, "keystone h", GTK_WIDGET(g->keystone_h));
  dt_accel_connect_slider_iop(self, "keystone v", GTK_WIDGET(g->keystone_v));
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

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
